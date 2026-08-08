#include "decoder.hpp"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>
#include <new>

// RIFF/WAVE decoding: uncompressed PCM at every bit depth in circulation, plus IMA ADPCM.
//
// WHY BOTH. PCM16 is what every tool exports and what a modder will drop on a card, so it
// has to work with no ceremony. IMA ADPCM is the one compressed format worth having on a
// microcontroller: a fixed 4:1 ratio at roughly the cost of a table lookup per sample, no
// bit-reservoir, no frame sync, seekable by arithmetic. That makes it the right storage
// format for effects specifically -- four times as many chirps fit in the PSRAM sample
// cache, and decoding one costs less than the FAT read that fetched it. MP3 is the wrong
// tool there (a 26 ms frame granularity and ~40 KB of decoder state to fire a 50 ms blip);
// it earns its keep on music, which is what mp3.cpp is for.
//
// The 8/24/32-bit and float paths exist because they cost five lines each and because the
// alternative is a modder's file silently not playing. Everything is narrowed to PCM16,
// which is the mixer's only input format.

namespace audio {

static const char* TAG = "AUD/WAV";

// Frames pulled per decode() call on the PCM path, which is also the size of one fread. 2048
// frames is 8 KB of 16-bit stereo: four times fewer SD transactions than the 512 this started
// at, for the same throughput, and still a quarter of the streaming ring (8192 SAMPLES), which
// is the ceiling that matters -- a decoder whose block does not fit the ring cannot be played
// at all (see attach_stream).
static constexpr int PCM_CHUNK_FRAMES = 2048;

// --- WAVE format tags ---
static constexpr uint16_t WF_PCM        = 0x0001;
static constexpr uint16_t WF_FLOAT      = 0x0003;
static constexpr uint16_t WF_IMA_ADPCM  = 0x0011;
static constexpr uint16_t WF_EXTENSIBLE = 0xFFFE;

// --- IMA ADPCM state machine (IMA/DVI, the version WAV tag 0x11 uses) ---
static const int8_t IMA_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t IMA_STEP[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct ImaState { int32_t pred; int8_t index; };

static inline int16_t ima_step_one(ImaState& s, uint8_t nib)
{
    const int32_t step = IMA_STEP[s.index];
    int32_t diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) diff = -diff;

    s.pred += diff;
    if (s.pred >  32767) s.pred =  32767;
    if (s.pred < -32768) s.pred = -32768;

    s.index = (int8_t)(s.index + IMA_INDEX[nib]);
    if (s.index < 0)  s.index = 0;
    if (s.index > 88) s.index = 88;

    return (int16_t)s.pred;
}

// --- little-endian readers (the host is little-endian too, but be explicit) ---
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

class WavDecoder : public Decoder {
public:
    explicit WavDecoder(FILE* f) : f_(f) {}
    ~WavDecoder() override
    {
        if (f_) fclose(f_);
        free(raw_);
    }

