#include "personality.hpp"
#include "gamedata.hpp"
#include "save.hpp"
#include "pet.hpp"               // LifeStage
#include "engine/pakfs.hpp"      // mounted mod packs are extra scan roots
#include "esp_log.h"
#include "cJSON.h"
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>

static const char* TAG = "PERS";

static const char* SD_NATURES = "/sdcard/natures";
static const char* SD_TRAITS  = "/sdcard/personalities";

// --- tuning (all validated together by tools/personality_sim.py) ------------------------
// EMA rate per nudge: identity tracks SUSTAINED behaviour (~50 notable actions of memory),
// so one unusual afternoon can't rewrite who the creature is.
static const float ALPHA = 0.02f;
// A challenger must beat the incumbent by this margin, and hold it, before identity changes.
static const float MARGIN = 0.08f;
// ...and hold it for this long in sim-seconds (~1h), so traits don't flicker at a boundary.
static const float DWELL_SECS = 3600.0f;
// Minimum drift magnitude before a Nature will crystallize. This is also what keeps an
// EXISTING save (which has no drift history) honest: it stays Unformed until the player has
// actually done enough for the game to have an opinion, rather than being assigned a
// personality from an all-zero vector, where every trait ties and the first one would win.
static const float CRYSTAL_MIN_MAG = 0.15f;
// How much better a rival Nature must be at evolution time to take over. Deliberately steep:
// jumping "way of being" should reward a long, consistent change of play.
static const float NATURE_JUMP_MARGIN = 0.35f;

static const uint32_t PERS_MAGIC   = 0x50525301;   // 'PRS\1'
static const uint16_t PERS_VERSION = 1;
static const char*    PERS_KEY     = "pers";

// --- helpers (file/JSON access shared via sim/gamedata) --------------------------

static const long MAX_FILE_BYTES = 32768;

// The shared axis-block parser (declared in personality.hpp; foods and conversations
// use it too, so axis names/order have exactly one interpretation in firmware).
void parse_drift(cJSON* o, const char* key, float out[AX_COUNT], float def)
{
    for (int i = 0; i < AX_COUNT; i++) out[i] = def;
    cJSON* b = o ? cJSON_GetObjectItem(o, key) : nullptr;
    if (!b) return;
    out[AX_BRAVE]     = (float)gd_num(b, "brave",     def);
    out[AX_ENERGETIC] = (float)gd_num(b, "energetic", def);
    out[AX_SOCIAL]    = (float)gd_num(b, "social",    def);
    out[AX_WILD]      = (float)gd_num(b, "wild",      def);
}

float drift_score(const float axis[AX_COUNT], const float ideal[AX_COUNT],
                  const float weight[AX_COUNT])
{
    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < AX_COUNT; i++) {
        const float w = weight[i];
        num += w * axis[i] * ideal[i];
        den += w * ideal[i] * ideal[i];
    }
    // An axis a trait doesn't declare has ideal 0, so it drops out of BOTH sums -- which is
    // why "ignoring" an axis and giving it a zero ideal are the same thing here.
    return den > 0.0f ? num / sqrtf(den) : 0.0f;
}

// --- registry ------------------------------------------------------------------

int PersonalityRegistry::natureIndexOf(const char* id) const
{
    for (int i = 0; i < natures_; i++) if (strcmp(nat_[i].id, id) == 0) return i;
    return -1;
}

int PersonalityRegistry::traitIndexOf(const char* id) const
{
    for (int i = 0; i < traits_; i++) if (strcmp(tr_[i].id, id) == 0) return i;
    return -1;
}

int PersonalityRegistry::bestNature(const float axis[AX_COUNT], float* outScore) const
{
    int best = -1;
    float bs = -1e9f;
    static const float W1[AX_COUNT] = { 1, 1, 1, 1 };
    for (int i = 0; i < natures_; i++) {
        float s = drift_score(axis, nat_[i].ideal, W1);
        if (s > bs) { bs = s; best = i; }
    }
    if (outScore) *outScore = bs;
    return best;
}

