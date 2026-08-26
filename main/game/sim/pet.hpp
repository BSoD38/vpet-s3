#pragma once
#include <cstdint>
#include "personality.hpp"   // DriftAxis/AX_COUNT + PersonalityTracker (drift sink)
#include "vitals.hpp"        // Condition + VitalsSave/VitalsTuning (death & lifespan, D0)

class SaveStore;          // defined in save.hpp
class CreatureRegistry;   // defined in creatures.hpp
struct Creature;          // defined in creatures.hpp
struct Food;              // defined in foods.hpp

constexpr int PET_NICK_MAX = 16;   // max player-chosen nickname length

// --- Bond scale -------------------------------------------------------------------
// The bond is the long-haul relationship the conversation system pays off, so it is
// deliberately slow: reaching the top tier should take weeks of returning to the
// creature, not an evening of grinding. Two things make that true -- a wide scale (so
// there is resolution for small daily gains and room for many milestone tiers), and a
// DAILY CAP on repeatable sources (below), because a wide scale on its own just turns
// farming into *more* farming.
constexpr uint16_t FRIENDSHIP_MAX = 10000;

// Where a bond gain came from. Routine acts (feeding, petting, poking, minigames) are
// spammable, so they share a small daily allowance; milestones (battle wins, evolution,
// story conversations) are rare by nature and bypass it.
enum BondSource : uint8_t {
    BOND_ROUTINE = 0,
    BOND_MILESTONE,
};

// How the creature currently feels about the player. A conversation choice can genuinely upset
// it, and that has teeth in the care loop: while upset it REFUSES to be petted or poked. Food
// still works, though -- it won't be touched but it will accept a gift, which is what keeps the
// rift mendable rather than a lockout. A follow-up conversation is the real repair.
//
// Owned by Pet (it's the creature's state, and the care loop must see it) but kept OUT of the
// versioned blob, in its own NVS key, so adding it wipes nobody's pet. Per-creature: a new egg
// starts on good terms.
enum PetMood : uint8_t {
    MOOD_OK = 0,
    MOOD_HURT,       // saddened
    MOOD_ANGRY,      // angered -- takes more mending
};

// How the player played with the creature. Both raise happiness, but they say opposite
// things about the creature being raised (cuddling breeds an attached, timid temperament;
// rough play an energetic, unruly one), which is what makes "keep it happy" a parenting
// style rather than one optimal action.
enum PlayKind : uint8_t {
    PLAY_AFFECTION = 0,   // sustained petting
    PLAY_ROUGH,           // poking
};

// The reverse of Pet::moodId(): parse a data-driven mood string ("ok"/"hurt"/"angry").
// Returns false on an unknown token -- callers must treat that as a no-op, never as "ok".
bool mood_from_id(const char* s, PetMood* out);

// Display names for raw persisted values. Free functions (the Pet methods delegate to
// them) because the lineage ledger stores stage/friendship of creatures that no longer
// exist as a Pet -- the journal's In Memory page names them through these.
const char* stage_name(uint8_t stage);           // "Egg".."Mega+"
const char* friendship_tier_name(uint16_t f);    // "Stranger".."Soulbound"

// Is the SAVED pet's care freeze on (see Pet::setFrozen)? Reads the flag straight out of NVS
// without constructing a Pet or the registries one needs, for the headless deep-sleep poll --
// which wants to know whether a catch-up is worth doing at all before it loads ~15 KB of data
// it may not need. Pet::frozen() is the normal accessor.
bool pet_frozen_saved(const SaveStore& save);

// --- Coarse care readout ----------------------------------------------------------
// Hunger and happiness are shown as named states rather than numbers on purpose: a
// precise gauge is an optimization target, and every player filling it the same way
// flattens personality drift (docs/conversations-and-personality.md 2.8). HP and Energy
// stay exact -- those are explicit game resources where precision is fairness.
// (The matching tier->colour mapping is care_tier_color() in ui/widgets.hpp -- kept
// out of here so the sim layer doesn't depend on the renderer's palette.)
int         care_tier(float v);          // 0..4 (0 = worst)
const char* hunger_label(float v);       // "Starving".."Full"
const char* mood_label(float v);         // "Miserable".."Delighted"

