#include "battle/battle.hpp"
#include "sim/creatures.hpp"   // Attribute enum, Creature
#include "sim/pet.hpp"         // Pet
#include "engine/util.hpp"     // clampf
#include "esp_log.h"
#include <cstring>
#include <cmath>

// --- tunable balance constants (design intent, not final; see docs/battle-system.md) ---
static const float SPECIAL_MULT     = 2.2f;    // Special uses INT * this (hits noticeably harder than Strike)
static const float Q_GLANCE         = 0.30f;   // timing quality below this = glancing hit
static const float Q_CRIT           = 0.85f;   // at/above this = crit
static const float SKILL_GLANCE     = 0.60f;
static const float SKILL_NORMAL_LO  = 0.90f;
static const float SKILL_NORMAL_HI  = 1.20f;
static const float SKILL_CRIT       = 1.75f;
static const float TYPE_ADV         = 1.25f;   // hitting the type you beat
static const float TYPE_DIS         = 0.75f;   // hitting the type that beats you
static const float PARRY_MAX        = 0.75f;   // a perfect parry blocks this fraction
static const float PARRY_OK_Q       = 0.60f;   // parry quality that counts as a "success" (flavor/event)
static const float METER_PER_HIT    = 0.34f;   // ~3 landed hits to fill the special meter
static const float METER_PER_PARRY  = 0.15f;
static const int64_t CHIP           = 1;       // minimum damage (keeps fair fights from stalling)
// ATB charge: gauge fill time scales smoothly from SLOW (at AGI 0) to FAST (at the AGI cap)
// with NO early plateau — training AGI keeps paying off all the way to 9999.
static const float ATB_FILL_SLOW    = 3.00f;   // seconds to fill at AGI 0
static const float ATB_FILL_FAST    = 1.25f;   // seconds to fill at AGI cap
static const float ATB_AGI_CAP      = 9999.0f; // AGI at which fill time bottoms out (the stat cap)
// Perfect-parry evasion: a perfect parry rolls a small, AGI-scaled chance to fully dodge.
static const float PARRY_PERFECT_Q  = 0.85f;   // parry quality needed to even roll for a dodge
static const float DODGE_MAX        = 0.15f;   // max full-evade chance, reached at the AGI cap
static const float DODGE_AGI_CAP    = 9999.0f;
static const float AI_Q_SPREAD      = 0.25f;   // timing noise band for auto/AI actors

// --- PACING: keeping the two ATB gauges out of phase -------------------------------------
// Left alone, the scheduler locks the two sides into lockstep. Both start at 0, both fill at
// a rate fixed by AGI, and both reset to exactly 0 after acting -- so two combatants with
// similar AGI arrive ready on the same frame, trade a matched pair of blows, and re-sync for
// the next cycle. The fight reads as call-and-response with dead air between, forever.
//
// Three small pieces of variation break that up. None of them touch damage, so none of them
// change who wins -- only when the blows land:
//
//   1. An opening stagger. The side that would have won the ready-tie anyway (higher AGI)
//      gets a rolled head start on its first charge, putting the gauges a quarter to a half
//      cycle apart from the first exchange. AGI still decides who opens.
//   2. A charge-rate roll, refreshed every time a gauge starts refilling. Without it the
//      phase set by the stagger would hold exactly; with it the two sides keep drifting past
//      each other, so no two exchanges are spaced quite alike.
//   3. A beat between actions. Even out of phase, both gauges do sometimes fill together --
//      and a defender whose own bar is already full would otherwise counter on the very next
//      frame, which reads as one simultaneous blow. Gauges keep charging through the beat;
//      only the *start* of the next action waits.
static const float STAGGER_MIN      = 0.25f;   // opening head start for the faster side, in bars
static const float STAGGER_MAX      = 0.55f;
static const float ATB_RATE_JITTER  = 0.12f;   // per-refill charge-rate roll, +/- this fraction
static const float ACTION_GAP_MIN   = 0.28f;   // shortest beat between two consecutive actions (s)
static const float ACTION_GAP_MAX   = 0.62f;

static float skill_mult(float q)
{
    if (q < Q_GLANCE) return SKILL_GLANCE;
    if (q >= Q_CRIT)  return SKILL_CRIT;
    float t = (q - Q_GLANCE) / (Q_CRIT - Q_GLANCE);
    return SKILL_NORMAL_LO + t * (SKILL_NORMAL_HI - SKILL_NORMAL_LO);
}

static BattleEvent skill_class(float q)
{
    if (q < Q_GLANCE) return BattleEvent::Glance;
    if (q >= Q_CRIT)  return BattleEvent::Crit;
    return BattleEvent::Hit;
}

