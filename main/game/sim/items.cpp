#include "items.hpp"
#include "gamedata.hpp"
#include "engine/gfx.hpp"        // col::dim (swatch fallback)
#include "engine/pakfs.hpp"      // mounted mod packs are extra scan roots
#include "esp_log.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <strings.h>             // strcasecmp
#include <cstdio>
#include <cstdlib>

static const char* TAG = "ITEM";

static const char* SD_ROOT = "/sdcard/items";

static const long MAX_FILE_BYTES = 16384;   // item configs are tiny

// --- small string -> enum parsers ------------------------------------------------
// Each returns a sentinel on an unknown token and the CALLER warns, rather than silently
// substituting a default. A mod that misspells "medicine" should be told, not quietly
// handed an inert item that never treats anything.

static uint8_t parse_kind(const char* s)
{
    if (strcasecmp(s, "toy")      == 0) return ITEM_TOY;
    if (strcasecmp(s, "decor")    == 0) return ITEM_DECOR;
    if (strcasecmp(s, "care")     == 0) return ITEM_CARE;
    if (strcasecmp(s, "special")  == 0) return ITEM_SPECIAL;
    if (strcasecmp(s, "keepsake") == 0) return ITEM_KEEPSAKE;
    return ITEM_KIND_COUNT;                 // unknown
}

static uint8_t parse_slot(const char* s)
{
    if (strcasecmp(s, "floor")   == 0) return DSLOT_FLOOR;
    if (strcasecmp(s, "wall")    == 0) return DSLOT_WALL;
    if (strcasecmp(s, "window")  == 0) return DSLOT_WINDOW;
    if (strcasecmp(s, "bed")     == 0) return DSLOT_BED;
    if (strcasecmp(s, "feature") == 0) return DSLOT_FEATURE;
    return DSLOT_NONE;
}

// A TRACK, not one state (see TreatTrack). "sick_bad" is accepted as a spelling of the
// sickness track so content written against the first schema keeps loading.
//
// `critical` is deliberately NOT accepted: it is past treating by design
// (docs/death-and-lifespan.md 3), so an item claiming to cure it would be a lie the engine
// then has to enforce against.
static uint8_t parse_treats(const char* s)
{
    if (strcasecmp(s, "sick")     == 0) return TRACK_SICK;
    if (strcasecmp(s, "sick_bad") == 0) return TRACK_SICK;
    if (strcasecmp(s, "injured")  == 0) return TRACK_INJURED;
    return TREATS_NONE;
}

static uint8_t parse_potency(const char* s)
{
    if (!s[0] || strcasecmp(s, "step") == 0) return POTENCY_STEP;   // the default
    if (strcasecmp(s, "full") == 0)          return POTENCY_FULL;
    return 0;                                                        // unknown
}

const char* item_kind_name(uint8_t kind)
{
    switch (kind) {
        case ITEM_FOOD:     return "Food";
        case ITEM_TOY:      return "Toy";
        case ITEM_DECOR:    return "Decor";
        case ITEM_CARE:     return "Medicine";
        case ITEM_SPECIAL:  return "Special";
        case ITEM_KEEPSAKE: return "Keepsake";
        default:            return "?";
    }
}

// --- registry ------------------------------------------------------------------

int ItemRegistry::indexOf(const char* id) const
{
    for (int i = 0; i < count_; i++)
        if (strcmp(list_[i].id, id) == 0) return i;
    return -1;
}

bool ItemRegistry::matches(int i, const char* key) const
{
    if (i < 0 || i >= count_ || !key || !*key) return false;
    const Item& it = list_[i];
    if (strcasecmp(it.id, key) == 0) return true;
    for (uint8_t t = 0; t < it.tagCount; t++)
        if (strcasecmp(it.tags[t], key) == 0) return true;
    return false;
}

int ItemRegistry::upsert(const char* id)
{
    int i = indexOf(id);
    if (i >= 0) return i;               // override the existing entry (SD over pak over flash)
    if (count_ >= MAX) return -1;
    return count_++;
}

void ItemRegistry::parseEntry(cJSON* root, Item& it)
{
    memset(&it, 0, sizeof it);
    gd_str(root, "id",   it.id,   sizeof it.id,   "");
    gd_str(root, "name", it.name, sizeof it.name, "?");
    gd_str(root, "desc", it.desc, sizeof it.desc, "");
    gd_str(root, "rarity", it.rarity, sizeof it.rarity, "common");
    it.cost      = (uint16_t)gd_num(root, "cost", 0);
    it.color     = gd_color(root, "color", col::dim);
    it.happiness = (int16_t)gd_num(root, "happiness", 0);

    char buf[16];
    gd_str(root, "kind", buf, sizeof buf, "");
    it.kind = parse_kind(buf);

    gd_str(root, "slot", buf, sizeof buf, "");
    it.slot = buf[0] ? parse_slot(buf) : (uint8_t)DSLOT_NONE;

    gd_str(root, "treats", buf, sizeof buf, "");
    it.treats = buf[0] ? parse_treats(buf) : TREATS_NONE;

    gd_str(root, "potency", buf, sizeof buf, "");
    it.potency = parse_potency(buf);
    it.health  = (int16_t)gd_num(root, "health", 0);

    cJSON* tags = cJSON_GetObjectItem(root, "tags");
    if (cJSON_IsArray(tags)) {
        cJSON* t = nullptr;
        cJSON_ArrayForEach(t, tags) {
            if (it.tagCount >= ITEM_MAX_TAGS) break;
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(it.tags[it.tagCount], t->valuestring, sizeof(it.tags[0]) - 1);
                it.tags[it.tagCount][sizeof(it.tags[0]) - 1] = '\0';
                it.tagCount++;
            }
        }
    }

    // Toys nudge temperament the way foods do, through the ONE drift-block parser, so axis
    // names and order have a single owner and an item cannot invent a fifth axis.
    parse_drift(root, "drift", it.drift, 0.0f);
}

