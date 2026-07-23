#pragma once
#include "core/scene.hpp"

// Pre-battle mode picker: Quick Battle (a similar-tier rival) or Tower (a persisted floor
// climb). Reads/persists the Tower floor via the "twr" NVS key; configures SceneBattle
// through app().battle.setup() before switching to it.
class SceneBattleSelect : public Scene {
public:
    void render() override;
    void onInput(const Input& in) override;
};
