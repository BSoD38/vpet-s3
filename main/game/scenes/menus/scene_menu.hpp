#pragma once
#include "core/scene.hpp"

// Action menu: list of care actions + Back. Phase 1 only navigates; Phase 2 wires actions.
class SceneMenu : public Scene {
public:
    void render() override;
    void onInput(const Input& in) override;
};
