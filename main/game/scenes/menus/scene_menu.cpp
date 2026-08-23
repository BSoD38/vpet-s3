#include "scene_menu.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include <cstring>

// "Other" menu: navigation to the stats sheet, activities (minigames), and settings.
// (Everyday care actions live on the Home screen; the live stat readout is on Stats.)
// Five options now, so the spacing is tighter than the old four-item layout (which would have
// run off the bottom) and still leaves room for the lock hint underneath.
static const int OPT_X = 30, OPT_Y = 76, OPT_W = 180, OPT_H = 34, OPT_G = 12;

// One row per entry -- label, destination and gating travel together, so inserting an entry
// is one line instead of renumbering magic indices in a lock predicate AND an if/else ladder
// (the Journal insertion had to shift both, which is exactly the mistake this table removes).
struct MenuEntry {
    const char* label;
    SceneId     scene;
    // Locked until Pet::activitiesUnlocked() -- and, for the same reason, while the care
    // freeze is on: both entries lead somewhere that trains, spends or wounds the creature.
    bool        gated;
};
static const MenuEntry OPTS[] = {
    { "Stats",      SceneId::Stats,        false },  // character sheet
    { "Journal",    SceneId::Journal,      false },  // memories + facts
    { "Battle",     SceneId::BattleSelect, true  },  // battle mode picker
    { "Activities", SceneId::Activities,   true  },  // minigame picker
    { "Settings",   SceneId::Settings,     false },  // game speed etc.
};
static const int OPT_N = (int)(sizeof(OPTS) / sizeof(OPTS[0]));

// The gated entries can be shut for three unrelated reasons; only one hint fits. The freeze
// wins it (it's the one the player can act on from here). The stage lock outranks the
// condition: telling a sick NEWBORN "treat it first" would promise an unlock that treating
// alone can't deliver.
static const char* gate_hint(const Pet& pet)   // nullptr = gate open
{
    if (pet.frozen())              return "Care paused (Settings > Game)";
    if (!pet.activitiesUnlocked()) return "Locked until In-Training II";
    if (pet.conditionBlocked())    return "Too unwell - treat or rest first";
    return nullptr;
}
static bool gate_closed(const Pet& pet) { return gate_hint(pet) != nullptr; }

void SceneMenu::render()
{
    fb.fillScreen(col::panel);
    gfx_text(30, 20, 2, col::accent, "Menu");

    bool closed = gate_closed(app().pet);
    for (int i = 0; i < OPT_N; i++) {
        int oy = OPT_Y + i * (OPT_H + OPT_G);
        bool locked = OPTS[i].gated && closed;
        Rect{ OPT_X, oy, OPT_W, OPT_H }.button(OPTS[i].label,
            locked ? rgb565(56, 60, 74) : col::accent, locked ? col::dim : col::black);
    }

    if (closed) {   // explain why Battle/Activities are greyed out
        const char* hint = gate_hint(app().pet);
        bool frozen = app().pet.frozen();
        int hw = (int)strlen(hint) * 6;
        gfx_text((GAME_W - hw) / 2, OPT_Y + OPT_N * (OPT_H + OPT_G) + 2, 1,
                 frozen ? kFrozenCol
                        : app().pet.conditionBlocked() ? col::warn : col::dim,
                 "%s", hint);
    }

    draw_back();
}

void SceneMenu::onInput(const Input& in)
{
    if (!in.pressed) return;

    if (kBack.contains(in)) {
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }
    bool closed = gate_closed(app().pet);
    for (int i = 0; i < OPT_N; i++) {
        int oy = OPT_Y + i * (OPT_H + OPT_G);
        if (Rect{ OPT_X, oy, OPT_W, OPT_H }.contains(in)) {
            if (OPTS[i].gated && closed) return;   // pre-In-Training II, or care frozen
            app().setScene(OPTS[i].scene, Slide::Forward);
            return;
        }
    }
}
