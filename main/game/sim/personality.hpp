#pragma once
#include <cstdint>

class SaveStore;

// Emergent personality. The creature's identity drifts from how you actually play: every
// notable action nudges four hidden axes, and the nearest declared trait wins. Full design,
// including the reachability analysis behind the numbers, is in
// docs/conversations-and-personality.md; tools/personality_sim.py validates any change here.

// CORE axes place the creature in a Nature quadrant (sticky, changes only on evolution);
// SURFACE axes choose the personality inside it (drifts freely with play).
enum DriftAxis : uint8_t {
    AX_BRAVE = 0,      // core:    timid <-> brave
    AX_ENERGETIC,      // surface: calm  <-> energetic
    AX_SOCIAL,         // core:    independent <-> attached
    AX_WILD,           // surface: disciplined <-> wild
    AX_COUNT
};

struct Nature {
    char  id[16];
    char  name[16];
    float ideal[AX_COUNT];     // in practice only the core axes are non-zero
};

struct Personality {
    char  id[16];
    char  name[16];
    char  natureId[16];
    int   natureIdx;           // resolved at load; -1 if its nature is missing
    float ideal[AX_COUNT];     // direction in the surface plane
    float weight[AX_COUNT];    // per-axis emphasis (defaults to 1)
};

// Weighted direction match: score = sum(w*axis*ideal) / ||w*ideal||. Linear in the drift
// vector, so it matches on DIRECTION rather than magnitude -- a creature never has to max
// an axis out to qualify for a trait. Highest score wins.
float drift_score(const float axis[AX_COUNT], const float ideal[AX_COUNT],
                  const float weight[AX_COUNT]);

struct cJSON;

// Read a {"brave":..,"energetic":..,"social":..,"wild":..} JSON block at o[key] into
// out[AX_COUNT]; absent axes get `def`. THE one parser for drift blocks -- foods,
// conversations and this registry must agree on axis names and order, so none of them
// may hand-roll its own copy.
void parse_drift(cJSON* o, const char* key, float out[AX_COUNT], float def);

// Natures and personalities are data, so mods can add either. Both load from
// <root>/natures/*.json and <root>/personalities/*.json, where each file holds an ARRAY of
// entries -- so a mod ships one file rather than a directory per trait. SD entries override
// base ones on an id collision, matching the creature/food contract.
class PersonalityRegistry {
public:
    static const int MAX_NATURES = 12;
    static const int MAX_TRAITS  = 48;

    void loadAll();
    int  natureCount() const { return natures_; }
    int  traitCount()  const { return traits_; }
    const Nature&      nature(int i) const { return nat_[i]; }
    const Personality& trait(int i)  const { return tr_[i]; }
    int  natureIndexOf(const char* id) const;
    int  traitIndexOf(const char* id) const;

    // Best-scoring nature for a drift vector, or -1 if none are loaded.
    int  bestNature(const float axis[AX_COUNT], float* outScore = nullptr) const;
    // Best-scoring trait WITHIN a nature, or -1 if that nature has none.
    int  bestTrait(int natureIdx, const float axis[AX_COUNT], float* outScore = nullptr) const;

private:
    Nature      nat_[MAX_NATURES];
    Personality tr_[MAX_TRAITS];
    int         natures_ = 0;
    int         traits_  = 0;

    void scanNatures(const char* dir, const char* srcTag);
    void scanTraits(const char* dir, const char* srcTag);
    void addBuiltins();        // safety net if no data files are readable
    void resolve();            // trait.natureId -> natureIdx
};

// Persisted drift state. Its OWN versioned blob, separate from PetState, so revising it
// never risks wiping the pet (and a pet reset never erases who the creature became).
struct PersonalityState {
    uint32_t magic;
    uint16_t version;
    float    axis[AX_COUNT];
    char     natureId[16];        // "" until crystallized (shown as "Unformed")
    char     traitId[16];
    char     challengerId[16];    // trait currently trying to take over
    float    dwell;               // sim-seconds the challenger has stayed ahead by the margin
};

class PersonalityTracker {
public:
    PersonalityTracker(SaveStore& save, PersonalityRegistry& reg) : save_(save), reg_(reg) {}

    void boot();                                   // load, or start blank
    void persist();                                // called from Pet::markSaved

    // Apply one action's nudge. `strength` scales it (1.0 = a normal action), letting a
    // rare/deliberate act count for more than a routine one.
    void nudge(const float d[AX_COUNT], float strength = 1.0f);

    // Advance crystallization + the hysteresis timer. `stage` is the pet's LifeStage.
    void tick(float dt, uint8_t stage);

    // Evolution is the one moment a Nature can change: if accumulated drift strongly
    // contradicts the current one, the creature re-forms as something else.
    void onEvolve();

    bool        crystallized() const { return s_.natureId[0] != '\0'; }
    // Stable IDS, for data that has to MATCH (conversation gates); the *Name() calls below
    // are display text and must never be compared against file contents.
    const char* natureId() const { return s_.natureId; }   // "" until crystallized
    const char* traitId()  const { return s_.traitId; }
    const char* natureName() const;                // "Unformed" until crystallized
    const char* traitName() const;                 // "" until crystallized
    const char* label(char* out, int n) const;     // "Curious (Clever)" / "Unformed"
    const float* axes() const { return s_.axis; }  // debug readout only

private:
    SaveStore&           save_;
    PersonalityRegistry& reg_;
    PersonalityState     s_{};
    bool                 dirty_ = false;

    // tick() re-derives scores only when these say something changed: an unchanged drift
    // vector is guaranteed to reach the same verdict, so per-frame work collapses to a
    // dwell increment instead of registry strcmp scans + dot products.
    bool    axesDirty_ = true;
    uint8_t lastStage_ = 0xFF;

    void crystallize();
    void setTrait(int idx);
};
