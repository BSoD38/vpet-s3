#pragma once
#include "core/scene.hpp"

// Settings: game-speed multiplier (a testing aid that accelerates the whole
// live simulation) plus read-only species info. Reached from Menu -> Settings.
class SceneSettings : public Scene {
public:
    void render() override;
    void onInput(const Input& in) override;
};
