#include "engine/fw_update.hpp"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "psa/crypto.h"   // IDF v6 / mbedtls 4: hashing is PSA-only (mbedtls/sha256.h is private)
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>

static const char* TAG       = "FWUP";
static const char* kBinPath  = "/sdcard/update.bin";   // legacy manifest-less fallback
static const char* kManifest = "/sdcard/update.json";
static const size_t BUFSZ    = 16 * 1024;

const char* fw_current_version() { return esp_app_get_description()->version; }

// ---------------------------------------------------------------------------------------
// Manifest (update.json): sizes + sha256 of the app image and each data-partition image.
// The shas let us (a) skip pieces that are already installed and (b) verify writes.

struct ManEntry {
    char     part[17];   // data partition label ("" for the app entry)
    char     path[80];   // absolute path on the card
    uint32_t size = 0;
    uint8_t  sha[32];
};
struct Manifest {
    char     project[33];
    ManEntry app;
    ManEntry data[4];
    int      nData = 0;
};

static bool hex2bytes(const char* s, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%02x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool parse_entry(const cJSON* o, ManEntry& e)
{
    const cJSON* file = cJSON_GetObjectItem(o, "file");
    const cJSON* size = cJSON_GetObjectItem(o, "size");
    const cJSON* sha  = cJSON_GetObjectItem(o, "sha256");
    if (!cJSON_IsString(file) || !cJSON_IsNumber(size) || !cJSON_IsString(sha)
        || strlen(sha->valuestring) != 64)
        return false;
    snprintf(e.path, sizeof e.path, "/sdcard/%s", file->valuestring);
    e.size = (uint32_t)size->valuedouble;
    return hex2bytes(sha->valuestring, e.sha, 32) && e.size > 0;
}

static bool load_manifest(Manifest& m)
{
    m = Manifest{};
    FILE* f = fopen(kManifest, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    char* txt = (char*)malloc(sz + 1);
    if (!txt) { fclose(f); return false; }
    bool readOk = fread(txt, 1, sz, f) == (size_t)sz;
    fclose(f);
    txt[sz] = '\0';

    cJSON* root = readOk ? cJSON_Parse(txt) : nullptr;
    free(txt);
    if (!root) { ESP_LOGW(TAG, "update.json unreadable -- treating card as app-only"); return false; }

    bool ok = false;
    const cJSON* proj = cJSON_GetObjectItem(root, "project");
    const cJSON* app  = cJSON_GetObjectItem(root, "app");
    if (cJSON_IsString(proj) && cJSON_IsObject(app) && parse_entry(app, m.app)) {
        snprintf(m.project, sizeof m.project, "%.32s", proj->valuestring);
        const cJSON* data = cJSON_GetObjectItem(root, "data");
        const cJSON* it;
        cJSON_ArrayForEach(it, data) {
            if (m.nData >= (int)(sizeof m.data / sizeof m.data[0])) break;
            const cJSON* part = cJSON_GetObjectItem(it, "partition");
            ManEntry& e = m.data[m.nData];
            if (cJSON_IsString(part) && parse_entry(it, e)) {
                snprintf(e.part, sizeof e.part, "%.16s", part->valuestring);
                m.nData++;
            }
        }
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------------------
// Helpers

static const esp_partition_t* find_data_part(const char* name)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, name);
}

// sha256 of the first `len` bytes of a partition. Flash reads are fast (tens of MB/s), so
// hashing the ~4 MB an update can touch stays well under a second.
static bool sha256_partition(const esp_partition_t* p, uint32_t len, uint8_t out[32],
                             uint8_t* buf, size_t bufsz)
{
    if (len > p->size) return false;
    static bool psaReady = false;                    // psa_crypto_init is idempotent-once
    if (!psaReady) psaReady = psa_crypto_init() == PSA_SUCCESS;
    if (!psaReady) return false;

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    bool ok = true;
    for (uint32_t off = 0; ok && off < len; ) {
        size_t n = (len - off < bufsz) ? (len - off) : bufsz;
        ok = esp_partition_read(p, off, buf, n) == ESP_OK
          && psa_hash_update(&op, buf, n) == PSA_SUCCESS;
        off += n;
    }
    size_t olen = 0;
    ok = ok && psa_hash_finish(&op, out, 32, &olen) == PSA_SUCCESS && olen == 32;
    if (!ok) psa_hash_abort(&op);                    // finish() cleans up on success
    return ok;
}

static bool part_matches(const esp_partition_t* p, const ManEntry& e, uint8_t* buf, size_t bufsz)
{
    uint8_t h[32];
    return sha256_partition(p, e.size, h, buf, bufsz) && memcmp(h, e.sha, 32) == 0;
}

// "Data update in progress" flag. Set before the first in-place data write, cleared after
// the last one verifies -- if power dies in between, fw_data_recovery() sees it at boot.
static void set_data_flag(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("fwup", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "dpend", v);
    nvs_commit(h);
    nvs_close(h);
}
static uint8_t get_data_flag()
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("fwup", NVS_READONLY, &h) != ESP_OK) return 0;   // namespace not created yet
    nvs_get_u8(h, "dpend", &v);
    nvs_close(h);
    return v;
}