int PersonalityRegistry::bestTrait(int natureIdx, const float axis[AX_COUNT], float* outScore) const
{
    int best = -1;
    float bs = -1e9f;
    for (int i = 0; i < traits_; i++) {
        if (tr_[i].natureIdx != natureIdx) continue;
        float s = drift_score(axis, tr_[i].ideal, tr_[i].weight);
        if (s > bs) { bs = s; best = i; }
    }
    if (outScore) *outScore = bs;
    return best;
}

// Both registries share the same walk: every *.json in a directory holds an ARRAY of
// entries (so a mod ships one file instead of a directory per trait); only the per-entry
// parsing differs. One walker means a robustness fix (size cap, logging, bad-JSON skip)
// can never apply to natures but not traits.
template <typename PerEntry>
static void scan_array_dir(const char* dir, PerEntry&& each)
{
    DIR* d = opendir(dir);
    if (!d) { ESP_LOGI(TAG, "no data dir at %s", dir); return; }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char path[224];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        char* buf = gd_read_file(path, MAX_FILE_BYTES);
        if (!buf) continue;
        cJSON* root = cJSON_Parse(buf);
        free(buf);
        if (!cJSON_IsArray(root)) { ESP_LOGW(TAG, "not a json array: %s", path); cJSON_Delete(root); continue; }
        cJSON* e = nullptr;
        cJSON_ArrayForEach(e, root) each(e);
        cJSON_Delete(root);
    }
    closedir(d);
}

void PersonalityRegistry::scanNatures(const char* dir, const char* srcTag)
{
    scan_array_dir(dir, [&](cJSON* e) {
        Nature n;
        memset(&n, 0, sizeof n);
        gd_str(e, "id",   n.id,   sizeof n.id,   "");
        gd_str(e, "name", n.name, sizeof n.name, n.id);
        parse_drift(e, "ideal", n.ideal, 0.0f);
        if (!n.id[0]) return;

        int idx = natureIndexOf(n.id);
        if (idx < 0) {
            if (natures_ >= MAX_NATURES) { ESP_LOGW(TAG, "natures full; dropped '%s'", n.id); return; }
            idx = natures_++;
        }
        nat_[idx] = n;                              // append or override (SD wins)
        if (gd_src_is_sd(srcTag)) gd_sd_loaded(GD_NATURES);
        ESP_LOGI(TAG, "%s nature: '%s' (%s)", srcTag, n.id, n.name);
    });
}

void PersonalityRegistry::scanTraits(const char* dir, const char* srcTag)
{
    scan_array_dir(dir, [&](cJSON* e) {
        {
            Personality t;
            memset(&t, 0, sizeof t);
            gd_str(e, "id",     t.id,       sizeof t.id,       "");
            gd_str(e, "name",   t.name,     sizeof t.name,     t.id);
            gd_str(e, "nature", t.natureId, sizeof t.natureId, "");
            // A trait's ideal is a DIRECTION in the surface plane, so the readable way to
            // author it is an angle in degrees: 0 = energetic, 90 = wild, 180 = calm,
            // 270 = disciplined. (Raw cos/sin pairs are the same thing but unreadable, and
            // this matches how the design doc and tools/personality_sim.py reason about it.)
            // An explicit "ideal" block still works, and is the only way to lean on a CORE
            // axis -- so mods keep full generality.
            cJSON* ang = cJSON_GetObjectItem(e, "angle");
            if (cJSON_IsNumber(ang)) {
                const float r = (float)(ang->valuedouble * 3.14159265358979 / 180.0);
                for (int i = 0; i < AX_COUNT; i++) t.ideal[i] = 0.0f;
                t.ideal[AX_ENERGETIC] = cosf(r);
                t.ideal[AX_WILD]      = sinf(r);
            } else {
                parse_drift(e, "ideal", t.ideal, 0.0f);
            }
            parse_drift(e, "weight", t.weight, 1.0f);     // unspecified axes weigh 1
            t.natureIdx = -1;
            if (!t.id[0]) return;

            int idx = traitIndexOf(t.id);
            if (idx < 0) {
                if (traits_ >= MAX_TRAITS) { ESP_LOGW(TAG, "traits full; dropped '%s'", t.id); return; }
                idx = traits_++;
            }
            tr_[idx] = t;
            if (gd_src_is_sd(srcTag)) gd_sd_loaded(GD_TRAITS);
            ESP_LOGI(TAG, "%s trait: '%s' (%s) of %s", srcTag, t.id, t.name, t.natureId);
        }
    });
}

