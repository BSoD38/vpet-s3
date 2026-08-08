#include "decoder.hpp"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
#include "mp3dec.h"     // components/chmorgan__esp-libhelix-mp3
}

// MP3 via Helix (fixed-point, no FPU pressure, ~29 KB of decoder state). This is the music
// path: MP3 buys perhaps 10:1 over PCM, which is the difference between a song fitting on
// the card and not, and the ~15% of a core it costs is affordable on a task that is not the
// render loop. It is deliberately NOT the effects path -- see wav.cpp for why.
//
// Everything here is stream-shaped. Helix decodes one ~26 ms frame per call from a sliding
// input window, so the decoder keeps a byte buffer, refills it from the file when it runs
// short, and resyncs by scanning for the next frame header. That resync is not just for
// corrupt files: it is how ID3 tags, album art and the junk some encoders leave between
// frames get skipped without special-casing any of them.

namespace audio {

static const char* TAG = "AUD/MP3";

// Input window. Must comfortably exceed one maximum-size frame (1441 bytes at 320 kbps) so
// a frame is never split across a refill more than once; 4 KB also makes the file reads big
// enough that FAT overhead disappears.
static constexpr int IN_CAP = 4096;

// Helix's worst case: 2 granules x 576 samples x 2 channels.
static constexpr int OUT_MAX = MAX_NGRAN * MAX_NSAMP * MAX_NCHAN;

// How many resync attempts before declaring a file unplayable. Generous, because a valid
// file legitimately needs several at the start (tags, padding) -- but bounded, so a file
// that is not MP3 at all fails fast instead of grinding through it byte by byte.
static constexpr int MAX_RESYNC = 32;

class Mp3Decoder : public Decoder {
public:
    explicit Mp3Decoder(FILE* f) : f_(f) {}
    ~Mp3Decoder() override
    {
        if (h_) MP3FreeDecoder(h_);
        if (f_) fclose(f_);
        free(in_);
    }

    // Set up the decoder and confirm the stream really is MP3 by decoding nothing but its
    // first frame header. Cheap, and it is the only honest way to answer "is this an MP3":
    // the extension lies often enough on modded content that it cannot be the test.
    bool parse()
    {
        in_ = (uint8_t*)malloc(IN_CAP);
        if (!in_) return false;

        skipId3();
        audioStart_ = ftell(f_);

        h_ = MP3InitDecoder();
        if (!h_) { ESP_LOGE(TAG, "decoder alloc failed (internal heap exhausted?)"); return false; }

        if (!fill()) return false;

        // Find a frame header and read the stream's shape from it, without consuming it --
        // the first decode() call re-finds the same sync and decodes it for real.
        for (int tries = 0; tries < MAX_RESYNC; tries++) {
            const int off = MP3FindSyncWord(in_ + pos_, (int)(fill_ - pos_));
            if (off < 0) {
                pos_ = fill_;                    // nothing usable here; slide the window on
                if (!fill()) return false;
                continue;
            }
            pos_ += off;

            MP3FrameInfo fi;
            if (MP3GetNextFrameInfo(h_, &fi, in_ + pos_) == ERR_MP3_NONE &&
                fi.samprate >= 8000 && fi.samprate <= 48000 && fi.nChans >= 1 && fi.nChans <= 2) {
                rate_  = (uint32_t)fi.samprate;
                chans_ = (uint8_t)fi.nChans;
                ESP_LOGI(TAG, "%u Hz, %u ch, %d kbps", (unsigned)rate_, chans_, fi.bitrate / 1000);
                return true;
            }
            pos_++;                              // false sync; step past it and keep looking
            if (fill_ - pos_ < 4 && !fill()) return false;
        }
        return false;
    }