    // Walk the chunk list for `fmt ` and `data`. Returns false on anything we cannot play,
    // having logged why -- a file that is a valid WAV of an unsupported flavour is a much
    // more likely mod-authoring mistake than a corrupt one, and it needs to say so.
    bool parse()
    {
        uint8_t hdr[12];
        if (fread(hdr, 1, 12, f_) != 12) return false;
        if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

        bool haveFmt = false;
        uint8_t ck[8];
        while (fread(ck, 1, 8, f_) == 8) {
            const uint32_t size = rd32(ck + 4);
            const long body = ftell(f_);

            if (memcmp(ck, "fmt ", 4) == 0) {
                uint8_t fmt[40] = {};
                const uint32_t want = size < sizeof fmt ? size : (uint32_t)sizeof fmt;
                if (fread(fmt, 1, want, f_) != want) return false;

                tag_        = rd16(fmt + 0);
                chans_      = (uint8_t)rd16(fmt + 2);
                rate_       = rd32(fmt + 4);
                blockAlign_ = rd16(fmt + 12);
                bits_       = rd16(fmt + 14);

                // WAVE_FORMAT_EXTENSIBLE wraps the real tag in the first two bytes of its
                // sub-format GUID. Common from newer exporters, and a file we would
                // otherwise reject for no reason.
                if (tag_ == WF_EXTENSIBLE && want >= 26) tag_ = rd16(fmt + 24);

                // IMA's extension field says how many samples a block decodes to. Derive it
                // from blockAlign when absent: it is fully determined by the layout (a
                // 4-byte header per channel, then one nibble per sample), and some encoders
                // do omit it.
                if (tag_ == WF_IMA_ADPCM) {
                    if (want >= 20) spb_ = rd16(fmt + 18);
                    if (spb_ == 0 && chans_ && blockAlign_ > 4 * chans_)
                        spb_ = (uint16_t)(1 + (blockAlign_ - 4 * chans_) * 2 / chans_);
                }
                haveFmt = true;
            } else if (memcmp(ck, "data", 4) == 0) {
                dataStart_ = body;
                dataBytes_ = size;
                // Some writers leave a placeholder length (streamed output, or a truncated
                // file). Trust the file's real extent over the header rather than reading
                // off the end.
                fseek(f_, 0, SEEK_END);
                const long fileEnd = ftell(f_);
                if (dataStart_ + (long)dataBytes_ > fileEnd || dataBytes_ == 0xFFFFFFFFu)
                    dataBytes_ = (uint32_t)(fileEnd - dataStart_);
                if (haveFmt) break;             // fmt already seen: nothing else to look for
            }

            // Chunk bodies are padded to an even length; the pad byte is not in `size`.
            if (fseek(f_, body + (long)size + (long)(size & 1), SEEK_SET) != 0) break;
        }

        if (!haveFmt || dataStart_ == 0 || chans_ < 1 || chans_ > 2 || rate_ < 4000 || rate_ > 96000) {
            ESP_LOGW(TAG, "unusable header (chans %u rate %u)", chans_, (unsigned)rate_);
            return false;
        }

        switch (tag_) {
            case WF_PCM:
                if (bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32) {
                    ESP_LOGW(TAG, "unsupported PCM width: %u-bit", bits_);
                    return false;
                }
                frameBytes_  = (uint16_t)(bits_ / 8 * chans_);
                totalFrames_ = frameBytes_ ? dataBytes_ / frameBytes_ : 0;
                maxBlk_      = PCM_CHUNK_FRAMES * chans_;
                rawCap_      = (uint32_t)PCM_CHUNK_FRAMES * frameBytes_;
                break;

            case WF_FLOAT:
                if (bits_ != 32) { ESP_LOGW(TAG, "unsupported float width: %u-bit", bits_); return false; }
                frameBytes_  = (uint16_t)(4 * chans_);
                totalFrames_ = dataBytes_ / frameBytes_;
                maxBlk_      = PCM_CHUNK_FRAMES * chans_;
                rawCap_      = (uint32_t)PCM_CHUNK_FRAMES * frameBytes_;
                break;

            case WF_IMA_ADPCM:
                if (spb_ == 0 || blockAlign_ < 4 * chans_) {
                    ESP_LOGW(TAG, "bad IMA layout (block %u, spb %u)", blockAlign_, spb_);
                    return false;
                }
                totalFrames_ = (dataBytes_ / blockAlign_) * spb_;
                maxBlk_      = (int)spb_ * chans_;
                rawCap_      = blockAlign_;
                break;

            default:
                ESP_LOGW(TAG, "unsupported WAVE format tag 0x%04X", tag_);
                return false;
        }

        // 16-bit PCM reads straight into the caller's buffer (see decodePcm), so it needs no
        // staging at all -- worth skipping now that a chunk is 8 KB rather than 2 KB.
        if (!(tag_ == WF_PCM && bits_ == 16)) {
            raw_ = (uint8_t*)malloc(rawCap_);
            if (!raw_) return false;
        }
        return rewind();
    }

    int decode(int16_t* out, int maxSamples) override
    {
        if (maxSamples < maxBlk_) return 0;                 // caller must offer a full block
        if (dataRead_ >= dataBytes_) return 0;              // clean end of stream

        return (tag_ == WF_IMA_ADPCM) ? decodeIma(out) : decodePcm(out, maxSamples);
    }

    bool rewind() override
    {
        dataRead_ = 0;
        return fseek(f_, dataStart_, SEEK_SET) == 0;
    }

    uint32_t rate()  const override { return rate_; }
    uint8_t  chans() const override { return chans_; }
    int      maxBlockSamples() const override { return maxBlk_; }
    uint32_t totalFrames() const override { return totalFrames_; }

