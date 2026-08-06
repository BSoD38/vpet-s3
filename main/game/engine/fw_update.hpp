#pragma once
#include <cstdint>

// SD-card firmware + game-data updates (see docs/firmware-updates.md).
//
// tools/make_update.py packages the card root with: update.bin (app image), one
// <partition>.img per FAT data partition (creatures, gamedata), and update.json — a
// manifest with the size + sha256 of each piece.
//
// The app streams into the app slot NOT currently running (ota_0/ota_1 A/B scheme), so a
// failed install can never damage the running game; the bootloader ROLLS BACK if the new
// firmware doesn't reach fw_confirm_running_image() (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
//
// Data partitions have no spare copy — they are rewritten IN PLACE, so only the ones whose
// content actually differs (manifest sha vs a hash of the installed partition) are touched,
// and an NVS flag brackets the writes: if power is cut mid-write, fw_data_recovery() redoes
// them from the card on the next boot, before anything mounts that data.
//
// A card with only update.bin (no manifest) still works as an app-only update.
// Wi-Fi OTA later reuses all of this: only the byte source changes.

struct FwDataStatus {
    char     name[17];        // data partition label (creatures, gamedata, ...)
    uint32_t size    = 0;     // image size in bytes
    bool     pending = false; // card content differs from what's installed
};

struct FwInfo {
    char     version[32];        // candidate image's embedded version (esp_app_desc)
    char     curVersion[32];     // currently running version
    char     built[36];          // candidate build date + time
    uint32_t size        = 0;    // candidate app file size in bytes
    bool     sameVersion = false;
    // From update.json (absent on a legacy app-only card):
    bool         hasManifest = false;
    bool         appPending  = true;  // app bytes differ from the running slot
    int          dataCount   = 0;     // data partitions listed in the manifest
    FwDataStatus data[4];
};

enum class FwProbe {
    NoCard,        // SD never mounted (no card at boot)
    NoFile,        // card mounted but no app image on it
    BadImage,      // wrong magic / chip / missing app descriptor
    WrongProject,  // a valid ESP32-S3 app, but not this game
    TooBig,        // does not fit an app slot
    NoSlot,        // no inactive app slot found (partition table problem)
    UpToDate,      // manifest present and NOTHING differs — install would be a no-op
    Ok
};

// Progress sink for the long operations: `stage` is "firmware" or a data partition label.
using FwProgressFn = void (*)(const char* stage, int pct, void* user);

// Inspect the card: header reads + one hash pass over the affected partitions (well under
// a second) — fine for a scene's onEnter().
FwProbe fw_probe(FwInfo& out);

// Apply whatever differs: app into the inactive slot, stale data partitions in place,
// then flip the boot choice (only if the app changed). Blocks for the duration.
// Returns nullptr on success (caller reboots), or a static error string on failure —
// the running app is untouched; interrupted DATA writes are healed at next boot.
const char* fw_apply(FwProgressFn progress, void* user);

// First boot after an app update runs in PENDING_VERIFY state: call once the game is
// demonstrably alive (end of App::init) to cancel the bootloader's rollback. No-op on
// ordinary boots.
void fw_confirm_running_image();

// Boot-time repair: if a previous install was interrupted mid data-partition write, redo
// those writes from the card. MUST run before creatures/gamedata are mounted/parsed, and
// after cJSON allocators are configured. No-op when nothing was interrupted.
void fw_data_recovery(FwProgressFn progress, void* user);

// Version string of the running app (from its embedded esp_app_desc).
const char* fw_current_version();
