#include "audio.hpp"
#include "bank.hpp"
#include "decoder.hpp"
#include "synth.hpp"
#include "engine/util.hpp"     // clampf / randf -- the game's shared numeric helpers
#include "sim/save.hpp"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "PCM5101.h"      // board I2S pinout for the DAC
}

// The mixer: voice pool, resampler, I2S sink, and the two tasks that drive them.
// Read audio.hpp first -- it explains the shape; this file is how it is built.
//
// THE SPLIT THAT MATTERS. Two tasks, both on core 0 (core 1 belongs to the 60 fps game loop):
//
//   mixer     Wakes every 5.8 ms, reads RAM only, writes I2S. Touches no file, takes no
//             allocation, blocks on nothing except the DMA queue -- which is the point, since
//             blocking is the same thing as a dropout. Its whole job is: for each live voice,
//             resample into a scratch block, accumulate with a ramped gain, saturate, ship.
//   streamer  Does every slow thing: opening files, decoding, loading samples into PSRAM,
//             refilling ring buffers. Free to block for tens of milliseconds, because the
//             ring buffers in front of it hold ~93 ms.
//
// play() runs on the CALLER's thread (the game loop) and is bounded by a mutex hold of a few
// microseconds. It never opens a file. A cached sample or a synth tone starts on the spot; a
// stream is handed to the streamer and starts a few milliseconds later, which is the correct
// trade -- a frame hitch is more noticeable than a music cue arriving 20 ms late.

namespace audio {

static const char* TAG = "AUDIO";

// --- tuning ------------------------------------------------------------------------------
// Port id is a plain int in IDF 6 (i2s_port_t was retired with the legacy driver).
static constexpr int      I2S_PORT      = BSP_I2S_PORT_NUM;   // the board's DAC wiring
static constexpr float    BLOCK_SECS    = (float)kMixFrames / (float)kMixRate;
static constexpr uint32_t RING_SAMPLES  = 8192;        // per stream; 16 KB = ~93 ms stereo
static constexpr int      IDLE_MS       = 800;         // silence before the DAC clock is cut
static constexpr uint32_t FRAC_ONE      = 1u << 16;    // resampler cursor is 16.16 fixed point

// --- helpers -------------------------------------------------------------------------------
//
// Ceiling on a voice's composed gain. The mix accumulates sample * gain in Q12 inside an
// int32, so a gain past ~16 overflows it -- and gain arrives from data (a mod's sounds.json)
// and from call sites, neither of which is checked anywhere else. 4x (+12 dB) is well past
// any legitimate boost and leaves the accumulator two bits of room even with every voice
// hard over.
static constexpr float MAX_VOICE_GAIN = 4.0f;

// Bus::Ui deliberately has no gain of its own: it rides the effects slider (see audio.hpp).
// Resolving that HERE means every reader and writer agrees automatically -- the two used to
// be kept in step by a mirrored assignment at each write site, which is exactly the shape
// that drifts the first time a third writer appears.
static inline int bus_slot(Bus b) { return (b == Bus::Ui) ? (int)Bus::Sfx : (int)b; }

// Slider position -> gain. Perceived loudness goes roughly as the SQUARE of a linear gain,
// so a linear slider spends its top half doing almost nothing audible. Squaring the
// player-facing value spreads the audible change evenly across the travel. Applied here,
// where every writer's value passes through on its way to the mix, rather than in the
// settings scene: the sliders, the percentages restored from NVS at boot and anything a mod
// or a future ducking feature sets all have to mean the same loudness.
static inline float perceptual(float sliderPos) { return sliderPos * sliderPos; }

// --- ring buffer ---------------------------------------------------------------------------
// Single producer (streamer) / single consumer (mixer), lock-free. Monotonic counters rather
// than wrapped indices, so "full" and "empty" are not the same state and no spare slot is
// wasted; the capacity is a power of two, so wrapping is a mask.
struct Ring {
    int16_t*              buf  = nullptr;
    uint32_t              mask = 0;
    std::atomic<uint32_t> r{0}, w{0};

    bool alloc(uint32_t samples)
    {
        buf = (int16_t*)heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!buf) buf = (int16_t*)malloc(samples * sizeof(int16_t));
        mask = samples - 1;
        return buf != nullptr;
    }
    void reset() { r.store(0, std::memory_order_relaxed); w.store(0, std::memory_order_relaxed); }
    uint32_t avail() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_relaxed); }
    uint32_t space() const { return (mask + 1) - avail(); }

    // Two spans rather than a masked store per sample: the producer is alone in here, so the
    // only wrap is the one at the end of the buffer, and a block copy is what PSRAM wants
    // (44.1 kHz stereo music is 88,200 samples a second through this call).
    void write(const int16_t* src, uint32_t n)
    {
        const uint32_t wi    = w.load(std::memory_order_relaxed);
        const uint32_t start = wi & mask;
        const uint32_t first = (start + n <= mask + 1) ? n : (mask + 1 - start);
        memcpy(buf + start, src, first * sizeof(int16_t));
        if (first < n) memcpy(buf, src + first, (n - first) * sizeof(int16_t));
        w.store(wi + n, std::memory_order_release);
    }
};

// --- streaming slot ---------------------------------------------------------------------
//
// A slot is IN USE exactly when it has a decoder: there is no separate `used` flag, because
// three overlapping liveness indicators (a flag, the decoder, the owning voice) had to be
// reset in lockstep and any future path that forgot one would leak the slot until reboot.
// Only the streamer task creates or destroys `dec`, so it is the one field that can serve as
// the answer.
struct Stream {
    Decoder*  dec     = nullptr;
    Ring      ring;
    int16_t*  scratch = nullptr;   // decoder output staging, sized to maxBlockSamples()
    int       scratchCap = 0;
    uint8_t   chans   = 2;
    uint32_t  rate    = kMixRate;
    bool      loop    = false;
    std::atomic<bool> eof{false};  // decoder exhausted; the voice ends once the ring drains
    // Which voice owns this slot, or -1 free / -2 "orphaned, streamer please reclaim".
    // Atomic because the two ends live on different tasks: the mixer stamps -2 when the
    // voice ends, and the streamer polls for it without taking the lock.
    std::atomic<int> voice{-1};
};

// --- voice ---------------------------------------------------------------------------------
enum class VKind  : uint8_t { None, Mem, Stream, Synth };
enum class VState : uint8_t { Free, Loading, Playing };

struct Voice {
    VState   state = VState::Free;
    VKind    kind  = VKind::None;
    uint32_t gen   = 1;            // bumped on every acquire; makes stale handles inert
    uint32_t seq   = 0;            // acquisition order, for oldest-first stealing
    Bus      bus   = Bus::Sfx;
    uint8_t  priority = 128;

    float gain = 1.0f, pan = 0.0f, pitch = 1.0f;
    bool  loop = false;
    bool  isMusic = false;

    // Fade envelope, stepped once per block. freeAtZero distinguishes "fading out to stop"
    // from "ducking".
    float fade = 1.0f, fadeRate = 0.0f;
    bool  freeAtZero = false;

    // Resampler cursor. Held here rather than in the source so all three source kinds share
    // one implementation (see resample() below).
    uint32_t step = FRAC_ONE, frac = 0;
    int16_t  prevL = 0, prevR = 0, curL = 0, curR = 0;
    bool     primed = false;

    const MemSample* mem    = nullptr;   // Kind::Mem
    uint32_t         memPos = 0;         // frame index into mem->pcm
    int              slot   = -1;        // Kind::Stream -> index into s_streams
    SynthVoice       synth;              // Kind::Synth

    int   bankIdx = -1;
    // Which table bankIdx indexes: -1 = the global bank, >=0 = that sound-set slot. A voice is
    // what pins a set against eviction (see set_in_use), and a melody voice makes that literal:
    // SynthVoice walks the Note array the Sound owns, so freeing the set underneath a playing
    // melody would be a use-after-free rather than merely a wrong noise.
    int8_t setIdx = -1;
    // Current per-channel gain in Q12, ramped toward the target across a block. Without this
    // a volume change (or a fade step) lands as a discontinuity, which is audible as a click.
    int32_t rampL = 0, rampR = 0;
    bool    rampInit = false;
};

// --- engine state ---------------------------------------------------------------------------
static i2s_chan_handle_t s_tx = nullptr;
static SemaphoreHandle_t s_lock = nullptr;
static QueueHandle_t     s_reqQ = nullptr;
static TaskHandle_t      s_mixTask = nullptr;
static TaskHandle_t      s_strTask = nullptr;

