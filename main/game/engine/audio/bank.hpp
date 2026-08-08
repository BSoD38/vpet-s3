#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "audio.hpp"
#include "synth.hpp"

// The sound bank: named sounds, loaded from data, overridable by mods.
//
// Game code never names a FILE or a frequency -- it calls audio::play("pet_happy") and the
// bank decides what that means. That indirection is the entire point: a mod pack can retune
// the whole game's voice by shipping one sounds.json, and a species could later carry its own
// cry without a line of code changing. Same contract as foods and creatures, so the scan
// order and override rules below are deliberately identical to FoodRegistry::loadAll().
//
// Sounds come from four places, weakest to strongest:
//   /gamedata/sounds/     the base game's own set (packed into base.pak at build time)
//   /pakN/sounds/         mounted mod packs, later pack wins
//   /sdcard/sounds/       loose files on the card -- the final word, for quick iteration
//
// Two ways to define one at each root:
//   sounds.json    an array of sound objects. One FAT open for the whole set, which is why
//                  the base game uses it (an open costs 7-12 ms here; see pakfs.hpp).
//   loose files    any *.wav / *.mp3 in the folder registers itself under its filename stem.
//                  Costs a directory walk, but it means dropping `hatch.wav` on the card and
//                  having it play is the whole install procedure.

namespace audio {

enum class SoundKind : uint8_t {
    Tone,      // synthesised blip; no file
    Melody,    // synthesised RTTTL sequence; no file
    Sample,    // decoded once into PSRAM, played from RAM
    Stream,    // decoded incrementally while playing
};

// A fully decoded clip held in PSRAM. Shared by every voice playing that sound, and never
// freed while the engine runs -- the cache is bounded at load time instead (see SAMPLE_BUDGET
// in bank.cpp), which avoids having to refcount buffers against a real-time mixer.
struct MemSample {
    int16_t* pcm    = nullptr;   // interleaved, `chans` per frame
    uint32_t frames = 0;
    uint32_t rate   = 0;
    uint8_t  chans  = 1;
};

struct Sound {
    // 48 and not 24 because of creature voices (see VoiceProfile in audio.hpp): a voiced id
    // is looked up as `<species>_<id>`, and a creature id is itself up to 23 characters. At
    // 24 those composites would silently truncate into each other -- `metalgreymon_pet_sad`
    // and `metalgreymon_pet_sick` becoming the same string is a sound bug with no symptom
    // except the wrong noise. 24 bytes more across the table is ~6 KB of PSRAM.
    char      id[48]   = {};
    SoundKind kind     = SoundKind::Tone;
    Bus       bus      = Bus::Sfx;

    // Does this sound belong to the creature making it, rather than to the device? Only a
    // voiced sound is resolved through the speaking creature's voice and pitched by it; a
    // button, a countdown or a fanfare must sound the same whoever is on screen.
    //
    // Off by default, so the flag is a deliberate act by whoever wrote the sound. The BASE
    // entry is what declares it -- `<species>_pet_happy` is only ever reached by resolving
    // `pet_happy`, so a mod that overrides a base sound and drops the flag turns voicing off
    // for that id, which is the correct reading of having replaced the whole entry.
    bool      voiced   = false;

    // Defaults applied to a play() that does not override them. Having them on the SOUND is
    // what lets a modder rebalance a too-loud effect without touching game code.
    float   gain     = 1.0f;
    float   pitch    = 1.0f;
    float   pan      = 0.0f;
    bool    loop     = false;
    uint8_t priority = 128;

    // Random pitch spread, as a +/- fraction. Small values (0.05) are the cheapest possible
    // fix for machine-gun repetition: a pet tapped ten times in a row should not produce ten
    // bit-identical chirps, and varying pitch is how every game has solved that for decades.
    float pitchVar = 0.0f;

    Tone   tone{};        // Kind::Tone
    Melody melody{};      // Kind::Melody

    char path[160] = {};  // Kind::Sample / Kind::Stream -- absolute, root included
    // Decode this one into the cache during the background warm-up rather than on first play.
    // OFF by default: a first play already falls back to a queued load, so preloading is an
    // optimisation for sounds that must be instant the very first time -- and defaulting it ON
    // meant a sample-heavy mod pack spent boot decoding its whole set into PSRAM, over the same
    // SD bus the game is loading sprites from, for sounds that may never fire.
    bool preload   = false;

    // Which scan root registered this entry (0 = gamedata, then one per pak, then SD last).
    // Needed to get the override rules right: a loose file must NOT clobber the manifest of
    // its OWN root, but it must still beat an entry from a weaker root.
    uint8_t srcRoot = 0;

    // Set by the streamer task once decoded; read by play() on the game thread to decide
    // whether the sound can start on this frame or has to go through the load queue.
    std::atomic<MemSample*> mem{nullptr};
    bool loadFailed = false;   // decode already failed once; do not keep retrying every play

