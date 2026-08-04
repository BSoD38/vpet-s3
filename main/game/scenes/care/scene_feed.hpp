#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"

// Food picker, reached from Home's Feed action. Replaces the old one-tap Feed: which food
// you choose is the player's main day-to-day way of shaping the creature's temperament
// (docs/food-and-feeding.md), so it deserves a real choice screen.
//
// Rows are drawn from the data-driven FoodRegistry and scroll, so a modded food pack of
// any size works without a layout change. Each row shows only a colour swatch, a name and
// a line of flavour text -- deliberately NO numbers, since a food's effect on personality
// is meant to be read from theme rather than min-maxed.
class SceneFeed : public Scene {
    ListView list_;

public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;
};
