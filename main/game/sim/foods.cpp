#include "foods.hpp"
#include "gamedata.hpp"
#include "engine/gfx.hpp"        // rgb565, col
#include "engine/pakfs.hpp"      // mounted mod packs are extra scan roots
#include "esp_log.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <strings.h>             // strcasecmp
#include <cstdio>
#include <cstdlib>

static const char* TAG = "FOOD";

static const char* SD_ROOT = "/sdcard/foods";

// File/JSON access shared via sim/gamedata (gd_read_file/gd_num/gd_str); the drift block
// parser is the personality system's parse_drift, so axis names/order have one owner.
static const long MAX_FILE_BYTES = 16384;   // food configs are tiny

// --- registry ------------------------------------------------------------------

int FoodRegistry::indexOf(const char* id) const
{
    for (int i = 0; i < count_; i++)
        if (strcmp(list_[i].id, id) == 0) return i;
    return -1;
}

bool FoodRegistry::matches(int i, const char* key) const
{
    if (i < 0 || i >= count_ || !key || !*key) return false;
    const Food& f = list_[i];
    if (strcasecmp(f.id, key) == 0) return true;
    for (uint8_t t = 0; t < f.tagCount; t++)
        if (strcasecmp(f.tags[t], key) == 0) return true;
    return false;
}

int FoodRegistry::upsert(const char* id)
{
    int i = indexOf(id);
    if (i >= 0) return i;               // override the existing entry (SD over flash)
    if (count_ >= MAX) return -1;
    return count_++;
}

void FoodRegistry::parseEntry(cJSON* root, Food& f)
{
    memset(&f, 0, sizeof f);
    gd_str(root, "id",   f.id,   sizeof f.id,   "");
    gd_str(root, "name", f.name, sizeof f.name, "?");
    gd_str(root, "desc", f.desc, sizeof f.desc, "");
    f.fills     = (int16_t)gd_num(root, "fills",     0);
    f.happiness = (int16_t)gd_num(root, "happiness", 0);
    f.health    = (int16_t)gd_num(root, "health",    0);
    f.color     = gd_color(root, "color", col::dim);
    f.cost      = (uint16_t)gd_num(root, "cost", 0);     // reserved (economy)
    gd_str(root, "rarity", f.rarity, sizeof f.rarity, "common");

    cJSON* tags = cJSON_GetObjectItem(root, "tags");
    if (cJSON_IsArray(tags)) {
        cJSON* t = nullptr;
        cJSON_ArrayForEach(t, tags) {
            if (f.tagCount >= FOOD_MAX_TAGS) break;
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(f.tags[f.tagCount], t->valuestring, sizeof(f.tags[0]) - 1);
                f.tags[f.tagCount][sizeof(f.tags[0]) - 1] = '\0';
                f.tagCount++;
            }
        }
    }

    // Personality nudge. Absent axes stay 0, so a food with no "drift" block (basic
    // kibble) is temperament-neutral -- which is what keeps a player who only fills
    // the gauge from being skewed (docs/food-and-feeding.md 1.2).
    parse_drift(root, "drift", f.drift, 0.0f);
}

bool FoodRegistry::parseFile(const char* path, Food& f)
{
    char* buf = gd_read_file(path, MAX_FILE_BYTES);
    if (!buf) return false;
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGW(TAG, "bad json: %s", path); return false; }
    parseEntry(root, f);
    cJSON_Delete(root);
    return true;
}

// A pack: <root>/foods.json holding an ARRAY of food objects. One file open instead of
// one per food -- the per-file FAT open (~7-12 ms measured) is what makes a large roster
// expensive at boot, exactly the argument conversation packs were built on.
void FoodRegistry::scanPack(const char* root, const char* srcTag)
{
    char path[192];
    snprintf(path, sizeof path, "%s/foods.json", root);
    char* buf = gd_read_file(path, 32768);
    if (!buf) return;                                  // no pack at this root: fine
    cJSON* arr = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "not a json array: %s", path);
        cJSON_Delete(arr);
        return;
    }
    cJSON* e = nullptr;
    cJSON_ArrayForEach(e, arr) {
        Food f;
        parseEntry(e, f);
        if (f.id[0] == '\0') continue;                 // pack entries need explicit ids
        int idx = upsert(f.id);
        if (idx < 0) { ESP_LOGW(TAG, "registry full; dropped '%s'", f.id); continue; }
        list_[idx] = f;
        if (gd_src_is_sd(srcTag)) gd_sd_loaded(GD_FOODS);
        ESP_LOGI(TAG, "%s(pack): '%s' (%s) fills %d", srcTag, f.id, f.name, (int)f.fills);
    }
    cJSON_Delete(arr);
}

void FoodRegistry::scanRoot(const char* root, const char* srcTag)
{
    DIR* d = opendir(root);
    if (!d) { ESP_LOGI(TAG, "no foods dir at %s", root); return; }

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
        snprintf(cfg, sizeof cfg, "%s/food.json", dir);

        Food f;
        if (!parseFile(cfg, f)) continue;
        if (f.id[0] == '\0') {                     // no explicit id -> use the folder name
            strncpy(f.id, ent->d_name, sizeof(f.id) - 1);
            f.id[sizeof(f.id) - 1] = '\0';
        }

        int idx = upsert(f.id);
        if (idx < 0) { ESP_LOGW(TAG, "registry full; dropped '%s'", f.id); continue; }
        list_[idx] = f;                            // append or override (SD wins)
        if (gd_src_is_sd(srcTag)) gd_sd_loaded(GD_FOODS);
        ESP_LOGI(TAG, "%s: '%s' (%s) fills %d", srcTag, f.id, f.name, (int)f.fills);
    }
    closedir(d);
}

void FoodRegistry::addBuiltinFood()
{
    Food f;
    memset(&f, 0, sizeof f);
    strncpy(f.id,   "kibble",      sizeof f.id - 1);
    strncpy(f.name, "Basic Food",  sizeof f.name - 1);
    strncpy(f.desc, "Plain, dependable pellets.", sizeof f.desc - 1);
    strncpy(f.rarity, "common", sizeof f.rarity - 1);
    f.fills     = 30;
    f.happiness = 3;
    f.color     = rgb565(168, 130, 60);
    // drift stays all-zero: the fallback food must never skew personality.
    list_[count_++] = f;
    ESP_LOGW(TAG, "no food data readable; using built-in basic food");
}

void FoodRegistry::loadAll()
{
    count_ = 0;
    gamedata_mount();                              // shared; idempotent

    char flashRoot[64];
    snprintf(flashRoot, sizeof flashRoot, "%s/foods", GAMEDATA_ROOT);

    scanPack(flashRoot, "flash");                  // base game: pack first, then per-dir
    scanRoot(flashRoot, "flash");
    for (int i = 0; i < pakfs_count(); i++) {      // mod packs (later pak wins)
        char root[32];
        snprintf(root, sizeof root, "%s/foods", pakfs_root(i));
        scanPack(root, "pak");
        scanRoot(root, "pak");
    }
    scanPack(SD_ROOT,   "sd");                     // loose-file mods: the final overlay
    scanRoot(SD_ROOT,   "sd");

    if (count_ == 0) { addBuiltinFood(); return; } // feeding must never be impossible
    ESP_LOGI(TAG, "registry ready: %d foods", count_);
}
