#pragma once
#include <cstdint>

// 4-channel game audio: a small software mixer that owns the I2S DAC outright.
//
// WHY NOT esp-audio-player (which this replaced): it decodes ONE file at a time straight
// into I2S. A game needs a chirp to land on top of the music that is already playing, so
// "one stream" is not a limitation we can work around at the call site -- it is the wrong
// shape. This engine instead mixes a small fixed number of voices into one I2S stream.
//
// FOUR VOICES, on purpose. Voice count is what costs CPU (every voice is resampled and
// accumulated for every output frame), and four is the classic budget: one for music plus
// three simultaneous effects is more than a virtual pet ever stacks. Raising kVoices is a
// one-line change and costs ~0.3% of a core each -- the reason not to is that voices past
// four are inaudible mush on a speaker this size, not that we cannot afford them.
//
// TWO WAYS TO SOURCE A VOICE, both reachable from the same play() call; which one a sound
// uses is a property of the SOUND (see bank.hpp), not of the call site:
//
//   sample  Decoded once, in full, into PSRAM and kept there. Playback is a pointer walk,
//           so it is instant and repeatable -- the right answer for effects, which must
//           fire on the exact frame the pet is tapped and may fire ten times a second.
//   stream  Decoded incrementally from the file while it plays, through a ring buffer.
//           Costs ~16 KB of RAM instead of the whole clip -- the right answer for music
//           and anything long, which cannot fit in RAM and only ever plays once at a time.
//
// THE ONE INVARIANT THAT MAKES THIS WORK: the mixer task never touches the filesystem.
// Opening a file on this hardware costs 7-12 ms (see pakfs.hpp), which is two whole audio
// blocks -- if the mixer could block on FAT, every lazy sprite load in the game would be
// audible as a dropout. So all slow work (FAT opens, decoding, sample loading) happens on
// a separate streamer task, and the mixer only ever reads RAM. play() likewise never
// blocks the caller on I/O: it reserves a voice, returns a handle immediately, and lets
// the streamer fill it in. A cached sample skips that hop and starts on the same frame.
//
// Formats live behind the Decoder seam in decoder.hpp (WAV and MP3 today). "Sounds" that
// are synthesised rather than decoded -- parametric blips and RTTTL melodies -- are a
// third voice kind with no file at all; see synth.hpp for why a pet game wants those.
//
// Threading: init() spawns both tasks on core 0, alongside the drivers, because core 1 is
// the 60 fps game loop and must not be shared with a task that wakes every 5.8 ms. Every
// entry point here is safe to call from the game loop.

class SaveStore;   // sim/save.hpp -- volume settings persist through it