void PersonalityRegistry::addBuiltins()
{
    // Minimal fallback so the game still has an identity to show if the data is unreadable.
    Nature n;
    memset(&n, 0, sizeof n);
    strncpy(n.id, "gentle", sizeof n.id - 1);
    strncpy(n.name, "Gentle", sizeof n.name - 1);
    nat_[natures_++] = n;

    Personality t;
    memset(&t, 0, sizeof t);
    strncpy(t.id, "sweet", sizeof t.id - 1);
    strncpy(t.name, "Sweet", sizeof t.name - 1);
    strncpy(t.natureId, "gentle", sizeof t.natureId - 1);
    for (int i = 0; i < AX_COUNT; i++) t.weight[i] = 1.0f;
    t.ideal[AX_ENERGETIC] = 1.0f;
    tr_[traits_++] = t;
    ESP_LOGW(TAG, "no personality data readable; using built-in Sweet (Gentle)");
}

void PersonalityRegistry::resolve()
{
    for (int i = 0; i < traits_; i++) {
        tr_[i].natureIdx = natureIndexOf(tr_[i].natureId);
        if (tr_[i].natureIdx < 0)
            ESP_LOGW(TAG, "trait '%s' names unknown nature '%s'; it can never be selected",
                     tr_[i].id, tr_[i].natureId);
    }
    // The mirror problem: a nature no trait belongs to. crystallize() skips such natures
    // (a creature must always get a trait), so a modder should hear about the dead entry.
    for (int i = 0; i < natures_; i++) {
        bool any = false;
        for (int j = 0; j < traits_; j++)
            if (tr_[j].natureIdx == i) { any = true; break; }
        if (!any)
            ESP_LOGW(TAG, "nature '%s' has no personalities; it can never be crystallized",
                     nat_[i].id);
    }
}

void PersonalityRegistry::loadAll()
{
    natures_ = 0;
    traits_  = 0;
    gamedata_mount();                                  // shared; idempotent

    char dir[64];
    snprintf(dir, sizeof dir, "%s/natures", GAMEDATA_ROOT);
    scanNatures(dir, "flash");
    for (int i = 0; i < pakfs_count(); i++) {          // mod packs (later pak wins)
        snprintf(dir, sizeof dir, "%s/natures", pakfs_root(i));
        scanNatures(dir, "pak");
    }
    scanNatures(SD_NATURES, "sd");                     // loose files: the final overlay

    snprintf(dir, sizeof dir, "%s/personalities", GAMEDATA_ROOT);
    scanTraits(dir, "flash");
    for (int i = 0; i < pakfs_count(); i++) {
        snprintf(dir, sizeof dir, "%s/personalities", pakfs_root(i));
        scanTraits(dir, "pak");
    }
    scanTraits(SD_TRAITS, "sd");

    if (natures_ == 0 || traits_ == 0) { natures_ = traits_ = 0; addBuiltins(); }
    resolve();
    ESP_LOGI(TAG, "registry ready: %d natures, %d personalities", natures_, traits_);
}

// --- tracker -------------------------------------------------------------------

