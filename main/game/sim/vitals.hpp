#pragma once
#include <cstdint>

// Long-term life meter + condition track + the brink (docs/death-and-lifespan.md).
// D0: vitality drains, conditions escalate and block activities, treatment works.
// D2: vitality reaching zero holds the pet at the BRINK -- a persisted, suspended state the
// death event resolves only with the player watching. The miracle roll happens at brink
// entry and hits NVS before a single frame renders, so the reset button can't reroll fate.

// The creature's condition. PERSISTED IN PetState.cond -- the old 0/1 `sick` flag widened
// into an enum. 0 and 1 keep their v6 meanings, so pre-condition saves load correctly with
// no blob migration, and OLD firmware reading a new save just sees any ailment as "sick".
enum Condition : uint8_t {
    COND_HEALTHY  = 0,
    COND_SICK     = 1,   // mild -- one dose of treatment clears it (the v6 "sick")
    COND_SICK_BAD = 2,   // serious: mild left untreated; the heavy vitality drain (the cliff)
    COND_INJURED  = 3,   // battle aftermath; treat it or it festers into sickness
    COND_CRITICAL = 4,   // held at death's door; treatment no longer works, only the roll
    COND_RECOVERY = 5,   // post-miracle convalescence: no battle/training, wants gentle care
};

// Why the pet is held at death's door. Persisted (VitalsSave) so the event survives power
// cuts and drawer time: a held brink resumes into the death event on the next boot.
enum Brink : uint8_t {
    BRINK_NONE = 0,
    BRINK_CRITICAL,   // the pool emptied on the sickness/neglect track (guilt, wordless)
    BRINK_OLDAGE,     // the pool emptied with nothing else wrong: a life fully lived
};

// What the persisted miracle roll decided. Meaningless while brink == BRINK_NONE.
enum Fate : uint8_t {
    FATE_DEATH   = 0,
    FATE_MIRACLE = 1,
};

// Marker text hung over the creature ("SICK"/"HURT"/...); nullptr when healthy.
const char* condition_marker(Condition c);

// The life track (docs/death-and-lifespan.md §4): where the creature is in its LIFE, as
// opposed to what CONDITION it is in. Deliberately presentation-only -- design rule 2 says
// restrictions and stat penalties come from the condition track alone, so a diligent
// player's year-old companion plays at full capability to the very end. The aging is READ:
// slower gait, earlier bedtime, changed conversation.
enum LifeTrack : uint8_t {
    LIFE_PRIME = 0,      // most of the life
    LIFE_ELDERLY,        // vitality < elderlyFrac: visibly old, fully playable
    LIFE_TWILIGHT,       // vitality < twilightFrac: the end is near, and the player knows
};

// Conversation-gate token for the current life stage: "prime" / "elderly" / "twilight".
const char* life_track_id(LifeTrack t);

// Persisted vitals, OUT of PetState: its own NVS blob with its own magic/version, so adding
// the death system wiped nobody's pet and revising it later won't either (the same argument
// as the personality-drift blob). POD, no padding holes -- SaveStore::loadBlob is exact-size.
struct VitalsSave {
    uint32_t magic;
    uint16_t version;
    uint8_t  savesUsed;   // miracles granted this lifetime (odds halve per one)
    uint8_t  scars;       // Critical survivals (ceiling = vitMax * scarMult^scars)
    float    vitality;    // the pool; every death is this reaching zero
    float    careEma;     // 0..1 trailing neglect score (drives the drain multiplier)
    float    condSecs;    // sim-seconds in the current condition (drives escalation)
    uint8_t  brink;       // Brink: held at death's door (suspends the whole sim)
    uint8_t  fate;        // Fate: the roll's verdict, decided AT brink entry, pre-render
    uint8_t  pad[2];      // explicit, so the blob has no compiler-chosen holes
};

// Tuning. All numbers are first guesses (docs/death-and-lifespan.md §11) and load from
// config/vitals.json across the usual roots -- flash, then paks, then loose SD, later file
// winning PER KEY -- so rebalancing the death system is a data edit, not a reflash.
struct VitalsTuning {
    float vitMax          = 10000.0f;
    float baseDrainPerDay = 33.3f;     // at care x1.0: a ~10-month life
    float multBest        = 0.7f;      // diligent care: ~14 months
    float multWorst       = 3.5f;      // sustained neglect: ~3 months
    float emaTauHrs       = 48.0f;     // care-quality trailing-average time constant
    float mistakeChip     = 40.0f;     // flat vitality cost per care mistake
    float sickBadPerDay   = 1500.0f;   // serious-sickness extra drain: fatal in days
    float sickEscalateHrs = 12.0f;     // mild -> serious after this long untreated
    float injuryFesterHrs = 24.0f;     // injured -> sick after this long untreated
    float elderlyFrac     = 0.15f;     // life-track thresholds (LifeTrack above)
    float twilightFrac    = 0.03f;
    // Life-track presentation (D1). Step multipliers stretch the footfall cadence -- and,
    // because ground travel is derived from it (engine/walk.hpp), the walking speed with
    // it. Extra sleep moves bedtime EARLIER; waking time is unchanged.
    float elderlyStepMult       = 1.6f;
    float twilightStepMult      = 2.2f;
    float elderlyExtraSleepHrs  = 1.0f;
    float twilightExtraSleepHrs = 2.0f;
    // The miracle (D2): p = miracleMax × bond × 0.5^savesUsed, where bond ramps 0..1 from
    // miracleBondFloor up to FRIENDSHIP_MAX. Below the floor the chance is effectively
    // zero -- the miracle IS the friendship.
    float miracleMax       = 0.85f;
    float miracleBondFloor = 4000.0f;
    float critRestoreFrac  = 0.30f;   // Critical save: refill to this × the (shrunk) ceiling
    float scarMult         = 0.85f;   // each Critical save multiplies the ceiling by this
    float reprieveFrac     = 0.05f;   // old-age reprieve: +this × ceiling (~2-3 more weeks)
    float recoveryDays     = 3.0f;    // convalescence length after a Critical save
};

const VitalsTuning& vitals_tuning();
void vitals_load_tuning();   // once at init, after gamedata_mount + pak mounts

// The vitality ceiling after `scars` Critical survivals: vitMax × scarMult^scars. Every
// threshold, refill and reprieve is a fraction OF THIS, which is how each save permanently
// shortens the rest of the life (docs/death-and-lifespan.md §5).
float vitals_effective_max(uint8_t scars);