// Vaccine > Data > Virus > Vaccine; Free (or same type) is neutral.
static float type_mult(uint8_t att, uint8_t def)
{
    if (att == ATTR_FREE || def == ATTR_FREE || att == def) return 1.0f;
    bool adv = (att == ATTR_VACCINE && def == ATTR_DATA) ||
               (att == ATTR_DATA    && def == ATTR_VIRUS) ||
               (att == ATTR_VIRUS   && def == ATTR_VACCINE);
    return adv ? TYPE_ADV : TYPE_DIS;
}

static float atb_rate(const Combatant& c)
{
    float a = clampf((float)c.stat[STAT_AGI] / ATB_AGI_CAP, 0.0f, 1.0f);
    float fill = ATB_FILL_SLOW + (ATB_FILL_FAST - ATB_FILL_SLOW) * a;   // 3.00s (AGI 0) -> 1.25s (AGI cap)
    return 1.0f / fill;   // bars per second
}

// --- Battle ------------------------------------------------------------------

float Battle::rnd01()
{
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return (float)(rng_ & 0xFFFFFF) / (float)0x1000000;
}

float Battle::rollRateJitter()
{
    return 1.0f + (rnd01() * 2.0f - 1.0f) * ATB_RATE_JITTER;
}

float Battle::synthQuality(float aiSkill)
{
    // Center on skill; better players are both higher and tighter (less spread).
    float noise = (rnd01() * 2.0f - 1.0f) * AI_Q_SPREAD * (1.0f - aiSkill * 0.5f);
    return clampf(aiSkill + noise, 0.0f, 1.0f);
}

Move Battle::chooseMove(const Combatant& c) const
{
    return c.meter >= 1.0f ? Move::Special : Move::Strike;   // spend the meter as soon as it's ready
}

void Battle::emit(BattleEvent t, int actor, int target, int32_t amount, Move m)
{
    uint8_t next = (uint8_t)((rHead_ + 1) & 63);
    if (next == rTail_) return;                     // ring full: drop (consumer should drain each frame)
    ring_[rHead_] = { t, (uint8_t)actor, (uint8_t)target, amount, m };
    rHead_ = next;
}

bool Battle::pollEvent(BattleEventRec& out)
{
    if (rTail_ == rHead_) return false;
    out = ring_[rTail_];
    rTail_ = (uint8_t)((rTail_ + 1) & 63);
    return true;
}

void Battle::begin(const Combatant& player, const Combatant& enemy, uint32_t seed)
{
    c_[0] = player;
    c_[1] = enemy;
    for (int i = 0; i < 2; i++) { c_[i].atb = 0.0f; c_[i].meter = 0.0f; }
    state_  = St::Charging;
    active_ = -1;
    winner_ = -1;
    rng_    = seed ? seed : 1u;
    rHead_ = rTail_ = 0;
    gap_    = 0.0f;

    // Opening stagger (PACING 1). The head start goes to the side that would have won the
    // ready-tie anyway, so AGI still decides who opens; on a true tie the coin decides,
    // which also drops the standing first-move edge side 0 used to get for free.
    for (int i = 0; i < 2; i++) rateJit_[i] = rollRateJitter();
    int lead = (c_[0].stat[STAT_AGI] > c_[1].stat[STAT_AGI]) ? 0
             : (c_[1].stat[STAT_AGI] > c_[0].stat[STAT_AGI]) ? 1
             : (rnd01() < 0.5f ? 0 : 1);
    c_[lead].atb = STAGGER_MIN + rnd01() * (STAGGER_MAX - STAGGER_MIN);

    emit(BattleEvent::BattleStart, 0);
}

void Battle::update(float dt)
{
    if (state_ != St::Charging) return;   // finished, or awaiting a manual submit

    for (int i = 0; i < 2; i++) {
        c_[i].atb += atb_rate(c_[i]) * rateJit_[i] * dt;   // PACING 2
        if (c_[i].atb > 1.0f) c_[i].atb = 1.0f;
    }

    // PACING 3: the beat runs down while the gauges keep filling, so it never costs the
    // fight time -- it only stops two actions from starting in the same breath.
    if (gap_ > 0.0f) { gap_ -= dt; return; }

    bool r0 = c_[0].atb >= 1.0f, r1 = c_[1].atb >= 1.0f;
    int rdy = -1;
    if (r0 && r1) rdy = (c_[0].stat[STAT_AGI] >= c_[1].stat[STAT_AGI]) ? 0 : 1;  // faster acts first
    else if (r0)  rdy = 0;
    else if (r1)  rdy = 1;
    if (rdy < 0) return;

    beginAction(rdy);   // resolve exactly one action per update to keep resolution serialized
}