void PersonalityTracker::boot()
{
    PersonalityState in{};
    if (save_.loadBlob(PERS_KEY, &in, sizeof in) &&
        in.magic == PERS_MAGIC && in.version == PERS_VERSION) {
        s_ = in;
        char lbl[40];
        ESP_LOGI(TAG, "loaded personality: %s", label(lbl, sizeof lbl));
    } else {
        memset(&s_, 0, sizeof s_);
        s_.magic   = PERS_MAGIC;
        s_.version = PERS_VERSION;
        ESP_LOGI(TAG, "no personality yet (Unformed)");
    }
}

void PersonalityTracker::persist()
{
    if (!dirty_) return;
    save_.storeBlob(PERS_KEY, &s_, sizeof s_);
    dirty_ = false;
}

void PersonalityTracker::nudge(const float d[AX_COUNT], float strength)
{
    if (!d || strength <= 0.0f) return;
    const float a = ALPHA * strength;
    for (int i = 0; i < AX_COUNT; i++)
        s_.axis[i] += (d[i] - s_.axis[i]) * a;         // EMA toward this action's direction
    dirty_     = true;
    axesDirty_ = true;                                 // tick() must re-derive its verdict
}

void PersonalityTracker::setTrait(int idx)
{
    if (idx < 0) return;
    strncpy(s_.traitId, reg_.trait(idx).id, sizeof s_.traitId - 1);
    s_.traitId[sizeof s_.traitId - 1] = '\0';
    s_.challengerId[0] = '\0';
    s_.dwell = 0.0f;
    dirty_ = true;
}

void PersonalityTracker::crystallize()
{
    // Best nature THAT HAS TRAITS. A trait-less nature (a modded natures file with no
    // matching personalities) would otherwise wedge the identity: natureId set, traitId
    // forever "", label stuck at "Unformed", and no later tick or evolution can repair it
    // because setTrait(-1) is a no-op.
    static const float W1[AX_COUNT] = { 1, 1, 1, 1 };
    int best = -1, bestTr = -1;
    float bs = -1e9f;
    for (int i = 0; i < reg_.natureCount(); i++) {
        int t = reg_.bestTrait(i, s_.axis);
        if (t < 0) continue;
        float s = drift_score(s_.axis, reg_.nature(i).ideal, W1);
        if (s > bs) { bs = s; best = i; bestTr = t; }
    }
    if (best < 0) return;
    strncpy(s_.natureId, reg_.nature(best).id, sizeof s_.natureId - 1);
    s_.natureId[sizeof s_.natureId - 1] = '\0';
    setTrait(bestTr);
    char lbl[40];
    ESP_LOGI(TAG, "personality crystallized: %s", label(lbl, sizeof lbl));
}

