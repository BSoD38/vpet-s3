#pragma once
#include <cstdint>

class SaveStore;          // defined in save.hpp
class CreatureRegistry;   // defined in creatures.hpp
struct Creature;          // defined in creatures.hpp

constexpr int PET_NICK_MAX = 16;   // max player-chosen nickname length

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
    uint8_t  sick;          // 0/1
    uint8_t  lightsOff;     // 0/1 (asleep: auto by schedule, or manual)
    uint8_t  lastSleepPhase;// internal: last scheduled sleep state (edge detect)
    uint8_t  starveFlag;    // internal: starvation episode currently counted
    uint16_t gameSpeed;     // sim speed multiplier (1,2,5,10,20,50,100)
    float    poopTimer;     // seconds since last poop (awake only)
    uint32_t careMistakes;  // neglect events (feeds evolution-branch conditions)
    // --- RPG stats: TRAINED MODIFIERS only (minigames/items). Effective stat =
    //     creature's innate base + this modifier. Modifiers reset to 0 on evolve. ---
    uint32_t statMod[STAT_COUNT];  // indexed by StatId (MAXHP wide; others clamp small)
    uint16_t friendship;    // 0..1000 bond meter (Stranger..Soulbound); PERSISTS across evolution
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
    bool     gotSick;      // the exit-HP sickness roll fired
};

// The virtual creature: simulation + care actions over a POD PetState.
class Pet {
    PetState  s_{};
    SaveStore& save_;
    CreatureRegistry& reg_;
    int       idx_ = -1;          // resolved registry index of the current creature
    char      nickname_[PET_NICK_MAX + 1] = {0};   // player-set name (empty = use species name)
    bool      refused_ = false;   // set when an action is refused; UI consumes it for a "no" wiggle

    void newEgg();
    int  pickEvolution() const;   // first eligible evolution edge (creature idx), or -1
    void evolveTo(int creatureIdx);       // switch creature, reset trained modifiers
    bool schedSleep() const;      // does the day-clock schedule want the pet asleep now?
    const Creature* cur() const;  // current creature definition (nullptr if unresolved)
public:
    Pet(SaveStore& save, CreatureRegistry& reg) : save_(save), reg_(reg) {}

    void boot();                  // seed RTC, load save + offline catch-up, else new egg
    void tick(float dt);          // advance sim by dt SIM-seconds (caller applies gameSpeed)
    void feed();
    void clean();
    void heal();
    void toggleLights();
    void play(float amount);      // raise happiness (petting); not saved per-call
    void markSaved();             // stamp lastUpdate = now and persist
    void setGameSpeed(uint16_t mult);
    bool checkRefused();          // true once if an action was just refused

    // --- stats / friendship (flat growth; callers persist via markSaved) ---
    void     trainStat(StatId id, uint32_t amount);  // add to the trained modifier (clamped so base+mod<=cap)
    void     addFriendship(int delta);               // clamp to 0..1000
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

    // --- DEV/CHEAT helpers (used only by the debug Cheats screen; bypass normal gating and
    //     persist immediately). Kept as explicit named ops rather than exposing raw state. ---
    void cheatRestore();                        // full HP/energy/hunger/happiness, cure sick, clear poop
    void cheatSetHealth(float pct);             // 0..100
    void cheatSetEnergy(float pct);             // 0..100 (stamina)
    void cheatSetFriendship(int value);         // 0..1000
    void cheatAdjustStat(StatId id, int delta); // nudge the trained modifier (clamped to 0..room)
    void cheatMaxStat(StatId id);               // set the modifier so the effective stat hits its cap
    void cheatSetSpecies(int creatureIdx);      // morph to any registry creature (keeps trained stats)

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