static Voice   s_voices[kVoices];
static Stream  s_streams[kStreams];
static uint32_t s_seqCounter = 1;

static std::atomic<bool> s_ready{false};
static std::atomic<bool> s_suspended{false};
static std::atomic<bool> s_muted{false};
static std::atomic<bool> s_running{true};
// Set by the mixer task as it leaves its loop. shutdown() waits on this before deleting the
// I2S channel, because the mixer spends most of its life blocked inside i2s_channel_write() on
// exactly that channel.
static std::atomic<bool> s_mixExited{false};

static float s_master = 0.8f;
static float s_busGain[(int)Bus::Count] = { 1.0f, 0.7f, 1.0f };   // Sfx, Music, (Ui: unused)

// Music is tracked separately from the voice because a switch has to survive the gap between
// "old track fading out" and "new track decoded and ready".
//
// BANK INDICES, not id strings. bank_load() runs once per boot and never moves an entry
// afterwards, so an index names a track for the whole session and bank_at(i)->id recovers the
// string when one is wanted. The id-string version of this needed three variables kept in
// step by hand at four sites (and the string buffer was read across tasks without the lock);
// two ints have nothing to desynchronise.
static std::atomic<int> s_musicIdx{-1};      // what is on the music voice, or -1
static std::atomic<int> s_pendingMusic{-1};  // queued, waiting for the music voice to free
static float s_pendingFadeIn = 0.0f;
// Track parked because nobody can hear it (muted, or a zero music/master slider), to be
// picked back up when they can. See music_gate_update().
static int s_gatedMusic = -1;

static std::atomic<uint16_t> s_underruns{0};
static std::atomic<uint16_t> s_mixPeakUs{0};

// Mix scratch, all internal RAM (touched every 5.8 ms; PSRAM would add cache pressure to the
// one path that must never stall).
static int32_t* s_acc     = nullptr;      // stereo accumulator, kMixFrames * 2
static int16_t* s_voiceBuf = nullptr;     // per-voice render target, kMixFrames * 2
static int16_t* s_out     = nullptr;      // saturated output handed to I2S

// What the streamer is being asked to do. REQ_START carries a voice already reserved by
// play(); REQ_WAKE carries nothing and exists only to break the streamer out of its idle wait
// when the music state changes -- the queue is its single wake source, so a music request has
// to arrive through it or wait out the idle timeout.
struct Req { uint8_t kind; int16_t voice; uint32_t gen; int16_t bankIdx; int8_t set; };
static constexpr uint8_t REQ_START = 0;
static constexpr uint8_t REQ_WAKE  = 1;

static inline void lock()   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock() { if (s_lock) xSemaphoreGive(s_lock); }

// Nudge the streamer so it services a music change now instead of at its next idle wakeup.
static void wake_streamer()
{
    if (!s_reqQ) return;
    Req r{ REQ_WAKE, -1, 0, -1, -1 };
    xQueueSend(s_reqQ, &r, 0);      // a full queue means it is already about to run anyway
}

static inline Handle make_handle(int idx, uint32_t gen) { return (gen << 8) | (uint32_t)(idx + 1); }
static Voice* resolve(Handle h)
{
    if (h == kNoHandle) return nullptr;
    const int idx = (int)(h & 0xFF) - 1;
    if (idx < 0 || idx >= kVoices) return nullptr;
    Voice& v = s_voices[idx];
    if (v.gen != (h >> 8) || v.state == VState::Free) return nullptr;
    return &v;
}

// --- resampling ----------------------------------------------------------------------------
//
// One linear-interpolating resampler, shared by the memory and streaming paths (the synth
// generates at kMixRate and skips it entirely). Linear rather than anything better because
// the alternative is a windowed-sinc kernel costing 10-30x for a difference nobody will hear
// through this speaker; linear is what trackers and console mixers have always used.
//
// The cursor never looks BACKWARD into its source: it carries the two straddling frames in
// the voice (prev/cur) and only ever pulls forward. That is what lets the same code read a
// ring buffer -- where the previous frame may already have been overwritten -- as safely as
// it reads a flat array.
template <class Src>
static int resample(Src& src, Voice& v, int16_t* dst, int frames)
{
    if (!v.primed) {
        if (!src.next(v.curL, v.curR)) return 0;
        v.prevL = v.curL; v.prevR = v.curR;
        if (!src.next(v.curL, v.curR)) { v.curL = v.prevL; v.curR = v.prevR; }
        v.frac = 0;
        v.primed = true;
    }

    int i = 0;
    for (; i < frames; i++) {
        // t is the fraction, narrowed to 15 bits so the interpolation stays in 32-bit
        // arithmetic: |cur-prev| <= 65535 and t <= 32767 multiply to 2147385345, which is
        // just inside INT32_MAX. Widening to 64-bit here would cost more than it buys.
        //
        // t CAN exceed 32767 in one case: the source running dry inside the carry loop below
        // returns with frac still >= FRAC_ONE, and the next call in enters with the excess.
        // That is safe only because the same exit leaves prev == cur (the copy at the top of
        // the carry precedes the failed fetch, and a failed fetch never writes cur), so the
        // oversized t multiplies a zero delta. Keep those two statements in that order.
        const int32_t t = (int32_t)(v.frac >> 1);
        dst[2 * i]     = (int16_t)(v.prevL + (((int32_t)(v.curL - v.prevL) * t) >> 15));
        dst[2 * i + 1] = (int16_t)(v.prevR + (((int32_t)(v.curR - v.prevR) * t) >> 15));

        v.frac += v.step;
        while (v.frac >= FRAC_ONE) {
            v.prevL = v.curL; v.prevR = v.curR;
            if (!src.next(v.curL, v.curR)) return i + 1;   // the frame just written counts
            v.frac -= FRAC_ONE;
        }
    }
    return i;
}

struct MemFrames {
    const int16_t* pcm; uint32_t frames; uint8_t chans; uint32_t* pos;
    bool next(int16_t& l, int16_t& r)
    {
        if (*pos >= frames) return false;
        if (chans == 1) { l = r = pcm[*pos]; }
        else            { l = pcm[*pos * 2]; r = pcm[*pos * 2 + 1]; }
        (*pos)++;
        return true;
    }
};

struct RingFrames {
    const int16_t* buf; uint32_t mask; uint32_t* pos; uint32_t end; uint8_t chans;
    bool next(int16_t& l, int16_t& r)
    {
        if (chans == 1) {
            if (*pos == end) return false;
            l = r = buf[(*pos)++ & mask];
        } else {
            if (end - *pos < 2) return false;
            l = buf[(*pos)++ & mask];
            r = buf[(*pos)++ & mask];
        }
        return true;
    }
};

// --- voice rendering ------------------------------------------------------------------------

static void voice_free_locked(Voice& v);
// Installed as the bank's eviction guard by init(); defined with the rest of the voice code
// further down, which is where the state it reads lives.
static bool set_in_use(int slot);

