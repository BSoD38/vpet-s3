#include "pet.hpp"
#include "save.hpp"
#include "creatures.hpp"
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
static const uint16_t FRIEND_CAP = 1000;

// Friendship deltas for care actions (0..1000 bond meter; neglect subtracts).
static const int FR_FEED    = 2;
static const int FR_CLEAN   = 3;
static const int FR_HEAL    = 5;
static const int FR_MISTAKE = 30;

// --- battle stakes/rewards (tunable) ---
static const int      FR_BATTLE_WIN   = 12;    // friendship gained on a win
static const int      FR_BATTLE_LOSS  = 15;    // friendship lost on a loss
static const float    HAP_BATTLE_LOSS = 14.0f; // happiness lost on a loss
static const uint32_t WIN_STAT_GAIN   = 2;     // per combat stat (STR/END/AGI/INT), per win
static const float    SICK_HP_THRESH  = 35.0f; // exit HP% below which sickness can roll
static const float    SICK_MAX_CHANCE = 0.9f;  // sickness chance approached as HP% -> 0

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
}

void Pet::markSaved()
{
    s_.lastUpdate = clock_now();
    save_.store(s_);
}

void Pet::boot()
{
    PCF85063_Read_Time(&datetime);   // seed the RTC global before first clock_now()

    if (save_.load(s_) && s_.magic == PET_MAGIC && s_.version == PET_VERSION) {
        if (s_.gameSpeed == 0) s_.gameSpeed = 1;
        s_.creatureId[sizeof(s_.creatureId) - 1] = '\0';   // guard against a corrupt/unterminated id
        idx_ = reg_.indexOf(s_.creatureId);                // resolve identity before ticking
        if (idx_ < 0) {                                    // creature gone (e.g. a mod was removed)
            ESP_LOGW(TAG, "saved creature '%s' not in registry; falling back to egg", s_.creatureId);
            idx_ = reg_.indexOf("egg");
            if (idx_ < 0 && reg_.count() > 0) idx_ = 0;
            const Creature* c = cur();
            if (c) { strncpy(s_.creatureId, c->id, sizeof(s_.creatureId) - 1); s_.stage = c->tier; }
        }
        uint32_t elapsed = clock_elapsed(s_.lastUpdate);
        if (elapsed > MAX_OFFLINE) elapsed = MAX_OFFLINE;
        char ab[16];
        ESP_LOGI(TAG, "loaded (%s %s, %s), offline catch-up %us",
                 speciesName(), stageName(), ageStr(ab, sizeof ab), (unsigned)elapsed);
        uint32_t rem = elapsed;   // catch up in REAL seconds (speed multiplier is live-only)
        while (rem > 0) { uint32_t step = rem > 60 ? 60 : rem; tick((float)step); rem -= step; }
    } else {
        newEgg();
    }
    save_.loadStr("nick", nickname_, sizeof nickname_, "");   // nickname lives outside the blob
    markSaved();
    Set_Backlight(s_.lightsOff ? SLEEP_BACKLIGHT : LCD_Backlight);
}

void Pet::feed()
{
    if (s_.stage == STAGE_EGG) { ESP_LOGI(TAG, "can't feed (egg)"); return; }
    if (s_.lightsOff) {                          // asleep: won't eat (matches pet/poke)
        ESP_LOGI(TAG, "refuses food (asleep)");
        refused_ = true;
        return;
    }
    if (s_.sick) {
        ESP_LOGI(TAG, "refuses food (sick)");
        refused_ = true;
        return;
    }
    s_.hunger = clampf(s_.hunger + 30, 0, 100);
    s_.happiness = clampf(s_.happiness + 3, 0, 100);
    addFriendship(FR_FEED);
    ESP_LOGI(TAG, "feed (hunger=%.0f)", s_.hunger);
    markSaved();
}

void Pet::clean()
{
    s_.poop = 0;
    addFriendship(FR_CLEAN);
    ESP_LOGI(TAG, "clean");
    markSaved();
}

void Pet::heal()
{
    s_.sick = 0;
    s_.health = clampf(s_.health + 40, 0, 100);
    addFriendship(FR_HEAL);
    ESP_LOGI(TAG, "heal (health=%.0f)", s_.health);
    markSaved();
}

void Pet::play(float amount)
{
    if (s_.stage == STAGE_EGG || s_.sick || s_.lightsOff) return;
    s_.happiness = clampf(s_.happiness + amount, 0, 100);
}

void Pet::toggleLights()
{
    s_.lightsOff = !s_.lightsOff;
    // Sync to the SCHEDULE (not to lightsOff) so this manual choice holds until the
    // next window edge — otherwise waking it mid-window re-triggers auto-sleep.
    s_.lastSleepPhase = schedSleep() ? 1 : 0;
    Set_Backlight(s_.lightsOff ? SLEEP_BACKLIGHT : LCD_Backlight);
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

void Pet::addFriendship(int delta)
{
    int v = (int)s_.friendship + delta;
    if (v < 0) v = 0;
    if (v > FRIEND_CAP) v = FRIEND_CAP;
    s_.friendship = (uint16_t)v;
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
        addFriendship(FR_BATTLE_WIN);
        o.friendDelta = FR_BATTLE_WIN;
        recordWin();
    } else {
        s_.health = 1.0f;                                    // defeat: scraped through at 1%
        addFriendship(-FR_BATTLE_LOSS);
        o.friendDelta = -FR_BATTLE_LOSS;
        s_.happiness = clampf(s_.happiness - HAP_BATTLE_LOSS, 0.0f, 100.0f);
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

const char* Pet::friendshipTier() const
{
    uint16_t f = s_.friendship;
    if (f < 100) return "Stranger";
    if (f < 300) return "Acquaintance";
    if (f < 550) return "Friend";
    if (f < 800) return "Close";
    if (f < 950) return "Bonded";
    return "Soulbound";
}
