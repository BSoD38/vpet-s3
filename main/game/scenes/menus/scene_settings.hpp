#pragma once
#include "core/scene.hpp"

// Settings: three tabbed pages. GAME = game-speed multiplier (a testing aid that accelerates
// the whole live simulation) + care freeze. SOUND = master/music/effects volume and mute.
// SYSTEM = debug overlay toggle, set time/date, cheats screen, factory reset, species info.
// Reached from Menu -> Settings.
class SceneSettings : public Scene {
    int page_ = 0;   // 0 = Game, 1 = Sound, 2 = System
    // Factory-reset confirmation: a dedicated sub-page with a HOLD-to-erase button, so a
    // stray double-tap can never wipe the save. holdT_ accumulates while the finger stays
    // on the button and resets the moment it leaves.
    bool    confirmReset_ = false;
    float   holdT_ = 0.0f;
    bool    down_ = false;         // live touch state (mirrored from onInput for update())
    int16_t tx_ = 0, ty_ = 0;
    // Which volume slider the finger grabbed, or -1. Latched on press so a drag keeps
    // controlling the slider it started on even when the finger wanders off it vertically,
    // and so the new value is only written to NVS once, on release.
    int8_t  dragSlider_ = -1;
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