    // Release the FILE* without closing it. Used only when parse() rejects the stream: the
    // dispatcher still needs the file open to offer it to the next format.
    void disown() { f_ = nullptr; }

private:
    int decodePcm(int16_t* out, int maxSamples)
    {
        uint32_t want = dataBytes_ - dataRead_;
        if (want > rawCap_) want = rawCap_;
        want -= want % frameBytes_;                          // whole frames only
        if (want == 0) return 0;

        // 16-bit is the common case and needs no conversion at all: read straight into the
        // caller's buffer and skip the staging copy entirely.
        if (bits_ == 16 && tag_ == WF_PCM) {
            const size_t got = fread(out, 1, want, f_);
            dataRead_ += got;
            return (int)(got / 2);
        }

        const size_t got = fread(raw_, 1, want, f_);
        dataRead_ += got;
        const int frames  = (int)(got / frameBytes_);
        const int samples = frames * chans_;
        if (samples > maxSamples) return 0;                  // defensive; sizing prevents it

        const uint8_t* p = raw_;
        if (tag_ == WF_FLOAT) {
            for (int i = 0; i < samples; i++, p += 4) {
                float v;
                memcpy(&v, p, sizeof v);                     // may be unaligned in the buffer
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                out[i] = (int16_t)(v * 32767.0f);
            }
        } else if (bits_ == 8) {
            // 8-bit WAV is UNSIGNED, centred on 128 -- reading it as signed inverts the
            // waveform and doubles the DC offset.
            for (int i = 0; i < samples; i++, p += 1)
                out[i] = (int16_t)(((int)p[0] - 128) << 8);
        } else if (bits_ == 24) {
            for (int i = 0; i < samples; i++, p += 3)
                out[i] = (int16_t)(p[1] | (p[2] << 8));      // keep the top 16 bits
        } else {   // 32-bit int
            for (int i = 0; i < samples; i++, p += 4)
                out[i] = (int16_t)(p[2] | (p[3] << 8));
        }
        return samples;
    }

    // One ADPCM block -> spb_ frames. Blocks are self-contained (each carries its own
    // starting predictor), which is what makes the format seekable and makes a corrupt
    // block cost one block of noise rather than the rest of the file.
    int decodeIma(int16_t* out)
    {
        uint32_t want = dataBytes_ - dataRead_;
        if (want > blockAlign_) want = blockAlign_;
        const size_t got = fread(raw_, 1, want, f_);
        dataRead_ += got;
        if (got < (size_t)(4 * chans_)) return 0;            // not even a header: treat as EOF

        ImaState st[2];
        for (int c = 0; c < chans_; c++) {
            st[c].pred  = (int16_t)rd16(raw_ + c * 4);
            st[c].index = (int8_t)raw_[c * 4 + 2];
            if (st[c].index < 0)  st[c].index = 0;
            if (st[c].index > 88) st[c].index = 88;
            out[c] = (int16_t)st[c].pred;                    // the header sample IS frame 0
        }

        const uint8_t* p    = raw_ + 4 * chans_;
        const uint8_t* end  = raw_ + got;
        int            frame = 1;                            // frame 0 came from the header

        if (chans_ == 1) {
            while (p < end && frame < spb_) {
                out[frame++] = ima_step_one(st[0], (uint8_t)(*p & 0x0F));
                if (frame < spb_) out[frame++] = ima_step_one(st[0], (uint8_t)(*p >> 4));
                p++;
            }
        } else {
            // Stereo interleaves in 4-byte groups: eight left nibbles, then eight right.
            while (p + 8 <= end && frame < spb_) {
                const int base = frame;
                for (int c = 0; c < 2; c++) {
                    int fr = base;
                    for (int b = 0; b < 4 && fr < spb_; b++) {
                        const uint8_t byte = p[c * 4 + b];
                        out[fr * 2 + c] = ima_step_one(st[c], (uint8_t)(byte & 0x0F));
                        fr++;
                        if (fr < spb_) {
                            out[fr * 2 + c] = ima_step_one(st[c], (uint8_t)(byte >> 4));
                            fr++;
                        }
                    }
                    if (c == 1) frame = fr;
                }
                p += 8;
            }
        }
        return frame * chans_;
    }

    FILE*    f_          = nullptr;
    uint16_t tag_        = WF_PCM;
    uint16_t bits_       = 16;
    uint8_t  chans_      = 1;
    uint32_t rate_       = 22050;
    uint16_t blockAlign_ = 0;
    uint16_t frameBytes_ = 0;
    uint16_t spb_        = 0;        // IMA: frames per block
    long     dataStart_  = 0;
    uint32_t dataBytes_  = 0;
    uint32_t dataRead_   = 0;
    uint32_t totalFrames_ = 0;
    int      maxBlk_     = 0;
    uint8_t* raw_        = nullptr;  // staging for formats that need conversion
    uint32_t rawCap_     = 0;
};

Decoder* wav_try(FILE* f)
{
    WavDecoder* d = new (std::nothrow) WavDecoder(f);
    if (!d) return nullptr;
    if (!d->parse()) {
        // The decoder owns f_ and its destructor closes it, but the dispatcher still needs
        // the file open to try the next format -- so hand ownership back before deleting.
        d->disown();
        delete d;
        return nullptr;
    }
    return d;
}

}   // namespace audio
