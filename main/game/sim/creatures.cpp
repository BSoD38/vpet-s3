#include "creatures.hpp"
#include "engine/gfx.hpp"                 // SPRITE_TRANSP, display
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "CREA";

static const char* FLASH_ROOT = "/creatures";
static const char* SD_ROOT     = "/sdcard/creatures";

// Max sprite dimension (px). 144 = 3x the 48px base; sprites may be non-square up to
// this in each axis. The Home layout (HUD->horizon budget) is sized to fit this.
static const uint32_t SPRITE_MAX_DIM = 144;

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

static double jnum(cJSON* o, const char* k, double def)
{
    if (!o) return def;
    cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void jstr(cJSON* o, const char* k, char* dst, int n, const char* def)
{
    const char* s = def;
    if (o) {
        cJSON* v = cJSON_GetObjectItem(o, k);
        if (cJSON_IsString(v) && v->valuestring) s = v->valuestring;
    }
    strncpy(dst, s, n - 1);
    dst[n - 1] = '\0';
}

// Decode a PNG file into a fresh PSRAM sprite. Transparent PNG pixels leave the
// color-key (SPRITE_TRANSP) showing through, which the blit then treats as clear.
static LGFX_Sprite* load_png_sprite(const char* path)
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
    if (w == 0 || w > SPRITE_MAX_DIM || h == 0 || h > SPRITE_MAX_DIM) {
        ESP_LOGW(TAG, "sprite %ux%u exceeds %ux%u cap: %s", (unsigned)w, (unsigned)h,
                 (unsigned)SPRITE_MAX_DIM, (unsigned)SPRITE_MAX_DIM, path);
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
    jstr(root, "id",   c.id,   sizeof c.id,   "");
    jstr(root, "name", c.name, sizeof c.name, "?");
    c.tier = (uint8_t)jnum(root, "tier", 0);

    cJSON* base = cJSON_GetObjectItem(root, "base");
    c.baseHp  = (uint32_t)jnum(base, "hp",  0);
    c.baseStr = (uint16_t)jnum(base, "str", 0);
    c.baseEnd = (uint16_t)jnum(base, "end", 0);
    c.baseAgi = (uint16_t)jnum(base, "agi", 0);
    c.baseInt = (uint16_t)jnum(base, "int", 0);

    cJSON* needs = cJSON_GetObjectItem(root, "needs");
    c.hungerPerHr   = (float)jnum(needs, "hungerPerHr",   0);
    c.happyPerHr    = (float)jnum(needs, "happyPerHr",    0);
    c.poopIntervalS = (float)jnum(needs, "poopIntervalS", 1e9);
    c.sleepStart    = (uint8_t)jnum(needs, "sleepStart", 0);
    c.sleepEnd      = (uint8_t)jnum(needs, "sleepEnd",   0);

    c.minStageSecs = (float)jnum(root, "minStageSecs", 1e9);
    jstr(root, "sprite", c.spriteFile, sizeof c.spriteFile, "sprite.png");

    c.evoCount = 0;
    cJSON* evos = cJSON_GetObjectItem(root, "evolutions");
    if (cJSON_IsArray(evos)) {
        cJSON* ev = nullptr;
        cJSON_ArrayForEach(ev, evos) {
            if (c.evoCount >= (uint8_t)(sizeof(c.evos) / sizeof(c.evos[0]))) break;
            EvoEdge& e = c.evos[c.evoCount];
            memset(&e, 0, sizeof e);
            jstr(ev, "to", e.to, sizeof e.to, "");
            e.toIdx = -1;
            e.minHp         = (uint32_t)jnum(ev, "minHp", 0);
            e.minStr        = (uint16_t)jnum(ev, "minStr", 0);
            e.minEnd        = (uint16_t)jnum(ev, "minEnd", 0);
            e.minAgi        = (uint16_t)jnum(ev, "minAgi", 0);
            e.minInt        = (uint16_t)jnum(ev, "minInt", 0);
            e.minFriendship = (uint16_t)jnum(ev, "minFriendship", 0);
            e.maxCareMistakes = (uint8_t)jnum(ev, "maxCareMistakes", 255);
            if (e.to[0]) c.evoCount++;
        }
    }
    cJSON_Delete(root);
    c.sprite = nullptr;
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

        int idx = upsert(c.id);
        if (idx < 0) { ESP_LOGW(TAG, "registry full; dropped '%s'", c.id); continue; }
        list_[idx] = c;                            // append or override (SD wins)
        ESP_LOGI(TAG, "%s: '%s' (%s) tier %u", srcTag, c.id, c.name, c.tier);
    }
    closedir(d);
}

// Lazy sprite access with an LRU cache. A creature's PNG is decoded into PSRAM on
// first display and kept until the cache is full, then the least-recently-shown one
// is evicted. This keeps the resident set tiny (only what's on screen) so the roster
// can grow far past what would fit if every sprite were decoded at once.
LGFX_Sprite* CreatureRegistry::sprite(int idx)
{
    if (idx < 0 || idx >= count_) return nullptr;
    Creature& c = list_[idx];

    if (c.sprite) { c.spriteTick = ++spriteClock_; return c.sprite; }   // hit -> mark MRU
    if (c.spriteMiss || c.spritePath[0] == '\0') return nullptr;        // no file / already failed

    // Evict the least-recently-used decoded sprite(s) to stay under the cap. Entries
    // with no file path (e.g. the built-in egg) are never evicted (can't reload).
    while (loadedSprites_ >= SPRITE_CACHE) {
        int victim = -1;
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < count_; i++)
            if (list_[i].sprite && list_[i].spritePath[0] && list_[i].spriteTick < oldest) {
                oldest = list_[i].spriteTick;
                victim = i;
            }
        if (victim < 0) break;                     // nothing evictable; load anyway
        delete list_[victim].sprite;
        list_[victim].sprite = nullptr;
        loadedSprites_--;
    }

    c.sprite = load_png_sprite(c.spritePath);
    if (c.sprite) { c.spriteTick = ++spriteClock_; loadedSprites_++; }
    else          { c.spriteMiss = 1; }            // stop retrying a bad/missing file
    return c.sprite;
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
    c.evoCount = 0;                                // can't evolve without data files

    // No baked sprite: c.sprite stays null (from memset) and spritePath is empty, so
    // sprite() returns null and the scenes draw the "?" placeholder for it.
    list_[count_++] = c;
    ESP_LOGW(TAG, "no creature data readable; using built-in egg (placeholder art)");
}

void CreatureRegistry::loadAll()
{
    count_ = 0;

    esp_vfs_fat_mount_config_t cfg = {};
    cfg.max_files = 4;
    cfg.format_if_mount_failed = false;
    esp_err_t e = esp_vfs_fat_spiflash_mount_ro(FLASH_ROOT, "creatures", &cfg);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "flash creatures mount failed (%s)", esp_err_to_name(e));

    scanRoot(FLASH_ROOT, "flash");   // base game
    scanRoot(SD_ROOT,    "sd");      // mods overlay (override base on id clash)

    if (count_ == 0) { addBuiltinEgg(); return; }

    resolveEdges();   // sprites decode lazily on first display (see sprite())
    ESP_LOGI(TAG, "registry ready: %d creatures", count_);
}
