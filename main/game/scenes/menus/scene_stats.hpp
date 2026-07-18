#pragma once
#include "core/scene.hpp"

// Creature "character sheet": identity + care meters + RPG battle stats + friendship
// TIER only (exact bond value hidden), with a Rename button. Reached from Menu -> Stats.
class SceneStats : public Scene {
public:
    void render() override;
    void onInput(const Input& in) override;
};
