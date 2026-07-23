#pragma once
#include "core/scene.hpp"

// Endless hurdle-runner minigame (old-Tamagotchi style): the pet auto-runs to the
// right, tap the screen to jump over hurdles, and the pace ramps up over time.
// The final score trains Agility + Max HP and nudges the friendship bond.
class SceneRun : public Scene {
    enum Phase { READY, RUNNING, OVER };
    static const int MAX_H = 8;         // max simultaneous hurdles (headroom over the ~3 typical)

    Phase   phase_ = READY;
    float   t_ = 0.0f;                  // animation clock
    float   runTime_ = 0.0f;            // seconds spent running (drives the speed ramp)
    float   speed_ = 0.0f;              // current scroll speed (px/s)
    float   dist_ = 0.0f;              // total distance scrolled (for ground ticks)
    float   py_ = 0.0f;                 // pet vertical offset (0 = ground, <0 = airborne)
    float   vy_ = 0.0f;                 // pet vertical velocity
    bool    grounded_ = true;
    float   spawnTimer_ = 0.0f;         // seconds since the last GROUND hurdle spawned
    float   nextSpawnT_ = 0.0f;         // seconds until the next GROUND hurdle (time-based so the
                                        // pixel gap widens with speed -> always jumpable)
    float   flyTimer_ = 0.0f;           // seconds since the last flyer attempt (independent stream)
    float   flyNextT_ = 0.0f;           // seconds until the next flyer attempt
    float   hx_[MAX_H];                 // hurdle x positions (only if active)
    uint8_t hType_[MAX_H];             // obstacle type (index into the OB[] table)
    bool    hActive_[MAX_H];
    bool    hCounted_[MAX_H];           // already added to the score
    int     score_ = 0;
    bool    tapped_ = false;            // input rising edge, consumed each update
    uint8_t prevType_ = 0;             // last obstacle type spawned
    uint8_t pendingType_ = 0;          // next obstacle type queued to spawn
    bool    rewarded_ = false;          // stat reward granted for this run (once)
    bool    tired_ = false;             // energy limited this run's stat gains (for the card)
    int     gainAgi_ = 0, gainHp_ = 0;  // amounts granted, shown on the game-over card

    void  reset();
    void  spawnHurdle(int type);
    int   pickType() const;            // random unlocked type for the current score
    float gapFor(int prev, int next) const;  // seconds until the next obstacle
    void  award();                     // grant stat/friendship gains once, on game over
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
    bool allowsSleep() const override { return false; }   // never nap mid-run
    float careSpeed() const override { return IN_PLAY_CARE_SPEED; }  // care frozen while playing
};
