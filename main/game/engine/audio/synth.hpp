#pragma once
#include <cstdint>

// A tiny chip synthesiser: the third kind of voice, alongside decoded samples and streams.
//
// WHY A SYNTH BELONGS IN A PET GAME'S AUDIO ENGINE, and why it is arguably the most
// game-appropriate format here:
//
//   * It ships no assets. A tone is nine numbers and a melody is one line of text, so the
//     base game has a complete sound set inside base.pak at a cost of about 4 KB -- versus
//     megabytes of WAVs for the same coverage. On a device whose data partition is 3 MB and
//     whose mods arrive on an SD card, that ratio is the whole argument.
//   * It is the right SOUND. This is a virtual pet with 16-frame sprite animations; square
//     waves and short arpeggios are the idiom, not recorded audio. Getting the beeps right
//     matters more here than fidelity does.
//   * It is free to play. No file, no decode, no PSRAM cache entry -- a synth voice is a
//     phase accumulator and an envelope, so an effect fires with genuinely zero latency and
//     costs about a microsecond per millisecond of sound.
//   * It is trivially moddable and trivially varied. Pitch a blip by the pet's mood, drop
//     an RTTTL tune off the internet into a mod pack, retune a species' voice by editing one
//     number. None of that needs an audio editor, which is the difference between mods that
//     get made and mods that do not.
//
// RTTTL specifically because it is a real, documented format with thousands of free tunes
// already written in it, it is human-editable in a text field, and it maps exactly onto what
// a monophonic chip voice can play. Melodies are monophonic per voice by design -- to get a
// chord, play two melody sounds and let the 4-voice mixer do what it is for.

namespace audio {

enum class Wave : uint8_t { Square, Pulse, Triangle, Saw, Noise, Sine };

// Timbre + envelope, shared by one-shot tones and by every note of a melody.
struct Timbre {
    Wave  wave     = Wave::Square;
    float duty     = 0.5f;     // Pulse only: 0..1 fraction of the cycle spent high
    float attack   = 0.003f;   // seconds ramping up (a hard start clicks)
    float release  = 0.030f;   // seconds ramping down at the note's tail
    float slide    = 1.0f;     // end/start frequency ratio across each note (1 = no slide)
    float vibHz    = 0.0f;     // vibrato rate; 0 = off
    float vibCents = 0.0f;     // vibrato depth in cents (100 = one semitone)
    float vol      = 0.6f;     // 0..1 before the voice/bus/master gains
};

// One note of a sequence. freq 0 is a rest, which is why this is not just a frequency list.
struct Note {
    float    freq;
    uint16_t ms;
};

// A parsed, immutable note sequence owned by the sound bank. Voices point at it and never
// modify it, so one melody can drive several voices at once.
struct Melody {
    Note*    notes = nullptr;
    uint16_t count = 0;
    Timbre   timbre;
};

// A one-shot blip. Expands to `repeat` identical notes separated by `gapMs`, so the same
// runtime handles "one chirp" and "three ascending chirps" with no special case.
struct Tone {
    Timbre   timbre;
    float    freq   = 880.0f;
    uint16_t ms     = 70;
    uint8_t  repeat = 1;
    uint16_t gapMs  = 30;
    float    step   = 1.0f;   // frequency ratio between repeats (1.26 = up a major third)
};

// Parse an RTTTL string ("Name:d=4,o=5,b=125:8e6,8e6,...") into `out`, allocating the note
// array from PSRAM (caller frees with melody_free). The name section is ignored; a leading
// colon with no name is accepted, as plenty of tunes in the wild are written that way.
// Returns false if no playable note came out of it.
bool rtttl_parse(const char* s, Melody& out);
void melody_free(Melody& m);

// Runtime state for one synth voice. Rendered by the mixer directly at kMixRate, which is
// why there is no resampling step in the synth path at all.
class SynthVoice {
public:
    void startTone(const Tone& t, float pitch);
    void startMelody(const Melody& m, float pitch, bool loop);

    // Write `frames` stereo frames at kMixRate. Returns how many were actually produced;
    // fewer than asked means the sequence ended (the tail is zero-filled by the caller).
    int  render(int16_t* dst, int frames);
    bool done() const { return done_; }

    void setPitch(float pitch);

private:
    void beginNote();

    Timbre       timbre_{};
    const Note*  seq_    = nullptr;   // melody notes, or inline_ for a tone
    uint16_t     count_  = 0;
    Note         inline_[8];          // a tone expands to at most 8 repeats, held in-place
    bool         loop_   = false;
    bool         done_   = true;

    int      idx_       = 0;          // current note
    uint32_t noteFrames_ = 0, notePos_ = 0;
    uint32_t atkFrames_ = 0, relFrames_ = 0;
    uint32_t phase_     = 0, phaseInc_ = 0;
    float    incMul_    = 1.0f;       // per-sample multiplier that realises `slide`
    float    pitch_     = 1.0f;
    uint32_t vibPhase_  = 0, vibInc_ = 0;
    float    vibDepth_  = 0.0f;       // as a frequency ratio deviation
    uint32_t lfsr_      = 0x7FFF;     // noise generator
    int16_t  noiseVal_  = 0;
    bool     rest_      = false;
};

}   // namespace audio