void Battle::beginAction(int actor)
{
    active_ = actor;
    emit(BattleEvent::TurnReady, actor);
    if (auto_[actor]) doAttack(actor, chooseMove(c_[actor]), synthQuality(c_[actor].aiSkill));
    else              state_ = St::AwaitAtk;   // wait for the player's submitAttack
}

void Battle::submitAttack(Move m, float quality)
{
    if (state_ != St::AwaitAtk) return;
    state_ = St::Charging;                     // doAttack will re-arm AwaitParry if needed
    doAttack(active_, m, clampf(quality, 0.0f, 1.0f));
}

void Battle::doAttack(int actor, Move m, float atkQ)
{
    if (m == Move::Special && c_[actor].meter < 1.0f) m = Move::Strike;   // safety: no meter, no special
    if (m == Move::Special) c_[actor].meter = 0.0f;                       // consume the meter
    emit(BattleEvent::AttackStart, actor, 1 - actor, 0, m);

    // Specials are UNPARRYABLE — they resolve immediately at full force, whoever the
    // defender is. That's what makes spending the meter matter.
    int def = 1 - actor;
    bool parryable = (m != Move::Special);
    if (!parryable || auto_[def]) {
        float pq = parryable ? synthQuality(c_[def].aiSkill) : 0.0f;
        resolveAttack(actor, m, atkQ, pq);
        finishAction();
    } else {
        pendMove_ = m; pendAtkQ_ = atkQ;       // hand off to the player's parry
        state_ = St::AwaitParry;
    }
}

void Battle::submitParry(float quality)
{
    if (state_ != St::AwaitParry) return;
    resolveAttack(active_, pendMove_, pendAtkQ_, clampf(quality, 0.0f, 1.0f));
    finishAction();
}

void Battle::resolveAttack(int actor, Move m, float atkQ, float parryQ)
{
    Combatant& A = c_[actor];
    Combatant& D = c_[1 - actor];

    // Perfect parry + agility: a small chance to completely evade the hit (no damage).
    // Specials are unparryable (parryQ = 0), so they can never be dodged.
    if (parryQ >= PARRY_PERFECT_Q) {
        float dodge = DODGE_MAX * clampf((float)D.stat[STAT_AGI] / DODGE_AGI_CAP, 0.0f, 1.0f);
        if (rnd01() < dodge) {
            emit(BattleEvent::Dodge, 1 - actor, actor, 0, m);
            float b = D.meter;
            D.meter = clampf(D.meter + METER_PER_PARRY, 0.0f, 1.0f);
            if (b < 1.0f && D.meter >= 1.0f) emit(BattleEvent::MeterFull, 1 - actor);
            return;
        }
    }

    float atkPow = (m == Move::Special) ? (float)A.stat[STAT_INT] * SPECIAL_MULT
                                        : (float)A.stat[STAT_STR];
    float raw = atkPow * skill_mult(atkQ) * type_mult(A.attribute, D.attribute);
    // Defense: diminishing curve. When END >> raw, damage collapses toward the chip
    // floor (the tier wall); when raw >> END, damage approaches raw.
    float mit = (raw <= 0.0f) ? 0.0f : raw * raw / (raw + (float)D.stat[STAT_END]);
    mit *= (1.0f - PARRY_MAX * clampf(parryQ, 0.0f, 1.0f));

    int64_t dmg = (int64_t)(mit + 0.5f);
    if (dmg < CHIP) dmg = CHIP;

    emit(skill_class(atkQ), actor, 1 - actor, (int32_t)dmg, m);
    // Only when a parry was actually possible. A Special is unparryable and resolves with
    // parryQ = 0, so emitting here called every special a failed parry -- an outcome for a
    // window that never opened, which the scene then sounded.
    if (m != Move::Special)
        emit(parryQ >= PARRY_OK_Q ? BattleEvent::ParryOk : BattleEvent::ParryMiss, 1 - actor, actor);

    D.hp -= dmg;
    if (D.hp < 0) D.hp = 0;
    emit(BattleEvent::Hurt, 1 - actor, actor, (int32_t)dmg, m);

    float beforeA = A.meter;
    A.meter = clampf(A.meter + METER_PER_HIT, 0.0f, 1.0f);
    if (beforeA < 1.0f && A.meter >= 1.0f) emit(BattleEvent::MeterFull, actor);
    if (parryQ >= PARRY_OK_Q) {
        float beforeD = D.meter;
        D.meter = clampf(D.meter + METER_PER_PARRY, 0.0f, 1.0f);
        if (beforeD < 1.0f && D.meter >= 1.0f) emit(BattleEvent::MeterFull, 1 - actor);
    }

    checkEnd();
}

