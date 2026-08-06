#include "pet.hpp"
#include "save.hpp"
#include "creatures.hpp"
#include "foods.hpp"            // Food (chosen food drives feeding effects + drift)
#include "engine/util.hpp"      // clampf (shared)
#include "engine/clock.hpp"
#include "engine/drivers.hpp"    // Set_Backlight, LCD_Backlight, PCF85063_Read_Time, datetime
#include "esp_log.h"
#include "esp_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static const char* TAG = "PET";

// Creature definitions (base stats / needs / evolution edges) now live in the
// data-driven CreatureRegistry (loaded from the flash + SD "creatures" dirs), not
// a compiled table. The pet keeps a resolved index (idx_) into that registry, and
// persists the stable string id (s_.creatureId) so a save survives roster changes.

// --- Real-time balance (per hour unless noted) ---
static const float    HUNGER_DANGER        = 20.0f;
static const float    STARVE_HP_PER_HR     = 100.0f / 4.0f;
static const float    SICK_HP_PER_HR       = 100.0f / 12.0f;
static const float    HEALTH_REC_PER_HR    = 100.0f / 4.0f;
static const float    HAP_SICK_THRESH      = 40.0f;
static const float    K_HAPPY_SICK         = 0.00020f;
static const float    K_POOP_SICK          = 0.00006f;
static const uint8_t  POOP_MAX             = 5;
static const float    SLEEP_HUNGER_FACTOR  = 0.3f;
static const float    SLEEP_HAP_REC_PER_HR = 100.0f / 10.0f;
static const float    AWAKE_NIGHT_HAP_HR   = 100.0f / 6.0f;
static const uint32_t MAX_OFFLINE          = 24u * 3600u;
static const int      SLEEP_BACKLIGHT      = 25;
static const float    DAY_LEN              = 86400.0f;
static const float    ENERGY_MAX           = 100.0f;
static const float    ENERGY_REGEN_HR      = 100.0f / 8.0f;   // training stamina: ~8h awake to refill
static const float    ENERGY_REGEN_SLEEP_HR= 100.0f / 4.0f;   // faster while asleep

// --- RPG stat caps (effective = creature base + trained modifier, clamped here) ---
static const uint32_t MAXHP_CAP  = 99999;
static const uint16_t STAT_CAP   = 9999;
static const uint16_t FRIEND_CAP = FRIENDSHIP_MAX;

// Friendship deltas for care actions. The bond is a weeks-long relationship, so routine
// acts contribute little AND share a daily allowance (see BOND_ROUTINE_DAILY): widening
// the scale alone would only have made grinding longer, not slower.
static const int FR_FEED    = 2;
static const int FR_CLEAN   = 3;
static const int FR_HEAL    = 6;
static const int FR_MISTAKE = 60;   // losses are NOT metered and scale with the wider range

// Daily allowance for repeatable bond sources (feeding, petting, poking, minigames).
// Once spent, further routine gains give nothing until the next day -- invisible to the
// player, since only the bond TIER is ever shown, never the number.
static const uint32_t BOND_ROUTINE_DAILY = 60;

// --- battle stakes/rewards (tunable) ---
static const int      FR_BATTLE_WIN   = 25;    // friendship gained on a win (milestone: unmetered)
static const int      FR_BATTLE_LOSS  = 40;    // friendship lost on a loss
static const float    HAP_BATTLE_LOSS = 14.0f; // happiness lost on a loss
// Winning together is one of the three routes to a happy creature (the others being
// affection and rough play), so that "keep it happy" is a parenting style rather than a
// single optimal action -- see docs/conversations-and-personality.md 2.8.
static const float    HAP_BATTLE_WIN  = 12.0f;
static const uint32_t WIN_STAT_GAIN   = 2;     // per combat stat (STR/END/AGI/INT), per win
static const float    SICK_HP_THRESH  = 35.0f; // exit HP% below which sickness can roll
static const float    SICK_MAX_CHANCE = 0.9f;  // sickness chance approached as HP% -> 0

// --- Personality drift vectors, indexed by DriftAxis { brave, energetic, social, wild }.
// Validated as a set by tools/personality_sim.py -- changing one in isolation can make a
// trait unreachable, so re-run that script after editing. Three rules they obey:
//   * DUTY is neutral: clean() and heal() emit nothing, and plain food carries no drift.
//     Every competent player does those constantly, so they say nothing about the player,
//     and a constant nudge would drag everyone into the same corner.
//   * `social` is interaction STYLE, not care quality -- otherwise "independent" would just
//     mean "neglected" and half the natures would be locked behind mistreatment.
//   * Axes must not always co-move, or whole quadrants become unreachable.
static const float DRIFT_OVERFEED[AX_COUNT] = { -0.25f,  0.00f,  0.40f,  0.70f };
static const float DRIFT_STARVE[AX_COUNT]   = { -0.40f,  0.00f, -0.60f,  0.80f };
static const float DRIFT_IDLE[AX_COUNT]     = { -0.50f, -0.50f, -0.60f,  0.80f };
static const float DRIFT_AFFECTION[AX_COUNT]= { -0.40f, -0.30f,  1.00f, -0.20f };
static const float DRIFT_ROUGH[AX_COUNT]    = {  0.10f,  1.00f,  0.50f,  0.60f };
static const float DRIFT_WIN[AX_COUNT]      = {  0.80f,  0.30f,  0.50f,  0.20f };
static const float DRIFT_LOSS[AX_COUNT]     = { -0.90f,  0.00f,  0.00f,  0.00f };
static const float DRIFT_SLEEP_OK[AX_COUNT] = { -0.15f, -0.20f,  0.00f, -0.70f };
static const float DRIFT_SLEEP_NO[AX_COUNT] = {  0.20f, -0.30f,  0.00f,  1.00f };