namespace audio {

// --- fixed mixer format ------------------------------------------------------------------
//
// 44100 stereo, and everything is resampled to it. 44.1 kHz specifically because it is what
// MP3 music is nearly always encoded at, so music passes through the resampler at a 1:1 step
// and stays bit-exact; a lower mix rate would decimate every song without a lowpass and alias
// audibly. It buys nothing to go lower anyway -- mixing is ~2% of a core here, and the actual
// cost of audio on this board is MP3 decode, which the mix rate does not affect.
constexpr int kMixRate  = 44100;
constexpr int kMixChans = 2;

// Frames per mix block. 256 frames = 5.8 ms of latency and a wakeup every 5.8 ms; small
// enough that a tap sounds immediate, large enough that per-block overhead is noise.
constexpr int kMixFrames = 256;

constexpr int kVoices = 4;    // see the header comment on why four

// Concurrent streaming decoders. Each costs a ring buffer plus decoder state (~40 KB for
// MP3), and nothing in the game plays two long clips at once, so two is generous: music,
// plus one long effect over the top of it.
constexpr int kStreams = 2;

// Voice 0 is where music lands, so a burst of effects can never steal the song out from
// under itself. Effects prefer voices 1..3 and only fall back to voice 0 when no music is
// playing -- otherwise a game with no music would be stuck with three channels out of four.
constexpr int kMusicVoice = 0;

// --- mixing buses ------------------------------------------------------------------------
// Every voice belongs to exactly one bus, and a bus is just a gain the player controls.
// Separate music and effects sliders because they fail differently: music is the thing you
// turn off on a bus, effects are the thing you keep because they carry information.
//
// Ui is a routing label, not a third slider: it resolves to the effects gain wherever gain is
// read, so there is nothing to keep in step. It stays a distinct bus value because sounds.json
// tags sounds with it, and because a UI slider of its own is a plausible thing to want later.
enum class Bus : uint8_t { Sfx, Music, Ui, Count };

// --- creature voices ---------------------------------------------------------------------
//
// Some sounds belong to the DEVICE (a button, a countdown) and some belong to the CREATURE
// making them (a cry, a chew, a refusal). The second kind should not be the same noise for
// every species, and the sound bank marks them: `"voiced": true` in sounds.json.
//
// A voiced sound resolves through the voice of whoever is speaking, in two layers, and the
// cheap layer carries most of the weight:
//
//   pitch    One scalar per creature. A synthesised cry played slower is also lower and
//            longer, so this alone makes a roster of hundreds sound like hundreds of
//            creatures, for zero bytes of new audio and no authoring at all.
//   prefix   Ids tried strongest-first -- `<species>_<id>`, then `<family>_<id>`, then the
//            bare `<id>`. So a handful of authored families cover a whole roster, and one
//            special creature can still be given a cry of its own. A species that defines
//            neither sounds exactly like the base game, with no check at any call site.
//
// The species-exact entry is the one thing the pitch scalar is NOT applied to: naming a sound
// after a species is an author saying "this is the sound", and pitching what they already
// tuned would be second-guessing them. A family entry is a shared starting point, so it keeps
// the per-creature scalar -- which is exactly what makes one authored family sound like N
// different creatures instead of N copies of one.
//
// Resolution happens inside play(), not at the call site, which is why every sfx::play() in
// the game became species-aware without changing: the call site names the EVENT, and who is
// speaking is context that belongs with the pet, not with the code that noticed the event.
struct VoiceProfile {
    const char* species = nullptr;   // creature id: names its sound set, and its bank prefix
    const char* family  = nullptr;   // shared authored family ("beast"); may be null
    // The creature's own folder. `<dir>/sounds/` is scanned into a sound set the first time
    // this creature speaks and dropped when it stops (see bank.hpp) -- which is what lets a
    // roster carry unlimited per-creature audio without any of it being resident at boot.
    // Null or empty simply means "no folder of its own", and everything still works.
    const char* dir     = nullptr;
    float       pitch   = 1.0f;      // playback-rate scalar for this creature
};

// The voice used when a play() does not name one: the player's own pet. Set by Pet whenever
// its species changes. The strings are COPIED, so the caller may pass pointers into a
// registry that a mod-pack reload will rebuild.
void set_voice(const VoiceProfile& v);
void clear_voice();                  // back to the unvoiced base sounds

// Start loading a creature's sound set without making it the default voice. For a speaker the
// game knows is about to arrive -- a battle opponent -- so its first cry is already its own
// rather than the shared fallback the set would otherwise be fetched during. Idempotent and
// safe to call every frame; it does nothing once the set is resident.
void request_voice(const char* species, const char* dir);

// --- playback parameters -------------------------------------------------------------
//
// These MODIFY the sound's own settings rather than replacing them. Which bus a sound plays
// on, how loud it is relative to its neighbours and how important it is are properties of
// the SOUND (see bank.hpp) -- putting them here would mean a mod could not rebalance an
// effect without the call site agreeing, which defeats the point of a data-driven bank.
// What a call site legitimately knows is context: play this one quieter, off to the left,
// pitched up because the pet is excited.
struct Params {
    float gain  = 1.0f;    // multiplied with the sound's gain
    float pan   = 0.0f;    // added to the sound's pan, then clamped to -1..+1
    float pitch = 1.0f;    // multiplied with the sound's pitch (also shifts duration)
    bool  loop  = false;   // force looping even if the sound is not marked loop

