#pragma once
#include "core/scene.hpp"

// Smash (STR trainer) - the first motion game. SHAKE the device to charge a power meter,
// TAP to unleash a smash on the training rock. Power bleeds away when you stop shaking, so
// the skill is charging fast and striking near full (a strike at >=PERFECT_AT lands a bonus).
// Score across a timed session trains Strength (and a little Max HP for the exertion).
// Reads motion from Input (accel), captured each frame in onInput.
class SceneSmash : public Scene {
    enum Phase { READY, PLAY, OVER };

    Phase phase_ = READY;
    float t_ = 0.0f;              // animation clock
    float timeLeft_ = 0.0f;       // session countdown
    float power_ = 0.0f;          // 0..100 charge, filled by shaking, decays over time
    float shakeInst_ = 0.0f;      // smoothed shake intensity (for the jitter visual)
    float ax_ = 0.0f, ay_ = 0.0f, az_ = 1.0f;   // latest accel snapshot (g), from onInput
    float pax_ = 0.0f, pay_ = 0.0f, paz_ = 1.0f; // previous frame's accel (for the shake delta)
    int   score_ = 0;
    bool  tapped_ = false;        // tap edge, consumed each update

    // hit feedback
    float hitT_ = 0.0f;           // pet-lunge / rock-recoil timer
    int   lastHitVal_ = 0;        // last smash value (for the popup)
    bool  perfect_ = false;       // last smash was a perfect (>=PERFECT_AT) hit
    float ringR_ = 0.0f;          // expanding shockwave radius
    bool  ringOn_ = false;
    float shakeScreen_ = 0.0f;    // screen-shake magnitude (decays)

    // reward (shown on the game-over card)
    bool  rewarded_ = false;
    bool  tired_ = false;
    int   gainStr_ = 0, gainHp_ = 0;

    void reset();
    void strike();
    void award();
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
    bool allowsSleep() const override { return false; }   // never nap mid-game
    float careSpeed() const override { return IN_PLAY_CARE_SPEED; }  // care frozen while playing
};
