#include "creatures.hpp"
#include "gamedata.hpp"                   // gd_num/gd_str (shared JSON accessors)
#include "engine/gfx.hpp"                 // SPRITE_TRANSP, display
#include "engine/pakfs.hpp"               // mounted mod packs are extra scan roots
#include "esp_timer.h"                    // boot-scan timing (sizes the pak win)
#include "esp_log.h"
#include "esp_heap_caps.h"      // the roster table is allocated in PSRAM
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <strings.h>   // strcasecmp (case-insensitive attribute parsing)
#include <cstdio>
#include <cstdlib>

static const char* TAG = "CREA";

// No flash root: the base roster ships inside base.pak (gamedata partition), scanned via the
// /pakN roots like any other pack. Loose SD folders remain the final, highest-priority overlay.
static const char* SD_ROOT = "/sdcard/creatures";

// Max sprite dimension (px). Sprites may be non-square up to this in each axis; a PNG larger
// than this in either axis is rejected (creature falls back to the "?" sprite). Kept modest so
// creatures don't dominate the 240-wide screen on the native-draw scenes (Home/Smash/Run).
static const uint32_t SPRITE_MAX_DIM = 128;

// --- small helpers -----------------------------------------------------------

// Read a whole file into a malloc'd, null-terminated buffer (caller frees).
static char* read_file(const char* path, long* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 262144) { fclose(f); return nullptr; }   // sanity cap
    char* buf = (char*)malloc(n + 1);
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = (long)rd;
    return buf;
}

// JSON field accessors come from sim/gamedata (gd_num/gd_str); the binary whole-file
// reader above stays local because it also loads PNGs (length out-param, 256 KB cap).

// Map the optional "attribute" string to the battle type enum (default Free).
static uint8_t parse_attribute(cJSON* o)
{
    char s[16];
    gd_str(o, "attribute", s, sizeof s, "free");
    if (strcasecmp(s, "vaccine") == 0) return ATTR_VACCINE;
    if (strcasecmp(s, "data")    == 0) return ATTR_DATA;
    if (strcasecmp(s, "virus")   == 0) return ATTR_VIRUS;
    return ATTR_FREE;
}