    // Who is speaking, for a voiced sound (see VoiceProfile). Null means the pet -- which is
    // right nearly everywhere, because nearly everywhere there is only one creature. Battle
    // is the exception and passes the other side's voice explicitly: an enemy taking a hit in
    // the player's own voice reads as the PLAYER being hit.
    //
    // Read synchronously inside play() and not retained, so a stack temporary is fine.
    const VoiceProfile* voice = nullptr;
};

// Identifies a playing voice. Carries a generation counter alongside the voice index, so a
// handle kept across the end of its sound refers to nothing rather than to whatever sound
// reused that voice next -- stop(staleHandle) is a no-op, not a stranger's sound cut short.
using Handle = uint32_t;
constexpr Handle kNoHandle = 0;

// --- lifecycle -------------------------------------------------------------------------

// Bring up I2S and start the mixer + streamer tasks. Safe to call before the sound bank is
// loaded: the engine simply has nothing to play until bank_load() runs. Returns false if
// I2S could not be claimed, after which every call below is a silent no-op -- audio failing
// must never be able to take the game down with it.
bool init();

// Stop everything and park the DAC at silence. Call before deep sleep or power-off: cutting
// the I2S clock mid-waveform leaves a step on the DAC output that the speaker clicks at.
void shutdown();

// Screen-off light sleep. Pauses the mixer and disables I2S (the clock alone is worth a few
// mA), holding voice state so resume() picks the music back up where it left off. The game
// keeps simulating while suspended -- there is just nobody to hear it.
void suspend();
void resume();

bool ready();        // init() succeeded and the engine is live

// --- playing sounds ---------------------------------------------------------------------

// Play a sound from the bank by id (see sfx.hpp for the ids the game itself uses, and
// bank.hpp for where the definitions come from). Returns a handle for later control, or
// kNoHandle if the id is unknown, audio is off, or every voice is busy with something more
// important. Never blocks on I/O.
//
// Also kNoHandle when the sound could not be heard anyway: muted, a bus or master at zero, or
// the screen off (which parks the mixer, so a sound started then would not play -- it would
// wait, frozen at its first sample, and fire when the screen came back). Callers already treat
// sound as fire-and-forget, so this is invisible to them and saves a voice, a decoder and an
// SD read each time.
Handle play(const char* id);
Handle play(const char* id, const Params& p);

// Convenience for the overwhelmingly common case: fire and forget at a given volume.
Handle play(const char* id, float gain);

// --- music -------------------------------------------------------------------------------
// Music is just a voice on the Music bus pinned to kMusicVoice, with crossfade handling.
// Playing the id that is already playing is a no-op, so a scene may call this every frame
// on entry without restarting the song. Asking for the track that is currently FADING OUT
// (a scene change reversed before the fade finished) cancels the fade and keeps it playing,
// rather than restarting it or letting the queued replacement land.
//
// Whether the song loops is the SOUND's business ("loop" in sounds.json, true by default on
// the music bus), so a one-shot sting can be played through here too.
//
// While the engine is muted or a slider is at zero, the track is remembered rather than
// played: decoding a song nobody can hear costs the same CPU and SD traffic as one they can.
// It starts for real when the volume comes back.
bool music(const char* id, float fadeSecs = 0.6f);
void music_stop(float fadeSecs = 0.6f);
const char* music_playing();      // current music id, or "" if none (safe from any task)

// --- voice control -----------------------------------------------------------------------
void stop(Handle h, float fadeSecs = 0.0f);
void stop_bus(Bus b, float fadeSecs = 0.0f);
void stop_all(float fadeSecs = 0.0f);
bool playing(Handle h);
void set_gain(Handle h, float gain);
void set_pitch(Handle h, float pitch);

// --- volume ------------------------------------------------------------------------------
// Bus and master are SLIDER POSITIONS in 0..1, which the UI shows as percentages. They are
// not linear gains: perceived loudness goes roughly as the square of a linear gain, so the
// engine squares them on the way into the mix and a slider's travel changes what you hear
// evenly along its length. The curve lives in the mixer rather than in the settings scene so
// that a position restored from NVS, set by a mod, or set by a future ducking feature all
// mean the same loudness. A voice's own gain (Params::gain, and the sound's) IS linear, and
// composes with them: it is a mix-down number, not something a person is looking at.
void  set_bus_gain(Bus b, float g);
float bus_gain(Bus b);
void  set_master(float g);
float master();
void  set_muted(bool m);
bool  muted();

// Load/persist the four volume settings (master, music, sfx, mute) as NVS flags. Kept here
// rather than in the settings scene so the boot path and the UI cannot disagree about key
// names, and so audio owns its own defaults.
void settings_load(const SaveStore& save);
void settings_store(const SaveStore& save);

// --- diagnostics -------------------------------------------------------------------------
// Surfaced by the on-screen debug overlay (Settings > System > Debug info). `underruns` is
// the number of blocks a streaming voice went dry mid-block: the one number that says "the
// SD card could not keep up", which is otherwise very hard to tell apart from a bad file.
struct Stats {
    uint8_t  activeVoices;
    uint8_t  activeStreams;
    uint16_t underruns;
    uint16_t mixPeakUs;      // longest single mix block since the last resetPeak read
};
// `resetPeak` starts a new peak window. Exactly one caller should ask -- the peak is a
// max-since-last-clear, so a second reader that also cleared would erase the spikes the first
// one exists to catch (which is what a debug overlay redrawing at 60 fps did to the periodic
// log). The periodic log owns the window; everyone else reads it.
Stats stats(bool resetPeak = false);

}   // namespace audio
