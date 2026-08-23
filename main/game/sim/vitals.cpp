#include "vitals.hpp"
#include "gamedata.hpp"
#include "engine/pakfs.hpp"      // mounted mod packs are extra config roots
#include "esp_log.h"
#include "cJSON.h"
#include <cstdio>
#include <cstdlib>

static const char* TAG = "VITALS";

static VitalsTuning g_tuning;    // compiled defaults until vitals_load_tuning() runs

const VitalsTuning& vitals_tuning() { return g_tuning; }

const char* condition_marker(Condition c)
{
    switch (c) {
        case COND_SICK:     return "SICK";
        case COND_SICK_BAD: return "VERY SICK";
        case COND_INJURED:  return "HURT";
        case COND_CRITICAL: return "CRITICAL";
        case COND_RECOVERY: return "RESTING";
        default:            return nullptr;
    }
}

const char* life_track_id(LifeTrack t)
{
    switch (t) {
        case LIFE_ELDERLY:  return "elderly";
        case LIFE_TWILIGHT: return "twilight";
        default:            return "prime";
    }
}

float vitals_effective_max(uint8_t scars)
{
    float m = g_tuning.vitMax;
    for (uint8_t i = 0; i < scars; i++) m *= g_tuning.scarMult;
    return m;
}

// Apply one config file OVER the current values: every key is optional, and a file that
// sets only "multWorst" leaves everything else exactly as the previous root (or the
// compiled defaults) had it. That per-key override is what makes a one-line loose-SD
// file a real tuning workflow instead of a full-copy maintenance burden.
// Returns true if a config was actually read and applied (the loose-SD layer reports that to
// the About screen's mod tally; every other caller ignores it).
static bool apply_file(const char* path)
{
    char* buf = gd_read_file(path, 8192);
    if (!buf) return false;                  // no config at this root: fine
    cJSON* o = cJSON_Parse(buf);
    free(buf);
    if (!o) { ESP_LOGW(TAG, "bad json: %s", path); return false; }
    VitalsTuning& t = g_tuning;
    t.vitMax          = (float)gd_num(o, "vitMax",          t.vitMax);
    t.baseDrainPerDay = (float)gd_num(o, "baseDrainPerDay", t.baseDrainPerDay);
    t.multBest        = (float)gd_num(o, "multBest",        t.multBest);
    t.multWorst       = (float)gd_num(o, "multWorst",       t.multWorst);
    t.emaTauHrs       = (float)gd_num(o, "emaTauHrs",       t.emaTauHrs);
    t.mistakeChip     = (float)gd_num(o, "mistakeChip",     t.mistakeChip);
    t.sickBadPerDay   = (float)gd_num(o, "sickBadPerDay",   t.sickBadPerDay);
    t.sickEscalateHrs = (float)gd_num(o, "sickEscalateHrs", t.sickEscalateHrs);
    t.injuryFesterHrs = (float)gd_num(o, "injuryFesterHrs", t.injuryFesterHrs);
    t.elderlyFrac     = (float)gd_num(o, "elderlyFrac",     t.elderlyFrac);
    t.twilightFrac    = (float)gd_num(o, "twilightFrac",    t.twilightFrac);
    t.elderlyStepMult       = (float)gd_num(o, "elderlyStepMult",       t.elderlyStepMult);
    t.twilightStepMult      = (float)gd_num(o, "twilightStepMult",      t.twilightStepMult);
    t.elderlyExtraSleepHrs  = (float)gd_num(o, "elderlyExtraSleepHrs",  t.elderlyExtraSleepHrs);
    t.twilightExtraSleepHrs = (float)gd_num(o, "twilightExtraSleepHrs", t.twilightExtraSleepHrs);
    t.miracleMax       = (float)gd_num(o, "miracleMax",       t.miracleMax);
    t.miracleBondFloor = (float)gd_num(o, "miracleBondFloor", t.miracleBondFloor);
    t.critRestoreFrac  = (float)gd_num(o, "critRestoreFrac",  t.critRestoreFrac);
    t.scarMult         = (float)gd_num(o, "scarMult",         t.scarMult);
    t.reprieveFrac     = (float)gd_num(o, "reprieveFrac",     t.reprieveFrac);
    t.recoveryDays     = (float)gd_num(o, "recoveryDays",     t.recoveryDays);
    cJSON_Delete(o);
    ESP_LOGI(TAG, "tuning <- %s", path);
    return true;
}

void vitals_load_tuning()
{
    gamedata_mount();                                   // shared; idempotent
    char path[64];
    snprintf(path, sizeof path, "%s/config/vitals.json", GAMEDATA_ROOT);
    apply_file(path);                                   // loose flash (normally absent)
    for (int i = 0; i < pakfs_count(); i++) {           // base.pak, then mod paks (later wins)
        snprintf(path, sizeof path, "%s/config/vitals.json", pakfs_root(i));
        apply_file(path);
    }
    if (apply_file("/sdcard/config/vitals.json"))       // loose SD: the final overlay
        gd_sd_loaded(GD_CONFIG);
    ESP_LOGI(TAG, "vitality max %.0f, base %.1f/day, care x%.2f..x%.2f, sickBad +%.0f/day",
             g_tuning.vitMax, g_tuning.baseDrainPerDay,
             g_tuning.multBest, g_tuning.multWorst, g_tuning.sickBadPerDay);
}