// Fill `dst` with up to `frames` stereo frames from this voice. Returns frames produced and
// sets `ended` when the source is finished for good (as opposed to a stream merely running
// dry for a moment, which is an underrun and NOT the end).
static int render_voice(Voice& v, int16_t* dst, int frames, bool& ended)
{
    ended = false;
    int done = 0;

    while (done < frames) {
        int n = 0;

        switch (v.kind) {
            case VKind::Mem: {
                if (!v.mem) { ended = true; break; }
                MemFrames src{ v.mem->pcm, v.mem->frames, v.mem->chans, &v.memPos };
                n = resample(src, v, dst + done * 2, frames - done);
                break;
            }
            case VKind::Synth:
                n = v.synth.render(dst + done * 2, frames - done);
                break;

            case VKind::Stream: {
                if (v.slot < 0) { ended = true; break; }
                Stream& st = s_streams[v.slot];
                uint32_t rd  = st.ring.r.load(std::memory_order_relaxed);
                const uint32_t end = st.ring.w.load(std::memory_order_acquire);
                RingFrames src{ st.ring.buf, st.ring.mask, &rd, end, st.chans };
                n = resample(src, v, dst + done * 2, frames - done);
                st.ring.r.store(rd, std::memory_order_release);

                if (n < frames - done) {
                    // Dry. If the decoder is done too, the sound has genuinely ended;
                    // otherwise the card could not keep up and we pad with silence rather
                    // than cutting the sound off.
                    if (st.eof.load(std::memory_order_acquire)) {
                        ended = true;
                    } else {
                        const int pad = frames - done - n;
                        memset(dst + (done + n) * 2, 0, (size_t)pad * 2 * sizeof(int16_t));
                        s_underruns.fetch_add(1, std::memory_order_relaxed);
                        return frames;
                    }
                }
                break;
            }
            default:
                ended = true;
                break;
        }

        done += n;
        if (ended) break;

        if (done < frames) {
            // The source ran out mid-block. Loop it in place so a looping sound has no gap
            // at the seam, or finish.
            if (!v.loop) { ended = true; break; }

            if (v.kind == VKind::Mem) {
                v.memPos = 0;
                v.primed = false;
            } else if (v.kind == VKind::Synth) {
                ended = true;              // synth loops internally; reaching here means done
            } else {
                ended = true;              // stream looping is the streamer's job (it rewinds)
            }
            if (ended) break;
            // A pass that produced nothing cannot be helped by looping it again, and this
            // runs on the mixer -- spinning here would wedge audio for good rather than
            // just dropping a sound.
            if (n == 0) { ended = true; break; }
        }
    }

    if (done < frames) memset(dst + done * 2, 0, (size_t)(frames - done) * 2 * sizeof(int16_t));
    return done;
}

// --- the mix ---------------------------------------------------------------------------------

static int mix_block()
{
    memset(s_acc, 0, sizeof(int32_t) * kMixFrames * 2);
    int active = 0;

    const float masterNow = s_muted.load(std::memory_order_relaxed) ? 0.0f : s_master;

    for (int i = 0; i < kVoices; i++) {
        Voice& v = s_voices[i];
        if (v.state != VState::Playing) continue;
        active++;

        // Advance the fade envelope one block. Doing it per block rather than per sample is
        // fine because the gain ramp below smooths whatever step it produces.
        if (v.fadeRate != 0.0f) {
            v.fade = clampf(v.fade + v.fadeRate * BLOCK_SECS, 0.0f, 1.0f);
            if (v.fade >= 1.0f && v.fadeRate > 0.0f) v.fadeRate = 0.0f;
            if (v.fade <= 0.0f && v.freeAtZero) { voice_free_locked(v); active--; continue; }
        }

        bool ended = false;
        render_voice(v, s_voiceBuf, kMixFrames, ended);

        // Equal-power pan: a centred sound keeps the same apparent loudness as a hard-panned
        // one, which a straight linear split does not.
        const float pan = clampf(v.pan, -1.0f, 1.0f);
        const float ang = (pan + 1.0f) * 0.78539816f * 0.5f;      // 0..pi/2
        const float g   = v.gain * v.fade * perceptual(s_busGain[bus_slot(v.bus)] * masterNow);
        const int32_t tgtL = (int32_t)(cosf(ang) * g * 4096.0f);
        const int32_t tgtR = (int32_t)(sinf(ang) * g * 4096.0f);

        if (!v.rampInit) { v.rampL = tgtL; v.rampR = tgtR; v.rampInit = true; }
        const int32_t dL = (tgtL - v.rampL) / kMixFrames;
        const int32_t dR = (tgtR - v.rampR) / kMixFrames;
        int32_t gl = v.rampL, gr = v.rampR;

        for (int f = 0; f < kMixFrames; f++) {
            s_acc[f * 2]     += (s_voiceBuf[f * 2]     * gl) >> 12;
            s_acc[f * 2 + 1] += (s_voiceBuf[f * 2 + 1] * gr) >> 12;
            gl += dL; gr += dR;
        }
        v.rampL = tgtL; v.rampR = tgtR;

        if (ended) { voice_free_locked(v); active--; }
    }

    // Saturate. Four voices can sum to 4x full scale, so this WILL engage if everything is
    // loud at once; hard clipping is the honest choice over a limiter that would duck the
    // whole mix whenever an effect fires.
    for (int i = 0; i < kMixFrames * 2; i++) {
        int32_t s = s_acc[i];
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        s_out[i] = (int16_t)s;
    }
    return active;
}

// --- I2S ------------------------------------------------------------------------------------

