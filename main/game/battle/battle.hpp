#pragma once
#include <cstdint>
#include "battle/combatant.hpp"

// The two moves a combatant can arm. Special spends a full meter and hits harder (INT);
// Strike is the always-available basic attack (STR).
enum class Move : uint8_t { Strike, Special };

// Battle events the core emits as it resolves. The consumer (a logger in Phase 1, the
// battle scene + audio in Phase 2, a sprite state-machine later) drains them and reacts.
// Sides: 0 = player, 1 = enemy. Victory/Defeat are from the player's (side 0) view.
enum class BattleEvent : uint8_t {
    BattleStart, TurnReady, AttackStart,
    Hit, Crit, Glance,       // attack result class (amount = final damage)
    ParryOk, ParryMiss,      // defender's parry outcome
    Dodge,                   // perfect parry + AGI fully evaded the hit (no damage)
    Hurt,                    // defender lost HP (amount = damage)
    MeterFull, Faint,
    Victory, Defeat,
};

struct BattleEventRec {
    BattleEvent type;
    uint8_t     actor;       // event's subject side (0/1)
    uint8_t     target;      // other side (0/1), or 255 if n/a
    int32_t     amount;      // damage for Hit/Crit/Glance/Hurt, else 0
    Move        move;        // move in play (for attack/hurt events)
};

// Headless, presentation-free battle core (docs/battle-system.md). Two Combatants charge
// ATB bars in real time (rate set by AGI); a ready actor executes a pre-armed move whose
// effect scales with a 0..1 timing quality. Auto sides synthesize timing from aiSkill;
// a manual (player) side supplies it via submitAttack/submitParry. Resolution is
// serialized — only one action is in flight at a time, and the two gauges are
// deliberately kept out of phase so the sides stop trading blows in lockstep (see the
// PACING block in battle.cpp).
class Battle {
public:
    void begin(const Combatant& player, const Combatant& enemy, uint32_t seed);
    void setAuto(int side, bool a) { auto_[side & 1] = a; }

    void update(float dt);   // advance charge; auto actors resolve immediately

    // --- manual control (Phase 2 player); no-ops unless the core is awaiting that input ---
    bool awaitingAttack() const { return state_ == St::AwaitAtk; }
    bool awaitingParry()  const { return state_ == St::AwaitParry; }
    int  activeActor()    const { return active_; }   // whose turn (attack) / the attacker (parry)
    bool canSpecial(int side) const { return c_[side & 1].meter >= 1.0f; }
    void submitAttack(Move m, float quality);
    void submitParry(float quality);

    // --- event drain + status ---
    bool pollEvent(BattleEventRec& out);
    bool finished() const { return state_ == St::Done; }
    int  winner()  const { return winner_; }          // 0 player, 1 enemy, -1 ongoing
    const Combatant& side(int i) const { return c_[i & 1]; }

private:
    enum class St : uint8_t { Charging, AwaitAtk, AwaitParry, Done };

    Combatant c_[2];
    bool      auto_[2] = { true, true };
    St        state_   = St::Charging;
    int       active_  = -1;
    int       winner_  = -1;
    Move      pendMove_ = Move::Strike;   // attack awaiting a parry decision
    float     pendAtkQ_ = 0.0f;

    // --- pacing (see the PACING block in battle.cpp) ---
    float     gap_ = 0.0f;                   // beat left before the next action may start
    float     rateJit_[2] = { 1.0f, 1.0f };  // per-side charge-rate roll, refreshed on each refill

    uint32_t  rng_ = 1;
    BattleEventRec ring_[64];
    uint8_t   rHead_ = 0, rTail_ = 0;

    float rnd01();
    float rollRateJitter();
    float synthQuality(float aiSkill);
    Move  chooseMove(const Combatant& c) const;
    void  emit(BattleEvent t, int actor, int target = 255, int32_t amount = 0, Move m = Move::Strike);
    void  beginAction(int actor);
    void  doAttack(int actor, Move m, float atkQ);
    void  resolveAttack(int actor, Move m, float atkQ, float parryQ);
    void  finishAction();
    void  checkEnd();
};

// Dev utility: run auto-vs-auto matchups and log the results over serial (ESP_LOG).
// Used to validate balance in Phase 1; not called in the shipping game loop.
void battle_selftest();