// Tier at/below which the creature needs attention. THE shared threshold: SceneHome's
// pulsing "!" badge and the conversation context's "hungry" gate must agree, or modded
// conversations fire about a creature that shows no cue on screen (or vice versa).
constexpr int CARE_TIER_NEEDY = 1;

// Digimon-style life stages. Egg hatches into In-Training I, then progresses up
// the ladder. Mega+ is a special terminal form (conditions TBD once stats exist).
enum LifeStage : uint8_t {
    STAGE_EGG = 0,
    STAGE_IN_TRAINING_1,
    STAGE_IN_TRAINING_2,
    STAGE_CHILD,
    STAGE_CHAMPION,
    STAGE_ULTIMATE,
    STAGE_MEGA,
    STAGE_MEGA_PLUS,
    STAGE_COUNT
};

// RPG battle stats, trained by minigames/items and (later) gating evolutions.
// Max HP has a far higher cap (99999) than the rest (9999).
enum StatId : uint8_t {
    STAT_MAXHP = 0,
    STAT_STR,      // strength  - attack
    STAT_END,      // endurance - defense
    STAT_AGI,      // agility   - turn order / dodge
    STAT_INT,      // intellect - special / tech power
    STAT_COUNT
};

// Persisted pet state (stored verbatim as an NVS blob; keep it POD + versioned).
struct PetState {
    uint32_t magic;
    uint16_t version;
    uint8_t  stage;         // LifeStage (mirror of the current creature's tier, for quick reads)
    char     creatureId[24];// stable string id of the current creature (resolved to a registry index)
    float    ageSecs;       // total age (advances live + during offline catch-up)
    float    stageSecs;     // time spent in the CURRENT stage (drives evolution)
    float    dayClock;      // sim seconds within a 24h day (drives sleep schedule)
    float    hunger;        // 0..100 (100 = full)
    float    happiness;     // 0..100
    float    health;        // 0..100
    uint8_t  poop;          // 0..POOP_MAX
    uint8_t  cond;          // Condition enum (was the 0/1 "sick" flag through v6 -- same byte,
                            // superset values, so old blobs load unchanged; see vitals.hpp)
    uint8_t  lightsOff;     // 0/1 (asleep: auto by schedule, or manual)
    uint8_t  lastSleepPhase;// internal: last scheduled sleep state (edge detect)
    uint8_t  starveFlag;    // internal: starvation episode currently counted
    uint16_t gameSpeed;     // sim speed multiplier (1,2,5,10,20,50,100)
    float    poopTimer;     // seconds since last poop (awake only)
    uint32_t careMistakes;  // neglect events (feeds evolution-branch conditions)
    // --- RPG stats: TRAINED MODIFIERS only (minigames/items). Effective stat =
    //     creature's innate base + this modifier. Modifiers reset to 0 on evolve. ---
    uint32_t statMod[STAT_COUNT];  // indexed by StatId (MAXHP wide; others clamp small)
    uint16_t friendship;    // 0..FRIENDSHIP_MAX bond (Stranger..Soulbound); PERSISTS across evolution
    uint32_t wins;          // battle wins; PERSISTS across evolution (gates evos via EvoEdge.minWins)
    uint32_t losses;        // battle losses; PERSISTS across evolution
    float    energy;        // 0..100 training stamina; gates stat training, regenerates over time
    uint32_t lastUpdate;    // RTC seconds captured at last save (offline-aging reference)
};

