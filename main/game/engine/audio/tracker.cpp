#include "decoder.hpp"
#include "audio.hpp"          // kMixRate
#include "sdkconfig.h"        // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (the default we restore)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
#include "xmp.h"              // components/libxmp-lite
}

// Tracker module playback -- MOD, XM, S3M and IT -- as a thin Decoder over libxmp-lite.
//
// WHY A MODULE IS WORTH SUPPORTING. A module carries its own instruments, so a three-minute
// looping song costs 50-300 KB instead of the ~30 MB the same audio would cost as PCM, and it
// stays musically editable afterwards: an MP3 is a photograph of a song, a module is the song.
// For a device with a 3 MB data partition whose mods arrive on an SD card, that is the whole
// argument -- plus thirty years of freely available material and free editors (OpenMPT,
// MilkyTracker) that still open it.
//
// WHY A LIBRARY AND NOT OUR OWN PLAYER. This file replaced a hand-written MOD/XM player of
// about 1,670 lines. That player worked -- correct pitch, correct tempo, no underruns -- but
// tracker playback is defined as much by the BUGS of the tracker that created each format as
// by any specification, and modules in the wild depend on those bugs. Matching them is a job
// measured in years of exposure to real music, which a from-scratch player validated against
// synthetic fixtures cannot claim however clean its test results look. libxmp has that
// exposure. It also made S3M and IT free, which the hand-written player had scoped out on the
// grounds of implementation cost -- a reason that stops existing the moment you link a library
// that already has them.
//
// HOW IT PLUGS IN. libxmp's xmp_play_buffer() is a pull API at a caller-chosen rate, which is
// exactly the shape of this engine's Decoder seam, so the adapter is mostly bookkeeping. The
// module mixes its own channels internally and hands back finished stereo, so a 16-channel XM
// costs ONE of the mixer's four voices, not sixteen of anything, and effects still land on top
// of it. It renders at kMixRate, so nothing resamples a module.
//
// MEMORY. Tracker samples are randomly accessed (loops, retriggers, sample offset), so a module
// has to be fully resident and cannot be streamed. libxmp has no allocator hooks and calls plain
// malloc, so getting a megabyte-scale module into PSRAM rather than the ~190 KB of free internal
// RAM is up to us -- see PsramBias below, which is not optional decoration but the thing that
// makes real music loadable at all.

namespace audio {

static const char* TAG = "AUD/TRK";

// Bias malloc toward PSRAM for the duration of a module load.
//
// WHY THIS IS NEEDED, measured rather than assumed: the project already sets
// CONFIG_SPIRAM_USE_MALLOC with CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384, so libxmp's big
// sample buffers land in PSRAM by themselves. Its MANY SMALL allocations do not -- a 24-channel,
// 60-pattern XM needs a track structure per pattern per channel, and 1440 allocations of a few
// hundred bytes each are all under the 16 KB threshold. Loading one took internal free heap from
// 190 KB down to 35 KB, which is not enough left for Wi-Fi to come up.
//
// heap_caps_malloc_extmem_enable() moves that threshold at runtime, so lowering it across the
// load routes the small allocations to PSRAM too, with no change to libxmp and no project-wide
// config change.
//
// CAVEAT, deliberately narrow: the threshold is global, so any OTHER allocation made anywhere
// during the load window also prefers PSRAM. That is safe (anything needing internal memory asks
// for it by capability -- MALLOC_CAP_DMA and friends bypass this entirely) and the window is the
// tens of milliseconds a load takes. It is scoped to the load and NOT to xmp_start_player, whose
// mixing buffers are touched every audio block and are better off internal.
class PsramBias {
public:
    PsramBias() { heap_caps_malloc_extmem_enable(64); }
    ~PsramBias() { heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL); }
    PsramBias(const PsramBias&) = delete;
    PsramBias& operator=(const PsramBias&) = delete;
};

