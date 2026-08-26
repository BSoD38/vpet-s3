#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"

// Medicine picker, reached from Home's Heal action. Replaces the old one-tap Heal, which was
// free, instant and paid friendship -- so an attentive player never faced a decision and the
// condition track was a non-event for anyone actually holding the device
// (docs/economy-and-inventory.md 4).
//
// Rows are the CARE items currently in the bag, so this screen is about what you prepared
// earlier rather than what exists in the world. That is the whole tension: medicine is
// pocket change, but only if you bought some before you needed it.
//
// Deliberately shows the pet's condition and any running cooldown at the top: those two
// facts decide whether a dose will do anything at all, and finding out by tapping and being
// refused is a bad way to learn it.
class SceneMedicine : public Scene {
    ListView list_;

    // Bag slots that hold a care item, resolved once on entry. The bag is small and this
    // screen is not on the render path, so it is rebuilt per visit rather than watched.
    static const int MAX_ROWS = 32;
    int16_t rows_[MAX_ROWS];      // inventory slot indices
    int     rowCount_ = 0;

    void build();

public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;
};