// Default voice pitch for a creature that does not state one (see Creature::voicePitch).
//
// Tier picks the band -- a hatchling is small and squeaky, a Mega is big and slow -- and a
// hash of the id spreads creatures WITHIN their band, so two Champions are not the same
// creature wearing a different sprite. The hash is what makes this worth doing at all: a
// table alone would give every creature of a tier one identical voice, which reads as a bug
// rather than as a design.
//
// Deterministic and derived, never stored: the same creature sounds the same on every boot
// and on every device, and an imported roster gets its voices without anyone editing 700
// files. An author who disagrees writes "voicePitch" and this is skipped entirely.
static float default_voice_pitch(const char* id, uint8_t tier)
{
    // Indexed by LifeStage (egg, in-training I/II, child, champion, ultimate, mega, mega+).
    // The egg is 1.0 because it barely speaks, and the hatch fanfare already belongs to the
    // creature coming OUT of it.
    static const float BAND[] = { 1.00f, 1.45f, 1.32f, 1.18f, 1.05f, 0.94f, 0.84f, 0.76f };
    const float band = BAND[tier < (sizeof BAND / sizeof BAND[0]) ? tier : 0];

    uint32_t h = 2166136261u;                       // FNV-1a over the id
    for (const char* p = id; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    const float spread = ((float)(h % 1000u) / 1000.0f - 0.5f) * 0.12f;   // +/-6%

    const float v = band * (1.0f + spread);
    return v < 0.5f ? 0.5f : (v > 2.0f ? 2.0f : v);
}

uint16_t attr_color(uint8_t a)
{
    switch (a) {
        case ATTR_VACCINE: return rgb565(90, 180, 240);
        case ATTR_DATA:    return rgb565(90, 210, 120);
        case ATTR_VIRUS:   return rgb565(190, 110, 220);
        default:           return col::dim;
    }
}

const char* attr_short(uint8_t a)
{
    switch (a) {
        case ATTR_VACCINE: return "VAC";
        case ATTR_DATA:    return "DAT";
        case ATTR_VIRUS:   return "VIR";
        default:           return "---";
    }
}

// Decode a PNG file into a fresh PSRAM sprite. Transparent PNG pixels leave the
// color-key (SPRITE_TRANSP) showing through, which the blit then treats as clear.
// maxW/maxH cap the decoded image (a 16-frame sheet may exceed the per-frame cap;
// its CELLS are checked by the caller instead).
static LGFX_Sprite* load_png_sprite(const char* path, uint32_t maxW, uint32_t maxH)
{
    long n = 0;
    char* buf = read_file(path, &n);
    if (!buf) { ESP_LOGW(TAG, "sprite missing: %s", path); return nullptr; }

    const uint8_t* u = (const uint8_t*)buf;
    static const uint8_t SIG[8] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    if (n < 24 || memcmp(u, SIG, 8) != 0) { free(buf); ESP_LOGW(TAG, "not a PNG: %s", path); return nullptr; }

    // PNG width/height are big-endian at IHDR (offsets 16 and 20).
    uint32_t w = ((uint32_t)u[16] << 24) | (u[17] << 16) | (u[18] << 8) | u[19];
    uint32_t h = ((uint32_t)u[20] << 24) | (u[21] << 16) | (u[22] << 8) | u[23];
    if (w == 0 || w > maxW || h == 0 || h > maxH) {
        ESP_LOGW(TAG, "sprite %ux%u exceeds %ux%u cap: %s", (unsigned)w, (unsigned)h,
                 (unsigned)maxW, (unsigned)maxH, path);
        free(buf);
        return nullptr;
    }

    LGFX_Sprite* s = new LGFX_Sprite(&display);
    s->setPsram(true);
    s->setColorDepth(16);
    if (!s->createSprite((int)w, (int)h)) { delete s; free(buf); ESP_LOGW(TAG, "sprite alloc failed"); return nullptr; }
    s->fillScreen(SPRITE_TRANSP);
    s->drawPng(u, (uint32_t)n);   // decode from memory onto the key-filled canvas
    free(buf);
    return s;
}

// Fill c.frames[] from the creature's PNG. Single-pose creatures decode straight into
// frames[0]; 16-frame sheets decode whole, then get carved into 16 small sprites (raw
// row memcpy -- both are 16bpp PSRAM canvases) so the blit helpers and the downscale
// cache only ever see plain per-frame sprites. Returns false on any failure.
static bool load_frames(Creature& c)
{
    if (c.frameCount <= 1) {
        c.frames[0] = load_png_sprite(c.spritePath, SPRITE_MAX_DIM, SPRITE_MAX_DIM);
        return c.frames[0] != nullptr;
    }

    // Sheet: 4x4 grid, cell = sheetW/4 x sheetH/4; the per-frame cap applies per cell.
    LGFX_Sprite* sheet = load_png_sprite(c.spritePath, SPRITE_MAX_DIM * 4, SPRITE_MAX_DIM * 4);
    if (!sheet) return false;
    int sw = sheet->width(), sh = sheet->height();
    if (sw % 4 || sh % 4) {
        ESP_LOGW(TAG, "sheet %dx%d not a 4x4 grid: %s", sw, sh, c.spritePath);
        delete sheet;
        return false;
    }
    int fw = sw / 4, fh = sh / 4;

    const uint16_t* src = (const uint16_t*)sheet->getBuffer();
    bool ok = true;
    for (int f = 0; f < FRM_COUNT && ok; f++) {
        LGFX_Sprite* s = new LGFX_Sprite(&display);
        s->setPsram(true);
        s->setColorDepth(16);
        if (!s->createSprite(fw, fh)) { delete s; ok = false; break; }
        uint16_t* dst = (uint16_t*)s->getBuffer();
        int ox = (f % 4) * fw, oy = (f / 4) * fh;
        for (int y = 0; y < fh; y++)
            memcpy(dst + y * fw, src + (oy + y) * sw + ox, fw * sizeof(uint16_t));
        c.frames[f] = s;
    }
    delete sheet;
    if (!ok) {
        ESP_LOGW(TAG, "frame alloc failed: %s", c.spritePath);
        for (int f = 0; f < FRM_COUNT; f++) { delete c.frames[f]; c.frames[f] = nullptr; }
    }
    return ok;
}

// Free every decoded frame of an entry (scaled copies are invalidated first: the
// cache keys on the source sprite's address, which is about to be freed/reused).
static void free_frames(Creature& c)
{
    for (int f = 0; f < FRM_COUNT; f++) {
        if (!c.frames[f]) continue;
        gfx_invalidate_scaled(c.frames[f]);
        delete c.frames[f];
        c.frames[f] = nullptr;
    }
}

// --- registry ----------------------------------------------------------------

int CreatureRegistry::indexOf(const char* id) const
{
    for (int i = 0; i < count_; i++)
        if (strcmp(list_[i].id, id) == 0) return i;
    return -1;
}

int CreatureRegistry::upsert(const char* id)
{
    int i = indexOf(id);
    if (i >= 0) return i;               // override the existing entry (SD over flash)
    if (count_ >= MAX) return -1;
    return count_++;
}

bool CreatureRegistry::parseFile(const char* path, Creature& c)
{
    long n = 0;
    char* buf = read_file(path, &n);
    if (!buf) return false;
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGW(TAG, "bad json: %s", path); return false; }

    memset(&c, 0, sizeof c);
    gd_str(root, "id",   c.id,   sizeof c.id,   "");
    gd_str(root, "name", c.name, sizeof c.name, "?");
    c.tier = (uint8_t)gd_num(root, "tier", 0);
    c.attribute = parse_attribute(root);

    cJSON* base = cJSON_GetObjectItem(root, "base");
    c.baseHp  = (uint32_t)gd_num(base, "hp",  0);
    c.baseStr = (uint16_t)gd_num(base, "str", 0);
    c.baseEnd = (uint16_t)gd_num(base, "end", 0);
    c.baseAgi = (uint16_t)gd_num(base, "agi", 0);
    c.baseInt = (uint16_t)gd_num(base, "int", 0);

    cJSON* needs = cJSON_GetObjectItem(root, "needs");
    c.hungerPerHr   = (float)gd_num(needs, "hungerPerHr",   0);
    c.happyPerHr    = (float)gd_num(needs, "happyPerHr",    0);
    c.poopIntervalS = (float)gd_num(needs, "poopIntervalS", 1e9);
    c.sleepStart    = (uint8_t)gd_num(needs, "sleepStart", 0);
    c.sleepEnd      = (uint8_t)gd_num(needs, "sleepEnd",   0);

    c.minStageSecs = (float)gd_num(root, "minStageSecs", 1e9);

    // Voice. "voice" names an authored family and is optional; "voicePitch" overrides the
    // derived default and is rarer still. Derived AFTER the id is read, since the id is half
    // of what the default is made from.
    gd_str(root, "voice", c.voiceFamily, sizeof c.voiceFamily, "");
    c.voicePitch = (float)gd_num(root, "voicePitch", 0.0);
    if (c.voicePitch <= 0.0f) c.voicePitch = default_voice_pitch(c.id, c.tier);

    gd_str(root, "sprite", c.spriteFile, sizeof c.spriteFile, "sprite.png");
    int frames = (int)gd_num(root, "frames", 1);
    if (frames != 1 && frames != FRM_COUNT) {
        ESP_LOGW(TAG, "%s: frames=%d unsupported (want 1 or %d); using 1", path, frames, FRM_COUNT);
        frames = 1;
    }
    c.frameCount = (uint8_t)frames;

    c.evoCount = 0;
    cJSON* evos = cJSON_GetObjectItem(root, "evolutions");
    if (cJSON_IsArray(evos)) {
        cJSON* ev = nullptr;
        cJSON_ArrayForEach(ev, evos) {
            if (c.evoCount >= (uint8_t)(sizeof(c.evos) / sizeof(c.evos[0]))) break;
            EvoEdge& e = c.evos[c.evoCount];
            memset(&e, 0, sizeof e);
            gd_str(ev, "to", e.to, sizeof e.to, "");
            e.toIdx = -1;
            e.minHp         = (uint32_t)gd_num(ev, "minHp", 0);
            e.minStr        = (uint16_t)gd_num(ev, "minStr", 0);
            e.minEnd        = (uint16_t)gd_num(ev, "minEnd", 0);
            e.minAgi        = (uint16_t)gd_num(ev, "minAgi", 0);
            e.minInt        = (uint16_t)gd_num(ev, "minInt", 0);
            e.minFriendship = (uint16_t)gd_num(ev, "minFriendship", 0);
            e.minWins       = (uint32_t)gd_num(ev, "minWins", 0);
            e.maxCareMistakes = (uint8_t)gd_num(ev, "maxCareMistakes", 255);
            if (e.to[0]) c.evoCount++;
        }
    }
    cJSON_Delete(root);
    for (int f = 0; f < FRM_COUNT; f++) c.frames[f] = nullptr;
    return true;
}