void PersonalityTracker::tick(float dt, uint8_t stage)
{
    if (dt <= 0.0f) return;

    if (!crystallized()) {
        // The first "who is this creature" beat, once it's developed enough AND the player
        // has given the game something to read. Only re-derived when the axes moved or the
        // stage advanced: an unchanged vector reaches the same verdict, so evaluating it
        // every frame (and every step of offline catch-up) was pure waste.
        if (!axesDirty_ && stage == lastStage_) return;
        lastStage_ = stage;
        axesDirty_ = false;
        float mag = 0.0f;
        for (int i = 0; i < AX_COUNT; i++) mag += s_.axis[i] * s_.axis[i];
        mag = sqrtf(mag);
        if (stage >= STAGE_IN_TRAINING_2 && mag >= CRYSTAL_MIN_MAG) crystallize();
        return;
    }

    // Scoring reruns only when a nudge moved the axes; with a fixed vector the standing
    // verdict (challenger or none) can't change, so the frame cost is a dwell increment.
    if (axesDirty_) {
        axesDirty_ = false;

        int nIdx = reg_.natureIndexOf(s_.natureId);
        if (nIdx < 0) return;                          // nature came from a since-removed mod

        int curIdx = reg_.traitIndexOf(s_.traitId);
        float curScore = (curIdx >= 0)
            ? drift_score(s_.axis, reg_.trait(curIdx).ideal, reg_.trait(curIdx).weight) : -1e9f;

        float bestScore = 0.0f;
        int best = reg_.bestTrait(nIdx, s_.axis, &bestScore);
        if (best < 0) return;

        if (best == curIdx || bestScore <= curScore + MARGIN) {
            if (s_.challengerId[0]) { s_.challengerId[0] = '\0'; s_.dwell = 0.0f; dirty_ = true; }
        } else if (strcmp(s_.challengerId, reg_.trait(best).id) != 0) {
            // A challenger is ahead by the margin: it has to STAY ahead before identity
            // shifts, so a brief excursion near a boundary doesn't rename the creature.
            strncpy(s_.challengerId, reg_.trait(best).id, sizeof s_.challengerId - 1);
            s_.challengerId[sizeof s_.challengerId - 1] = '\0';
            s_.dwell = 0.0f;
            dirty_ = true;
        }
    }

    if (s_.challengerId[0]) {
        s_.dwell += dt;
        dirty_ = true;
        if (s_.dwell >= DWELL_SECS) {
            int best = reg_.traitIndexOf(s_.challengerId);
            if (best >= 0) {
                int curIdx = reg_.traitIndexOf(s_.traitId);
                const char* was = (curIdx >= 0) ? reg_.trait(curIdx).name : "?";
                setTrait(best);
                ESP_LOGI(TAG, "personality drifted: %s -> %s", was, reg_.trait(best).name);
            } else {
                s_.challengerId[0] = '\0';             // challenger's mod was removed
                s_.dwell = 0.0f;
            }
        }
    }
}

void PersonalityTracker::onEvolve()
{
    if (!crystallized()) return;

    int curN = reg_.natureIndexOf(s_.natureId);
    float bestScore = 0.0f;
    int bestN = reg_.bestNature(s_.axis, &bestScore);
    if (bestN < 0 || bestN == curN) {
        setTrait(reg_.bestTrait(curN, s_.axis));       // same nature: re-pick the trait
        return;
    }

    static const float W1[AX_COUNT] = { 1, 1, 1, 1 };
    float curScore = (curN >= 0) ? drift_score(s_.axis, reg_.nature(curN).ideal, W1) : -1e9f;
    int   jumpTrait = reg_.bestTrait(bestN, s_.axis);   // -1 = trait-less nature: never jump there
    if (jumpTrait >= 0 && bestScore > curScore + NATURE_JUMP_MARGIN) {
        const char* was = (curN >= 0) ? reg_.nature(curN).name : "?";
        strncpy(s_.natureId, reg_.nature(bestN).id, sizeof s_.natureId - 1);
        s_.natureId[sizeof s_.natureId - 1] = '\0';
        setTrait(jumpTrait);
        ESP_LOGI(TAG, "nature changed on evolution: %s -> %s", was, reg_.nature(bestN).name);
    } else {
        setTrait(reg_.bestTrait(curN, s_.axis));
    }
    dirty_ = true;
}

const char* PersonalityTracker::natureName() const
{
    if (!crystallized()) return "Unformed";
    int i = reg_.natureIndexOf(s_.natureId);
    return i >= 0 ? reg_.nature(i).name : s_.natureId;
}

const char* PersonalityTracker::traitName() const
{
    if (!s_.traitId[0]) return "";
    int i = reg_.traitIndexOf(s_.traitId);
    return i >= 0 ? reg_.trait(i).name : s_.traitId;
}

const char* PersonalityTracker::label(char* out, int n) const
{
    if (!crystallized() || !s_.traitId[0]) snprintf(out, n, "Unformed");
    else                                   snprintf(out, n, "%s (%s)", traitName(), natureName());
    return out;
}
