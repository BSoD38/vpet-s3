#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"

// The relationship, made legible. Three pages:
//   MEMORIES  - conversations you've had, newest first (from the bounded journal ring).
//   ABOUT YOU - what the creature has learned about the PLAYER, in the phrasing the writer
//               chose rather than the machine key/value the gates use.
//   IN MEMORY - the lineage ledger: every creature that came before, newest first. Unlike
//               the other two pages this one survives new eggs -- it is the family history,
//               and the memorial the death event promises (docs/death-and-lifespan.md §7).
// The creature's current identity sits in the header, since it's the thing the pages are
// really about.
class SceneJournal : public Scene {
    int      tab_ = 0;
    ListView list_;

public:
    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;
};
