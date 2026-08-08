#include "bank.hpp"
#include "decoder.hpp"
#include "sim/gamedata.hpp"     // gd_read_file / gd_num / gd_str / gd_bool, GAMEDATA_ROOT
#include "engine/pakfs.hpp"     // mounted mod packs are extra scan roots
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <new>

namespace audio {

static const char* TAG = "AUD/BANK";

static const char* SD_ROOT = "/sdcard/sounds";

// 256 named sounds in the GLOBAL bank -- device sounds, the base game, and shared voice
// families. It does not bound how much audio a roster can carry: a creature's own sounds live
// in its folder and load on demand (see SoundSet in bank.hpp), so what sits here is a small
// fixed set plus however many families a pack defines, none of which grows with the number of
// creatures installed. 256 is roughly four times what the base game defines, and the array
// lives in PSRAM (see bank_load), so it costs ~85 KB of the plentiful memory and none of the
// scarce internal heap, for something only read at play() time.
static constexpr int  MAX_SOUNDS   = 256;
static constexpr long MAX_JSON     = 65536;

// Ceiling on decoded audio held in PSRAM. Generous -- 512 KB is about 6 seconds of 44.1 kHz
// stereo, or a couple of minutes of the short mono effects this is actually for. Anything
// that does not fit is STREAMED rather than dropped, so exceeding the budget costs a little
// latency and an SD read, never a missing sound.
static constexpr uint32_t SAMPLE_BUDGET = 512 * 1024;

// One clip's own ceiling. A sound this long is music by any reasonable reading, and music
// belongs on the streaming path whatever the manifest claims.
static constexpr uint32_t MAX_SAMPLE_BYTES = 192 * 1024;

// A table of sounds. There are two kinds: the global bank (device sounds, the base game, voice
// families -- eager, loaded once at boot) and one per resident creature sound set (lazy, see
// SoundSet below). Parameterising the scan over a table is what lets the exact same manifest
// and loose-file code fill both, so a creature folder supports everything sounds.json does with
// no second implementation to keep in step.
struct Table {
    Sound* list  = nullptr;
    int    count = 0;
    int    cap   = 0;
};

static Table    s_bank;                  // the global bank
static uint32_t s_cacheBytes = 0;
static std::atomic<bool> s_bankReady{false};

// --- sound sets -----------------------------------------------------------------------------
//
// One resident creature's own sounds, loaded from its folder on demand. See bank.hpp for why
// this exists rather than everything living in the global bank.
//
// STATE MACHINE, and which task may drive each edge. Only these two tasks touch a slot, and
// the split is what makes the whole thing safe without a refcount:
//
//   Empty  --(game)-->  Pending  --(streamer)-->  Ready  --(game)-->  Evicting
//     ^                                                                   |
//     +-------------------------(streamer)--------------------------------+
//
// The GAME thread claims and releases slots (it is the only thing that starts voices, so it is
// the only thing that can decide nobody is using one). The STREAMER does the slow work -- the
// directory scan, the frees -- because it owns every FAT open in this engine.
enum : uint8_t { SET_EMPTY = 0, SET_PENDING, SET_READY, SET_EVICTING };

struct SoundSet {
    Table    tab;
    char     key[24]  = {};      // creature id currently resident
    char     want[24] = {};      // creature id requested but not loaded yet
    char     wantDir[128] = {};  // its folder; `<wantDir>/sounds` is the scan root
    uint32_t lastUse  = 0;       // LRU clock, bumped on every acquire
    std::atomic<uint8_t> state{SET_EMPTY};
};

static SoundSet s_sets[SOUNDSET_SLOTS];
static uint32_t s_setClock = 0;
static SetInUseFn s_inUse = nullptr;

// A set index addresses one of the tables; anything out of range means the global bank, so a
// caller that never heard of sound sets keeps working unchanged.
static Table* table_of(int set)
{
    if (set < 0 || set >= SOUNDSET_SLOTS) return &s_bank;
    return &s_sets[set].tab;
}

int          bank_count() { return s_bank.count; }
uint32_t     bank_cache_bytes() { return s_cacheBytes; }
bool         bank_ready() { return s_bankReady.load(std::memory_order_acquire); }

static int table_find(const Table& t, const char* id)
{
    if (!t.list || !id) return -1;
    // Case-insensitive: see bank.hpp. Everything else at this boundary already is, and FAT
    // hands back whatever case the file was written with.
    for (int i = 0; i < t.count; i++)
        if (strcasecmp(t.list[i].id, id) == 0) return i;
    return -1;
}

int          bank_find(const char* id) { return table_find(s_bank, id); }
const Sound* bank_at(int i)
{
    return (s_bank.list && i >= 0 && i < s_bank.count) ? &s_bank.list[i] : nullptr;
}

// Release everything one entry owns. Shared by the override path (a later root replacing an
// earlier definition) and by set eviction, which have exactly the same job to do.
static void entry_release(Sound& s)
{
    melody_free(s.melody);
    // ...and the decoded PCM, if this entry had already been cached. Whoever calls this is the
    // last reference to the buffer and to the cache budget it is holding.
    if (MemSample* ms = s.mem.exchange(nullptr, std::memory_order_acq_rel)) {
        const uint32_t bytes = ms->frames * ms->chans * sizeof(int16_t);
        s_cacheBytes = (s_cacheBytes > bytes) ? s_cacheBytes - bytes : 0;
        free(ms->pcm);
        free(ms);
    }
}

static int upsert(Table& t, const char* id)
{
    const int i = table_find(t, id);
    if (i >= 0) {
        // A later root is overriding this sound. Drop anything the previous definition
        // allocated, or a mod that replaces a melody leaks its note array for the session.
        entry_release(t.list[i]);
        return i;
    }
    if (t.count >= t.cap) return -1;
    return t.count++;
}

// --- JSON -> Sound ------------------------------------------------------------------------

static Wave parse_wave(cJSON* o)
{
    char w[12];
    gd_str(o, "wave", w, sizeof w, "square");
    if (strcasecmp(w, "pulse") == 0)    return Wave::Pulse;
    if (strcasecmp(w, "tri") == 0 || strcasecmp(w, "triangle") == 0) return Wave::Triangle;
    if (strcasecmp(w, "saw") == 0)      return Wave::Saw;
    if (strcasecmp(w, "noise") == 0)    return Wave::Noise;
    if (strcasecmp(w, "sine") == 0)     return Wave::Sine;
    return Wave::Square;
}

static Bus parse_bus(cJSON* o, Bus def)
{
    char b[10];
    gd_str(o, "bus", b, sizeof b, "");
    if (strcasecmp(b, "music") == 0) return Bus::Music;
    if (strcasecmp(b, "ui") == 0)    return Bus::Ui;
    if (strcasecmp(b, "sfx") == 0)   return Bus::Sfx;
    return def;
}

static Timbre parse_timbre(cJSON* o)
{
    Timbre t;
    t.wave     = parse_wave(o);
    t.duty     = (float)gd_num(o, "duty",     0.5);
    t.attack   = (float)gd_num(o, "attack",   0.003);
    t.release  = (float)gd_num(o, "release",  0.030);
    t.slide    = (float)gd_num(o, "slide",    1.0);
    t.vibHz    = (float)gd_num(o, "vibHz",    0.0);
    t.vibCents = (float)gd_num(o, "vibCents", 0.0);
    t.vol      = (float)gd_num(o, "vol",      0.6);
    return t;
}

// Turn one JSON object into a Sound. Returns false only when there is nothing playable in it.
static bool parse_entry(cJSON* o, const char* root, Sound& s)
{
    s = Sound();
    gd_str(o, "id", s.id, sizeof s.id, "");

    char file[128];
    gd_str(o, "file", file, sizeof file, "");
    cJSON* rt = cJSON_GetObjectItem(o, "rtttl");
    const bool hasRtttl = cJSON_IsString(rt) && rt->valuestring && rt->valuestring[0];

    // Kind is inferred from what the entry actually contains, so the common cases need no
    // "kind" field at all. The explicit key is for the one ambiguous choice: a file can be
    // either a cached sample or a stream, and only the author knows which they meant.
    char kind[10];
    gd_str(o, "kind", kind, sizeof kind, "");
    if (strcasecmp(kind, "tone") == 0)         s.kind = SoundKind::Tone;
    else if (strcasecmp(kind, "melody") == 0)  s.kind = SoundKind::Melody;
    else if (strcasecmp(kind, "sample") == 0)  s.kind = SoundKind::Sample;
    else if (strcasecmp(kind, "stream") == 0)  s.kind = SoundKind::Stream;
    else if (hasRtttl)                         s.kind = SoundKind::Melody;
    else if (file[0]) {
        // Default by extension, because it tracks intent almost perfectly in practice:
        // people reach for MP3 when the clip is long (music) and WAV when it is short.
        const char* dot = strrchr(file, '.');
        s.kind = (dot && strcasecmp(dot, ".mp3") == 0) ? SoundKind::Stream : SoundKind::Sample;
    } else {
        s.kind = SoundKind::Tone;
    }

    s.bus      = parse_bus(o, s.kind == SoundKind::Stream ? Bus::Music : Bus::Sfx);
    s.gain     = (float)gd_num(o, "gain",  1.0);
    s.pitch    = (float)gd_num(o, "pitch", 1.0);
    s.pan      = (float)gd_num(o, "pan",   0.0);
    s.loop     = gd_bool(o, "loop", s.bus == Bus::Music);
    s.priority = (uint8_t)gd_num(o, "priority", s.bus == Bus::Music ? 200 : 128);
    s.pitchVar = (float)gd_num(o, "pitchVar", 0.0);
    s.preload  = gd_bool(o, "preload", false);   // opt in; see Sound::preload
    s.voiced   = gd_bool(o, "voiced",  false);   // opt in; see Sound::voiced

    switch (s.kind) {
        case SoundKind::Tone:
            s.tone.timbre = parse_timbre(o);
            s.tone.freq   = (float)gd_num(o, "freq", 880.0);
            s.tone.ms     = (uint16_t)gd_num(o, "ms", 70);
            s.tone.repeat = (uint8_t)gd_num(o, "repeat", 1);
            s.tone.gapMs  = (uint16_t)gd_num(o, "gapMs", 30);
            s.tone.step   = (float)gd_num(o, "step", 1.0);
            break;

        case SoundKind::Melody:
            if (!hasRtttl) return false;
            s.melody.timbre = parse_timbre(o);
            if (!rtttl_parse(rt->valuestring, s.melody)) {
                ESP_LOGW(TAG, "'%s': unparseable RTTTL", s.id);
                return false;
            }
            break;

        default:
            if (!file[0]) return false;
            // A leading slash means the author gave an absolute path (another partition, or
            // somewhere else on the card); otherwise it is relative to THE DIRECTORY THIS
            // MANIFEST IS IN -- i.e. the sounds/ dir, not the mod root. So a file sitting next
            // to sounds.json is just "theme.xm", NOT "sounds/theme.xm". Same convention as a
            // creature's sprite being relative to its own folder, and it is what keeps a pack
            // self-contained and relocatable. decoder_open() logs loudly when the result does
            // not exist, because this is the field people get wrong.
            if (file[0] == '/') snprintf(s.path, sizeof s.path, "%s", file);
            else                snprintf(s.path, sizeof s.path, "%s/%s", root, file);
            // A tracker module is a generator, not a finite clip: "decode it all into the
            // sample cache" has no end condition and would fill PSRAM until it hit the cap.
            // Force streaming whatever the manifest asked for.
            if (decoder_is_module(s.path)) s.kind = SoundKind::Stream;
            break;
    }

    return s.id[0] != '\0';
}

// --- scanning ---------------------------------------------------------------------------

// <root>/sounds.json: an array of sound objects. One FAT open for the whole set.
//
// `srcRoot` is which root the scan is on, stamped onto every entry so scan_loose() can tell
// "the manifest I sit next to defined this" (leave it alone) from "a weaker root defined this"
// (override it). A PARAMETER and not a file-global: sound sets are scanned by the streamer
// while bank_load() may be scanning on the game thread, and a shared cursor between the two
// would silently corrupt the manifest-beats-loose-file rule in whichever scan lost the race.
static void scan_manifest(Table& t, const char* root, uint8_t srcRoot, const char* srcTag)
{
    char path[224];
    snprintf(path, sizeof path, "%s/sounds.json", root);
    char* buf = gd_read_file(path, MAX_JSON);
    if (!buf) return;                       // no manifest at this root is normal

    cJSON* arr = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "not a json array: %s", path);
        cJSON_Delete(arr);
        return;
    }

    int n = 0;
    cJSON* e = nullptr;
    cJSON_ArrayForEach(e, arr) {
        Sound s;
        if (!parse_entry(e, root, s)) continue;
        // JSON has no comments, so an id starting with '_' is the convention for an entry
        // that exists only to hold "_comment" keys. Registering it would put a sound in the
        // bank that nothing can ever play.
        if (s.id[0] == '_') continue;
        const int idx = upsert(t, s.id);
        if (idx < 0) {
            // The local Sound owns a parsed note array that nothing will ever adopt.
            ESP_LOGW(TAG, "table full (%d); dropped '%s' from %s", t.cap, s.id, path);
            melody_free(s.melody);
            continue;
        }
        s.srcRoot = srcRoot;
        t.list[idx] = s;   // ownership of s.melody moves into the registry
        n++;
    }
    cJSON_Delete(arr);
    if (n) ESP_LOGI(TAG, "%s: %d sounds from %s", srcTag, n, path);
}

