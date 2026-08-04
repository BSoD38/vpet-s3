#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"

// Activities picker: a list of training minigames, each labelled with the stat(s) it
// trains. Reached from Menu -> Activities (gated by Pet::activitiesUnlocked). The list is
// a static registry table in the .cpp; adding a minigame = adding one row. Species/stage-
// exclusive games use a per-row `available(pet)` predicate: unavailable entries render
// greyed with a hint (so the player learns what to raise to unlock them).
//
// The roster is drag-scrollable (ui/widgets.hpp ListView), so it can grow past a
// screenful without truncation.
class SceneActivities : public Scene {
    ListView list_;

public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;
};
