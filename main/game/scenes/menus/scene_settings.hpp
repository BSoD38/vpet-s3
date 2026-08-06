#pragma once
#include "core/scene.hpp"

// Settings: two tabbed pages. GAME = game-speed multiplier (a testing aid that accelerates
// the whole live simulation). SYSTEM = debug overlay toggle, set time/date, cheats screen,
// factory reset, species info. Reached from Menu -> Settings.
class SceneSettings : public Scene {
    int page_ = 0;   // 0 = Game, 1 = System
    // Factory-reset confirmation: a dedicated sub-page with a HOLD-to-erase button, so a
    // stray double-tap can never wipe the save. holdT_ accumulates while the finger stays
    // on the button and resets the moment it leaves.
    bool    confirmReset_ = false;
    float   holdT_ = 0.0f;
    bool    down_ = false;         // live touch state (mirrored from onInput for update())
    int16_t tx_ = 0, ty_ = 0;
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