// Result of a finished battle, applied to the pet and returned for the result screen.
struct BattleOutcome {
    bool     won;
    int      healthPct;    // health (0..100) after the HP write-back
    int      friendDelta;  // friendship change (+ on win, - on loss)
    uint32_t statGain;     // total combat-stat points gained (win only)
    bool     gotSick;      // the exit-HP roll fired on a WIN: caught a cold
    bool     gotInjured;   // the exit-HP roll fired on a LOSS: wounded (festers if ignored)
};

// The virtual creature: simulation + care actions over a POD PetState.
class Pet {
    PetState  s_{};
    SaveStore& save_;
    CreatureRegistry& reg_;
    int       idx_ = -1;          // resolved registry index of the current creature
    char      nickname_[PET_NICK_MAX + 1] = {0};   // player-set name (empty = use species name)
    bool      refused_ = false;   // set when an action is refused; UI consumes it for a "no" wiggle
    bool      ate_     = false;   // set when food was actually eaten; UI consumes it for the eat animation
    // Daily bond allowance (see BondSource). Kept OUT of PetState -- and so out of the
    // versioned blob -- in its own NVS keys, flushed by markSaved(). The window is a
    // rolling 24h anchored at the last refill (see addFriendship for why not a day index).
    uint32_t  bondWinT_ = 0;      // RTC second the current allowance window began
    uint32_t  bondToday_ = 0;     // routine bond points already granted this window
    PersonalityTracker* drift_ = nullptr;   // optional: personality drift sink (set by App)
    float     idleSecs_ = 0.0f;   // REAL seconds since the last deliberate interaction (persisted)
    bool      catchingUp_ = false;   // inside boot()'s offline replay (attenuates idle drift)
    int       cuIdleNudges_ = 0;     // idle nudges already granted during this catch-up
    bool      startedFresh_ = false;   // boot() hatched a new egg (no save was loaded)
    uint8_t   mood_ = MOOD_OK;         // PetMood; out-of-blob, per-creature
    uint8_t   mend_ = 0;               // care shown since being upset (softens a step when full)
    bool      frozen_ = false;         // care freeze: the whole sim is suspended (see setFrozen)
    VitalsSave v_{};                   // life meter + condition timers; own NVS blob (see vitals.hpp)

    void newEgg();
    void freshVitals();                // full pool, clean trailing average (birth / migration base)
    void setCondition(Condition c, const char* why);   // logs + resets the escalation timer
    void enterBrink(Brink why);        // death's door: roll fate + persist BEFORE any rendering
    // Tell the audio engine whose voice creature-voiced sounds should now use (see
    // VoiceProfile in engine/audio/audio.hpp). Called from every path that moves idx_,
    // because the alternative -- resolving the voice at each play() site -- would put the
    // creature registry in front of every sound in the game.
    void applyVoice() const;
    bool careBlocked();           // frozen? then refuse the care action and arm the "no" wiggle
    void applyBacklight() const;  // panel brightness for the current lights + freeze state
    int  pickEvolution() const;   // first eligible evolution edge (creature idx), or -1
    void evolveTo(int creatureIdx);       // switch creature, reset trained modifiers
    bool schedSleep() const;      // does the day-clock schedule want the pet asleep now?
    const Creature* cur() const;  // current creature definition (nullptr if unresolved)
public:
    Pet(SaveStore& save, CreatureRegistry& reg) : save_(save), reg_(reg) {}

    // Personality drift. The pet is the funnel every notable action already passes through,
    // so it is where drift is emitted from; the tracker itself is optional (null = the
    // feature is simply off). Minigames call nudgeDrift() directly with their own vector,
    // the same way each already owns its stat-gain table.
    void setDriftSink(PersonalityTracker* d) { drift_ = d; }

    // Re-assert the panel brightness after the player changes it (Settings -> SCREEN).
    // The pet is the only thing allowed to drive the backlight, because it is the only
    // thing that knows whether the creature's lights are currently off.
    void refreshBacklight() const { applyBacklight(); }

