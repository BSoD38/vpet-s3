#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"      // ListView (species picker)
#include "sim/creatures.hpp"   // CreatureRegistry::MAX (picker display-order table)
#include <cstdint>

// Cheats / debug screen (reached from Settings). A playtesting aid: instantly restore
// HP / stamina / care, nudge or max/zero each battle stat, and morph the pet into any
// species in the roster. All actions bypass normal gating and persist immediately via the
// Pet cheat helpers. Not gated behind the debug flag - it's a dev build convenience.
//
// Species selection is a full-screen scrolling picker: a < / > cycler stopped being
// usable once modded rosters pushed the count past a few dozen.
class SceneCheats : public Scene {
    bool     picking_ = false;   // species picker open (replaces the whole screen)?
    ListView list_;
    // Picker display order (registry indices sorted stage-then-name). The registry itself
    // must stay in load order -- saves and evolution edges address creatures by index.
    int16_t  order_[CreatureRegistry::MAX];

public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;

private:
    void renderPicker();
    void inputPicker(const Input& in);
};