// Loose *.wav / *.mp3 dropped in the folder, registered under the filename stem. Costs a
// directory walk, and exists so that "copy hatch.wav to <SD>/sounds/" is the entire install
// procedure for someone who has never opened a JSON file.
static void scan_loose(Table& t, const char* root, uint8_t srcRoot, const char* srcTag)
{
    DIR* d = opendir(root);
    if (!d) return;

    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        if (!decoder_handles_extension(ent->d_name)) continue;

        Sound s;
        // The id is the filename with its format marker removed. Normally that is the stem
        // before the last dot -- but a tracker module written the Amiga way puts the format
        // FIRST ("mod.songname", which decoder.cpp accepts on purpose), and taking the stem
        // there would register every such file under the id "mod": the first one would answer
        // to a name no one would guess, and every one after it would be silently skipped as a
        // duplicate.
        const char*  name = ent->d_name;
        size_t       stem;
        if (strncasecmp(name, "mod.", 4) == 0 && name[4]) {
            name += 4;
            stem  = strlen(name);
        } else {
            const char* dot = strrchr(name, '.');
            stem = dot ? (size_t)(dot - name) : strlen(name);
        }
        if (stem == 0 || stem >= sizeof s.id) continue;
        memcpy(s.id, name, stem);
        s.id[stem] = '\0';

        // A manifest entry is the author being explicit; a loose file is a default. So the
        // manifest of THIS root wins -- but only this root's. An entry from a weaker root
        // still gets overridden, or dropping theme.xm on the card could not replace the base
        // game's bgm_home, which is the single most likely thing anyone will try.
        const int prev = table_find(t, s.id);
        if (prev >= 0 && t.list[prev].srcRoot == srcRoot) continue;

        snprintf(s.path, sizeof s.path, "%s/%s", root, ent->d_name);
        // Long-form audio and modules are music and stream; short WAVs are effects and cache.
        const char* ext = strrchr(ent->d_name, '.');
        s.kind = (decoder_is_module(s.path) || (ext && strcasecmp(ext, ".mp3") == 0))
                     ? SoundKind::Stream : SoundKind::Sample;
        s.bus  = (s.kind == SoundKind::Stream) ? Bus::Music : Bus::Sfx;
        s.loop = (s.bus == Bus::Music);
        s.priority = (s.bus == Bus::Music) ? 200 : 128;

        const int idx = upsert(t, s.id);
        if (idx < 0) {
            ESP_LOGW(TAG, "table full (%d); dropped '%s' from %s", t.cap, s.id, root);
            continue;
        }
        s.srcRoot = srcRoot;
        t.list[idx] = s;
        n++;
    }
    closedir(d);
    if (n) ESP_LOGI(TAG, "%s: %d loose sounds in %s", srcTag, n, root);
}