static bool i2s_start()
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    cc.dma_desc_num  = 6;
    cc.dma_frame_num = 240;
    // Send zeros when we fall behind instead of replaying the last DMA buffer, which would
    // turn a dropout into a loud buzz.
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t cfg = {};
    cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kMixRate);
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    // The board wires no MCLK; the PCM5101A derives its internal clock from BCK, which is
    // the part's documented "BCK-only" mode and what the stock driver already relied on.
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = (gpio_num_t)BSP_I2S_SCLK;
    cfg.gpio_cfg.ws   = (gpio_num_t)BSP_I2S_LCLK;
    cfg.gpio_cfg.dout = (gpio_num_t)BSP_I2S_DOUT;
    cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.invert_flags.mclk_inv = false;
    cfg.gpio_cfg.invert_flags.bclk_inv = false;
    cfg.gpio_cfg.invert_flags.ws_inv   = false;

    if (i2s_channel_init_std_mode(s_tx, &cfg) != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    if (i2s_channel_enable(s_tx) != ESP_OK) {
        // Hand the channel back. Leaving it allocated would keep the I2S port claimed with
        // nothing able to release it -- shutdown() returns early because init() never set
        // s_ready, so a later retry would fail in i2s_new_channel() for no visible reason.
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    return true;
}

// --- mixer task --------------------------------------------------------------------------

static void mixer_task(void*)
{
    bool i2sOn   = true;
    int  silent  = 0;
    const int idleBlocks = (int)(IDLE_MS / (BLOCK_SECS * 1000.0f));

    while (s_running.load(std::memory_order_relaxed)) {
        if (s_suspended.load(std::memory_order_acquire)) {
            if (i2sOn) { i2s_channel_disable(s_tx); i2sOn = false; }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        lock();
        const int active = mix_block();
        unlock();
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_mixPeakUs.load(std::memory_order_relaxed))
            s_mixPeakUs.store((uint16_t)(us > 65535 ? 65535 : us), std::memory_order_relaxed);

        if (active == 0) {
            silent++;
            if (silent > idleBlocks) {
                // Nothing to play. Park the DAC: the I2S clock alone costs a few mA, and on
                // a battery pet that runs for days it is worth cutting. The `idleBlocks` of
                // silence written first settle the output at mid-scale so the speaker does
                // not click when the clock stops.
                if (i2sOn) { i2s_channel_disable(s_tx); i2sOn = false; }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));   // play() pokes us awake
                continue;
            }
        } else {
            silent = 0;
        }

        if (!i2sOn) {
            if (i2s_channel_enable(s_tx) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            i2sOn = true;
        }

        size_t written = 0;
        // Blocking write IS the clock: it returns when DMA has taken the block, which paces
        // this loop at exactly real time with no timer of its own.
        i2s_channel_write(s_tx, s_out, sizeof(int16_t) * kMixFrames * 2, &written,
                          portMAX_DELAY);
    }
    s_mixExited.store(true, std::memory_order_release);

    // PARK rather than delete. shutdown() deliberately does not wait for the streamer (it can
    // be blocked in a long read on storage that was just pulled), and the streamer wakes this
    // task BY HANDLE when a load finishes. vTaskDelete() here would free the TCB that handle
    // points at, so a load that lands after shutdown -- easily reachable on the card-yank path,
    // which then holds a restart prompt for 30 seconds -- would notify freed memory and corrupt
    // the heap. Parking keeps the handle valid and costs one idle task's stack until the reboot,
    // sleep or power-off that every shutdown() caller is already seconds away from.
    for (;;) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// --- streaming (streamer task side) --------------------------------------------------------

static void stream_release(int slot)
{
    Stream& st = s_streams[slot];
    delete st.dec;
    st.dec = nullptr;
    free(st.scratch);
    st.scratch = nullptr;
    st.scratchCap = 0;
    st.voice.store(-1, std::memory_order_relaxed);
    st.eof.store(false, std::memory_order_relaxed);
    st.ring.reset();
}

// Streamer only, so there is no window to guard: the caller attaches a decoder (or releases
// the slot again) before anything else can ask for one.
static int stream_acquire()
{
    for (int i = 0; i < kStreams; i++)
        if (!s_streams[i].dec && s_streams[i].voice.load(std::memory_order_relaxed) == -1)
            return i;
    return -1;
}

// Top every active stream back up. Called on the streamer task, so a slow FAT read here just
// eats into the ring's ~93 ms of headroom rather than glitching the output.
static void refill_streams()
{
    for (int i = 0; i < kStreams; i++) {
        Stream& st = s_streams[i];
        if (!st.dec || st.eof.load(std::memory_order_relaxed)) continue;
        // -2 is "the voice ended, reclaim me" -- decoding more for it would be work for a
        // sound nobody will hear. (-1 is a slot being primed by attach_stream, which does
        // want filling.)
        if (st.voice.load(std::memory_order_relaxed) == -2) continue;

        // ONE rewind per dry spell, and only if the rewind actually yields something.
        //
        // The obvious spelling of this loop -- "if it ran dry and it loops, rewind and carry
        // on" -- hangs the device. fseek() on a dead file handle still SUCCEEDS (it sets an
        // offset; it does no I/O), so a source that has stopped producing gives
        // decode()==0 -> rewind()==true -> decode()==0 forever, and this loop never yields the
        // CPU. The streamer runs at priority 4 on core 0, so it then starves the driver task
        // and sdwatch beneath it until the task watchdog fires: a hang and a reboot, not a
        // dropout, and with sdwatch starved the game never even notices why.
        //
        // Reachable from an SD card pulled out from under an open handle, from a zero-length
        // or truncated file, and from anything that parses as valid but decodes to nothing.
        int retried = 0;
        while (st.ring.space() >= (uint32_t)st.scratchCap) {
            const int n = st.dec->decode(st.scratch, st.scratchCap);
            if (n > 0) {
                st.ring.write(st.scratch, (uint32_t)n);
                retried = 0;                       // real data: the next dry spell may retry
                continue;
            }
            if (n == 0 && st.loop && retried == 0 && st.dec->rewind()) {
                retried = 1;                       // seamless repeat, once
                continue;
            }
            // Distinguish the two ways to get here. A non-looping source reaching its end is
            // routine and silent; a LOOPING one that yielded nothing even after rewinding is a
            // broken file or vanished storage, and saying so beats leaving someone to wonder
            // why their music stopped.
            if (retried) ESP_LOGW(TAG, "looping source produced nothing after a rewind; ending it");
            st.eof.store(true, std::memory_order_release);
            break;
        }
    }
}

// --- voice lifecycle -------------------------------------------------------------------------

// Must be called with the lock held (the mixer calls it mid-block). Detaching the stream slot
// is deferred to the streamer, because freeing a decoder means free() and possibly fclose(),
// neither of which belongs on the mixer's path.
static void voice_free_locked(Voice& v)
{
    if (v.kind == VKind::Stream && v.slot >= 0)
        s_streams[v.slot].voice.store(-2, std::memory_order_release);   // streamer reclaims it
    if (v.isMusic) s_musicIdx.store(-1, std::memory_order_release);

    v.state    = VState::Free;
    v.kind     = VKind::None;
    v.mem      = nullptr;
    v.slot     = -1;
    v.bankIdx  = -1;
    v.setIdx   = -1;              // releases whatever sound set this voice was pinning
    v.primed   = false;
    v.rampInit = false;
    v.isMusic  = false;
    v.freeAtZero = false;
    v.fadeRate = 0.0f;
    v.fade     = 1.0f;
}

// Lock held. Fade a voice down to silence and free it there -- or free it now, if a fade would
// be pointless.
//
// ONE spelling of this, because there were five and they had already drifted into two
// different slopes (constant rate vs constant time-to-zero). The `fade > 0` guard is the part
// that matters: a voice whose fade-IN has not been stepped yet sits at exactly 0.0, which
// makes -fade/fadeSecs a NEGATIVE ZERO, and mix_block only advances an envelope whose rate is
// non-zero. Such a voice would never reach zero and so never be freed -- silent, but holding
// its voice slot (and a stream slot, and a decoder, and an open file) until reboot.
static void fade_out_or_free_locked(Voice& v, float fadeSecs)
{
    if (v.state == VState::Free) return;
    if (fadeSecs > 0.01f && v.fade > 0.0f) {
        v.fadeRate   = -v.fade / fadeSecs;      // constant time to silence, whatever it is at
        v.freeAtZero = true;
    } else {
        voice_free_locked(v);
    }
}

// Pick a voice for a new sound. Lock held.
//
// Music always lands on kMusicVoice, so an effect storm can never evict the song. Effects
// prefer the other voices and fall back to the music voice only when no music is playing --
// otherwise a game with no soundtrack would waste a quarter of its polyphony.
static int acquire_voice(bool isMusic, uint8_t priority)
{
    if (isMusic) return kMusicVoice;

    for (int i = 0; i < kVoices; i++) {
        if (i == kMusicVoice) continue;
        if (s_voices[i].state == VState::Free) return i;
    }
    if (s_voices[kMusicVoice].state == VState::Free &&
        s_musicIdx.load(std::memory_order_relaxed) < 0 &&
        s_pendingMusic.load(std::memory_order_relaxed) < 0)
        return kMusicVoice;

    // All busy: steal the least important, oldest voice -- but never the music, and never
    // something more important than what is asking.
    int best = -1;
    for (int i = 0; i < kVoices; i++) {
        Voice& v = s_voices[i];
        if (v.isMusic || v.state == VState::Loading) continue;
        if (v.priority > priority) continue;
        if (best < 0 || v.priority < s_voices[best].priority ||
            (v.priority == s_voices[best].priority && v.seq < s_voices[best].seq))
            best = i;
    }
    return best;
}

// Lock held. Reserve `idx` for `snd`, applying the sound's data-driven defaults and the
// caller's per-shot modifiers.
static void voice_begin(int idx, const Sound& snd, int bankIdx, int setIdx,
                        const Params& p, bool isMusic)
{
    Voice& v = s_voices[idx];
    if (v.state != VState::Free) voice_free_locked(v);

    v.gen++;
    v.seq      = s_seqCounter++;
    v.state    = VState::Loading;
    v.kind     = VKind::None;
    v.bus      = snd.bus;
    v.priority = snd.priority;
    // Clamped, because both halves come from outside: snd.gain is whatever a mod's sounds.json
    // said and p.gain is whatever a call site passed, and neither is checked anywhere else. An
    // unbounded product overflows the Q12 accumulate in mix_block, which turns "too loud" into
    // sign-flipped noise rather than the honest clipping the saturator is there to provide.
    v.gain     = clampf(snd.gain * p.gain, 0.0f, MAX_VOICE_GAIN);
    v.pan      = snd.pan + p.pan;
    v.loop     = snd.loop || p.loop;
    v.isMusic  = isMusic;
    v.bankIdx  = bankIdx;
    v.setIdx   = (int8_t)setIdx;   // pins the set for as long as this voice lives
    v.primed   = false;
    v.rampInit = false;
    v.frac     = 0;
    v.memPos   = 0;
    v.slot     = -1;
    v.fade     = 1.0f;
    v.fadeRate = 0.0f;
    v.freeAtZero = false;

    float pitch = snd.pitch * p.pitch;
    if (snd.pitchVar > 0.0f) pitch *= 1.0f + snd.pitchVar * (randf() * 2.0f - 1.0f);
    v.pitch = clampf(pitch, 0.25f, 4.0f);
}

// Lock held. Source rate -> resampler step. A synth needs none (it generates at kMixRate).
static void voice_set_rate(Voice& v, uint32_t srcRate)
{
    double s = (double)srcRate / (double)kMixRate * (double)v.pitch * 65536.0;
    if (s < 1.0)        s = 1.0;
    if (s > 16.0 * 65536.0) s = 16.0 * 65536.0;
    v.step = (uint32_t)s;
}

// Lock held. Everything a voice needs to become audible in the very next mix block, for the
// source kinds that need no I/O to get there. Returns false if the voice was stolen or stopped
// in the meantime (only possible for a caller that dropped the lock to do work), or if a
// sample turns out not to be cached after all.
//
// One copy of this: going live was spelled out four times (tone, melody and cached sample in
// play(), then the melody again for music), and the four had already diverged in what they
// set. The mixer must never see a half-started voice, so this is exactly the set of fields
// that has to land before state becomes Playing.
static bool go_live_locked(Voice& v, const Sound& snd, uint32_t gen)
{
    if (v.gen != gen || v.state != VState::Loading) return false;

    switch (snd.kind) {
        case SoundKind::Tone:
            v.kind = VKind::Synth;
            v.synth.startTone(snd.tone, v.pitch);
            v.step = FRAC_ONE;                       // the synth generates at kMixRate
            break;
        case SoundKind::Melody:
            v.kind = VKind::Synth;
            v.synth.startMelody(snd.melody, v.pitch, v.loop);
            v.step = FRAC_ONE;
            break;
        default: {                                   // a sample already decoded into PSRAM
            MemSample* ms = snd.mem.load(std::memory_order_acquire);
            if (!ms) return false;
            v.kind = VKind::Mem;
            v.mem  = ms;
            voice_set_rate(v, ms->rate);
            break;
        }
    }
    v.state = VState::Playing;
    return true;
}

// --- streamer task -----------------------------------------------------------------------

// Attach an already-open decoder to a reserved voice. Streamer only.
static bool attach_stream(int voiceIdx, uint32_t gen, Decoder* dec, bool loop)
{
    // A decoder whose native block does not fit in the ring can never be serviced at all:
    // refill_streams() only decodes while there is room for a WHOLE block, and a ring smaller
    // than one block never has it. The voice would then sit on a permanently empty ring --
    // silent, never reaching eof, holding one of two stream slots and counting an underrun
    // every 5.8 ms forever. Refuse it here so the sound fails cleanly instead.
    //
    // Not hypothetical: a WAV's IMA ADPCM block size is read straight from the file's own
    // header, so a legal-but-unusual encoder setting (or any hand-made file) reaches this.
    const int blk = dec->maxBlockSamples();
    if (blk <= 0 || (uint32_t)blk > RING_SAMPLES) {
        ESP_LOGW(TAG, "block of %d samples does not fit the %u-sample stream ring; not playing it",
                 blk, (unsigned)RING_SAMPLES);
        return false;
    }

    const int slot = stream_acquire();
    if (slot < 0) return false;

    Stream& st = s_streams[slot];
    if (!st.ring.buf && !st.ring.alloc(RING_SAMPLES)) { stream_release(slot); return false; }
    st.ring.reset();
    // A tracker module knows where its own song loops back to, which is rarely the first row,
    // so it needs to be told whether to repeat rather than being rewound from the outside.
    // The file formats ignore this and are looped by rewind() below.
    dec->setLooping(loop);
    st.dec        = dec;
    st.chans      = dec->chans();
    st.rate       = dec->rate();
    st.loop       = loop;
    st.scratchCap = blk;
    st.scratch    = (int16_t*)malloc(sizeof(int16_t) * st.scratchCap);
    st.eof.store(false, std::memory_order_relaxed);
    // On EVERY false return the CALLER still owns `dec` and deletes it, so the slot must let
    // go of it first -- stream_release() deletes st.dec.
    if (!st.scratch) { st.dec = nullptr; stream_release(slot); return false; }

    // Prime before the voice goes live, so playback never starts on an empty ring (which
    // would register as an underrun on the very first block).
    refill_streams();

    lock();
    Voice& v = s_voices[voiceIdx];
    if (v.gen != gen || v.state != VState::Loading) {   // stopped or stolen while we loaded
        unlock();
        st.dec = nullptr;          // ...including here: see the scratch path above
        stream_release(slot);
        return false;
    }
    v.kind  = VKind::Stream;
    v.slot  = slot;
    st.voice.store(voiceIdx, std::memory_order_relaxed);
    voice_set_rate(v, st.rate);
    v.state = VState::Playing;
    unlock();

    xTaskNotifyGive(s_mixTask);
    return true;
}

static void start_request(const Req& r)
{
    // sound_at() refuses a set that is no longer READY, so a request queued just before its
    // creature stopped speaking resolves to null here and quietly releases the voice below --
    // rather than decoding against entries the eviction path is freeing.
    const Sound* snd = sound_at(r.set, r.bankIdx);
    if (!snd) {
        lock();
        Voice& v = s_voices[r.voice];
        if (v.gen == r.gen && v.state == VState::Loading) voice_free_locked(v);
        unlock();
        return;
    }

    if (snd->kind == SoundKind::Sample) {
        // Try the RAM path first; bank_load_sample falls back to false when the cache is
        // full or the file will not decode, and we stream it instead of dropping it.
        if (bank_load_sample(r.set, r.bankIdx)) {
            lock();
            const bool ok = go_live_locked(s_voices[r.voice], *snd, r.gen);
            unlock();
            if (ok) { xTaskNotifyGive(s_mixTask); return; }
        }
    }

    if (Decoder* dec = decoder_open(snd->path)) {
        bool loop;
        lock();
        loop = (s_voices[r.voice].gen == r.gen) && s_voices[r.voice].loop;
        unlock();
        if (attach_stream(r.voice, r.gen, dec, loop)) return;
        delete dec;
    }

    // Could not play it: release the reservation so the voice is not stuck Loading.
    lock();
    Voice& v = s_voices[r.voice];
    if (v.gen == r.gen && v.state == VState::Loading) voice_free_locked(v);
    unlock();
}

// The music voice was fading out while the new track loaded. Once it is free, start it.
static void service_pending_music()
{
    lock();
    const int idx = s_pendingMusic.load(std::memory_order_relaxed);
    const bool voiceFree = (s_voices[kMusicVoice].state == VState::Free);
    unlock();
    if (idx < 0 || !voiceFree) return;

    const Sound* snd = bank_at(idx);
    if (!snd) {
        lock();
        // Only clear the request we actually looked at: a newer one may have arrived.
        int expect = idx;
        s_pendingMusic.compare_exchange_strong(expect, -1, std::memory_order_relaxed);
        unlock();
        return;
    }

    Params p;
    lock();
    // RE-CHECK before committing. The game thread can replace the request, stop the music or
    // take the voice while we are outside the lock, and consuming the slot blind would start a
    // track that was just cancelled -- or start the one we looked up while recording the id of
    // a different one, which then makes every future request for THAT id a no-op.
    if (s_pendingMusic.load(std::memory_order_relaxed) != idx ||
        s_voices[kMusicVoice].state != VState::Free) { unlock(); return; }

    const float fadeIn = s_pendingFadeIn;
    // -1: music always comes from the global bank. A creature's folder is scanned into a set
    // that gets evicted the moment it stops speaking, which is no place for a track that has
    // to keep playing across scene changes.
    voice_begin(kMusicVoice, *snd, idx, -1, p, true);
    Voice& v = s_voices[kMusicVoice];
    // No forced loop here: voice_begin already took the sound's own `loop`, which defaults to
    // true on the music bus. Overriding it left a manifest's "loop": false with no way to
    // express a one-shot sting played as music.
    if (fadeIn > 0.0f) { v.fade = 0.0f; v.fadeRate = 1.0f / fadeIn; }
    const uint32_t gen = v.gen;
    s_musicIdx.store(idx, std::memory_order_release);
    s_pendingMusic.store(-1, std::memory_order_relaxed);

    // A synthesised track needs no file, so it starts inside this same lock.
    const bool live = (snd->kind == SoundKind::Melody || snd->kind == SoundKind::Tone)
                      && go_live_locked(v, *snd, gen);
    unlock();

    if (live) {
        ESP_LOGI(TAG, "music '%s' (melody, %u notes)", snd->id, snd->melody.count);
        xTaskNotifyGive(s_mixTask);
        return;
    }
    ESP_LOGI(TAG, "music '%s' (%s)", snd->id,
             snd->kind == SoundKind::Stream ? "stream" : "sample");

    Req r{ REQ_START, (int16_t)kMusicVoice, gen, (int16_t)idx, -1 };
    start_request(r);
}

// How long the streamer sleeps between passes. 5 ms while it has streams to keep fed (the
// rings hold ~93 ms, so this is nowhere near a risk), but a full half second when there is
// nothing live: a play() or music() posts to the queue and wakes it instantly, so polling
// faster than that buys nothing. It used to poll at 5 ms unconditionally, which on a device
// that spends most of its life with the screen off meant 200 wakeups and 200 uncontended
// mutex round-trips a second, all night, for no work.
static constexpr int STREAM_TICK_MS = 5;
static constexpr int IDLE_TICK_MS   = 500;

static void streamer_task(void*)
{
    Req r;
    int  warmIdx = 0;
    bool warmed  = false;

    while (s_running.load(std::memory_order_relaxed)) {
        // Suspended = screen off, mixer parked. Nothing is draining the rings, so there is
        // nothing to top up and no reason to be awake at all.
        const bool suspended = s_suspended.load(std::memory_order_acquire);
        bool busy = false;

        if (!suspended) {
            // Reclaim slots whose voice ended (the mixer marks them; freeing is our job).
            for (int i = 0; i < kStreams; i++)
                if (s_streams[i].voice.load(std::memory_order_acquire) == -2) stream_release(i);

            refill_streams();
            for (int i = 0; i < kStreams; i++) if (s_streams[i].dec) busy = true;

            // Only touch the lock when there is actually a track waiting for the voice.
            if (s_pendingMusic.load(std::memory_order_relaxed) >= 0) {
                service_pending_music();
                busy = true;
            }

            // Creature sound sets: one load or eviction per pass, on the task that owns FAT
            // opens. Ahead of the cache warm-up on purpose -- a set is wanted by a creature
            // that is on screen NOW, while the warm-up is speculative work for later.
            if (soundset_service()) busy = true;

            // Warm the sample cache in the background: one entry per iteration, so the first
            // tap of a sound is instant without boot paying for the whole bank up front. Walks
            // the bank exactly once -- entries registered later simply load on first use.
            //
            // WAITS FOR bank_ready(). The scan builds entries in place and a later root's
            // override copy-assigns over an earlier one, so decoding against an entry that is
            // still being rewritten can cache the OLD file's audio under the NEW id, or leak
            // the buffer it just published. bank_load() runs on the game task while this one
            // is already up, so without the gate the two genuinely overlap.
            if (!warmed && bank_ready()) {
                if (warmIdx < bank_count()) {
                    const Sound* s = bank_at(warmIdx);
                    if (s && s->kind == SoundKind::Sample && s->preload &&
                        !s->mem.load(std::memory_order_relaxed))
                        bank_load_sample(-1, warmIdx);
                    warmIdx++;
                    busy = true;               // keep the fast cadence until the walk is done
                } else {
                    warmed = true;
                    // The promised report: a mod that fills the cache says so here rather than
                    // silently degrading every effect it defines into a stream.
                    ESP_LOGI(TAG, "sample cache holds %u KB after warm-up",
                             (unsigned)(bank_cache_bytes() / 1024));
                }
            }
        }

        if (xQueueReceive(s_reqQ, &r, pdMS_TO_TICKS(busy ? STREAM_TICK_MS : IDLE_TICK_MS))
            == pdTRUE && r.kind == REQ_START)
            start_request(r);      // REQ_WAKE has already done its job by arriving
    }
    vTaskDelete(nullptr);
}

// --- public API -------------------------------------------------------------------------------

// Give back everything init() allocated. Only used when init() FAILS: from then on every entry
// point is a no-op because s_ready was never set, and shutdown() returns early for the same
// reason, so this is the one chance to reclaim it.
static void release_engine_memory()
{
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = nullptr; }
    if (s_reqQ) { vQueueDelete(s_reqQ); s_reqQ = nullptr; }
    free(s_acc);      s_acc      = nullptr;
    free(s_voiceBuf); s_voiceBuf = nullptr;
    free(s_out);      s_out      = nullptr;
}