void CreatureRegistry::scanRoot(const char* root, const char* srcTag)
{
    DIR* d = opendir(root);
    if (!d) { ESP_LOGI(TAG, "no creatures dir at %s", root); return; }

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        char dir[192];
        snprintf(dir, sizeof dir, "%s/%s", root, ent->d_name);

        bool isdir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {           // some FS don't fill d_type
            struct stat st;
            if (stat(dir, &st) == 0) isdir = S_ISDIR(st.st_mode);
        }
        if (!isdir) continue;

        char cfg[224];
        snprintf(cfg, sizeof cfg, "%s/creature.json", dir);

        Creature c;
        if (!parseFile(cfg, c)) continue;
        if (c.id[0] == '\0') {                     // no explicit id -> use the folder name
            strncpy(c.id, ent->d_name, sizeof(c.id) - 1);
            c.id[sizeof(c.id) - 1] = '\0';
        }
        snprintf(c.spritePath, sizeof c.spritePath, "%s/%s", dir, c.spriteFile);
        snprintf(c.dir,        sizeof c.dir,        "%s",    dir);

        int idx = upsert(c.id);
        if (idx < 0) { ESP_LOGW(TAG, "registry full; dropped '%s'", c.id); continue; }
        list_[idx] = c;                            // append or override (SD wins)
        ESP_LOGI(TAG, "%s: '%s' (%s) tier %u", srcTag, c.id, c.name, c.tier);
    }
    closedir(d);
}