// The base game's data partition is where sounds.json normally lives, so this only fires if
// that partition is unreadable. Deliberately minimal: enough that the device is not silent
// and the audio path is demonstrably alive, not a second copy of the real sound set (which
// would drift from the data one the first time either changed).
static void add_builtins()
{
    struct { const char* id; float freq; uint16_t ms; Wave w; Bus bus; } B[] = {
        { "ui_tap",    1046.5f, 28, Wave::Square,   Bus::Ui  },
        { "ui_back",    523.3f, 40, Wave::Square,   Bus::Ui  },
        { "pet_happy",  880.0f, 90, Wave::Triangle, Bus::Sfx },
        { "pet_sad",    330.0f, 180, Wave::Triangle, Bus::Sfx },
        { "eat",        440.0f, 60, Wave::Pulse,    Bus::Sfx },
        { "error",      196.0f, 140, Wave::Saw,     Bus::Sfx },
    };
    for (auto& b : B) {
        const int idx = upsert(s_bank, b.id);
        if (idx < 0) break;
        Sound s;
        snprintf(s.id, sizeof s.id, "%s", b.id);
        s.kind = SoundKind::Tone;
        s.bus  = b.bus;
        s.tone.freq = b.freq;
        s.tone.ms   = b.ms;
        s.tone.timbre.wave = b.w;
        s_bank.list[idx] = s;
    }
    ESP_LOGW(TAG, "no sound data found; using %d built-in tones", s_bank.count);
}

