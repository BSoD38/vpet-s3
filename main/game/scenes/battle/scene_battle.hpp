#pragma once
#include "core/scene.hpp"
#include "battle/battle.hpp"

// Which mode drove this fight (set via setup() before the scene is entered).
enum class BattleMode : uint8_t { Quick, Tower };

// The live battle scene (docs/battle-system.md, Phase 2). Renders two combatants over an
// animated arena with stylized HP/ATB/special gauges, pre-armed Strike/Special buttons, a
// timing ring for attacks and a parry ring for defense, judgment feedback (MISS/GOOD/
// GREAT/PERFECT), hit-sparks, damage popups, particles, shockwaves and screen-shake.
// Specials use a distinct 3-hit combo QTE and are unparryable. Player taps feed the Battle
// core's manual hooks; the enemy is auto. All effects are spawned off the core's events.
class SceneBattle : public Scene {
public:
    // Configure the mode before switching to this scene. towerFloor is the floor to
    // attempt (Tower mode only; ignored for Quick).
    void setup(BattleMode mode, int towerFloor) { mode_ = mode; towerFloor_ = towerFloor; }

    void onEnter() override;
    void onExit()  override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;

private:
    enum class Ring { None, Attack, Parry };

    Battle  battle_;
    float   t_ = 0.0f;                 // scene clock (bob/pulse phases)
    Move    armed_ = Move::Strike;     // pre-armed offensive move

    // input latch, consumed in update()
    bool    tapped_ = false;
    int16_t tapX_ = 0, tapY_ = 0;

    // timing ring
    Ring    ring_  = Ring::None;
    float   ringT_ = 0.0f;             // 0..1 shrink progress

    // Special = a distinct, lengthier 3-hit combo QTE (aggregate timing -> one big hit)
    static const int COMBO_HITS = 3;
    int     comboStep_ = -1;           // -1 = not a combo; 0..COMBO_HITS during a Special
    float   comboSum_  = 0.0f;

    // judgment banner (MISS/GOOD/GREAT/PERFECT/SPECIAL/PARRY)
    const char* judgeText_ = nullptr;
    uint16_t judgeColor_ = 0;
    float    judgeT_ = 0.0f;

    // per-side animation state (seconds remaining, count down to 0)
    float   lunge_[2] = {0, 0};
    float   flash_[2] = {0, 0};
    float   faint_[2] = {0, 0};

    // HP phantom-gauge: the front bar tracks HP promptly; a dark "ghost" lags behind to
    // show the chunk just lost, holds ~0.5s, then eases down to the front (JRPG-style).
    float   hpFront_[2] = {0, 0};
    float   hpGhost_[2] = {0, 0};
    float   hpHold_[2]  = {0, 0};      // seconds the ghost holds before catching down
    float   hpPrev_[2]  = {0, 0};      // last real HP (to detect a hit)

    // screen shake (offset applied to the whole scene incl. HUD; specials shake harder)
    float   shake_ = 0.0f;
    float   shakeAmp_ = 6.0f;
    float   shakeX_ = 0.0f, shakeY_ = 0.0f;

    // single expanding shockwave (special impact / great parry)
    float    shock_ = 0.0f;
    int      shockX_ = 0, shockY_ = 0;
    uint16_t shockColor_ = 0;

    struct Popup { float x, y, vy, life; int32_t amount; uint8_t kind; bool used; };
    static const int MAX_POP = 8;
    Popup   pops_[MAX_POP] = {};

    struct Part { float x, y, vx, vy, life; uint16_t color; bool used; };
    static const int MAX_PART = 48;
    Part    parts_[MAX_PART] = {};

    bool    done_ = false;
    float   resultT_ = 0.0f;
    BattleOutcome outcome_ {};          // stakes/rewards applied at battle end (for the banner)

    // mode config
    BattleMode mode_ = BattleMode::Quick;
    int   towerFloor_ = 1;              // Tower: the floor being fought
    bool  bossFight_ = false;           // Tower: this floor is a boss (every 5th)

    void  buildCombatants();
    void  drainEvents();
    void  startRing(Ring k);
    void  resolveRing(float quality);
    float ringRadius() const;
    float ringQuality() const;
    void  setJudge(const char* text, uint16_t color);
    void  spawnPopup(int side, int32_t amount, uint8_t kind);
    void  spawnParticles(int side, int n, uint16_t color);
    void  spawnShock(int side, uint16_t color);

    void  drawArena();
    void  drawCombatant(int side);
    void  drawGauge(int x, int y, int w, int h, float frac, uint16_t lo, uint16_t hi, uint16_t border, bool glow);
    void  drawHpGauge(int x, int y, int w, int h, float frac, float ghost, uint16_t lo, uint16_t hi, uint16_t border);
    void  drawHud();
    void  drawButtons();
    void  drawBtn(int x, const char* label, bool armed, bool enabled, float pulse);
    void  drawRing();
    void  drawEffects();
    void  drawJudge();
    void  drawResult();
};