// Stream a card image into a data partition (erase-as-we-go), then verify by re-hashing
// the flash against the manifest -- this also catches a corrupt file on the card.
// NOT crash-atomic (no spare copy): callers bracket it with the NVS flag.
static const char* write_data_partition(const ManEntry& e, const esp_partition_t* p,
                                        FwProgressFn progress, void* user,
                                        uint8_t* buf, size_t bufsz)
{
    FILE* f = fopen(e.path, "rb");
    if (!f) return "A data file is missing on the card";
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if ((uint32_t)fsz != e.size) { fclose(f); return "Data file size mismatch"; }

    ESP_LOGI(TAG, "writing %lu bytes to data partition %s", (unsigned long)e.size, e.part);
    if (progress) progress(e.part, 0, user);
    esp_err_t err = ESP_OK;
    uint32_t  off = 0;
    int       lastPct = 0;
    while (off < e.size && err == ESP_OK) {
        size_t n = fread(buf, 1, bufsz, f);
        if (n == 0) { err = ESP_FAIL; break; }
        uint32_t er = (uint32_t)((n + 4095) & ~4095u);      // erase length: 4K multiples
        if (off + er > p->size) er = p->size - off;
        err = esp_partition_erase_range(p, off, er);
        if (err == ESP_OK) err = esp_partition_write(p, off, buf, n);
        off += (uint32_t)n;
        int pct = (int)((uint64_t)off * 100 / e.size);
        if (pct != lastPct && progress) { progress(e.part, pct, user); lastPct = pct; }
    }
    fclose(f);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "data write %s failed at %lu: %s", e.part, (unsigned long)off, esp_err_to_name(err));
        return "Writing a data partition failed";
    }
    if (!part_matches(p, e, buf, bufsz)) return "Data verification failed";
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// Public API

