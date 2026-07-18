#include "save.hpp"
#include "pet.hpp"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char* TAG = "SAVE";
static const char* NS  = "pet";
static const char* KEY = "state";

void save_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool SaveStore::load(PetState& s) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(s);
    esp_err_t e = nvs_get_blob(h, KEY, &s, &len);
    nvs_close(h);
    return e == ESP_OK && len == sizeof(s);
}

void SaveStore::store(const PetState& s) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_blob(h, KEY, &s, sizeof(s));
    nvs_commit(h);
    nvs_close(h);
}

uint8_t SaveStore::loadU8(const char* key, uint8_t def) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    uint8_t v = def;
    esp_err_t e = nvs_get_u8(h, key, &v);
    nvs_close(h);
    return e == ESP_OK ? v : def;
}

void SaveStore::storeU8(const char* key, uint8_t v) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void SaveStore::loadStr(const char* key, char* out, int n, const char* def) const
{
    strncpy(out, def, n - 1);          // default first
    out[n - 1] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = (size_t)n;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (e != ESP_OK) { strncpy(out, def, n - 1); out[n - 1] = '\0'; }   // restore default on miss
}

void SaveStore::storeStr(const char* key, const char* v) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}