// Neglect: with no interaction for this long, the creature draws its own conclusions. This
// is what makes the withdrawn/listless traits reachable at all -- deliberately, they are the
// only ones you have to earn by being a poor owner. Counted in REAL seconds (see tick()):
// neglect is about the player's absence, and at 60x gameSpeed a sim-time period would fire
// every 30 REAL seconds and numerically swamp every deliberate action's drift.
static const float IDLE_PERIOD = 1800.0f;   // real seconds between idle nudges

// Offline catch-up replays absence as neglect too -- but attenuated and capped. A single
// >=24h replay into the EMA would otherwise converge the whole vector toward the idle
// direction in one boot (and instantly crystallize a fresh tracker on migration), erasing
// weeks of deliberate play. Half strength (passive absence, like the auto-sleep nudge) and
// at most this many nudges per catch-up keep absence a signal instead of a verdict.
static const int   CATCHUP_IDLE_MAX      = 6;
static const float CATCHUP_IDLE_STRENGTH = 0.5f;

// Out-of-blob NVS keys. Anything stored here instead of in PetState avoids changing the
// blob layout, which would bump PET_VERSION and wipe the player's pet.
static const char* K_MOOD       = "mood";    // packed: (PetMood << 8) | mendProgress
static const char* K_BONDP      = "bondp";   // routine bond points granted this window
static const char* K_BONDT      = "bondt";   // RTC second the current 24h window began
static const char* K_IDLE       = "idle";    // idle REAL-seconds since the last interaction
static const char* K_BOND_SCALE = "bscale";  // 1 once the bond has been rescaled to the wide range

static const uint32_t PET_MAGIC   = 0x50455401;   // 'PET\1'
static const uint16_t PET_VERSION = 6;             // v6: + battle record (wins/losses) + training energy

static float rnd01(void) { return esp_random() * (1.0f / 4294967296.0f); }

const Creature* Pet::cur() const
{
    return (idx_ >= 0 && idx_ < reg_.count()) ? &reg_.at(idx_) : nullptr;
}

// sleep window test; window may wrap past midnight (e.g. 22 -> 7)
static bool in_window(int h, int start, int end)
{
    if (start == end) return false;
    return (start < end) ? (h >= start && h < end) : (h >= start || h < end);
}

bool Pet::checkRefused() { bool r = refused_; refused_ = false; return r; }
bool Pet::checkAte()     { bool r = ate_;     ate_     = false; return r; }

const char* Pet::stageName() const
{
    switch (s_.stage) {
        case STAGE_EGG:           return "Egg";
        case STAGE_IN_TRAINING_1: return "In-Training I";
        case STAGE_IN_TRAINING_2: return "In-Training II";
        case STAGE_CHILD:         return "Child";
        case STAGE_CHAMPION:      return "Champion";
        case STAGE_ULTIMATE:      return "Ultimate";
        case STAGE_MEGA:          return "Mega";
        case STAGE_MEGA_PLUS:     return "Mega+";
        default:                  return "?";
    }
}

const char* Pet::speciesName() const { const Creature* c = cur(); return c ? c->name : "?"; }

const char* Pet::displayName() const { return nickname_[0] ? nickname_ : speciesName(); }

void Pet::setNickname(const char* n)
{
    int j = 0;
    for (int i = 0; n && n[i] && j < PET_NICK_MAX; i++) nickname_[j++] = n[i];
    nickname_[j] = '\0';
    while (j > 0 && nickname_[j - 1] == ' ') nickname_[--j] = '\0';   // trim trailing spaces
    save_.storeStr("nick", nickname_);
    ESP_LOGI(TAG, "nickname = '%s'", nickname_);
}

const char* Pet::ageStr(char* out, int n) const
{
    int s = (int)s_.ageSecs;
    int d = s / 86400; s %= 86400;
    int h = s / 3600;  s %= 3600;
    int m = s / 60;    s %= 60;
    if (d > 0)      snprintf(out, n, "%dd %dh", d, h);
    else if (h > 0) snprintf(out, n, "%dh %dm", h, m);
    else if (m > 0) snprintf(out, n, "%dm %ds", m, s);
    else            snprintf(out, n, "%ds", s);
    return out;
}

int  Pet::simHour() const      { return (int)(s_.dayClock / 3600.0f) % 24; }
bool Pet::isSleepTime() const  { return schedSleep(); }

bool Pet::schedSleep() const
{
    const Creature* c = cur();
    if (!c) return false;
    return in_window(simHour(), c->sleepStart, c->sleepEnd);
}