// Drop every resident sound set. Only for bank_load(), and only valid when nothing can be
// playing from one -- see the call site for why that holds.
static void soundset_free(SoundSet& ss);
void soundset_reset_all()
{
    for (int i = 0; i < SOUNDSET_SLOTS; i++) {
        soundset_free(s_sets[i]);
        s_sets[i].want[0] = '\0';
        s_sets[i].state.store(SET_EMPTY, std::memory_order_release);
    }
}

// Allocate a table's storage once. PSRAM by preference (see MAX_SOUNDS); the internal-heap
// fallback exists so a board with dead PSRAM still makes noise rather than booting silent.
static bool table_alloc(Table& t, int cap)
{
    if (t.list) return true;
    t.list = (Sound*)heap_caps_malloc(sizeof(Sound) * cap, MALLOC_CAP_SPIRAM);
    if (!t.list) t.list = (Sound*)malloc(sizeof(Sound) * cap);
    if (!t.list) return false;
    for (int i = 0; i < cap; i++) new (&t.list[i]) Sound();
    t.cap   = cap;
    t.count = 0;
    return true;
}

void bank_load()
{
    if (!table_alloc(s_bank, MAX_SOUNDS)) {
        ESP_LOGE(TAG, "no memory for the sound bank");
        return;
    }
    // Down for the duration of the scan: the streamer must not decode against entries that are
    // still being written (see bank_ready()).
    s_bankReady.store(false, std::memory_order_release);
    s_bank.count = 0;

    // Every resident sound set is now stale: a rescan means the mounted packs changed, and a
    // set holds resolved paths into those mounts. Safe to drop outright because the only caller
    // that rescans mid-session (a card change) shuts audio down first, so no voice can be
    // playing from one. At boot they are all empty and this does nothing.
    soundset_reset_all();

    gamedata_mount();

    // Same order, and for the same reason, as FoodRegistry::loadAll(): weakest root first,
    // so a mod pack can replace a base-game sound and a loose file on the card wins over
    // everything (which is what makes iterating on a sound bearable -- no repacking).
    // srcRoot increments once per root, so every entry records how strong its source was.
    uint8_t srcRoot = 0;
    char flashRoot[64];
    snprintf(flashRoot, sizeof flashRoot, "%s/sounds", GAMEDATA_ROOT);
    scan_manifest(s_bank, flashRoot, srcRoot, "flash");
    scan_loose(s_bank, flashRoot, srcRoot, "flash");

    for (int i = 0; i < pakfs_count(); i++) {
        srcRoot++;
        char root[40];
        snprintf(root, sizeof root, "%s/sounds", pakfs_root(i));
        scan_manifest(s_bank, root, srcRoot, "pak");
        scan_loose(s_bank, root, srcRoot, "pak");
    }

    srcRoot++;
    scan_manifest(s_bank, SD_ROOT, srcRoot, "sd");
    scan_loose(s_bank, SD_ROOT, srcRoot, "sd");

    if (s_bank.count == 0) add_builtins();
    else ESP_LOGI(TAG, "bank ready: %d sounds", s_bank.count);

    // Publish last: this is what releases the streamer's warm-cache walk.
    s_bankReady.store(true, std::memory_order_release);
}