// Frames per decode() call. Small enough to sit comfortably inside the streamer's ring, large
// enough that per-call overhead disappears.
static constexpr int TRK_BLOCK_FRAMES = 512;

// Ceiling on a module file. Generous -- the biggest modules in circulation are a couple of MB
// and PSRAM has ~7 MB free -- but bounded, because the length comes from a file on a card.
static constexpr long TRK_MAX_FILE = 4L * 1024 * 1024;

class XmpDecoder : public Decoder {
public:
    explicit XmpDecoder(FILE* f) : f_(f) {}

    ~XmpDecoder() override
    {
        if (ctx_) {
            if (started_) xmp_end_player(ctx_);
            xmp_release_module(ctx_);
            xmp_free_context(ctx_);
        }
        if (f_) fclose(f_);
    }

    // Read the file, ask libxmp whether it is a module at all, and if so load and start it.
    // Returns false for anything that is not a module we can play, which is what makes this
    // safe to offer every file in decoder_open()'s sniff order.
    bool load()
    {
        fseek(f_, 0, SEEK_END);
        const long size = ftell(f_);
        fseek(f_, 0, SEEK_SET);
        if (size <= 0) return false;
        if (size > TRK_MAX_FILE) {
            ESP_LOGW(TAG, "module is %ld KB (cap %ld KB)", size / 1024, TRK_MAX_FILE / 1024);
            return false;
        }

        // PSRAM: this buffer is only alive across the load, but it can be megabytes and
        // internal heap has ~190 KB free.
        void* image = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
        if (!image) image = malloc((size_t)size);
        if (!image) return false;
        const size_t got = fread(image, 1, (size_t)size, f_);

        // Cheap format probe first: it parses headers WITHOUT allocating a player, so
        // declining a WAV or an MP3 costs almost nothing.
        struct xmp_test_info ti;
        memset(&ti, 0, sizeof ti);
        if (xmp_test_module_from_memory(image, (long)got, &ti) != 0) { free(image); return false; }

        ctx_ = xmp_create_context();
        if (!ctx_) { free(image); return false; }

        const uint32_t freeBefore = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int rc;
        {
            PsramBias bias;      // see the class comment: without this, big modules eat the
            rc = xmp_load_module_from_memory(ctx_, image, (long)got);   // internal heap
        }
        // libxmp builds its own structures and copies what it needs, so the file image is dead
        // the moment load returns -- hold it no longer than that.
        free(image);
        if (rc != 0) {
            ESP_LOGW(TAG, "'%s' (%s) failed to load: libxmp error %d", ti.name, ti.type, -rc);
            xmp_free_context(ctx_);
            ctx_ = nullptr;
            return false;
        }

        // 0 flags = signed 16-bit stereo, which is the mixer's own format.
        if (xmp_start_player(ctx_, kMixRate, 0) != 0) {
            ESP_LOGW(TAG, "'%s': player would not start at %d Hz", ti.name, kMixRate);
            return false;
        }
        started_ = true;

        // Linear interpolation, explicitly: spline costs noticeably more CPU for a difference
        // nobody will hear through a speaker this size, and being explicit means an upstream
        // change of default cannot quietly cost us frames.
        xmp_set_player(ctx_, XMP_PLAYER_INTERP, XMP_INTERP_LINEAR);

        logModule(ti);
        // Report what the load actually cost in INTERNAL heap specifically. That is the scarce
        // pool on this board and the one a large module can quietly exhaust, so it is worth a
        // line rather than leaving it to be inferred from the periodic memory log.
        const uint32_t freeAfter = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "  internal heap cost %d KB (%u KB free)",
                 ((int)freeBefore - (int)freeAfter) / 1024, (unsigned)(freeAfter / 1024));
        return true;
    }

    int decode(int16_t* out, int maxSamples) override
    {
        if (ended_ || !started_) return 0;

        int frames = maxSamples / 2;
        if (frames > TRK_BLOCK_FRAMES) frames = TRK_BLOCK_FRAMES;
        if (frames <= 0) return 0;

        // loop = 0 means unlimited. A module decides where its own song repeats to (rarely the
        // first row), which is why the engine tells the decoder to loop rather than rewinding
        // it from outside -- see Decoder::setLooping.
        const int rc = xmp_play_buffer(ctx_, out, frames * 2 * (int)sizeof(int16_t), loop_ ? 0 : 1);
        if (rc != 0) {
            // -XMP_END. The tail of this block is not guaranteed to be initialised, so end
            // here rather than shipping whatever was in it; a module's last block is silence
            // in practice, and a looping one never reaches this at all.
            ended_ = true;
            return 0;
        }
        return frames * 2;
    }

    bool rewind() override
    {
        if (!ctx_ || !started_) return false;
        xmp_restart_module(ctx_);
        ended_ = false;
        return true;
    }

    uint32_t rate()  const override { return kMixRate; }
    uint8_t  chans() const override { return 2; }
    int      maxBlockSamples() const override { return TRK_BLOCK_FRAMES * 2; }
    void     setLooping(bool loop) override { loop_ = loop; }

    void disown() { f_ = nullptr; }

