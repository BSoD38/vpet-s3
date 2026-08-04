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

// --- write-handle plumbing (see beginBatch in the header) --------------------------

bool SaveStore::acquire(uint32_t& h) const
{
    if (batchOpen_) { h = batch_; return true; }
    nvs_handle_t nh;
    if (nvs_open(NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return false;
    }
    h = nh;
    return true;
}

void SaveStore::release(uint32_t h) const
{
    if (batchOpen_) return;               // one commit+close in endBatch()
    nvs_commit((nvs_handle_t)h);
    nvs_close((nvs_handle_t)h);
}

void SaveStore::beginBatch() const
{
    if (batchOpen_) return;               // already inside one: keep using it
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;   // store* fall back to per-key opens
    batch_     = h;
    batchOpen_ = true;
}

void SaveStore::endBatch() const
{
    if (!batchOpen_) return;
    batchOpen_ = false;                   // clear FIRST so release() semantics can't recurse
    nvs_commit((nvs_handle_t)batch_);
    nvs_close((nvs_handle_t)batch_);
    batch_ = 0;
}

// --- typed accessors ---------------------------------------------------------------

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
    uint32_t h;
    if (!acquire(h)) return;
    nvs_set_blob((nvs_handle_t)h, KEY, &s, sizeof(s));
    release(h);
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
    uint32_t h;
    if (!acquire(h)) return;
    nvs_set_u8((nvs_handle_t)h, key, v);
    release(h);
}

uint32_t SaveStore::loadU32(const char* key, uint32_t def) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    uint32_t v = def;
    esp_err_t e = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return e == ESP_OK ? v : def;
}

void SaveStore::storeU32(const char* key, uint32_t v) const
{
    uint32_t h;
    if (!acquire(h)) return;
    nvs_set_u32((nvs_handle_t)h, key, v);
    release(h);
}

bool SaveStore::loadBlob(const char* key, void* out, unsigned size) const
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = size;
    esp_err_t e = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return e == ESP_OK && len == size;
}

void SaveStore::storeBlob(const char* key, const void* data, unsigned size) const
{
    uint32_t h;
    if (!acquire(h)) return;
    nvs_set_blob((nvs_handle_t)h, key, data, size);
    release(h);
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
    uint32_t h;
    if (!acquire(h)) return;
    nvs_set_str((nvs_handle_t)h, key, v);
    release(h);
}
