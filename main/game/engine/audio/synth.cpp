#include "synth.hpp"
#include "audio.hpp"          // kMixRate
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace audio {

static const char* TAG = "AUD/SYN";

// Synth waveforms are full-scale and harmonically brutal compared with recorded audio -- a
// square wave at amplitude 1.0 is perceptually about twice as loud as a WAV that peaks at
// the same number. This factor makes a synth sound at vol=1.0 sit at roughly the same
// apparent level as a normalised sample, so a modder mixing the two does not have to
// discover the imbalance by ear.
static constexpr float SYNTH_HEADROOM = 0.45f;

// Phase is 0..2^32; this converts Hz into the per-sample increment.
static inline uint32_t hz_to_inc(float hz)
{
    if (hz <= 0.0f) return 0;
    const float inc = hz * 4294967296.0f / (float)kMixRate;
    return (inc >= 4294967295.0f) ? 0xFFFFFFFFu : (uint32_t)inc;
}

// 256-entry quarter-resolution sine, built once. A table is used rather than sinf() because
// vibrato evaluates it per sample; 256 entries is inaudibly coarse for an LFO and for the
// Sine waveform at chip-tune volumes.
static int16_t s_sine[256];
static bool    s_sineReady = false;
static void sine_init()
{
    if (s_sineReady) return;
    for (int i = 0; i < 256; i++)
        s_sine[i] = (int16_t)(32767.0f * sinf((float)i * 6.283185307f / 256.0f));
    s_sineReady = true;
}

// --- RTTTL ---------------------------------------------------------------------------------

// Semitone offset from C for each letter a..g. RTTTL writes accidentals as sharps only.
static const int8_t LETTER_SEMI[7] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g

// A4 = 440 Hz (scientific pitch), which is the convention every RTTTL tune in circulation
// is written against: o=5 puts middle C at 523 Hz, and a default o=6 melody sits in the
// bright register ringtones live in.
static float note_freq(int semi, int octave)
{
    return 440.0f * exp2f((float)(octave - 4) + (float)(semi - 9) / 12.0f);
}

bool rtttl_parse(const char* s, Melody& out)
{
    if (!s || !*s) return false;

    // Section 1 is the name and is purely decorative. Tunes written without one begin at
    // the colon, so find the FIRST colon rather than requiring text before it.
    const char* p = strchr(s, ':');
    if (!p) return false;
    p++;

    int defDur = 4, defOct = 6, bpm = 120;

    // Section 2: comma-separated k=v defaults, ending at the next colon.
    const char* body = strchr(p, ':');
    if (!body) return false;
    while (p < body) {
        while (p < body && (*p == ',' || isspace((unsigned char)*p))) p++;
        if (p >= body) break;
        const char key = (char)tolower((unsigned char)*p);
        const char* eq = p;
        while (eq < body && *eq != '=') eq++;
        if (eq < body) {
            const int v = atoi(eq + 1);
            if (key == 'd' && v > 0) defDur = v;
            if (key == 'o' && v > 0) defOct = v;
            if (key == 'b' && v > 0) bpm    = v;
        }
        while (p < body && *p != ',') p++;
    }
    body++;

    if (bpm < 20)  bpm = 20;
    if (bpm > 900) bpm = 900;
    // A whole note is four beats; every duration denominator divides into that.
    const float wholeMs = 60000.0f / (float)bpm * 4.0f;

    // Two passes so the note array is exactly the right size: melodies live for the whole
    // session in PSRAM, and over-allocating every tune in a mod pack adds up.
    int count = 0;
    for (const char* q = body; *q; q++) if (*q == ',') count++;
    count++;                                   // no trailing comma after the last note
    if (count <= 0 || count > 512) return false;

    Note* notes = (Note*)heap_caps_malloc(sizeof(Note) * count, MALLOC_CAP_SPIRAM);
    if (!notes) notes = (Note*)malloc(sizeof(Note) * count);
    if (!notes) return false;

    int n = 0;
    const char* q = body;
    while (*q && n < count) {
        while (*q == ',' || isspace((unsigned char)*q)) q++;
        if (!*q) break;

        int dur = 0;
        while (isdigit((unsigned char)*q)) dur = dur * 10 + (*q++ - '0');
        if (dur <= 0) dur = defDur;

        bool dotted = false;
        if (*q == '.') { dotted = true; q++; }   // some writers dot before the letter

        int semi = -1;
        const char c = (char)tolower((unsigned char)*q);
        if (c == 'p') { semi = -1; q++; }
        else if (c >= 'a' && c <= 'g') { semi = LETTER_SEMI[c - 'a']; q++; }
        else { q++; continue; }                  // junk character: skip it, keep the tune

        if (*q == '#') { semi++; q++; }
        if (*q == '.') { dotted = true; q++; }

        int oct = defOct;
        if (isdigit((unsigned char)*q)) oct = *q++ - '0';
        if (*q == '.') { dotted = true; q++; }

        float ms = wholeMs / (float)dur;
        if (dotted) ms *= 1.5f;
        if (ms > 8000.0f) ms = 8000.0f;

        notes[n].freq = (semi < 0) ? 0.0f : note_freq(semi, oct);
        notes[n].ms   = (uint16_t)ms;
        n++;

        while (*q && *q != ',') q++;
    }

    if (n == 0) { free(notes); return false; }
    out.notes = notes;
    out.count = (uint16_t)n;
    return true;
}

void melody_free(Melody& m)
{
    free(m.notes);
    m.notes = nullptr;
    m.count = 0;
}

// --- voice ----------------------------------------------------------------------------------

void SynthVoice::startTone(const Tone& t, float pitch)
{
    sine_init();
    timbre_ = t.timbre;
    pitch_  = (pitch > 0.05f) ? pitch : 1.0f;

    uint8_t reps = t.repeat ? t.repeat : 1;
    if (reps > 8) reps = 8;                      // inline_ capacity; see the header

    // Expand the repeat into an explicit note sequence (with rests for the gaps) so the
    // render loop only ever has to know about "a list of notes".
    int n = 0;
    float f = t.freq;
    for (uint8_t r = 0; r < reps && n < (int)(sizeof inline_ / sizeof inline_[0]); r++) {
        inline_[n].freq = f;
        inline_[n].ms   = t.ms ? t.ms : 1;
        n++;
        if (r + 1 < reps && t.gapMs && n < (int)(sizeof inline_ / sizeof inline_[0])) {
            inline_[n].freq = 0.0f;              // rest
            inline_[n].ms   = t.gapMs;
            n++;
        }
        f *= t.step;
    }

    seq_    = inline_;
    count_  = (uint16_t)n;
    loop_   = false;
    idx_    = 0;
    done_   = false;
    lfsr_   = 0x7FFF;
    beginNote();
}

void SynthVoice::startMelody(const Melody& m, float pitch, bool loop)
{
    sine_init();
    if (!m.notes || m.count == 0) { done_ = true; return; }
    timbre_ = m.timbre;
    pitch_  = (pitch > 0.05f) ? pitch : 1.0f;
    seq_    = m.notes;
    count_  = m.count;
    loop_   = loop;
    idx_    = 0;
    done_   = false;
    lfsr_   = 0x7FFF;
    beginNote();
}

void SynthVoice::setPitch(float pitch)
{
    if (pitch < 0.05f) pitch = 0.05f;
    if (pitch > 8.0f)  pitch = 8.0f;
    // Rescale the running phase increment rather than recomputing from the note, so a pitch
    // bend applied mid-note takes effect immediately and keeps any slide already under way.
    if (pitch_ > 0.0f) phaseInc_ = (uint32_t)((float)phaseInc_ * (pitch / pitch_));
    pitch_ = pitch;
}

void SynthVoice::beginNote()
{
    const Note& nt = seq_[idx_];
    notePos_    = 0;
    noteFrames_ = (uint32_t)nt.ms * kMixRate / 1000;
    if (noteFrames_ == 0) noteFrames_ = 1;

    rest_     = (nt.freq <= 0.0f);
    phaseInc_ = rest_ ? 0 : hz_to_inc(nt.freq * pitch_);

    // A slide is a constant frequency RATIO per sample, so it comes out as one multiply in
    // the inner loop instead of a pow() per sample.
    incMul_ = 1.0f;
    if (!rest_ && timbre_.slide > 0.0f && timbre_.slide != 1.0f)
        incMul_ = powf(timbre_.slide, 1.0f / (float)noteFrames_);

    atkFrames_ = (uint32_t)(timbre_.attack * kMixRate);
    relFrames_ = (uint32_t)(timbre_.release * kMixRate);
    // Envelope segments must fit inside the note, or a short blip would never reach full
    // amplitude and the release would start before the attack finished.
    if (atkFrames_ + relFrames_ > noteFrames_) {
        const uint32_t total = atkFrames_ + relFrames_;
        atkFrames_ = total ? atkFrames_ * noteFrames_ / total : 0;
        relFrames_ = noteFrames_ - atkFrames_;
    }

    vibInc_   = hz_to_inc(timbre_.vibHz);
    vibPhase_ = 0;
    // Cents to a frequency ratio deviation: 2^(cents/1200) - 1.
    vibDepth_ = (timbre_.vibCents > 0.0f) ? exp2f(timbre_.vibCents / 1200.0f) - 1.0f : 0.0f;
}

int SynthVoice::render(int16_t* dst, int frames)
{
    if (done_ || !seq_) return 0;

    const uint32_t dutyThresh = (uint32_t)(4294967296.0 *
        (double)((timbre_.duty > 0.01f && timbre_.duty < 0.99f) ? timbre_.duty : 0.5f));
    const float baseAmp = SYNTH_HEADROOM * (timbre_.vol > 0.0f ? timbre_.vol : 1.0f);

    int produced = 0;
    while (produced < frames) {
        if (notePos_ >= noteFrames_) {                 // advance the sequence
            idx_++;
            if (idx_ >= count_) {
                if (!loop_) { done_ = true; break; }
                idx_ = 0;
            }
            beginNote();
        }

        // Render the shorter of "rest of this note" and "rest of the caller's block".
        uint32_t run = noteFrames_ - notePos_;
        if (run > (uint32_t)(frames - produced)) run = (uint32_t)(frames - produced);

        if (rest_ || phaseInc_ == 0) {
            memset(dst + produced * 2, 0, run * 2 * sizeof(int16_t));
            notePos_ += run;
            produced += (int)run;
            continue;
        }

        for (uint32_t i = 0; i < run; i++) {
            // --- envelope (linear; an exponential tail is not worth a expf per sample here)
            float amp;
            if (notePos_ < atkFrames_)                    amp = (float)notePos_ / (float)atkFrames_;
            else if (noteFrames_ - notePos_ < relFrames_) amp = (float)(noteFrames_ - notePos_) / (float)relFrames_;
            else                                          amp = 1.0f;
            amp *= baseAmp;

            // --- oscillator
            int32_t v;
            switch (timbre_.wave) {
                case Wave::Square:
                    v = (phase_ < 0x80000000u) ? 32767 : -32768;
                    break;
                case Wave::Pulse:
                    v = (phase_ < dutyThresh) ? 32767 : -32768;
                    break;
                case Wave::Triangle: {
                    const uint32_t p = phase_ >> 16;                   // 0..65535
                    v = (p < 32768) ? (int32_t)p * 2 - 32768
                                    : 32767 - ((int32_t)(p - 32768) * 2);
                    break;
                }
                case Wave::Saw:
                    v = (int32_t)(phase_ >> 16) - 32768;
                    break;
                case Wave::Sine:
                    v = s_sine[phase_ >> 24];
                    break;
                case Wave::Noise:
                default: {
                    // 15-bit Galois LFSR clocked once per oscillator cycle, so "frequency"
                    // controls the grain of the noise the way it does on a chip sound
                    // generator -- low notes rumble, high notes hiss.
                    const uint32_t prev = phase_;
                    if (phase_ + phaseInc_ < prev) {                   // wrapped
                        lfsr_ = (lfsr_ >> 1) ^ ((lfsr_ & 1u) ? 0x6000u : 0u);
                        noiseVal_ = (lfsr_ & 1u) ? 32767 : -32768;
                    }
                    v = noiseVal_;
                    break;
                }
            }

            const int16_t out = (int16_t)((float)v * amp);
            dst[(produced + (int)i) * 2]     = out;
            dst[(produced + (int)i) * 2 + 1] = out;    // mono; the mixer applies pan

            // --- advance
            uint32_t inc = phaseInc_;
            if (vibDepth_ > 0.0f) {
                const float lfo = (float)s_sine[vibPhase_ >> 24] * (1.0f / 32768.0f);
                inc = (uint32_t)((float)inc * (1.0f + vibDepth_ * lfo));
                vibPhase_ += vibInc_;
            }
            phase_ += inc;
            if (incMul_ != 1.0f) phaseInc_ = (uint32_t)((float)phaseInc_ * incMul_);
            notePos_++;
        }
        produced += (int)run;
    }

    return produced;
}

}   // namespace audio
