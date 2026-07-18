#pragma once
#include "core/scene.hpp"
#include "sim/pet.hpp"   // PET_NICK_MAX

// On-screen keyboard for naming the creature (up to PET_NICK_MAX chars). A-Z + space
// + backspace, a case toggle, and Done/Cancel. Reached from Stats -> Rename.
class SceneRename : public Scene {
    char  buf_[PET_NICK_MAX + 1] = {0};
    int   len_   = 0;
    bool  lower_ = false;    // lowercase entry mode
    float t_     = 0.0f;     // cursor-blink clock

    int keyAt(int x, int y) const;   // decode a touch into a letter/space (>0) or action code
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
