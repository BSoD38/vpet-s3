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

    // Force-evolve result, shown ON the button itself for a couple of seconds. It needs to be
    // said somewhere: the interesting outcomes are the ones where nothing visibly happens
    // ("no gate is met yet", "this form is terminal"), and an unlabelled dead button reads as
    // a broken one. The button is its own status line rather than a banner because this screen
    // has no spare vertical space and the message only ever concerns this button.
    char     evoMsg_[22] = {0};
    float    evoMsgT_    = 0.0f;   // seconds remaining
    uint16_t evoMsgCol_  = 0;

public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;

private:
    void renderPicker();
    void inputPicker(const Input& in);
};