    void nudgeDrift(const float d[AX_COUNT], float strength = 1.0f);

    // Apply one conversation choice. One-shot story conversations are MILESTONE bonding --
    // talking is the payoff the bond exists for. REPEATABLE conversations pass BOND_ROUTINE
    // instead, so replaying small talk shares the daily allowance rather than bypassing it.
    // Drift nudges are the most deliberate in the game (the player is answering in words),
    // so they land at full strength either way.
    void applyConversationChoice(int friendshipDelta, int happinessDelta,
                                 const float drift[AX_COUNT], BondSource src = BOND_MILESTONE);

    // --- mood (see PetMood) ---
    PetMood     mood() const { return (PetMood)mood_; }
    bool        isUpset() const { return mood_ != MOOD_OK; }
    const char* moodId() const;              // "ok" / "hurt" / "angry", for conversation gates
    void        setMood(PetMood m);          // RAM only; the caller's markSaved persists it
    void        mendMood(int amount);        // care progress; softens a step when it fills

    // Does a save exist whose species is NOT in the current registry? Then booting would
    // rewrite the creature as an egg and persist that immediately (boot() ends in markSaved),
    // losing a pet the player could still get back by putting the data in place -- typically
    // the SD card it came from, or an uninstalled mod pack. Call BEFORE boot() so the player
    // can be asked instead; this only reads, and touches no state. Fills `idOut` with the
    // missing species id for the message. See App::init.
    bool savedSpeciesMissing(char* idOut, int n);
    // The player has chosen to give up on that creature. Invalidates the stored blob so the
    // next boot() takes its ordinary "no save" path -- newEgg() plus the fresh-start resets
    // (mood, bond allowance, idle clock) -- instead of this needing its own copy of them.
    void forgetSave();

    void boot();                  // seed RTC, load save + offline catch-up, else new egg
    void tick(float dt);          // advance sim by dt SIM-seconds (caller applies gameSpeed)

    // --- Care freeze ("I can't look after it for a while") -----------------------------
    // Suspends the ENTIRE simulation: tick() returns immediately, so nothing ages, decays,
    // evolves, poops, falls ill, regenerates or drifts. In exchange the creature can't be
    // cared for either -- feeding, cleaning, healing, petting, the lights and training all
    // refuse. The two halves are deliberately inseparable: topping the creature up while
    // nothing decays would make this a cheat rather than a pause.
    //
    // tick() being the ONE gate is what makes the guarantee hold across power cycles too:
    // boot()'s offline catch-up replays absence through tick(), so a frozen pet that spends
    // a week switched off replays that week as nothing at all. Persisted OUTSIDE the
    // versioned blob (its own NVS key), so adding it wiped nobody's pet.
    bool frozen() const { return frozen_; }
    void setFrozen(bool on);      // no-op if unchanged; persists immediately
    // Feed a specific food. Which food is chosen is the player's main day-to-day way of
    // shaping the creature's temperament, so this is the hook the personality system
    // reads (see docs/food-and-feeding.md). Refuses (and arms the "no" wiggle) when the
    // creature can't eat.
    void feed(const Food& f);
    bool canEat();                // false + arms the refusal wiggle when egg/asleep/sick
    // Voice a refused care action. Public because a caller that pre-gates on canEat() (the
    // home screen's Feed button) never reaches the action itself, and has to make the same
    // noise the action would have -- otherwise the identical refusal is audible from the food
    // picker and silent from the home screen.
    void playRefusal() const;
    void clean();
    void heal();
    void toggleLights();
    // Raise happiness; not saved per-call. Returns false when the touch was REFUSED
    // (egg/sick/asleep/upset) so the caller withholds friendship and reward FX too.
    bool play(float amount, PlayKind kind);
    void markSaved();             // stamp lastUpdate = now and persist
    // Same, with the aging baseline given explicitly -- for the one caller that knows the
    // wall clock better than clock_now() does: the screen that has just re-set the RTC.
    void markSavedAt(uint32_t now);
    void setGameSpeed(uint16_t mult);
    bool checkRefused();          // true once if an action was just refused
    bool checkAte();              // true once if food was just eaten (drives the eat animation)