bool init()
{
    if (s_ready.load()) return true;

    // Reuse rather than reallocate: init() runs once per boot today, but every one of these is
    // a leak if it is ever called a second time, and a leak inside "audio failed, try again"
    // is the worst possible place for one.
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_reqQ) s_reqQ = xQueueCreate(12, sizeof(Req));
    if (!s_acc)      s_acc      = (int32_t*)heap_caps_malloc(sizeof(int32_t) * kMixFrames * 2, MALLOC_CAP_INTERNAL);
    if (!s_voiceBuf) s_voiceBuf = (int16_t*)heap_caps_malloc(sizeof(int16_t) * kMixFrames * 2, MALLOC_CAP_INTERNAL);
    if (!s_out)      s_out      = (int16_t*)heap_caps_malloc(sizeof(int16_t) * kMixFrames * 2, MALLOC_CAP_INTERNAL);
    if (!s_lock || !s_reqQ || !s_acc || !s_voiceBuf || !s_out) {
        ESP_LOGE(TAG, "out of memory");
        release_engine_memory();
        return false;
    }

    if (!i2s_start()) {
        ESP_LOGE(TAG, "I2S init failed; audio disabled");
        release_engine_memory();
        return false;
    }

    s_running.store(true);
    s_mixExited.store(false);
    // Core 0 alongside the drivers: core 1 runs the 60 fps game loop and must not share with
    // a task that wakes every 5.8 ms. The mixer outranks the streamer so a slow decode can
    // never delay the block that is already buffered and due.
    //
    // The streamer's 6 KB is sized for the deepest thing it runs, which is not decoding but
    // libxmp LOADING a module: chained loader frames plus a heap call reach ~2.7 KB, and an
    // interrupt can land on top of that. (Most of libxmp's big loader locals are compiled out
    // by LIBXMP_CORE_PLAYER; turning DEBUG on for that component would put printf inside those
    // same frames and is the one change that would make this tight.)
    xTaskCreatePinnedToCore(mixer_task,    "aud_mix", 4096, nullptr, 6, &s_mixTask, 0);
    xTaskCreatePinnedToCore(streamer_task, "aud_str", 6144, nullptr, 4, &s_strTask, 0);

    // The bank cannot evict a creature's sound set without knowing whether a voice is still
    // playing from it, and only the mixer knows that. Installed here rather than at the call
    // site so there is no window in which a set could be freed unchecked.
    soundset_set_inuse_hook(set_in_use);

    s_ready.store(true);
    ESP_LOGI(TAG, "%d Hz stereo, %d voices, %d streams", kMixRate, kVoices, kStreams);
    return true;
}

