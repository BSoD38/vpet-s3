#pragma once
#include <cstdint>
#include <cstdio>

// The format seam. Everything the mixer knows about a compressed file is behind this
// interface: give it a FILE*, pull interleaved PCM16 out of it until it says it is done.
//
// The SAME decoder serves both playback modes, which is why the interface is a pull and
// not a "decode this whole file" call. A `sample` is a stream drained in a loop at load
// time into one PSRAM buffer; a `stream` is the same decoder drained a chunk at a time
// while it plays. One implementation per format, two ways to run it.
//
// Decoders run ONLY on the streamer task, never on the mixer or the game loop, so an
// implementation is free to block on file I/O and to be slow.
//
// ADDING A FORMAT (Vorbis, a tracker module, ADPCM in some other container) means writing
// a subclass and adding one line to decoder_open(). Nothing above this file changes.

namespace audio {

class Decoder {
public:
    virtual ~Decoder() {}

    // Decode the next run of samples into `out`, which has room for `maxSamples` int16s.
    // Returns how many INTERLEAVED SAMPLES were written (frames x channels), 0 at clean
    // end-of-stream, or -1 on a corrupt file. Callers must offer at least
    // maxSamples >= maxBlockSamples(), since a decoder cannot split its native frame.
    virtual int decode(int16_t* out, int maxSamples) = 0;

    // Rewind to the first audio sample, for looping. False if the file cannot be re-read
    // (in which case a looping voice just ends instead of looping).
    virtual bool rewind() = 0;

    virtual uint32_t rate()  const = 0;    // source sample rate in Hz
    virtual uint8_t  chans() const = 0;    // 1 or 2

    // Upper bound on what one decode() call can emit, so callers can size a scratch buffer.
    virtual int maxBlockSamples() const = 0;

    // Total frames if the container states it (WAV does, MP3 does not without scanning).
    // 0 means unknown -- used only to pre-size the buffer when loading a sample, never for
    // playback logic, so an unknown length costs a few reallocs and nothing else.
    virtual uint32_t totalFrames() const { return 0; }

    // Tell the decoder whether the voice wants the sound repeated. Ignored by the file
    // formats, where looping is the CALLER's business (rewind() and play again) -- but a
    // tracker module decides for itself where the song loops back to, which is usually not
    // the beginning, and only it knows where that is. Called once after a successful open.
    virtual void setLooping(bool loop) { (void)loop; }
};

// Open `path` and return a decoder positioned at the first audio sample, or nullptr if the
// file is missing or is not a format we handle. Takes ownership of nothing: the returned
// decoder owns its own FILE*, and deleting it closes the file.
//
// Dispatch is by CONTENT (magic bytes), with the extension used only to order the sniffing.
// A game's audio arrives from mod packs and SD cards where extensions lie routinely, and a
// mislabelled .wav that is really an MP3 should just play.
Decoder* decoder_open(const char* path);

// Per-format factories, tried in turn by decoder_open(). Each inspects the stream and
// returns nullptr if it is not its format, leaving the file position undefined (the
// dispatcher rewinds between attempts). On success the decoder OWNS `f` and closes it.
// Listed here rather than hidden in the .cpp so the extension point is visible from the
// interface it extends: a new format is a factory plus one line in decoder_open().
Decoder* wav_try(FILE* f);
Decoder* mp3_try(FILE* f);
// Every tracker module format at once -- MOD, XM, S3M, IT -- because one library (libxmp-lite)
// handles them all, so there is nothing to gain from asking about them one at a time.
Decoder* module_try(FILE* f);

// True if `path` names a tracker module rather than a plain audio file. Modules must always
// be STREAMED: a module is a generator, not a finite clip, so "decode it all into RAM" has no
// end condition. The sound bank uses this to force the right playback mode regardless of what
// a manifest asks for. Extension-based on purpose -- the bank needs the answer WITHOUT opening
// the file, and being wrong only costs the wrong default playback mode.
bool decoder_is_module(const char* path);

// True if the extension is one we could plausibly decode. Used by the bank's directory scan
// to decide which loose files in a sounds/ folder to register, WITHOUT opening each one --
// a FAT open per file is exactly the boot cost mod packs exist to avoid.
bool decoder_handles_extension(const char* path);

}   // namespace audio