FwProbe fw_probe(FwInfo& out)
{
    out = FwInfo{};
    const esp_app_desc_t* cur = esp_app_get_description();
    snprintf(out.curVersion, sizeof out.curVersion, "%.31s", cur->version);

    // The SD card is mounted once at boot; if the VFS root doesn't open, there is no card.
    DIR* d = opendir("/sdcard");
    if (!d) return FwProbe::NoCard;
    closedir(d);

    Manifest m;
    bool hasMan = load_manifest(m);
    const char* binPath = hasMan ? m.app.path : kBinPath;

    FILE* f = fopen(binPath, "rb");
    if (!f) return FwProbe::NoFile;

    // The fields of interest sit at fixed offsets in every app image: esp_image_header_t,
    // then the first segment header, then esp_app_desc_t (version/project/build date).
    esp_image_header_t         img;
    esp_image_segment_header_t seg;
    esp_app_desc_t             desc;
    bool ok = fread(&img,  1, sizeof img,  f) == sizeof img
           && fread(&seg,  1, sizeof seg,  f) == sizeof seg
           && fread(&desc, 1, sizeof desc, f) == sizeof desc;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);

    if (!ok || sz <= 0
        || img.magic       != ESP_IMAGE_HEADER_MAGIC
        || img.chip_id     != ESP_CHIP_ID_ESP32S3
        || desc.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return FwProbe::BadImage;

    out.size = (uint32_t)sz;
    snprintf(out.version, sizeof out.version, "%.31s", desc.version);
    snprintf(out.built,   sizeof out.built,   "%.15s %.15s", desc.date, desc.time);
    out.sameVersion = strncmp(desc.version, cur->version, sizeof desc.version) == 0;

    if (strncmp(desc.project_name, cur->project_name, sizeof desc.project_name) != 0)
        return FwProbe::WrongProject;
    if (hasMan && strncmp(m.project, cur->project_name, sizeof m.project) != 0)
        return FwProbe::WrongProject;

    const esp_partition_t* dst = esp_ota_get_next_update_partition(nullptr);
    if (!dst) return FwProbe::NoSlot;
    if (out.size > dst->size) return FwProbe::TooBig;

    out.hasManifest = hasMan;
    if (!hasMan) return FwProbe::Ok;             // legacy card: app-only, always installable

    uint8_t* buf = (uint8_t*)heap_caps_malloc(BUFSZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return FwProbe::Ok;                // can't hash: fail open, treat as pending

    // App pending = the running slot's first size bytes differ from the card image's sha.
    out.appPending = m.app.size != out.size
                  || !part_matches(esp_ota_get_running_partition(), m.app, buf, BUFSZ);

    bool anyData = false;
    for (int i = 0; i < m.nData; i++) {
        const ManEntry& e = m.data[i];
        const esp_partition_t* p = find_data_part(e.part);
        if (!p || e.size > p->size) {
            // A manifest for a partition this table doesn't have (or can't hold) -- that
            // combination can only ship over USB anyway, so skip it rather than fail.
            ESP_LOGW(TAG, "manifest entry '%s' has no matching partition -- ignored", e.part);
            continue;
        }
        FwDataStatus& s = out.data[out.dataCount++];
        snprintf(s.name, sizeof s.name, "%s", e.part);
        s.size    = e.size;
        s.pending = !part_matches(p, e, buf, BUFSZ);
        anyData  |= s.pending;
    }
    free(buf);

    return (!out.appPending && !anyData) ? FwProbe::UpToDate : FwProbe::Ok;
}

const char* fw_apply(FwProgressFn progress, void* user)
{
    Manifest m;
    bool hasMan = load_manifest(m);
    const char* binPath = hasMan ? m.app.path : kBinPath;

    uint8_t* buf = (uint8_t*)heap_caps_malloc(BUFSZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return "Out of memory";

    // --- 1. app image -> inactive slot (skipped when the card app is already installed).
    //     The boot choice is NOT flipped yet: if a later data write dies, the old app
    //     still boots and fw_data_recovery() heals the data.
    const esp_partition_t* dst = nullptr;
    bool appPending = !hasMan
                   || !part_matches(esp_ota_get_running_partition(), m.app, buf, BUFSZ);
    if (appPending) {
        FILE* f = fopen(binPath, "rb");
        if (!f) { free(buf); return "Could not open the app image"; }
        fseek(f, 0, SEEK_END);
        long total = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (total <= 0) { fclose(f); free(buf); return "App image is empty"; }

        dst = esp_ota_get_next_update_partition(nullptr);
        if (!dst) { fclose(f); free(buf); return "No inactive app slot"; }

        // SEQUENTIAL_WRITES erases the slot incrementally as data arrives instead of
        // blocking for a whole-partition erase up front, so the bar moves immediately.
        esp_ota_handle_t h  = 0;
        esp_err_t       err = esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h);
        if (err != ESP_OK) { fclose(f); free(buf); return "Could not start the update"; }

        ESP_LOGI(TAG, "writing %ld bytes to %s", total, dst->label);
        if (progress) progress("firmware", 0, user);
        long done = 0;
        int  lastPct = 0;
        while (done < total && err == ESP_OK) {
            size_t n = fread(buf, 1, BUFSZ, f);
            if (n == 0) { err = ESP_FAIL; break; }    // short read = card yanked / IO error
            err = esp_ota_write(h, buf, n);
            done += (long)n;
            int pct = (int)(done * 100 / total);
            if (pct != lastPct && progress) { progress("firmware", pct, user); lastPct = pct; }
        }
        fclose(f);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            free(buf);
            ESP_LOGE(TAG, "app write failed at %ld/%ld: %s", done, total, esp_err_to_name(err));
            return "Writing the firmware failed";
        }
        err = esp_ota_end(h);   // full image validation: structure + embedded SHA-256
        if (err != ESP_OK) {
            free(buf);
            ESP_LOGE(TAG, "validation failed: %s", esp_err_to_name(err));
            return (err == ESP_ERR_OTA_VALIDATE_FAILED) ? "Firmware failed validation"
                                                        : "Could not finish the update";
        }
    }

    // --- 2. stale data partitions, in place. Flag bracketed: a power cut in here is
    //     healed from the card at next boot.
    if (hasMan) {
        bool started = false;
        for (int i = 0; i < m.nData; i++) {
            const ManEntry& e = m.data[i];
            const esp_partition_t* p = find_data_part(e.part);
            if (!p || e.size > p->size) continue;                  // logged by probe already
            if (part_matches(p, e, buf, BUFSZ)) continue;          // already installed
            if (!started) { set_data_flag(1); started = true; }
            const char* derr = write_data_partition(e, p, progress, user, buf, BUFSZ);
            if (derr) { free(buf); return derr; }                  // flag stays set -> boot repair
        }
        if (started) set_data_flag(0);
    }
    free(buf);

    // --- 3. only now hand the next boot to the new app.
    if (dst && esp_ota_set_boot_partition(dst) != ESP_OK)
        return "Could not select the new slot";
    ESP_LOGI(TAG, "update installed (%s); reboot to run it", dst ? dst->label : "data only");
    return nullptr;
}

