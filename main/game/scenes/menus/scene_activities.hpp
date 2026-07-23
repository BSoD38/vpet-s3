#pragma once
#include "core/scene.hpp"

// Activities picker: a list of training minigames, each labelled with the stat(s) it
// trains. Reached from Menu -> Activities (gated by Pet::activitiesUnlocked). The list is
// a static registry table in the .cpp; adding a minigame = adding one row. Species/stage-
// exclusive games use a per-row `available(pet)` predicate: unavailable entries render
// greyed with a hint (so the player learns what to raise to unlock them). The list is
// sized to fit the current roster on one screen; if it outgrows MAX_VISIBLE it shows a
// "(+N more)" hint -- a scroll/paging pass is a later polish, tracked in the .cpp.
class SceneActivities : public Scene {
public:
    void render() override;
    void onInput(const Input& in) override;
};
