#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"

// "Other" menu: navigation to the stats sheet, journal, battle, activities, shop, work and
// settings. (Everyday care actions live on the Home screen; the live stat readout is on Stats.)
//
// A SCROLLING LIST rather than a fixed block of buttons. The block version had a hard ceiling:
// six entries at a 30+10 pitch already filled 292px of a 320px panel, and adding a seventh
// meant shrinking the buttons again -- which is a trick that works exactly once more. The list
// costs one widget and removes the ceiling for good, the same way it retired the Activities
// picker's "(+N more)" limit.
class SceneMenu : public Scene {
    ListView list_;

public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;
};