// --- sound sets: acquire, service, evict ----------------------------------------------------

void soundset_set_inuse_hook(SetInUseFn fn) { s_inUse = fn; }

const Sound* sound_at(int set, int i)
{
    Table* t = table_of(set);
    if (!t->list || i < 0 || i >= t->count) return nullptr;
    // A set being torn down must never hand an entry back: the melody note arrays and cached
    // PCM behind it are exactly what eviction frees.
    if (set >= 0 && set < SOUNDSET_SLOTS &&
        s_sets[set].state.load(std::memory_order_acquire) != SET_READY) return nullptr;
    return &t->list[i];
}

int soundset_find(int set, const char* id)
{
    if (set < 0 || set >= SOUNDSET_SLOTS) return -1;
    if (s_sets[set].state.load(std::memory_order_acquire) != SET_READY) return -1;
    return table_find(s_sets[set].tab, id);
}

int soundset_resident(const char* key)
{
    if (!key || !*key) return -1;
    for (int i = 0; i < SOUNDSET_SLOTS; i++)
        if (s_sets[i].state.load(std::memory_order_acquire) == SET_READY &&
            strcasecmp(s_sets[i].key, key) == 0) return i;
    return -1;
}

// GAME THREAD ONLY. Make `key`'s sounds resident, evicting another set if that is what it
// takes. Returns true if it queued work for the streamer (the caller then wakes it).
//
// Failing is normal and harmless: with every slot busy the creature simply speaks with the
// global bank's sounds, which is the same thing that happens for a creature that ships none.
// Silence is never a possible outcome here, which is why nothing above this checks.
bool soundset_acquire(const char* key, const char* dir)
{
    if (!key || !*key || !dir || !*dir) return false;

    // Already here, or already on its way. Touch the LRU so it is not the next one evicted.
    for (int i = 0; i < SOUNDSET_SLOTS; i++) {
        const uint8_t st = s_sets[i].state.load(std::memory_order_acquire);
        const char* have = (st == SET_READY) ? s_sets[i].key : s_sets[i].want;
        if (st != SET_EMPTY && strcasecmp(have, key) == 0) {
            s_sets[i].lastUse = ++s_setClock;
            return false;
        }
    }

    int  pick    = -1;
    bool evicting = false;
    for (int i = 0; i < SOUNDSET_SLOTS; i++)
        if (s_sets[i].state.load(std::memory_order_acquire) == SET_EMPTY) { pick = i; break; }

    if (pick < 0) {
        evicting = true;
        // Evict the least recently used slot that no voice is still playing from. The in-use
        // test runs under the mixer lock (see the hook in mixer.cpp), and THIS thread is the
        // only one that can start a voice -- so once the answer is "nobody", it stays "nobody"
        // for as long as the slot is not SET_READY, which is the very next thing we do.
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < SOUNDSET_SLOTS; i++) {
            if (s_sets[i].state.load(std::memory_order_acquire) != SET_READY) continue;
            if (s_inUse && s_inUse(i)) continue;
            if (s_sets[i].lastUse < oldest) { oldest = s_sets[i].lastUse; pick = i; }
        }
        if (pick < 0) {
            ESP_LOGD(TAG, "no free sound-set slot for '%s'; using the global bank", key);
            return false;
        }
    }

    SoundSet& ss = s_sets[pick];
    snprintf(ss.want,    sizeof ss.want,    "%s", key);
    snprintf(ss.wantDir, sizeof ss.wantDir, "%s", dir);
    ss.lastUse = ++s_setClock;
    // Ready -> Evicting hands the slot to the streamer to free first; an empty slot is filled
    // directly. Either way it leaves SET_READY here, which is what stops any later play() from
    // referencing entries the streamer is about to free.
    ss.state.store(evicting ? SET_EVICTING : SET_PENDING, std::memory_order_release);
    return true;
}