    Sound() = default;
    // Sound lives in a fixed array that is built by assignment during the scan, and
    // std::atomic is not copyable, so spell the copy out. Only the scan uses it, and only
    // before any voice can reference the entry.
    Sound(const Sound& o) { *this = o; }
    Sound& operator=(const Sound& o)
    {
        if (this == &o) return *this;
        memcpy(id, o.id, sizeof id);
        kind = o.kind; bus = o.bus; voiced = o.voiced;
        gain = o.gain; pitch = o.pitch; pan = o.pan; loop = o.loop; priority = o.priority;
        pitchVar = o.pitchVar; tone = o.tone; melody = o.melody;
        memcpy(path, o.path, sizeof path);
        preload = o.preload; srcRoot = o.srcRoot;
        mem.store(o.mem.load(std::memory_order_relaxed), std::memory_order_relaxed);
        loadFailed = o.loadFailed;
        return *this;
    }
};

// --- sound sets ------------------------------------------------------------------------------
//
// The global bank above is EAGER: every entry is parsed at boot and held resident for the whole
// session. That is right for the sounds the device itself makes and for shared voice families,
// which are few and always potentially needed. It is wrong for a creature's own sounds, and the
// reason is scale rather than taste:
//
//   * A roster is unbounded. Hundreds of creatures x ~13 voiced ids is thousands of entries
//     that would all have to fit under one cap and all be parsed before the game starts.
//   * A flat manifest cannot even describe it -- sounds.json is read under a 64 KB cap, which
//     is a few hundred entries, not a few thousand.
//   * It is nearly all waste. At most TWO creatures can be making a noise at any moment (the
//     pet, plus an opponent in battle), so resident sound for the other 736 buys nothing.
//
// So a creature's sounds live in its OWN folder -- `<creature dir>/sounds/` -- and are loaded
// when that creature starts speaking. Same shape as the sprite cache in sim/creatures.hpp,
// which solved the identical problem for art. A set is scanned by the very same manifest and
// loose-file code as the global bank, so a creature folder supports everything sounds.json
// does, and ids inside it need no prefix: `pet_happy` in agumon's folder IS agumon's pet_happy.
//
// Dropping `pet_happy.wav` into `creatures/agumon/sounds/` is the entire install procedure --
// which is the same promise loose files already make for the game's own sounds.
//
// Two slots would do (pet + opponent); three means a battle can start without evicting the
// pet's set first. At SOUNDSET_MAX entries of ~332 bytes, the whole feature costs ~48 KB of
// PSRAM no matter how big the roster gets.
static constexpr int SOUNDSET_SLOTS = 3;
static constexpr int SOUNDSET_MAX   = 48;

// Answers "is any voice still playing from set `slot`?". Installed by the mixer, which is the
// only thing that knows; called by the eviction path. Kept as a hook so bank.cpp needs no
// knowledge of voices and mixer.cpp needs none of tables.
using SetInUseFn = bool (*)(int slot);
void soundset_set_inuse_hook(SetInUseFn fn);

// GAME THREAD. Make `key`'s folder resident, evicting the least recently used idle set if every
// slot is taken. Returns true if it queued work, in which case wake the streamer.
//
// Failure is ordinary, not an error: the creature simply speaks with the global bank's sounds,
// exactly as one that ships no audio of its own does. Nothing downstream needs to check.
bool soundset_acquire(const char* key, const char* dir);

// Resident slot for a creature id, or -1. Only ever true of a fully loaded set.
int  soundset_resident(const char* key);

// Index of `id` within a resident set, or -1.
int  soundset_find(int slot, const char* id);

// STREAMER THREAD. Perform at most one pending load or eviction. True if it did work.
bool soundset_service();

// Drop every resident set. Called by bank_load(), because a rescan means the mounted packs
// changed and a set holds resolved paths into them. Requires that no voice be playing from a
// set -- true of the one mid-session caller, which shuts audio down first.
void soundset_reset_all();

// Read an entry from a set (`set` < 0 means the global bank). Returns null for a set that is
// not READY, so a caller can never be handed an entry whose melody or cached PCM is mid-free.
const Sound* sound_at(int set, int i);

// Scan every root and build the registry. Call once from App::init(), AFTER the mod packs are
// mounted (their roots are scan targets) and after gamedata_json_use_psram().
void bank_load();

// True once bank_load() has finished. The streamer's warm-cache walk waits for it: the scan
// builds entries IN PLACE (a later root's override copy-assigns over an earlier one), so a
// decode running against an entry mid-rewrite can publish the old file's audio under the new
// id, or leak the buffer it just cached. bank_load() runs on the game task while the streamer
// is already up, so the two genuinely overlap without this.
bool         bank_ready();

int          bank_count();
// Case-INSENSITIVE, like every other name at this boundary (extensions, bus and kind keys).
// FAT preserves the case a file was written with, so `BGM_Home.mp3` dropped on the card has to
// find and override `bgm_home` -- which is the headline reason loose files exist.
int          bank_find(const char* id);      // index, or -1
const Sound* bank_at(int i);                 // nullptr out of range

// Decode a Kind::Sample entry into PSRAM and publish it. ONLY the streamer task may call
// this: it opens files and can take tens of milliseconds. Returns false if the sound could
// not be loaded, in which case the caller should stream it instead. `set` < 0 = global bank.
bool bank_load_sample(int set, int idx);

// Total bytes currently held in the sample cache. Reported by the streamer once its warm-up
// walk finishes, so a mod that fills the cache says so in the log rather than silently
// degrading every effect it defines into a stream.
uint32_t bank_cache_bytes();

}   // namespace audio
