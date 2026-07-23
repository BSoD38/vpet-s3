#pragma once
#include "core/scene.hpp"

// Cheats / debug screen (reached from Settings). A playtesting aid: instantly restore
// HP / stamina / care, nudge or max/zero each battle stat, and morph the pet into any
// species in the roster. All actions bypass normal gating and persist immediately via the
// Pet cheat helpers. Not gated behind the debug flag - it's a dev build convenience.
class SceneCheats : public Scene {
public:
    void render() override;
    void onInput(const Input& in) override;
};