float Pet::stageProgress() const
{
    const Creature* c = cur();
    if (!c || c->evoCount == 0) return 1.0f;   // terminal form (nowhere to evolve)
    float dur = c->minStageSecs;
    if (dur <= 0.0f) return 1.0f;
    float p = s_.stageSecs / dur;
    return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

// Choose an evolution: walk this creature's edges in order and take the first whose
// conditions are all met (effective stats + friendship + care). The last edge is a
// trivially-met fallback, so a creature with edges always has somewhere to go.
int Pet::pickEvolution() const
{
    const Creature* c = cur();
    if (!c) return -1;
    for (int i = 0; i < c->evoCount; i++) {
        const EvoEdge& e = c->evos[i];
        if (e.toIdx < 0) continue;          // target creature missing from the registry
        if (stat(STAT_MAXHP) >= e.minHp &&
            stat(STAT_STR)   >= e.minStr &&
            stat(STAT_END)   >= e.minEnd &&
            stat(STAT_AGI)   >= e.minAgi &&
            stat(STAT_INT)   >= e.minInt &&
            s_.friendship    >= e.minFriendship &&
            s_.wins          >= e.minWins &&
            s_.careMistakes  <= e.maxCareMistakes)
            return e.toIdx;
    }
    return -1;   // none eligible yet (keep aging until conditions are met)
}

void Pet::evolveTo(int creatureIdx)
{
    idx_ = creatureIdx;
    const Creature* c = cur();
    if (c) {
        strncpy(s_.creatureId, c->id, sizeof(s_.creatureId) - 1);
        s_.creatureId[sizeof(s_.creatureId) - 1] = '\0';
        s_.stage = c->tier;
    }
    for (int i = 0; i < STAT_COUNT; i++) s_.statMod[i] = 0;   // trained modifiers reset
    s_.stageSecs = 0.0f;                                      // fresh timer in the new form
    // Evolution is the one moment a Nature can change: the creature re-forms, and if the
    // drift has strongly contradicted its way of being, it comes back as something else.
    if (drift_) drift_->onEvolve();
    if (c && c->tier == STAGE_IN_TRAINING_1) {               // just hatched -> fresh needs
        s_.hunger = 100; s_.happiness = 80; s_.health = 100;
        s_.poop = 0; s_.poopTimer = 0;
    }
    char ab[16];
    ESP_LOGI(TAG, "evolved -> %s (%s, age=%s, friend=%u, mistakes=%u)",
             c ? c->name : "?", stageName(), ageStr(ab, sizeof ab),
             (unsigned)s_.friendship, (unsigned)s_.careMistakes);
}

void Pet::newEgg()
{
    s_ = PetState{};
    s_.magic = PET_MAGIC;
    s_.version = PET_VERSION;
    s_.stage = STAGE_EGG;
    strncpy(s_.creatureId, "egg", sizeof(s_.creatureId) - 1);  // the base line starts at "egg"
    idx_ = reg_.indexOf("egg");
    const Creature* c = cur();
    if (c) s_.stage = c->tier;
    s_.gameSpeed = 1;
    s_.hunger = 100; s_.happiness = 80; s_.health = 100;
    s_.energy = ENERGY_MAX;            // full training stamina at birth
    // statMod[] all 0; friendship/wins/losses 0 (evolution branches are earned, not rolled at birth)
    s_.dayClock = (float)((datetime.hour * 3600) + (datetime.minute * 60) + datetime.second);
    s_.lastSleepPhase = 0;
    s_.lastUpdate = clock_now();
    nickname_[0] = '\0';               // a reborn pet starts nameless
    save_.storeStr("nick", "");
    ESP_LOGI(TAG, "new egg (creature '%s', idx %d)", s_.creatureId, idx_);
}

void Pet::tick(float dt)   // dt is SIM seconds (the caller has applied gameSpeed)
{
    if (dt <= 0) return;
    s_.ageSecs   += dt;
    s_.stageSecs += dt;

    // Evolve when eligible: enough time in this form AND some edge's gate is met.
    // One step per tick; the fresh timer (stageSecs=0) paces the next evolution.
    {
        const Creature* c = cur();
        if (c && c->evoCount > 0 && s_.stageSecs >= c->minStageSecs) {
            int target = pickEvolution();
            if (target >= 0) evolveTo(target);
        }
    }

    if (s_.stage == STAGE_EGG) return;   // egg only ages; no needs yet

    // day/night clock + auto sleep at the window edges only
    s_.dayClock += dt;
    if (s_.dayClock >= DAY_LEN) s_.dayClock = fmodf(s_.dayClock, DAY_LEN);
    bool wantSleep = schedSleep();
    if (wantSleep != (bool)s_.lastSleepPhase) {
        s_.lastSleepPhase = wantSleep ? 1 : 0;
        s_.lightsOff = wantSleep ? 1 : 0;
        Set_Backlight(s_.lightsOff ? SLEEP_BACKLIGHT : LCD_Backlight);
        ESP_LOGI(TAG, "%s (hour=%d)", s_.lightsOff ? "auto-sleep" : "auto-wake", simHour());
        // Letting the schedule run its course IS respecting it. Half strength (it's passive,
        // not a decision) and direct, so it doesn't count as breaking a neglect streak.
        if (s_.lightsOff && drift_) drift_->nudge(DRIFT_SLEEP_OK, 0.5f);
    }

    const Creature* c = cur();
    const float hungerPerHr = c ? c->hungerPerHr   : 0.0f;
    const float happyPerHr  = c ? c->happyPerHr    : 0.0f;
    const float poopEveryS  = c ? c->poopIntervalS : 1e9f;
    const float perS = dt / 3600.0f;
    if (s_.lightsOff) {
        s_.hunger    -= hungerPerHr * SLEEP_HUNGER_FACTOR * perS;
        s_.happiness += SLEEP_HAP_REC_PER_HR * perS;
    } else {
        s_.hunger    -= hungerPerHr * perS;
        s_.happiness -= happyPerHr  * perS;
        if (wantSleep) s_.happiness -= AWAKE_NIGHT_HAP_HR * perS;
        s_.poopTimer += dt;
        if (s_.poopTimer >= poopEveryS) {
            s_.poopTimer = 0;
            if (s_.poop < POOP_MAX) s_.poop++;
        }
    }
    s_.hunger    = clampf(s_.hunger, 0, 100);
    s_.happiness = clampf(s_.happiness, 0, 100);

    // training energy refills over time (faster asleep); gates stat training
    float eRegenHr = s_.lightsOff ? ENERGY_REGEN_SLEEP_HR : ENERGY_REGEN_HR;
    s_.energy = clampf(s_.energy + eRegenHr * perS, 0, ENERGY_MAX);

    if (s_.hunger <= 0.0f) {
        if (!s_.starveFlag) {                 // one care-mistake + bond hit per starve episode
            s_.careMistakes++;
            s_.starveFlag = 1;
            addFriendship(-FR_MISTAKE);
            // Direct, not nudgeDrift(): starving IS neglect, so it must not reset the
            // idle timer the way a deliberate interaction does.
            if (drift_) drift_->nudge(DRIFT_STARVE);
        }
    } else if (s_.hunger > HUNGER_DANGER) {
        s_.starveFlag = 0;
    }

    if (!s_.sick) {
        float rate = 0.0f;
        if (s_.happiness < HAP_SICK_THRESH)
            rate += K_HAPPY_SICK * (HAP_SICK_THRESH - s_.happiness) / HAP_SICK_THRESH;
        rate += K_POOP_SICK * s_.poop;
        if (rate > 0.0f && rnd01() < (1.0f - expf(-rate * dt))) {
            s_.sick = 1;
            ESP_LOGI(TAG, "got sick (hap=%.0f poop=%u)", s_.happiness, s_.poop);
        }
    }

    float hpDrainHr = 0.0f;
    if (s_.sick) hpDrainHr += SICK_HP_PER_HR;
    if (s_.hunger < HUNGER_DANGER)
        hpDrainHr += STARVE_HP_PER_HR * (HUNGER_DANGER - s_.hunger) / HUNGER_DANGER;
    if (hpDrainHr > 0.0f)                             s_.health -= hpDrainHr * perS;
    else if (s_.happiness > 25.0f && !s_.lightsOff)   s_.health += HEALTH_REC_PER_HR * perS;
    s_.health = clampf(s_.health, 0, 100);

    // Personality: long stretches with no interaction are their own signal (only counted
    // while awake -- you can't interact with a sleeping creature), then the tracker
    // re-evaluates identity. dt here is SIM seconds; divide the live multiplier back out
    // so the neglect clock runs on REAL time (catch-up already ticks in real seconds).
    if (!s_.lightsOff) {
        idleSecs_ += catchingUp_ ? dt : dt / (float)(s_.gameSpeed ? s_.gameSpeed : 1);
        if (idleSecs_ >= IDLE_PERIOD) {
            idleSecs_ = 0.0f;
            if (drift_) {
                if (!catchingUp_) {
                    drift_->nudge(DRIFT_IDLE);
                } else if (cuIdleNudges_ < CATCHUP_IDLE_MAX) {   // attenuated + capped replay
                    cuIdleNudges_++;
                    drift_->nudge(DRIFT_IDLE, CATCHUP_IDLE_STRENGTH);
                }
            }
        }
    }
    if (drift_) drift_->tick(dt, s_.stage);
}

void Pet::markSaved()
{
    s_.lastUpdate = clock_now();
    save_.beginBatch();                 // blob + bond + mood + idle + drift: ONE nvs commit
    save_.store(s_);
    save_.storeU32(K_BONDP, bondToday_);
    save_.storeU32(K_BONDT, bondWinT_);
    save_.storeU32(K_MOOD, ((uint32_t)mood_ << 8) | (uint32_t)mend_);
    // Persisted so the neglect clock survives reboots AND the 15-min deep-sleep wake
    // slices, each of which is shorter than IDLE_PERIOD on its own.
    save_.storeU32(K_IDLE, (uint32_t)idleSecs_);
    if (drift_) drift_->persist();      // no-op unless the drift state actually changed
    save_.endBatch();
}

bool Pet::savedSpeciesMissing(char* idOut, int n)
{
    // Deliberately a separate read into a LOCAL rather than a peek at s_: this runs before
    // boot(), and must leave the object exactly as it found it. The validity test mirrors
    // boot()'s, because a blob boot() would reject is not a creature at risk -- it just
    // hatches a new egg, which is correct and not worth a prompt.
    PetState probe{};
    if (!save_.load(probe) || probe.magic != PET_MAGIC || probe.version != PET_VERSION)
        return false;
    probe.creatureId[sizeof(probe.creatureId) - 1] = '\0';
    if (reg_.indexOf(probe.creatureId) >= 0) return false;
    if (idOut && n > 0) {
        strncpy(idOut, probe.creatureId, (size_t)n - 1);
        idOut[n - 1] = '\0';
    }
    return true;
}

void Pet::forgetSave()
{
    // No id in this log: forgetSave() runs BEFORE boot(), so s_ has not been loaded yet and
    // its creatureId is still empty. The caller names the creature it is discarding.
    ESP_LOGW(TAG, "discarding the saved creature at the player's request");
    PetState blank{};      // magic 0, so boot()'s validity check fails and newEgg() runs
    save_.store(blank);
}

void Pet::boot()
{
    PCF85063_Read_Time(&datetime);   // seed the RTC global before first clock_now()

    if (save_.load(s_) && s_.magic == PET_MAGIC && s_.version == PET_VERSION) {
        if (s_.gameSpeed == 0) s_.gameSpeed = 1;
        s_.creatureId[sizeof(s_.creatureId) - 1] = '\0';   // guard against a corrupt/unterminated id
        idx_ = reg_.indexOf(s_.creatureId);                // resolve identity before ticking
        // BACKSTOP ONLY. App::init calls savedSpeciesMissing() first and prompts the player,
        // because reaching here rewrites the species and markSaved() at the end of boot()
        // persists it on the spot -- irreversible. This branch survives so a caller that
        // skips the check still gets a running game rather than a null creature.
        if (idx_ < 0) {
            ESP_LOGW(TAG, "saved creature '%s' not in registry; falling back to egg", s_.creatureId);
            idx_ = reg_.indexOf("egg");
            if (idx_ < 0 && reg_.count() > 0) idx_ = 0;
            const Creature* c = cur();
            if (c) { strncpy(s_.creatureId, c->id, sizeof(s_.creatureId) - 1); s_.stage = c->tier; }
        }
        // One-time bond rescale: FRIENDSHIP_MAX widened from 1000 to 10000, so an existing
        // save's bond would otherwise read as a much lower tier than the player earned.
        // Migrated behind an out-of-blob flag rather than a PET_VERSION bump, which would
        // have wiped the pet. MUST run BEFORE the catch-up loop: a starve mistake during
        // catch-up subtracts the new-scale FR_MISTAKE, and applying that to a still
        // old-scale value and THEN multiplying by 10 turned a -60 into an effective -600.
        if (save_.loadU8(K_BOND_SCALE, 0) != 1) {
            uint32_t scaled = (uint32_t)s_.friendship * 10u;
            s_.friendship = (uint16_t)(scaled > FRIENDSHIP_MAX ? FRIENDSHIP_MAX : scaled);
            save_.storeU8(K_BOND_SCALE, 1);
            ESP_LOGI(TAG, "bond rescaled -> %u/%u (%s)",
                     (unsigned)s_.friendship, (unsigned)FRIENDSHIP_MAX, friendshipTier());
        }
        // The neglect clock resumes where it left off -- loaded BEFORE catch-up so the
        // replayed absence stacks on top of what had already accrued. Without this, the
        // 15-min deep-sleep wake slices (each < IDLE_PERIOD) could never complete a
        // period and days of neglect produced zero idle drift.
        idleSecs_ = (float)save_.loadU32(K_IDLE, 0);

        uint32_t elapsed = clock_elapsed(s_.lastUpdate);
        if (elapsed > MAX_OFFLINE) elapsed = MAX_OFFLINE;
        char ab[16];
        ESP_LOGI(TAG, "loaded (%s %s, %s), offline catch-up %us",
                 speciesName(), stageName(), ageStr(ab, sizeof ab), (unsigned)elapsed);
        uint32_t rem = elapsed;   // catch up in REAL seconds (speed multiplier is live-only)
        catchingUp_   = true;     // idle drift replays attenuated + capped (see tick())
        cuIdleNudges_ = 0;
        while (rem > 0) { uint32_t step = rem > 60 ? 60 : rem; tick((float)step); rem -= step; }
        catchingUp_ = false;
    } else {
        newEgg();
        startedFresh_ = true;
    }
    save_.loadStr("nick", nickname_, sizeof nickname_, "");   // nickname lives outside the blob

    bondToday_ = save_.loadU32(K_BONDP, 0);                   // daily bond allowance state
    bondWinT_  = save_.loadU32(K_BONDT, 0);

    uint32_t mp = save_.loadU32(K_MOOD, 0);                   // how we left things
    mood_ = (uint8_t)(mp >> 8);
    mend_ = (uint8_t)(mp & 0xFFu);
    if (mood_ > MOOD_ANGRY) { mood_ = MOOD_OK; mend_ = 0; }   // guard a corrupt value
    if (startedFresh_) {
        mood_ = MOOD_OK; mend_ = 0;      // a new creature bears no grudge...
        bondToday_ = 0;                  // ...and doesn't inherit a spent daily allowance
        bondWinT_  = clock_now();
        idleSecs_  = 0.0f;
    }

    markSaved();
    Set_Backlight(s_.lightsOff ? SLEEP_BACKLIGHT : LCD_Backlight);
}

// Hunger at or above this counts as overfeeding: an indulgent DEVIATION from routine
// care rather than a duty, and the personality system reads it as such.
static const float OVERFEED_AT = 85.0f;

bool Pet::canEat()
{
    if (s_.stage == STAGE_EGG) { ESP_LOGI(TAG, "can't feed (egg)"); return false; }
    if (s_.lightsOff) { ESP_LOGI(TAG, "refuses food (asleep)"); refused_ = true; return false; }
    if (s_.sick)      { ESP_LOGI(TAG, "refuses food (sick)");   refused_ = true; return false; }
    return true;
}

void Pet::feed(const Food& f)
{
    if (!canEat()) return;
    ate_ = true;   // accepted -- the home scene plays the eat animation off this

    const bool overfed = (s_.hunger >= OVERFEED_AT);

    s_.hunger    = clampf(s_.hunger    + (float)f.fills,     0, 100);
    s_.happiness = clampf(s_.happiness + (float)f.happiness, 0, 100);
    if (f.health != 0)
        s_.health = clampf(s_.health + (float)f.health, 0, 100);
    addFriendship(FR_FEED);

    // The chosen food's own nudge, plus an indulgence nudge if it wasn't even hungry --
    // overfeeding is a deviation from duty regardless of WHICH food it was.
    nudgeDrift(f.drift);
    if (overfed) nudgeDrift(DRIFT_OVERFEED);
    // Feeding an upset creature is the peace offering it WILL accept. (Once species food
    // preferences land, a favourite should mend more than an indifferent meal.)
    mendMood(1);

    ESP_LOGI(TAG, "feed '%s'%s (hunger=%.0f hap=%.0f hp=%.0f)",
             f.id, overfed ? " [overfed]" : "", s_.hunger, s_.happiness, s_.health);
    markSaved();
}

void Pet::clean()
{
    s_.poop = 0;
    addFriendship(FR_CLEAN);
    // Duty is drift-NEUTRAL (no vector) but it IS an interaction: it must break a neglect
    // streak the same way feeding does, or a dutiful owner who cleans and heals on schedule
    // still accrues idle "neglect" nudges.
    idleSecs_ = 0.0f;
    ESP_LOGI(TAG, "clean");
    markSaved();
}

void Pet::heal()
{
    s_.sick = 0;
    s_.health = clampf(s_.health + 40, 0, 100);
    addFriendship(FR_HEAL);
    idleSecs_ = 0.0f;                  // see clean(): duty still counts as interaction
    ESP_LOGI(TAG, "heal (health=%.0f)", s_.health);
    markSaved();
}

bool Pet::play(float amount, PlayKind kind)
{
    if (s_.stage == STAGE_EGG || s_.sick || s_.lightsOff) return false;
    // Upset creatures don't want to be touched. The "no" wiggle carries the message, and food
    // is still accepted -- that's the way back (see PetMood). Returning false lets the scene
    // withhold the bond/reward FX too: a refused touch must grant NOTHING.
    if (isUpset()) { refused_ = true; return false; }
    s_.happiness = clampf(s_.happiness + amount, 0, 100);
    nudgeDrift(kind == PLAY_AFFECTION ? DRIFT_AFFECTION : DRIFT_ROUGH);
    return true;
}

void Pet::nudgeDrift(const float d[AX_COUNT], float strength)
{
    if (drift_) drift_->nudge(d, strength);
    idleSecs_ = 0.0f;      // any deliberate interaction breaks a neglect streak
}

// Care shown per step needed to soften an upset creature. Being angry takes two softenings
// (angry -> hurt -> ok), so it's a slower thaw than being merely hurt.
static const uint8_t MEND_PER_STEP = 3;

const char* Pet::moodId() const
{
    switch (mood_) {
        case MOOD_HURT:  return "hurt";
        case MOOD_ANGRY: return "angry";
        default:         return "ok";
    }
}

// The ONE string->enum interpreter for data-driven mood ids (the reverse of moodId()).
// Returns false on an unknown token so a typo'd modded value ("Angry", "sad") is a loud
// no-op instead of silently mapping to MOOD_OK and mending a rift the writer just opened.
bool mood_from_id(const char* s, PetMood* out)
{
    if (!s || !out) return false;
    if (strcmp(s, "ok") == 0)    { *out = MOOD_OK;    return true; }
    if (strcmp(s, "hurt") == 0)  { *out = MOOD_HURT;  return true; }
    if (strcmp(s, "angry") == 0) { *out = MOOD_ANGRY; return true; }
    return false;
}

void Pet::setMood(PetMood m)
{
    if (mood_ == (uint8_t)m) return;
    mood_ = (uint8_t)m;
    mend_ = 0;
    ESP_LOGI(TAG, "mood -> %s", moodId());
    // Not persisted here: the only caller (a conversation choice) follows up with
    // applyConversationChoice -> markSaved(), which writes K_MOOD in the same batch.
}

void Pet::mendMood(int amount)
{
    if (mood_ == MOOD_OK || amount <= 0) return;
    int v = (int)mend_ + amount;
    while (v >= MEND_PER_STEP && mood_ != MOOD_OK) {
        v -= MEND_PER_STEP;
        mood_--;                                  // angry -> hurt -> ok
        ESP_LOGI(TAG, "mood softened -> %s", moodId());
    }
    mend_ = (uint8_t)(mood_ == MOOD_OK ? 0 : v);
}

void Pet::applyConversationChoice(int friendshipDelta, int happinessDelta,
                                  const float drift[AX_COUNT], BondSource src)
{
    // One-shot story conversations are milestones; REPEATABLE ones are routine chatter and
    // go through the daily allowance (the scene passes the right source), or replaying the
    // same small talk would be an unmetered bond grind.
    if (friendshipDelta) addFriendship(friendshipDelta, src);
    if (happinessDelta)  s_.happiness = clampf(s_.happiness + (float)happinessDelta, 0, 100);
    if (drift) nudgeDrift(drift);
    markSaved();
}

void Pet::toggleLights()
{
    s_.lightsOff = !s_.lightsOff;
    // Sync to the SCHEDULE (not to lightsOff) so this manual choice holds until the
    // next window edge — otherwise waking it mid-window re-triggers auto-sleep.
    s_.lastSleepPhase = schedSleep() ? 1 : 0;
    Set_Backlight(s_.lightsOff ? SLEEP_BACKLIGHT : LCD_Backlight);
    // Overriding the schedule (waking it during its sleep window, or forcing it under early)
    // is a real choice about how strictly this creature is being raised.
    nudgeDrift(schedSleep() != (bool)s_.lightsOff ? DRIFT_SLEEP_NO : DRIFT_SLEEP_OK);
    ESP_LOGI(TAG, "lights %s", s_.lightsOff ? "off" : "on");
    markSaved();
}

void Pet::setGameSpeed(uint16_t mult)
{
    if (mult < 1) mult = 1;
    s_.gameSpeed = mult;
    ESP_LOGI(TAG, "game speed = %ux", mult);
    markSaved();
}

// --- RPG stats: flat growth added to the TRAINED MODIFIER. Effective = base + mod,
//     clamped to the stat's cap, so the modifier can only fill the room above base. ---
static uint32_t stat_cap(StatId id) { return id == STAT_MAXHP ? MAXHP_CAP : STAT_CAP; }

uint32_t Pet::baseStat(StatId id) const
{
    const Creature* c = cur();
    if (!c) return 0;
    switch (id) {
        case STAT_MAXHP: return c->baseHp;
        case STAT_STR:   return c->baseStr;
        case STAT_END:   return c->baseEnd;
        case STAT_AGI:   return c->baseAgi;
        case STAT_INT:   return c->baseInt;
        default:         return 0;
    }
}

uint32_t Pet::modifier(StatId id) const
{
    return id < STAT_COUNT ? s_.statMod[id] : 0;
}

uint32_t Pet::stat(StatId id) const
{
    if (id >= STAT_COUNT) return 0;
    uint64_t v = (uint64_t)baseStat(id) + s_.statMod[id];
    uint32_t cap = stat_cap(id);
    return (uint32_t)(v > cap ? cap : v);
}

void Pet::trainStat(StatId id, uint32_t amount)
{
    if (amount == 0 || id >= STAT_COUNT) return;
    uint32_t base = baseStat(id);
    uint32_t cap  = stat_cap(id);
    uint32_t room = base >= cap ? 0 : cap - base;   // most the modifier may reach
    uint64_t v = (uint64_t)s_.statMod[id] + amount;
    s_.statMod[id] = (uint32_t)(v > room ? room : v);
}

void Pet::addFriendship(int delta, BondSource src)
{
    // Meter repeatable sources against a daily allowance so the bond grows by returning
    // to the creature over days rather than by grinding one action in a single sitting.
    // Losses and milestones are never metered.
    if (delta > 0 && src == BOND_ROUTINE) {
        // ROLLING 24h window anchored at the last refill, not the calendar epoch-day: the
        // old dayIndex key refilled on ANY day change, so a TimeSet hop back and forth (or
        // an innocent RTC correction) handed out a fresh allowance on demand. A backward
        // clock move now just re-anchors the window without refilling.
        uint32_t now = clock_now();
        if (now < bondWinT_) {
            bondWinT_ = now;                                           // clock moved back
        } else if (now - bondWinT_ >= 86400u) {
            bondWinT_ = now; bondToday_ = 0;                           // new day: refill
        }
        if (bondToday_ >= BOND_ROUTINE_DAILY) return;                  // allowance spent
        uint32_t room = BOND_ROUTINE_DAILY - bondToday_;
        if ((uint32_t)delta > room) delta = (int)room;
        bondToday_ += (uint32_t)delta;
        // Deliberately NOT persisted here: this runs as often as every rub chunk, and a
        // write per chunk would burn NVS. markSaved() flushes it (autosave >= every 120s),
        // so at worst a reboot hands back part of the day's allowance.
    }

    int v = (int)s_.friendship + delta;
    if (v < 0) v = 0;
    if (v > FRIEND_CAP) v = FRIEND_CAP;
    s_.friendship = (uint16_t)v;
}

// --- DEV/CHEAT helpers (debug Cheats screen only) ---------------------------------------
void Pet::cheatRestore()
{
    s_.health = 100; s_.energy = ENERGY_MAX;
    s_.hunger = 100; s_.happiness = 100;
    s_.sick = 0; s_.poop = 0;
    markSaved();
}

void Pet::cheatSetHealth(float pct) { s_.health = clampf(pct, 0.0f, 100.0f);       markSaved(); }
void Pet::cheatSetEnergy(float pct) { s_.energy = clampf(pct, 0.0f, ENERGY_MAX);   markSaved(); }

void Pet::cheatSetFriendship(int value)
{
    if (value < 0) value = 0;
    if (value > FRIEND_CAP) value = FRIEND_CAP;
    s_.friendship = (uint16_t)value;
    markSaved();
}

void Pet::cheatAdjustStat(StatId id, int delta)
{
    if (id >= STAT_COUNT) return;
    uint32_t base = baseStat(id), cap = stat_cap(id);
    uint32_t room = base >= cap ? 0 : cap - base;      // most the modifier may reach
    long v = (long)s_.statMod[id] + delta;
    if (v < 0) v = 0;
    if (v > (long)room) v = (long)room;
    s_.statMod[id] = (uint32_t)v;
    markSaved();
}

void Pet::cheatMaxStat(StatId id)
{
    if (id >= STAT_COUNT) return;
    uint32_t base = baseStat(id), cap = stat_cap(id);
    s_.statMod[id] = base >= cap ? 0 : cap - base;     // effective stat hits its cap
    markSaved();
}

void Pet::cheatSetSpecies(int creatureIdx)
{
    if (creatureIdx < 0 || creatureIdx >= reg_.count()) return;
    idx_ = creatureIdx;
    const Creature* c = cur();
    if (c) {
        strncpy(s_.creatureId, c->id, sizeof(s_.creatureId) - 1);
        s_.creatureId[sizeof(s_.creatureId) - 1] = '\0';
        s_.stage = c->tier;                            // keep trained mods / friendship (stat() re-clamps)
    }
    markSaved();
}

void Pet::recordWin()  { if (s_.wins   != 0xFFFFFFFFu) s_.wins++; }
void Pet::recordLoss() { if (s_.losses != 0xFFFFFFFFu) s_.losses++; }

bool Pet::spendEnergy(float amount)
{
    if (amount <= 0.0f) return true;
    if (s_.energy < amount) return false;
    s_.energy -= amount;
    return true;
}

BattleOutcome Pet::applyBattleResult(bool won, float hpFrac)
{
    BattleOutcome o{};
    o.won = won;
    hpFrac = clampf(hpFrac, 0.0f, 1.0f);

    if (won) {
        s_.health = clampf(hpFrac * 100.0f, 1.0f, 100.0f);   // keep whatever HP survived
        trainStat(STAT_STR, WIN_STAT_GAIN);
        trainStat(STAT_END, WIN_STAT_GAIN);
        trainStat(STAT_AGI, WIN_STAT_GAIN);
        trainStat(STAT_INT, WIN_STAT_GAIN);
        o.statGain = WIN_STAT_GAIN * 4;
        addFriendship(FR_BATTLE_WIN, BOND_MILESTONE);   // rare + earned: not daily-metered
        o.friendDelta = FR_BATTLE_WIN;
        s_.happiness = clampf(s_.happiness + HAP_BATTLE_WIN, 0.0f, 100.0f);
        nudgeDrift(DRIFT_WIN);
        recordWin();
    } else {
        s_.health = 1.0f;                                    // defeat: scraped through at 1%
        addFriendship(-FR_BATTLE_LOSS);
        o.friendDelta = -FR_BATTLE_LOSS;
        s_.happiness = clampf(s_.happiness - HAP_BATTLE_LOSS, 0.0f, 100.0f);
        nudgeDrift(DRIFT_LOSS);
        recordLoss();
    }

    // exit-HP sickness roll: the lower the HP you walked away with, the likelier a cold
    if (!s_.sick && s_.health < SICK_HP_THRESH) {
        float chance = (SICK_HP_THRESH - s_.health) / SICK_HP_THRESH * SICK_MAX_CHANCE;
        if (rnd01() < chance) { s_.sick = 1; o.gotSick = true; }
    }

    o.healthPct = (int)s_.health;
    markSaved();
    ESP_LOGI(TAG, "battle %s hp%%=%d friend%+d sick=%d", won ? "WIN" : "LOSS",
             o.healthPct, o.friendDelta, (int)o.gotSick);
    return o;
}

// Eight tiers across the wide scale so a milestone still lands every week or two even
// though the whole climb takes months. The exact number stays hidden -- only this name
// is ever shown (a deliberate choice: the bond is a relationship, not a progress bar).
const char* Pet::friendshipTier() const
{
    uint16_t f = s_.friendship;
    if (f <  500) return "Stranger";
    if (f < 1500) return "Acquaintance";
    if (f < 3000) return "Familiar";
    if (f < 5000) return "Friend";
    if (f < 7000) return "Trusted";
    if (f < 8500) return "Close";
    if (f < 9500) return "Bonded";
    return "Soulbound";
}

// --- coarse care readout (see pet.hpp for why these exist) ---
int care_tier(float v)
{
    if (v < 15.0f) return 0;
    if (v < 40.0f) return 1;
    if (v < 70.0f) return 2;
    if (v < 92.0f) return 3;
    return 4;
}

const char* hunger_label(float v)
{
    static const char* L[5] = { "Starving", "Hungry", "Peckish", "Content", "Full" };
    return L[care_tier(v)];
}

const char* mood_label(float v)
{
    static const char* L[5] = { "Miserable", "Glum", "Okay", "Happy", "Delighted" };
    return L[care_tier(v)];
}