void fw_data_recovery(FwProgressFn progress, void* user)
{
    if (get_data_flag() == 0) return;
    ESP_LOGW(TAG, "interrupted data update detected");

    Manifest m;
    if (!load_manifest(m) || m.nData == 0) {
        // Without the card we can't repair -- boot on (data may be stale or corrupt) and
        // keep the flag so the next boot with the card in retries.
        ESP_LOGE(TAG, "no update manifest on the card; data partitions may be corrupt. "
                      "Insert the update card and reboot to repair.");
        return;
    }
    if (strncmp(m.project, esp_app_get_description()->project_name, sizeof m.project) != 0) {
        ESP_LOGE(TAG, "card manifest is for another project; not repairing from it");
        return;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(BUFSZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return;
    bool allOk = true;
    for (int i = 0; i < m.nData; i++) {
        const ManEntry& e = m.data[i];
        const esp_partition_t* p = find_data_part(e.part);
        if (!p || e.size > p->size) { allOk = false; continue; }
        if (part_matches(p, e, buf, BUFSZ)) continue;   // this one finished before the cut
        const char* err = write_data_partition(e, p, progress, user, buf, BUFSZ);
        if (err) { ESP_LOGE(TAG, "repair of '%s' failed: %s", e.part, err); allOk = false; }
    }
    free(buf);
    if (allOk) {
        set_data_flag(0);
        ESP_LOGI(TAG, "data partitions repaired from the card");
    }
}

void fw_confirm_running_image()
{
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t   st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "first boot after update: %s marked valid, rollback cancelled", run->label);
    }
}
