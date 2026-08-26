#include "gamedata.hpp"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "engine/gfx.hpp"      // rgb565, for gd_color
#include <cstdlib>
#include <cstdio>
#include <cstring>

static const char* TAG = "GDATA";

static void* json_malloc(size_t sz)
{
    void* p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    // Degrade rather than fail a parse: internal heap is smaller but always there.
    return p ? p : malloc(sz);
}

static void json_free(void* p)
{
    free(p);      // ESP-IDF's free() accepts pointers from any heap region
}

void gamedata_json_use_psram()
{
    cJSON_Hooks hooks;
    hooks.malloc_fn = json_malloc;
    hooks.free_fn   = json_free;
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON allocating from PSRAM");
}

char* gd_read_file(const char* path, long cap)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;                       // absent files are a normal probe, not an error
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > cap) {
        ESP_LOGW(TAG, "%s is %ld bytes (cap %ld) -- SKIPPED", path, n, cap);
        fclose(f);
        return nullptr;
    }
    char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (char*)malloc(n + 1);         // degrade to internal heap rather than fail
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

double gd_num(cJSON* o, const char* k, double def)
{
    if (!o) return def;
    cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

void gd_str(cJSON* o, const char* k, char* dst, int n, const char* def)
{
    const char* s = def;
    if (o) {
        cJSON* v = cJSON_GetObjectItem(o, k);
        if (cJSON_IsString(v) && v->valuestring) s = v->valuestring;
    }
    strncpy(dst, s, n - 1);
    dst[n - 1] = '\0';
}

bool gd_bool(cJSON* o, const char* k, bool def)
{
    if (!o) return def;
    cJSON* v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

uint16_t gd_color(cJSON* o, const char* k, uint16_t def)
{
    char s[12];
    gd_str(o, k, s, sizeof s, "");
    const char* p = (s[0] == '#') ? s + 1 : s;
    if (strlen(p) != 6) return def;
    char* end = nullptr;
    long v = strtol(p, &end, 16);
    if (end != p + 6) return def;
    return rgb565((uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)(v & 0xFF));
}

bool gamedata_mount()
{
    static bool tried = false;
    static bool ok    = false;
    if (tried) return ok;
    tried = true;

    esp_vfs_fat_mount_config_t cfg = {};
    // Generous: the conversation scan holds a directory handle open ACROSS frames while other
    // systems still open files (sprites, configs), so a tight limit would exhaust FDs.
    cfg.max_files = 8;
    cfg.format_if_mount_failed = false;

    esp_err_t e = esp_vfs_fat_spiflash_mount_ro(GAMEDATA_ROOT, "gamedata", &cfg);
    ok = (e == ESP_OK);
    if (!ok) ESP_LOGW(TAG, "gamedata mount failed (%s)", esp_err_to_name(e));
    else     ESP_LOGI(TAG, "mounted %s", GAMEDATA_ROOT);
    return ok;
}

// --- loose-SD mod accounting (see gamedata.hpp) ------------------------------------------
// Written during the boot-time loads (single-threaded, before the game task starts) and read
// by the About screen afterwards, so plain ints are enough.
static int s_sdLoaded[GD_SYS_COUNT] = { 0 };

void gd_sd_loaded(GdSystem s, int n) { if (s < GD_SYS_COUNT && n > 0) s_sdLoaded[s] += n; }
int  gd_sd_count(GdSystem s)  { return s < GD_SYS_COUNT ? s_sdLoaded[s] : 0; }

int gd_sd_total()
{
    int n = 0;
    for (int i = 0; i < GD_SYS_COUNT; i++) n += s_sdLoaded[i];
    return n;
}

const char* gd_sd_name(GdSystem s)
{
    switch (s) {
        case GD_CREATURES: return "Creatures";
        case GD_FOODS:     return "Foods";
        case GD_ITEMS:     return "Items";
        case GD_NATURES:   return "Natures";
        case GD_TRAITS:    return "Traits";
        case GD_SOUNDS:    return "Sounds";
        case GD_CONFIG:    return "Config";
        default:           return "?";
    }
}

bool gd_src_is_sd(const char* srcTag)
{
    return srcTag && srcTag[0] == 's' && srcTag[1] == 'd' && srcTag[2] == '\0';
}