// STREAMER THREAD ONLY. Free a set's entries. The slot is already out of SET_READY, so nothing
// can be looking at these.
static void soundset_free(SoundSet& ss)
{
    for (int i = 0; i < ss.tab.count; i++) entry_release(ss.tab.list[i]);
    ss.tab.count = 0;
    ss.key[0] = '\0';
}

// STREAMER THREAD ONLY. Do at most one unit of set work. Returns true if it did something, so
// the streamer can keep its fast cadence while sets are settling.
bool soundset_service()
{
    for (int i = 0; i < SOUNDSET_SLOTS; i++) {
        SoundSet& ss = s_sets[i];
        const uint8_t st = ss.state.load(std::memory_order_acquire);

        if (st == SET_EVICTING) {
            ESP_LOGD(TAG, "set slot %d: evicting '%s'", i, ss.key);
            soundset_free(ss);
            // Straight into the load the eviction was for; want[] is always set by acquire().
            ss.state.store(SET_PENDING, std::memory_order_release);
            return true;
        }

        if (st == SET_PENDING) {
            if (!table_alloc(ss.tab, SOUNDSET_MAX)) {
                ESP_LOGW(TAG, "no memory for sound set '%s'", ss.want);
                ss.want[0] = '\0';
                ss.state.store(SET_EMPTY, std::memory_order_release);
                return true;
            }
            ss.tab.count = 0;

            char root[160];
            snprintf(root, sizeof root, "%s/sounds", ss.wantDir);
            // srcRoot only orders manifest-vs-loose WITHIN one scan, and a set has exactly one
            // root by construction, so any fixed value does the job here.
            scan_manifest(ss.tab, root, 0, "set");
            scan_loose(ss.tab, root, 0, "set");

            // memcpy, not snprintf: both are members of the same struct, and GCC cannot prove
            // two distinct arrays do not overlap (-Wrestrict). Equal sizes, and `want` is
            // always NUL-terminated by the snprintf in acquire(), so this is a plain move.
            static_assert(sizeof ss.key == sizeof ss.want, "key/want must be interchangeable");
            memcpy(ss.key, ss.want, sizeof ss.key);
            ss.want[0] = '\0';
            // Published LAST. A set that turned out to have no sounds at all is still READY with
            // zero entries -- that is a legitimate answer, and it is what stops the game asking
            // again on every single play() for a creature that ships no audio.
            ss.state.store(SET_READY, std::memory_order_release);
            if (ss.tab.count)
                ESP_LOGI(TAG, "set slot %d: %d sounds for '%s'", i, ss.tab.count, ss.key);
            return true;
        }
    }
    return false;
}

