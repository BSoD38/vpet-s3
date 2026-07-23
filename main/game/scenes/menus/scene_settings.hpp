#pragma once
#include "core/scene.hpp"

// Settings: two tabbed pages. GAME = game-speed multiplier (a testing aid that accelerates
// the whole live simulation). SYSTEM = debug overlay toggle, set time/date, cheats screen,
// species info. Reached from Menu -> Settings.
class SceneSettings : public Scene {
    int page_ = 0;   // 0 = Game, 1 = System
public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;
};
