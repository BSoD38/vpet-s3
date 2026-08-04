#pragma once

// The read-only `gamedata` flash partition holds every data-driven system's base
// content (foods now, conversations next), mounted once at /gamedata. Kept as a shared
// helper because several registries need the same mount and FAT will not mount twice.
//
// Base content is packed into the FAT image at build time from flash_gamedata/ (see
// main/CMakeLists.txt); SD-card mods live under /sdcard/<system>/ and are scanned
// separately by each registry.

inline constexpr const char* GAMEDATA_ROOT = "/gamedata";

struct cJSON;

// Mount the partition if it isn't already. Idempotent; returns true if /gamedata is
// usable. Safe to call from every registry's loadAll().
bool gamedata_mount();

// --- shared JSON-loading helpers -------------------------------------------------------
// Every data-driven registry (foods, personalities, conversations) reads whole JSON files
// and picks typed fields out of cJSON objects. These are THE shared implementations: the
// per-registry copies had already diverged (different size caps, internal-heap vs PSRAM
// buffers, silent vs logged skips), which is exactly how an oversized data file ends up
// vanishing with no trace in one system but not another.

// Read a whole file into a null-terminated buffer (caller free()s). The buffer comes from
// PSRAM (parse buffers are transient and read sequentially, so slow memory costs nothing
// and scarce internal heap is spared). A file over `cap` bytes is skipped WITH a warning:
// silently dropping content is the one failure mode this layer must never have. Returns
// nullptr if the file doesn't exist (probing optional files is normal and stays quiet).
char* gd_read_file(const char* path, long cap);

// Typed field accessors: missing key or wrong type returns `def`.
double gd_num(cJSON* o, const char* k, double def);
void   gd_str(cJSON* o, const char* k, char* dst, int n, const char* def);
bool   gd_bool(cJSON* o, const char* k, bool def);

// Route ALL cJSON allocation to PSRAM. cJSON's hooks are process-global, so this is a policy
// decision for the whole app and must be made before anything parses -- call it once from
// App::init() ahead of the registry loads.
//
// Why: a parse tree runs several times the size of its source (cJSON spends ~48 bytes per value
// plus duplicated strings), so a large conversation would spike tens of KB of scarce internal
// heap, transiently, for every candidate a scan touches. PSRAM has megabytes free and the parse
// is not on the render path. Falls back to internal heap if PSRAM can't serve a request, so a
// parse degrades rather than failing outright.
void gamedata_json_use_psram();