// --- sample cache -------------------------------------------------------------------------

bool bank_load_sample(int set, int idx)
{
    Table* t = table_of(set);
    if (!t || !t->list || idx < 0 || idx >= t->count) return false;
    Sound& s = t->list[idx];
    if (s.kind != SoundKind::Sample) return false;
    if (s.mem.load(std::memory_order_acquire)) return true;
    if (s.loadFailed) return false;

    if (s_cacheBytes >= SAMPLE_BUDGET) {
        ESP_LOGW(TAG, "sample cache full (%u KB); '%s' will stream instead",
                 (unsigned)(s_cacheBytes / 1024), s.id);
        s.loadFailed = true;
        return false;
    }

    Decoder* d = decoder_open(s.path);
    if (!d) { s.loadFailed = true; return false; }
    // Draining a decoder to the end only terminates if it HAS an end: a tracker module told to
    // loop is an endless generator. Say so explicitly rather than relying on the default.
    d->setLooping(false);

    const int blk = d->maxBlockSamples();
    // Trust the container's length when it states one (WAV always does), so the common case
    // is a single exact allocation rather than a growth sequence in PSRAM.
    uint32_t cap = d->totalFrames() ? d->totalFrames() * d->chans() + (uint32_t)blk
                                    : (uint32_t)blk * 16;
    int16_t* pcm = (int16_t*)heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) { delete d; s.loadFailed = true; return false; }

    uint32_t n = 0;
    bool over = false;
    bool bad  = false;
    while (true) {
        if (cap - n < (uint32_t)blk) {
            const uint32_t grow = cap + cap / 2 + (uint32_t)blk;
            if (grow * sizeof(int16_t) > MAX_SAMPLE_BYTES) { over = true; break; }
            int16_t* bigger = (int16_t*)heap_caps_realloc(pcm, grow * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!bigger) { over = true; break; }
            pcm = bigger;
            cap = grow;
        }
        const int got = d->decode(pcm + n, (int)(cap - n));
        if (got == 0) break;                       // clean end of stream
        if (got < 0) {                             // corrupt: NOT the same thing as the end
            ESP_LOGW(TAG, "'%s': decode failed %u samples in; streaming it instead",
                     s.id, (unsigned)n);
            bad = true;
            break;
        }
        n += (uint32_t)got;
        if (n * sizeof(int16_t) > MAX_SAMPLE_BYTES) { over = true; break; }
    }

    if (over || bad || n == 0) {
        // Too big for the cache, or empty. Streaming is the right answer for both, and the
        // caller falls back to it when we return false -- so this is a downgrade, not a loss.
        if (over) ESP_LOGI(TAG, "'%s' exceeds the per-sample cap; streaming it", s.id);
        free(pcm);
        delete d;
        s.loadFailed = true;
        return false;
    }

    // Hand back the slack from the growth path so the cache accounting is honest.
    if (int16_t* fit = (int16_t*)heap_caps_realloc(pcm, n * sizeof(int16_t), MALLOC_CAP_SPIRAM))
        pcm = fit;

    MemSample* ms = (MemSample*)malloc(sizeof(MemSample));
    if (!ms) { free(pcm); delete d; s.loadFailed = true; return false; }
    ms->pcm    = pcm;
    ms->chans  = d->chans();
    ms->rate   = d->rate();
    ms->frames = n / ms->chans;
    delete d;

    s_cacheBytes += n * sizeof(int16_t);
    // Publish last: play() on the game thread reads this pointer without the lock, and must
    // never see a MemSample whose fields are not written yet.
    s.mem.store(ms, std::memory_order_release);

    ESP_LOGI(TAG, "cached '%s': %u frames @ %u Hz x%u (%u KB, %u KB total)",
             s.id, (unsigned)ms->frames, (unsigned)ms->rate, ms->chans,
             (unsigned)(n * 2 / 1024), (unsigned)(s_cacheBytes / 1024));
    return true;
}

}   // namespace audio