// Lazy frame access with an LRU cache. A creature's PNG is decoded into PSRAM on
// first display (a 16-frame sheet is carved into its frames right away -- one cache
// entry covers the whole set) and kept until the cache is full, then the least-
// recently-shown ENTRY is evicted. This keeps the resident set tiny (only what's on
// screen) so the roster can grow far past what would fit decoded all at once.
LGFX_Sprite* CreatureRegistry::frame(int idx, int f)
{
    if (idx < 0 || idx >= count_) return nullptr;
    Creature& c = list_[idx];
    if (f < 0) f = 0;
    if (f >= c.frameCount) f = (c.frameCount <= 1) ? 0 : FRM_COUNT - 1;

    if (c.frames[0]) { c.spriteTick = ++spriteClock_; return c.frames[f]; }   // hit -> mark MRU
    if (c.spriteMiss || c.spritePath[0] == '\0') return nullptr;              // no file / already failed

    // Evict the least-recently-used decoded entry(s) to stay under the cap. Entries
    // with no file path (e.g. the built-in egg) are never evicted (can't reload).
    while (loadedSprites_ >= SPRITE_CACHE) {
        int victim = -1;
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < count_; i++)
            if (list_[i].frames[0] && list_[i].spritePath[0] && list_[i].spriteTick < oldest) {
                oldest = list_[i].spriteTick;
                victim = i;
            }
        if (victim < 0) break;                     // nothing evictable; load anyway
        free_frames(list_[victim]);
        loadedSprites_--;
    }

    if (load_frames(c)) { c.spriteTick = ++spriteClock_; loadedSprites_++; }
    else                { c.spriteMiss = 1; }      // stop retrying a bad/missing file
    return c.frames[f];
}