void Battle::finishAction()
{
    if (active_ >= 0) {
        c_[active_].atb = 0.0f;                        // reset the actor's charge
        rateJit_[active_] = rollRateJitter();          // fresh roll for the refill (PACING 2)
    }
    active_ = -1;
    gap_ = ACTION_GAP_MIN + rnd01() * (ACTION_GAP_MAX - ACTION_GAP_MIN);   // PACING 3
    if (state_ != St::Done) state_ = St::Charging;
}

void Battle::checkEnd()
{
    for (int i = 0; i < 2; i++) {
        if (c_[i].hp <= 0) {
            emit(BattleEvent::Faint, i);
            winner_ = 1 - i;
            emit(winner_ == 0 ? BattleEvent::Victory : BattleEvent::Defeat, winner_);
            state_ = St::Done;
            return;
        }
    }
}

// --- factories ---------------------------------------------------------------

// Guarantee playable minimums (covers a freshly-hatched/egg pet whose battle stats are
// still ~0). Applied by every factory so any battle entry point yields a fightable side.
static void floor_stats(Combatant& c)
{
    if (c.stat[STAT_MAXHP] < 10) { c.stat[STAT_MAXHP] = 40; c.maxHp = 40; c.hp = 40; }
    if (c.stat[STAT_STR] < 1) c.stat[STAT_STR] = 6;
    if (c.stat[STAT_END] < 1) c.stat[STAT_END] = 5;
    if (c.stat[STAT_AGI] < 1) c.stat[STAT_AGI] = 8;
    if (c.stat[STAT_INT] < 1) c.stat[STAT_INT] = 5;
}

Combatant combatant_from_pet(const Pet& pet)
{
    Combatant c{};
    strncpy(c.name, pet.displayName(), sizeof c.name - 1);
    const Creature* cr = pet.creature();
    c.attribute = cr ? cr->attribute : (uint8_t)ATTR_FREE;
    for (int i = 0; i < STAT_COUNT; i++) c.stat[i] = pet.stat((StatId)i);
    c.spriteIdx = pet.creatureIndex();
    c.maxHp = c.stat[STAT_MAXHP];
    float frac = clampf(pet.state().health / 100.0f, 0.0f, 1.0f);
    c.hp = (int64_t)((float)c.maxHp * frac + 0.5f);
    c.aiSkill = 1.0f;
    floor_stats(c);
    return c;
}

Combatant combatant_from_creature(const Creature& cr, int idx, float scale, float aiSkill)
{
    Combatant c{};
    strncpy(c.name, cr.name, sizeof c.name - 1);
    c.attribute = cr.attribute;
    c.stat[STAT_MAXHP] = (uint32_t)((float)cr.baseHp  * scale);
    c.stat[STAT_STR]   = (uint32_t)((float)cr.baseStr * scale);
    c.stat[STAT_END]   = (uint32_t)((float)cr.baseEnd * scale);
    c.stat[STAT_AGI]   = (uint32_t)((float)cr.baseAgi * scale);
    c.stat[STAT_INT]   = (uint32_t)((float)cr.baseInt * scale);
    c.spriteIdx = idx;
    c.maxHp = c.stat[STAT_MAXHP];
    c.hp = c.maxHp;
    c.aiSkill = clampf(aiSkill, 0.0f, 1.0f);
    floor_stats(c);
    return c;
}

// --- self-test (dev utility; not on the game path) ---------------------------

static const char* STAG = "BTL";

static Combatant mk(const char* name, uint8_t attr, uint32_t hp,
                    uint32_t str, uint32_t end, uint32_t agi, uint32_t intel, float skill)
{
    Combatant c{};
    strncpy(c.name, name, sizeof c.name - 1);
    c.attribute = attr;
    c.stat[STAT_MAXHP] = hp; c.stat[STAT_STR] = str; c.stat[STAT_END] = end;
    c.stat[STAT_AGI] = agi;  c.stat[STAT_INT] = intel;
    c.maxHp = hp; c.hp = hp; c.aiSkill = skill; c.spriteIdx = -1;
    return c;
}