void shutdown()
{
    if (!s_ready.load()) return;

    // Stop making noise first, and mark the engine down so nothing can start a new sound while
    // we tear it apart -- play() and music() are no-ops from here on.
    stop_all(0.05f);
    vTaskDelay(pdMS_TO_TICKS(80));       // let the fade and a little silence reach the DAC
    s_ready.store(false);
    s_running.store(false);

    // The mixer parks in ulTaskNotifyTake() when idle and blocks in i2s_channel_write() when
    // not, so poke it and then WAIT for it to confirm it has left the loop. Deleting the
    // channel out from under a blocked write would be a use-after-free; this used to be
    // survivable only because every caller went straight to sleep or a reboot afterwards, and
    // the SD-removal path does not.
    if (s_mixTask) xTaskNotifyGive(s_mixTask);
    for (int i = 0; i < 40 && !s_mixExited.load(std::memory_order_acquire); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!s_mixExited.load(std::memory_order_acquire))
        ESP_LOGW(TAG, "mixer task did not exit; leaving I2S up");
    else if (s_tx) { i2s_channel_disable(s_tx); i2s_del_channel(s_tx); s_tx = nullptr; }

    // The streamer is deliberately NOT waited on: it may be blocked in a long read on storage
    // that has just been removed, and it touches no I2S state, so there is nothing to race.
    // It may still finish that load and notify the mixer afterwards, which is why the mixer
    // task parks instead of deleting itself (see the end of mixer_task).
}

void suspend()
{
    if (!s_ready.load()) return;
    s_suspended.store(true, std::memory_order_release);
}

void resume()
{
    if (!s_ready.load()) return;
    s_suspended.store(false, std::memory_order_release);
    if (s_mixTask) xTaskNotifyGive(s_mixTask);
}

bool ready() { return s_ready.load(); }

// --- creature voices (see VoiceProfile in audio.hpp) ---------------------------------------
//
// The default voice: whoever speaks when a play() does not name one, i.e. the player's pet.
// Copied rather than pointed at, because the creature registry these strings come from is
// rebuilt whenever mod packs are remounted, and a dangling species prefix would be read on
// the audio path at some unrelated later moment.
//
// Written by the game task (Pet, on a species change) and read by play() on the same task, so
// there is nothing to synchronise. The streamer never resolves an id -- it is handed a bank
// index and a set that play() already settled on.
static char  s_voiceSpecies[24] = {};
static char  s_voiceFamily[16]  = {};
static char  s_voiceDir[128]    = {};
static float s_voicePitch       = 1.0f;