void CreatureRegistry::resolveEdges()
{
    for (int i = 0; i < count_; i++)
        for (int j = 0; j < list_[i].evoCount; j++) {
            list_[i].evos[j].toIdx = indexOf(list_[i].evos[j].to);
            if (list_[i].evos[j].toIdx < 0)
                ESP_LOGW(TAG, "'%s' evolution target '%s' not found",
                         list_[i].id, list_[i].evos[j].to);
        }
}

void CreatureRegistry::addBuiltinEgg()
{
    Creature c;
    memset(&c, 0, sizeof c);
    strncpy(c.id, "egg", sizeof c.id - 1);
    strncpy(c.name, "Egg", sizeof c.name - 1);
    c.tier = 0;
    c.minStageSecs = 120.0f;
    c.poopIntervalS = 1e9f;
    c.voicePitch = 1.0f;                           // memset left it 0, which is not a pitch
    c.evoCount = 0;                                // can't evolve without data files

    // No baked sprite: c.frames[] stays null (from memset) and spritePath is empty, so
    // frame() returns null and the scenes draw the "?" placeholder for it.
    list_[count_++] = c;
    ESP_LOGW(TAG, "no creature data readable; using built-in egg (placeholder art)");
}

void CreatureRegistry::loadAll()
{
    // The roster table lives in PSRAM (see list_): ~120 KB at the current cap, which has no
    // business occupying internal RAM for data that's read on lookup and on draw. Aborting on
    // failure matches gfx_init(): without the table every later access is a null dereference.
    if (!list_) {
        const size_t bytes = sizeof(Creature) * MAX;
        list_ = (Creature*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!list_) {
            ESP_LOGE(TAG, "PSRAM alloc for the roster table (%u bytes, %d slots) failed",
                     (unsigned)bytes, MAX);
            abort();
        }
        memset(list_, 0, bytes);
        ESP_LOGI(TAG, "roster table: %u bytes in PSRAM (%d slots, %u bytes each)",
                 (unsigned)bytes, MAX, (unsigned)sizeof(Creature));
    }
    count_ = 0;

    // Overlay order = who wins an id clash: packs in mount order, then loose SD files beat
    // everything -- so a player can always drop a single folder on the card to patch over a
    // pack without re-packing it.
    //
    // There is no separate flash root any more. The base roster used to be loose files in a
    // dedicated `creatures` partition mounted at /creatures; it now lives inside base.pak in
    // the gamedata partition and is reached through /pak0, which app.cpp mounts FIRST so the
    // base game stays the weakest layer. Note the cost of that: the base roster is one file
    // now, so a corrupt pack loses the whole roster rather than one creature -- which is what
    // the update system's per-partition hashing and boot repair exist to catch.
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < pakfs_count(); i++) {
        char root[32];
        snprintf(root, sizeof root, "%s/creatures", pakfs_root(i));
        scanRoot(root, "pak");
    }
    scanRoot(SD_ROOT, "sd");         // loose-file mods: the final overlay

    if (count_ == 0) { addBuiltinEgg(); return; }

    resolveEdges();   // sprites decode lazily on first display (see sprite())
    ESP_LOGI(TAG, "registry ready: %d creatures in %d ms", count_,
             (int)((esp_timer_get_time() - t0) / 1000));
}
