#pragma once
#include "core/scene.hpp"

// Steady Stance (END + Max HP trainer) - the second motion game. Keep your pet balanced on
// a beam by TILTING the device left/right. The beam is unstable (it drifts further the more
// off-center you are) and random gusts shove it, so you must constantly correct. Fall off
// and it's over; the longer you hold, the more Endurance and Max HP you train.
//
// Tilt is read from the accelerometer relative to a neutral captured when the round starts
// (so you can hold the device however is comfortable). Which sensor axis and sign map to
// "roll left/right" depends on the board mounting -> TILT_AXIS / TILT_SIGN in the .cpp are
// the calibration knobs to flip after a quick on-device test.
class SceneStance : public Scene {
    enum Phase { READY, PLAY, OVER };

    Phase phase_ = READY;
    float t_ = 0.0f;             // animation clock
    float survived_ = 0.0f;      // seconds balanced (the score)
    float bal_ = 0.0f;           // balance position -1..1 (0 = centered); fall at |bal|>=1
    float v_ = 0.0f;             // balance velocity
    float neutralA_ = 0.0f;      // tilt-axis accel captured at round start
    int   rollAxis_ = 0;         // which accel axis reads left/right roll (0=x,1=y); auto-picked at start
    float tiltInput_ = 0.0f;     // smoothed tilt control -1..1 (also drives the visual)
    float ax_ = 0.0f, ay_ = 0.0f, az_ = 1.0f;   // latest accel snapshot (g)

    float gustT_ = 0.0f;         // countdown to the next gust
    float gustFlash_ = 0.0f;     // gust visual timer
    int   gustDir_ = 1;          // last gust direction (-1/+1)

    bool  tapped_ = false;

    bool  rewarded_ = false;
    bool  tired_ = false;
    int   gainEnd_ = 0, gainHp_ = 0;

    void  reset();
    void  startPlay();
    void  award();
    float tiltAxis() const;      // the accel component used for roll (per TILT_AXIS)
    float gustInterval() const;
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
    bool allowsSleep() const override { return false; }   // never nap mid-game
    float careSpeed() const override { return IN_PLAY_CARE_SPEED; }  // care frozen while playing
};