    int decode(int16_t* out, int maxSamples) override
    {
        if (maxSamples < OUT_MAX) return 0;      // a frame cannot be split

        for (int tries = 0; tries < MAX_RESYNC; tries++) {
            // Keep at least one maximum frame in the window before asking Helix to decode,
            // so INDATA_UNDERFLOW means "file ended", not "buffer happened to be short".
            if (fill_ - pos_ < MAINBUF_SIZE && !eof_) fill();
            if (fill_ - pos_ == 0) return 0;     // clean end of stream

            const int off = MP3FindSyncWord(in_ + pos_, (int)(fill_ - pos_));
            if (off < 0) {
                // No header in the whole window. Keep the last few bytes in case a sync
                // straddles the boundary, drop the rest.
                pos_ = (fill_ > 3) ? fill_ - 3 : fill_;
                if (eof_) return 0;
                continue;
            }
            pos_ += off;

            uint8_t* p    = in_ + pos_;
            int      left = (int)(fill_ - pos_);
            const int err = MP3Decode(h_, &p, &left, out, 0);
            pos_ = fill_ - (uint32_t)left;       // Helix advanced p; mirror that into pos_

            if (err == ERR_MP3_NONE) {
                MP3FrameInfo fi;
                MP3GetLastFrameInfo(h_, &fi);
                if (fi.outputSamps <= 0) continue;
                // A stream may legitimately change rate/channels at a frame boundary. The
                // mixer reads rate() once per voice, so rather than let playback silently
                // run at the wrong speed, stop at the change; VBR (which varies bitrate,
                // not rate) is unaffected and is what actually occurs in practice.
                if ((uint32_t)fi.samprate != rate_ || (uint8_t)fi.nChans != chans_) {
                    ESP_LOGW(TAG, "format changed mid-stream (%d Hz %d ch); stopping",
                             fi.samprate, fi.nChans);
                    return 0;
                }
                return fi.outputSamps;
            }

            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                if (eof_) return 0;
                continue;                        // the loop's fill() at the top tops us up
            }

            // Anything else is a bad frame. One bad frame is normal near a tag boundary; a
            // run of them is a broken file, which MAX_RESYNC bounds.
            pos_++;
        }
        ESP_LOGW(TAG, "gave up resyncing");
        return -1;
    }

    bool rewind() override
    {
        if (fseek(f_, audioStart_, SEEK_SET) != 0) return false;
        pos_ = fill_ = 0;
        eof_ = false;
        // Reset the bit reservoir too: Helix carries main data across frames, and replaying
        // from the top with a previous frame's reservoir still loaded makes the loop point
        // click. Cheap enough at loop boundaries (a few hundred microseconds).
        if (h_) MP3FreeDecoder(h_);
        h_ = MP3InitDecoder();
        return h_ != nullptr;
    }

    uint32_t rate()  const override { return rate_; }
    uint8_t  chans() const override { return chans_; }
    int      maxBlockSamples() const override { return OUT_MAX; }

    void disown() { f_ = nullptr; }

private:
    // An ID3v2 tag sits in front of the audio and is large enough (album art) to swamp the
    // resync scan. Its header states its own length, so skip it outright.
    void skipId3()
    {
        uint8_t t[10];
        if (fread(t, 1, 10, f_) != 10) { fseek(f_, 0, SEEK_SET); return; }
        if (memcmp(t, "ID3", 3) != 0) { fseek(f_, 0, SEEK_SET); return; }
        // Syncsafe integer: 7 usable bits per byte, so no byte can look like a frame sync.
        const long size = ((long)(t[6] & 0x7F) << 21) | ((long)(t[7] & 0x7F) << 14) |
                          ((long)(t[8] & 0x7F) << 7)  |  (long)(t[9] & 0x7F);
        fseek(f_, 10 + size, SEEK_SET);
    }

    // Slide unread bytes to the front and read more in. False only when the buffer is empty
    // and the file is exhausted.
    bool fill()
    {
        if (pos_ > 0) {
            memmove(in_, in_ + pos_, fill_ - pos_);
            fill_ -= pos_;
            pos_ = 0;
        }
        if (!eof_ && fill_ < IN_CAP) {
            const size_t got = fread(in_ + fill_, 1, IN_CAP - fill_, f_);
            fill_ += (uint32_t)got;
            if (got == 0) eof_ = true;
        }
        return fill_ > 0;
    }

    FILE*       f_    = nullptr;
    HMP3Decoder h_    = nullptr;
    uint8_t*    in_   = nullptr;
    uint32_t    fill_ = 0, pos_ = 0;
    bool        eof_  = false;
    long        audioStart_ = 0;
    uint32_t    rate_  = 44100;
    uint8_t     chans_ = 2;
};

Decoder* mp3_try(FILE* f)
{
    Mp3Decoder* d = new (std::nothrow) Mp3Decoder(f);
    if (!d) return nullptr;
    if (!d->parse()) {
        d->disown();       // leave the file open for the dispatcher's next attempt
        delete d;
        return nullptr;
    }
    return d;
}

}   // namespace audio