// Is any live voice still sourced from sound-set `slot`? Asked by the eviction path before it
// frees anything. Under the lock, because voice state is what it is reading.
static bool set_in_use(int slot)
{
    bool used = false;
    lock();
    for (int i = 0; i < kVoices; i++)
        if (s_voices[i].state != VState::Free && s_voices[i].setIdx == (int8_t)slot) {
            used = true;
            break;
        }
    unlock();
    return used;
}

// Ask for a creature's own sound folder to be made resident. Cheap and idempotent once it is:
// soundset_acquire() only queues work the first time, so calling this from the resolution path
// costs a few string compares per play() after the first.
static void want_set(const char* species, const char* dir)
{
    if (!species || !*species || !dir || !*dir) return;
    if (soundset_acquire(species, dir)) wake_streamer();
}

void set_voice(const VoiceProfile& v)
{
    snprintf(s_voiceSpecies, sizeof s_voiceSpecies, "%s", v.species ? v.species : "");
    snprintf(s_voiceFamily,  sizeof s_voiceFamily,  "%s", v.family  ? v.family  : "");
    snprintf(s_voiceDir,     sizeof s_voiceDir,     "%s", v.dir     ? v.dir     : "");
    s_voicePitch = clampf(v.pitch > 0.0f ? v.pitch : 1.0f, 0.25f, 4.0f);
    // Start the load NOW rather than on the first cry. A species change is a hatch, an evolve
    // or a boot -- all of which are followed within a second or two by a sound, and none of
    // which is a moment to be waiting on the SD card.
    want_set(s_voiceSpecies, s_voiceDir);
}

void clear_voice()
{
    s_voiceSpecies[0] = s_voiceFamily[0] = s_voiceDir[0] = '\0';
    s_voicePitch = 1.0f;
}

void request_voice(const char* species, const char* dir) { want_set(species, dir); }

// Resolve a voiced sound through `vp`, most specific first. Returns the index to play, writes
// which table it lives in to `setOut`, and reports the pitch scalar still owed on top.
//
// Order, and why each rung exists:
//   1. the creature's OWN folder      -- self-contained, unbounded, no prefix needed
//   2. `<species>_<id>` in the bank   -- for a pack defining several creatures in one manifest
//   3. `<family>_<id>` in the bank    -- shared authored voices, a handful covering a roster
//   4. the plain id                   -- the base game
//
// The scratch buffer is deliberately sized to Sound::id: any id that cannot fit in one cannot
// exist in the bank, so truncating a longer composite here can only ever fail a lookup that
// was already guaranteed to miss.
static int resolve_voice(const char* id, int baseIdx, const VoiceProfile& vp,
                         int& setOut, float& pitchOut)
{
    setOut = -1;

    if (vp.species && vp.species[0]) {
        // Rung 1. Only ever hits once the set is resident; until then this falls through and
        // the creature speaks with the shared sounds, which is the correct thing to do while
        // waiting on a disk read rather than blocking the game for one.
        const int slot = soundset_resident(vp.species);
        if (slot >= 0) {
            const int hit = soundset_find(slot, id);
            if (hit >= 0) { setOut = slot; pitchOut = 1.0f; return hit; }
        } else if (vp.dir && vp.dir[0]) {
            want_set(vp.species, vp.dir);   // self-priming: a speaker nobody announced (an
        }                                   // opponent's first hit) still gets its own voice

        char buf[sizeof(Sound::id)];
        snprintf(buf, sizeof buf, "%s_%s", vp.species, id);
        const int hit = bank_find(buf);
        // Authored for this exact species: play it as written, scalar and all left alone.
        if (hit >= 0) { pitchOut = 1.0f; return hit; }
    }

    // Everything below here is a shared sound standing in for this creature, so it carries the
    // creature's own pitch -- that is what turns one family into a roster.
    pitchOut = vp.pitch > 0.0f ? vp.pitch : 1.0f;

    if (vp.family && vp.family[0]) {
        char buf[sizeof(Sound::id)];
        snprintf(buf, sizeof buf, "%s_%s", vp.family, id);
        const int hit = bank_find(buf);
        if (hit >= 0) return hit;
    }
    return baseIdx;
}

static Handle play_internal(const char* id, const Params& p)
{
    if (!s_ready.load() || !id || !*id) return kNoHandle;

    // Nobody is listening. Screen-off suspend parks the MIXER, so a sound started now would
    // not play -- it would freeze at its first sample and then burst out, minutes stale, the
    // moment the screen came back. And a muted or zeroed-out engine would spend a voice, a
    // decoder and an SD read to produce silence. Music is deliberately not handled here: it is
    // remembered and restarted (see music_gate_update), because a song is stateful in a way a
    // chirp is not.
    if (s_suspended.load(std::memory_order_acquire)) return kNoHandle;
    if (s_muted.load(std::memory_order_relaxed) || s_master <= 0.001f) return kNoHandle;

    int idx = bank_find(id);
    if (idx < 0) return kNoHandle;
    const Sound* snd = bank_at(idx);
    if (!snd) return kNoHandle;

    // Resolve the speaking creature's voice BEFORE anything else reads the entry, so bus,
    // priority and voice-stealing all judge the sound that will actually play. The BASE entry
    // is what declares an id voiced at all, which is why the plain lookup above comes first --
    // an unknown id, or an ordinary device sound, still costs exactly one scan.
    Params pp = p;
    int    set = -1;
    if (snd->voiced) {
        VoiceProfile self;
        if (!p.voice) {                 // nobody named a speaker: it is the player's pet
            self.species = s_voiceSpecies;
            self.family  = s_voiceFamily;
            self.dir     = s_voiceDir;
            self.pitch   = s_voicePitch;
        }
        float vpitch = 1.0f;
        idx = resolve_voice(id, idx, p.voice ? *p.voice : self, set, vpitch);
        pp.pitch *= vpitch;
        snd = sound_at(set, idx);
        if (!snd) return kNoHandle;
    }

    if (s_busGain[bus_slot(snd->bus)] <= 0.001f) return kNoHandle;

    lock();
    const int vi = acquire_voice(false, snd->priority);
    if (vi < 0) { unlock(); return kNoHandle; }     // everything playing is more important

    voice_begin(vi, *snd, idx, set, pp, false);
    Voice& v = s_voices[vi];
    const uint32_t gen = v.gen;
    Handle h = make_handle(vi, gen);

    // Synth and already-cached samples need no I/O, so they go live inside this same lock
    // and are audible in the very next mix block.
    if (go_live_locked(v, *snd, gen)) {
        unlock();
        xTaskNotifyGive(s_mixTask);
        return h;
    }
    unlock();

    // Needs a file: hand it to the streamer. The voice stays reserved (Loading) so it cannot
    // be stolen in the meantime and the handle the caller already holds stays valid.
    Req r{ REQ_START, (int16_t)vi, gen, (int16_t)idx, (int8_t)set };
    if (xQueueSend(s_reqQ, &r, 0) != pdTRUE) {
        lock();
        if (s_voices[vi].gen == gen) voice_free_locked(s_voices[vi]);
        unlock();
        return kNoHandle;
    }
    return h;
}

Handle play(const char* id)                    { Params p; return play_internal(id, p); }
Handle play(const char* id, const Params& p)   { return play_internal(id, p); }
Handle play(const char* id, float gain)        { Params p; p.gain = gain; return play_internal(id, p); }

// Can music be heard at all right now? Used to decide whether to play it or just remember it.
static bool music_audible()
{
    if (s_muted.load(std::memory_order_relaxed)) return false;
    return s_master > 0.001f && s_busGain[(int)Bus::Music] > 0.001f;
}

