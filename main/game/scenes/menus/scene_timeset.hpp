#pragma once
#include "core/scene.hpp"

// Edit the RTC date/time with +/- steppers (press-and-hold to auto-repeat) and
// write it to the PCF85063. Reached from Settings -> Set Time/Date.
class SceneTimeSet : public Scene {
    int y_ = 2026, mon_ = 1, day_ = 1, hour_ = 0, min_ = 0;
    // held-stepper auto-repeat state
    bool    down_ = false;
    int16_t px_ = 0, py_ = 0;
    int     heldRow_ = -1, heldDir_ = 0;
    float   heldTime_ = 0.0f;    // how long the current stepper has been held
    float   repeatAcc_ = 0.0f;   // accumulates toward the next repeat step

    void step(int row, int dir);
public:
    void onEnter() override;   // load current RTC values into the editor
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