// Reject an entry that cannot work, loudly. The alternative -- storing it anyway -- puts a
// row in the Shop that does nothing when bought, which is far harder to diagnose from a
// serial log than a refusal at load.
static bool validate(const Item& it, const char* where)
{
    if (it.id[0] == '\0') { ESP_LOGW(TAG, "%s: entry with no id, skipped", where); return false; }
    if (it.kind >= ITEM_KIND_COUNT || it.kind == ITEM_FOOD) {
        ESP_LOGW(TAG, "%s: '%s' has an unknown kind, skipped (use toy/decor/care/special/keepsake)",
                 where, it.id);
        return false;
    }
    // A care item must DO something: cure a condition, restore HP, or both. A tonic that
    // only tops up HP is legitimate and declares no track at all.
    if (it.kind == ITEM_CARE && it.treats == TREATS_NONE && it.health == 0) {
        ESP_LOGW(TAG, "%s: care item '%s' does nothing, skipped "
                      "(needs treats: sick/injured, or health: N)", where, it.id);
        return false;
    }
    if (it.kind == ITEM_CARE && it.potency == 0) {
        ESP_LOGW(TAG, "%s: care item '%s' has an unknown potency, skipped (use step/full)",
                 where, it.id);
        return false;
    }
    if (it.kind == ITEM_DECOR && it.slot >= DSLOT_COUNT) {
        ESP_LOGW(TAG, "%s: decor '%s' has no valid slot, skipped "
                      "(use floor/wall/window/bed/feature)", where, it.id);
        return false;
    }
    return true;
}

bool ItemRegistry::parseFile(const char* path, Item& it)
{
    char* buf = gd_read_file(path, MAX_FILE_BYTES);
    if (!buf) return false;
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGW(TAG, "bad json: %s", path); return false; }
    parseEntry(root, it);
    cJSON_Delete(root);
    return true;
}

// A pack: <root>/items.json holding an ARRAY of item objects. One file open instead of one
// per item -- the same argument foods and conversations are packed on (a FAT open costs
// ~7-12 ms, and a cluster is allocated per file however small it is).
void ItemRegistry::scanPack(const char* root, const char* srcTag)
{
    char path[192];
    snprintf(path, sizeof path, "%s/items.json", root);
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
        Item it;
        parseEntry(e, it);
        if (!validate(it, path)) continue;             // pack entries need explicit ids
        int idx = upsert(it.id);
        if (idx < 0) { ESP_LOGW(TAG, "registry full; dropped '%s'", it.id); continue; }
        list_[idx] = it;
        if (gd_src_is_sd(srcTag)) gd_sd_loaded(GD_ITEMS);
        ESP_LOGI(TAG, "%s(pack): '%s' (%s) %s %uB", srcTag, it.id, it.name,
                 item_kind_name(it.kind), (unsigned)it.cost);
    }
    cJSON_Delete(arr);
}

void ItemRegistry::scanRoot(const char* root, const char* srcTag)
{
    DIR* d = opendir(root);
    if (!d) { ESP_LOGI(TAG, "no items dir at %s", root); return; }

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
        snprintf(cfg, sizeof cfg, "%s/item.json", dir);

        Item it;
        if (!parseFile(cfg, it)) continue;
        if (it.id[0] == '\0') {                    // no explicit id -> use the folder name
            strncpy(it.id, ent->d_name, sizeof(it.id) - 1);
            it.id[sizeof(it.id) - 1] = '\0';
        }
        if (!validate(it, cfg)) continue;

        int idx = upsert(it.id);
        if (idx < 0) { ESP_LOGW(TAG, "registry full; dropped '%s'", it.id); continue; }
        list_[idx] = it;                           // append or override (SD wins)
        if (gd_src_is_sd(srcTag)) gd_sd_loaded(GD_ITEMS);
        ESP_LOGI(TAG, "%s: '%s' (%s) %s %uB", srcTag, it.id, it.name,
                 item_kind_name(it.kind), (unsigned)it.cost);
    }
    closedir(d);
}

void ItemRegistry::loadAll()
{
    count_ = 0;
    gamedata_mount();                              // shared; idempotent

    char flashRoot[64];
    snprintf(flashRoot, sizeof flashRoot, "%s/items", GAMEDATA_ROOT);

    scanPack(flashRoot, "flash");                  // base game: pack first, then per-dir
    scanRoot(flashRoot, "flash");
    for (int i = 0; i < pakfs_count(); i++) {      // mod packs (later pak wins)
        char root[32];
        snprintf(root, sizeof root, "%s/items", pakfs_root(i));
        scanPack(root, "pak");
        scanRoot(root, "pak");
    }
    scanPack(SD_ROOT,   "sd");                     // loose-file mods: the final overlay
    scanRoot(SD_ROOT,   "sd");

    // No fallback item, unlike foods: feeding must never be impossible, but an empty item
    // roster is a perfectly valid game -- it is exactly what the base game ships until the
    // medicine set lands in E2.
    ESP_LOGI(TAG, "registry ready: %d items", count_);
}