bool music(const char* id, float fadeSecs)
{
    if (!s_ready.load() || !id || !*id) return false;

    const int idx = bank_find(id);
    if (idx < 0) return false;

    // Muted, or a slider at zero: remember the track instead of decoding a song nobody can
    // hear. It starts for real the moment the volume comes back up.
    if (!music_audible()) {
        lock();
        s_gatedMusic = idx;
        s_pendingMusic.store(-1, std::memory_order_relaxed);
        unlock();
        return true;
    }

    lock();
    Voice& v = s_voices[kMusicVoice];
    const bool live = (v.state != VState::Free);

    if (s_musicIdx.load(std::memory_order_relaxed) == idx && live) {
        if (!v.freeAtZero) { unlock(); return true; }   // already playing: not a restart
        // It IS the track being asked for -- it is just on its way out to make room for
        // something else (a scene change that got reversed before the fade finished). Take it
        // back rather than letting the replacement land: cancelling the fade means no restart
        // and no gap, where the old code's "same id, nothing to do" left the OTHER track queued
        // and playing on the screen that had just asked for this one.
        v.freeAtZero = false;
        if (v.fade < 1.0f && fadeSecs > 0.01f) v.fadeRate = 1.0f / fadeSecs;
        else                                   { v.fade = 1.0f; v.fadeRate = 0.0f; }
        s_pendingMusic.store(-1, std::memory_order_relaxed);
        unlock();
        return true;
    }
    if (s_pendingMusic.load(std::memory_order_relaxed) == idx) { unlock(); return true; }

    // Fade whatever is on the music voice out; the streamer starts the new track once the
    // voice frees. Sequential rather than crossfaded, because a crossfade would need two of
    // only four voices for the length of the fade -- a poor trade on this budget.
    fade_out_or_free_locked(v, fadeSecs);
    s_pendingMusic.store(idx, std::memory_order_relaxed);
    s_pendingFadeIn = fadeSecs;
    s_gatedMusic    = -1;
    unlock();

    xTaskNotifyGive(s_mixTask);
    wake_streamer();
    return true;
}

void music_stop(float fadeSecs)
{
    if (!s_ready.load()) return;
    lock();
    s_pendingMusic.store(-1, std::memory_order_relaxed);
    s_gatedMusic = -1;
    fade_out_or_free_locked(s_voices[kMusicVoice], fadeSecs);
    s_musicIdx.store(-1, std::memory_order_release);
    unlock();
}

// Safe to call from any task: the index is atomic and a bank id, once scanned, never moves or
// changes -- so the string this returns is stable for the life of the session.
const char* music_playing()
{
    const int i = s_musicIdx.load(std::memory_order_acquire);
    const Sound* s = bank_at(i);
    return s ? s->id : "";
}

void stop(Handle h, float fadeSecs)
{
    if (!s_ready.load()) return;
    lock();
    if (Voice* v = resolve(h)) fade_out_or_free_locked(*v, fadeSecs);
    unlock();
}

void stop_bus(Bus b, float fadeSecs)
{
    if (!s_ready.load()) return;
    lock();
    if (b == Bus::Music) {
        s_pendingMusic.store(-1, std::memory_order_relaxed);
        s_gatedMusic = -1;
    }
    for (int i = 0; i < kVoices; i++) {
        Voice& v = s_voices[i];
        if (v.state == VState::Free || v.bus != b) continue;
        fade_out_or_free_locked(v, fadeSecs);
    }
    unlock();
}

void stop_all(float fadeSecs)
{
    if (!s_ready.load()) return;
    lock();
    s_pendingMusic.store(-1, std::memory_order_relaxed);
    s_gatedMusic = -1;
    for (int i = 0; i < kVoices; i++) fade_out_or_free_locked(s_voices[i], fadeSecs);
    unlock();
}

bool playing(Handle h)
{
    if (!s_ready.load()) return false;
    lock();
    const bool live = resolve(h) != nullptr;
    unlock();
    return live;
}

void set_gain(Handle h, float gain)
{
    if (!s_ready.load()) return;
    lock();
    if (Voice* v = resolve(h)) v->gain = clampf(gain, 0.0f, MAX_VOICE_GAIN);
    unlock();
}

void set_pitch(Handle h, float pitch)
{
    if (!s_ready.load()) return;
    pitch = clampf(pitch, 0.25f, 4.0f);
    lock();
    if (Voice* v = resolve(h)) {
        v->pitch = pitch;
        if (v->kind == VKind::Synth)      v->synth.setPitch(pitch);
        else if (v->kind == VKind::Mem && v->mem) voice_set_rate(*v, v->mem->rate);
        else if (v->kind == VKind::Stream && v->slot >= 0) voice_set_rate(*v, s_streams[v->slot].rate);
    }
    unlock();
}

// Music that nobody can hear should not be decoded. Muting used to leave a streamed track
// pulling blocks off the card, running the MP3/tracker decoder and resampling for the whole
// time it was silent -- and, because the mixer still counted it as an active voice, holding
// the I2S clock up as well. Park the track instead and pick it up on the way back.
static void music_gate_update()
{
    if (!s_ready.load()) return;

    if (!music_audible()) {
        lock();
        const int pending = s_pendingMusic.load(std::memory_order_relaxed);
        const int park    = (pending >= 0) ? pending : s_musicIdx.load(std::memory_order_relaxed);
        if (park >= 0) {
            s_gatedMusic = park;
            s_pendingMusic.store(-1, std::memory_order_relaxed);
            voice_free_locked(s_voices[kMusicVoice]);
            s_musicIdx.store(-1, std::memory_order_release);   // nothing is playing now
        }
        unlock();
        return;
    }

    lock();
    const int park = s_gatedMusic;
    s_gatedMusic = -1;
    unlock();
    if (park < 0) return;
    if (const Sound* s = bank_at(park)) music(s->id, 0.4f);
}

void set_bus_gain(Bus b, float g)
{
    // Bus::Ui has no gain of its own (bus_slot); writing it lands on the effects bus, which is
    // the behaviour the two mirrored assignments this replaced were reaching for.
    s_busGain[bus_slot(b)] = clampf(g, 0.0f, 1.0f);
    if (bus_slot(b) == (int)Bus::Music) music_gate_update();
}

float bus_gain(Bus b)   { return s_busGain[bus_slot(b)]; }
void  set_master(float g) { s_master = clampf(g, 0.0f, 1.0f); music_gate_update(); }
float master()          { return s_master; }

void set_muted(bool m)
{
    s_muted.store(m, std::memory_order_relaxed);
    music_gate_update();
}
bool muted() { return s_muted.load(std::memory_order_relaxed); }

// NVS keys are four characters because that is what the rest of the game uses for standalone
// flags (see SaveStore); volumes are stored as 0..100 so a corrupt/absent key reads as a
// sensible number rather than a raw float.
void settings_load(const SaveStore& save)
{
    s_master              = save.loadU8("avol", 80) / 100.0f;
    // Music defaults LOW. A looping chip tune on a device that sits on a desk all day wears
    // out its welcome long before the effects do, and a player who wants it can find the
    // slider; one who does not should not have to go looking for it in the first hour.
    s_busGain[(int)Bus::Music] = save.loadU8("amus", 40) / 100.0f;
    s_busGain[(int)Bus::Sfx]   = save.loadU8("asfx", 100) / 100.0f;
    // Bus::Ui needs no value of its own: bus_slot() resolves it to the effects bus wherever it
    // is read. (It used to be a copy made here and again in the settings scene.)
    s_muted.store(save.loadU8("amut", 0) != 0, std::memory_order_relaxed);
}

void settings_store(const SaveStore& save)
{
    save.beginBatch();
    save.storeU8("avol", (uint8_t)(s_master * 100.0f + 0.5f));
    save.storeU8("amus", (uint8_t)(s_busGain[(int)Bus::Music] * 100.0f + 0.5f));
    save.storeU8("asfx", (uint8_t)(s_busGain[(int)Bus::Sfx] * 100.0f + 0.5f));
    save.storeU8("amut", s_muted.load(std::memory_order_relaxed) ? 1 : 0);
    save.endBatch();
}

Stats stats(bool resetPeak)
{
    Stats s{};
    if (!s_ready.load()) return s;
    lock();
    for (int i = 0; i < kVoices; i++)
        if (s_voices[i].state != VState::Free) s.activeVoices++;
    unlock();
    // Attached to a live voice, read through the atomic the streamer and mixer already use to
    // hand slots over -- no lock, and no separate flag to fall out of step with.
    for (int i = 0; i < kStreams; i++)
        if (s_streams[i].voice.load(std::memory_order_relaxed) >= 0) s.activeStreams++;

    s.underruns = s_underruns.load(std::memory_order_relaxed);
    // Reading the peak is only destructive when the caller ASKS, because two readers that both
    // cleared it destroyed each other's window: with the settings overlay open (~44 reads a
    // second) the periodic log could only ever report the last frame, which is precisely when
    // someone is looking for a spike.
    s.mixPeakUs = resetPeak ? s_mixPeakUs.exchange(0, std::memory_order_relaxed)
                            : s_mixPeakUs.load(std::memory_order_relaxed);
    return s;
}

}   // namespace audio