    // --- stats / friendship (flat growth; callers persist via markSaved) ---
    void     trainStat(StatId id, uint32_t amount);  // add to the trained modifier (clamped so base+mod<=cap)
    // Routine gains are metered by a daily allowance (see BondSource); milestones aren't.
    // Losses always apply in full regardless of source.
    void     addFriendship(int delta, BondSource src = BOND_ROUTINE);
    uint32_t stat(StatId id) const;                  // EFFECTIVE stat = base + modifier (clamped)
    uint32_t baseStat(StatId id) const;              // innate base from the current creature
    uint32_t modifier(StatId id) const;              // trained bonus on top of the base
    uint16_t friendship() const { return s_.friendship; }
    const char* friendshipTier() const;              // "Stranger".."Soulbound"

    // --- battle record (persists across evolution; feeds evolution gates) ---
    void     recordWin();
    void     recordLoss();
    uint32_t wins() const   { return s_.wins; }
    uint32_t losses() const { return s_.losses; }

    // --- training energy (0..100 stamina; gates stat training via minigames) ---
    float    energy() const { return s_.energy; }
    bool     spendEnergy(float amount);   // deduct if available; returns false if insufficient

    // Training (minigames) and battles unlock at In-Training II; a fresh egg / In-Training I
    // is too undeveloped to train or fight. Gates the menu's Activities + Battle entries.
    bool     activitiesUnlocked() const { return s_.stage >= STAGE_IN_TRAINING_2; }

    // --- condition & vitality (docs/death-and-lifespan.md; design rule 2: restrictions come
    //     from the CONDITION track only, never from age) ---
    Condition condition() const { return (Condition)s_.cond; }
    // Battle, training and ALL minigames are blocked while anything is wrong. The pet-less
    // "earn medicine money" game planned for the economy is the deliberate future exception.
    bool conditionBlocked() const { return condition() != COND_HEALTHY; }
    // Being properly ill refuses food and touch (the classic sick-pet lockout, and how it has
    // always behaved); an injured or convalescing creature still eats and accepts comfort.
    bool foodBlocked() const  { Condition c = condition();
                                return c == COND_SICK || c == COND_SICK_BAD || c == COND_CRITICAL; }
    bool touchBlocked() const { return foodBlocked(); }
    const char* conditionMarker() const { return condition_marker(condition()); }   // nullptr if healthy
    const VitalsSave& vitals() const { return v_; }   // read-only (debug overlay / stats)
    LifeTrack lifeTrack() const;   // Prime/Elderly/Twilight from the vitality thresholds
    float vitalityFrac() const;    // vitality / effective ceiling (scars shrink the ceiling)

    // --- the brink & the death event (docs/death-and-lifespan.md §5-§7) ----------------
    // While at the brink the whole sim is suspended (tick returns immediately), so the
    // event can only ever resolve with the player watching. Fate was rolled and persisted
    // at entry; the death scene reads it and calls exactly one of the two resolvers.
    bool  atBrink() const { return v_.brink != BRINK_NONE; }
    Brink brink()  const  { return (Brink)v_.brink; }
    Fate  fate()   const  { return (Fate)v_.fate; }
    // Fate said MIRACLE: apply the survival -- Critical: scar + partial refill + Recovery +
    // the strongest personality push in the game; old age: a few borrowed weeks, still
    // Twilight. Either way the next roll's odds are halved.
    void applyMiracle();
    // Fate said DEATH and the player has seen the memorial. Advances the generation and
    // invalidates the save so the next BOOT hatches the successor through the ordinary
    // fresh-start path -- the caller restarts the chip (App::restartAfterDeath).
    void concludeDeath();