private:
    // Worth logging in full: this is the only place that says what a player's module actually
    // is, and "4ch MOD" vs "16ch IT" changes what to expect of both CPU and memory. The sample
    // peak is a cheap "there is real audio in here" check -- a module that loads clean but
    // whose samples are empty renders silence, which every other diagnostic would pass.
    void logModule(const struct xmp_test_info& ti)
    {
        struct xmp_module_info mi;
        xmp_get_module_info(ctx_, &mi);
        const struct xmp_module* m = mi.mod;
        if (!m) { ESP_LOGI(TAG, "'%s' (%s) loaded", ti.name, ti.type); return; }

        uint32_t bytes = 0;
        int peak = 0;
        for (int i = 0; i < m->smp; i++) {
            const struct xmp_sample& s = m->xxs[i];
            if (!s.data || s.len <= 0) continue;
            const bool wide = (s.flg & XMP_SAMPLE_16BIT) != 0;
            bytes += (uint32_t)s.len;
            if (peak == 0) {
                const int n = wide ? s.len / 2 : s.len;
                for (int k = 0; k < n; k++) {
                    const int v = wide ? abs((int)((const int16_t*)s.data)[k])
                                       : abs((int)((const int8_t*)s.data)[k]) << 8;
                    if (v > peak) peak = v;
                }
            }
        }

        ESP_LOGI(TAG, "'%s' (%s): %d ch, %d pat, %d ins, %d smp, %d ord, %d bpm/%d spd",
                 m->name[0] ? m->name : ti.name, m->type, m->chn, m->pat, m->ins, m->smp,
                 m->len, m->bpm, m->spd);
        ESP_LOGI(TAG, "  %u KB of samples, first-sample peak %d", (unsigned)(bytes / 1024), peak);
    }

    FILE*       f_       = nullptr;
    xmp_context ctx_     = nullptr;
    bool        started_ = false;
    // OPT-IN, per the Decoder contract: a caller that never calls setLooping gets a decoder
    // that ends. Defaulting to true made this the one decoder whose decode() never returns 0,
    // so a caller draining it to the end -- the sample-cache loader, which is reachable if a
    // module is named with a lying extension -- had nothing to stop it but a size cap.
    bool        loop_    = false;
    bool        ended_   = false;
};

// ONE factory for every module format, because one library handles them all. (This replaced
// separate mod_try/xm_try, which only existed because the hand-written player had a loader
// per format and each had to be offered the file separately.)
Decoder* module_try(FILE* f)
{
    XmpDecoder* d = new (std::nothrow) XmpDecoder(f);
    if (!d) return nullptr;
    if (!d->load()) {
        d->disown();      // leave the file open for the dispatcher's next attempt
        delete d;
        return nullptr;
    }
    return d;
}

}   // namespace audio
