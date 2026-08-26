#include "scene_menu.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/audio/sfx.hpp"
#include "ui/widgets.hpp"
#include <cstring>

// layout
static const int PAD_X  = 12;
static const int ROW_H  = 44;
static const int LIST_Y = 52;
static const int HINT_H = 18;                              // fixed strip for the lock hint
static const int VIEW_H = GAME_H - LIST_Y - HINT_H - 6;
static const int BTN_W  = GAME_W - 2 * PAD_X - 10;         // 10px kept clear for the scrollbar

// One row per entry -- label, destination and gating travel together, so inserting an entry
// is one line instead of renumbering magic indices in a lock predicate AND an if/else ladder
// (the Journal insertion had to shift both, which is exactly the mistake this table removes).
struct MenuEntry {
    const char* label;
    SceneId     scene;
    // Locked until Pet::activitiesUnlocked() -- and, for the same reason, while the care
    // freeze is on: these lead somewhere that trains, spends or wounds the creature.
    bool        gated;
};
static const MenuEntry OPTS[] = {
    { "Stats",      SceneId::Stats,        false },  // character sheet
    { "Journal",    SceneId::Journal,      false },  // memories + facts
    { "Battle",     SceneId::BattleSelect, true  },  // battle mode picker
    { "Activities", SceneId::Activities,   true  },  // minigame picker
    { "Shop",       SceneId::Shop,         false },  // shop + bag (the player's screen)
    // Deliberately NOT gated: earning while the creature is ill is the entire point of it
    // (docs/economy-and-inventory.md 4). If this were locked by condition like the others,
    // a sick pet and an empty wallet would be the lockout the floor exists to prevent.
    { "Work",       SceneId::Work,         false },  // odd jobs; pays whatever the pet cannot
    { "Settings",   SceneId::Settings,     false },  // game speed etc.
};
static const int OPT_N = (int)(sizeof(OPTS) / sizeof(OPTS[0]));

// The gated entries can be shut for three unrelated reasons; only one hint fits. The freeze
// wins because it is the one the player chose and can undo.
static const char* gate_hint(const Pet& pet)
{
    if (pet.frozen())              return "Care paused (Settings > Game)";
    if (!pet.activitiesUnlocked()) return "Unlocks as it grows up";
    if (pet.conditionBlocked())    return "Too unwell - treat or rest first";
    return nullptr;
}
static bool gate_closed(const Pet& pet) { return gate_hint(pet) != nullptr; }

void SceneMenu::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, ROW_H);
    list_.reset();
}

void SceneMenu::render()
{
    fb.fillScreen(col::panel);
    gfx_text(PAD_X + 18, 20, 2, col::accent, "Menu");

    const bool closed = gate_closed(app().pet);

    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(OPT_N); i++) {
        Rect row = list_.rowRect(i);
        const bool locked = OPTS[i].gated && closed;
        Rect{ PAD_X, row.y + 4, BTN_W, ROW_H - 10 }.button(
            OPTS[i].label,
            locked ? rgb565(56, 60, 74) : col::accent,
            locked ? col::dim           : col::black);
    }
    list_.endClip();
    list_.drawScrollbar(OPT_N);

    if (closed) {   // explain why Battle/Activities are greyed out
        const char* hint = gate_hint(app().pet);
        const int hw = (int)strlen(hint) * 6;
        gfx_text((GAME_W - hw) / 2, GAME_H - HINT_H + 4, 1,
                 app().pet.frozen()          ? kFrozenCol
                 : app().pet.conditionBlocked() ? col::warn
                                                : col::dim,
                 "%s", hint);
    }

    draw_back();
}

void SceneMenu::onInput(const Input& in)
{
    // Back sits above the viewport, so a scroll gesture can never swallow it.
    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }

    list_.update(in, OPT_N);

    const int row = list_.tapped();
    if (row < 0 || row >= OPT_N) return;

    if (OPTS[row].gated && gate_closed(app().pet)) {   // pre-In-Training II, unwell, or frozen
        sfx::play(sfx::kDenied);
        return;
    }
    app().setScene(OPTS[row].scene, Slide::Forward);
}