static void log_event(Battle& b, const BattleEventRec& ev)
{
    switch (ev.type) {
        case BattleEvent::Hit:
        case BattleEvent::Crit:
        case BattleEvent::Glance:
            ESP_LOGI(STAG, "  %s %s -> %s %s for %d",
                     b.side(ev.actor).name, ev.move == Move::Special ? "SPECIAL" : "strike",
                     b.side(ev.target).name,
                     ev.type == BattleEvent::Crit ? "CRIT" : ev.type == BattleEvent::Glance ? "glance" : "hit",
                     (int)ev.amount);
            break;
        case BattleEvent::Hurt:
            ESP_LOGI(STAG, "    %s hp %lld/%u", b.side(ev.actor).name,
                     (long long)b.side(ev.actor).hp, b.side(ev.actor).maxHp);
            break;
        case BattleEvent::Faint:   ESP_LOGI(STAG, "  %s fainted", b.side(ev.actor).name); break;
        case BattleEvent::Victory: ESP_LOGI(STAG, "  >>> side0 VICTORY"); break;
        case BattleEvent::Defeat:  ESP_LOGI(STAG, "  >>> side0 DEFEAT"); break;
        default: break;
    }
}

// Run one auto-vs-auto battle to completion; returns winner (0/1). Counts hits landed.
static int run_one(Combatant p, Combatant e, uint32_t seed, int* hits, float* winnerHpFrac)
{
    Battle b; b.begin(p, e, seed); b.setAuto(0, true); b.setAuto(1, true);
    int h = 0, guard = 0;
    BattleEventRec ev;
    while (!b.finished() && guard++ < 20000) {
        b.update(0.05f);
        while (b.pollEvent(ev)) if (ev.type == BattleEvent::Hurt) h++;
    }
    int w = b.winner() < 0 ? 0 : b.winner();
    if (hits) *hits = h;
    if (winnerHpFrac) *winnerHpFrac = b.side(w).maxHp ? (float)b.side(w).hp / (float)b.side(w).maxHp : 0.0f;
    return b.winner();
}

static void run_series(const char* label, Combatant p, Combatant e, int n)
{
    int w0 = 0; long totalHits = 0; float totalHp = 0.0f;
    for (int i = 0; i < n; i++) {
        int hits = 0; float hpf = 0.0f;
        int w = run_one(p, e, 1000u + (uint32_t)i * 7919u, &hits, &hpf);
        if (w == 0) w0++;
        totalHits += hits; totalHp += hpf;
    }
    ESP_LOGI(STAG, "%s: side0 wins %d/%d | avg hits %ld | avg winner HP %.0f%%",
             label, w0, n, totalHits / n, 100.0f * totalHp / (float)n);
}

void battle_selftest()
{
    ESP_LOGI(STAG, "===== battle self-test =====");

    // A) mirror match — full blow-by-blow so the mechanics are visible.
    ESP_LOGI(STAG, "-- A) mirror match (blow-by-blow) --");
    {
        Combatant p = mk("Alpha", ATTR_DATA, 60, 10, 8, 12, 9, 0.7f);
        Combatant e = mk("Beta",  ATTR_DATA, 60, 10, 8, 12, 9, 0.7f);
        Battle b; b.begin(p, e, 12345); b.setAuto(0, true); b.setAuto(1, true);
        int guard = 0; BattleEventRec ev;
        while (!b.finished() && guard++ < 4000) {
            b.update(0.05f);
            while (b.pollEvent(ev)) log_event(b, ev);
        }
        ESP_LOGI(STAG, "  result: winner=%d  Alpha %lld/%u  Beta %lld/%u", b.winner(),
                 (long long)b.side(0).hp, b.side(0).maxHp, (long long)b.side(1).hp, b.side(1).maxHp);
    }

    // B) tier wall: a baby cannot out-DPS a mega no matter what.
    run_series("-- B) baby(side0) vs mega(side1)",
               mk("Baby", ATTR_DATA,  30,  4,  4,  5,  4, 0.7f),
               mk("Mega", ATTR_DATA, 220, 22, 16, 30, 18, 0.7f), 8);

    // C) type advantage decides an otherwise-even fight (side0 Vaccine beats side1 Data).
    run_series("-- C) type adv: Vaccine(side0) vs Data(side1)",
               mk("Vac", ATTR_VACCINE, 60, 10, 8, 12, 9, 0.7f),
               mk("Dat", ATTR_DATA,    60, 10, 8, 12, 9, 0.7f), 8);

    // D) skill decides an even-stat fight, but doesn't jump tiers (same pool both sides).
    run_series("-- D) skill: 0.95(side0) vs 0.30(side1)",
               mk("Pro",  ATTR_DATA, 60, 10, 8, 12, 9, 0.95f),
               mk("Newb", ATTR_DATA, 60, 10, 8, 12, 9, 0.30f), 8);

    ESP_LOGI(STAG, "===== self-test done =====");
}