    // True when boot() hatched a fresh egg rather than loading a save. Lets the composition
    // root clear per-creature state (conversation history/journal) that Pet itself shouldn't
    // know about.
    bool     startedFresh() const { return startedFresh_; }

    // --- DEV/CHEAT helpers (used only by the debug Cheats screen; bypass normal gating and
    //     persist immediately). Kept as explicit named ops rather than exposing raw state. ---
    void cheatRestore();                        // full HP/energy/hunger/happiness, cure sick, clear poop
    void cheatSetHealth(float pct);             // 0..100
    void cheatSetVitality(float frac);          // 0..1 of max -- the life-track test lever
    void cheatSetCondition(Condition c);        // condition-track test lever (resets its clock)
    // Straight to the death event, either flavour. Not a special path: it empties the pool,
    // makes the condition match the story, and lets enterBrink roll fate FOR REAL -- the
    // outcome is as binding as any natural death. False if refused (egg, already at brink).
    bool cheatTriggerBrink(Brink why);
    void cheatSetEnergy(float pct);             // 0..100 (stamina)
    void cheatSetFriendship(int value);         // 0..FRIENDSHIP_MAX
    void cheatAdjustStat(StatId id, int delta); // nudge the trained modifier (clamped to 0..room)
    void cheatMaxStat(StatId id);               // set the modifier so the effective stat hits its cap
    void cheatSetSpecies(int creatureIdx);      // morph to any registry creature (keeps trained stats)

    // Force the evolution the creature has ALREADY EARNED, skipping only the stage timer.
    //
    // Deliberately not a species swap. cheatSetSpecies() answers "make it a Greymon"; this
    // answers "do the thing tick() would do once minStageSecs elapses" -- which means it walks
    // the real edge list, so which branch it takes is decided by the stats, bond, wins and care
    // mistakes the pet actually has. That is the whole point of it as a testing tool: it
    // exercises the evolution PATH (branch choice, stat reset, Nature re-roll, the fanfare)
    // rather than bypassing it, so a mod author can check that their gates route the way they
    // intended without waiting out two real days per stage.
    //
    // Refusing is a real outcome, not a failure: a creature whose gates are all unmet has not
    // earned anything yet, and silently inventing a target would make this a species swap with
    // extra steps. `nameOut` receives the new species name on Evolved.
    enum class ForceEvo : uint8_t {
        Evolved,        // done -- nameOut holds the new species
        Terminal,       // this creature has no outgoing edges at all
        NotEligible,    // it has edges, but nothing currently satisfies one
    };
    ForceEvo cheatForceEvolve(char* nameOut, int n);

    // Apply a finished battle: write remaining HP% back to health, grant win/loss effects
    // (stats/friendship/happiness/W-L record) and roll exit-HP sickness. hpFrac = the player's
    // remaining HP / maxHP (0..1). Persists immediately; returns the outcome for the UI.
    BattleOutcome applyBattleResult(bool won, float hpFrac);

    const PetState& state() const { return s_; }      // read-only view for scenes/HUD
    const Creature* creature() const { return cur(); }  // current creature definition
    int  creatureIndex() const { return idx_; }         // registry index (for reg.sprite(idx))
    const char* stageName() const;
    const char* speciesName() const;
    const char* nickname() const { return nickname_; }   // raw nickname ("" if unset)
    const char* displayName() const;                     // nickname if set, else species name
    void        setNickname(const char* n);              // copy (<=PET_NICK_MAX), trim, persist
    const char* ageStr(char* out, int n) const;  // "3d 4h"/"5h 12m"/"42s" into caller buf; returns out
    int   simHour() const;        // 0..23 from the sim day clock
    bool  isSleepTime() const;    // schedule currently wants the pet asleep
    float stageProgress() const;  // 0..1 through the current stage (1.0 if terminal)
};
