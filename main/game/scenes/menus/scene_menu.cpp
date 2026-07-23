#include "scene_menu.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/widgets.hpp"
#include <cstring>

// "Other" menu: navigation to the stats sheet, activities (minigames), and settings.
// (Everyday care actions live on the Home screen; the live stat readout is on Stats.)
static const int OPT_X = 30, OPT_Y = 84, OPT_W = 180, OPT_H = 34, OPT_G = 22;
static const char* OPTS[] = { "Stats", "Battle", "Activities", "Settings" };
static const int OPT_N = 4;

// Battle (i==1) and Activities (i==2) are training/combat; the rest are always available.
static bool opt_locked(int i, bool unlocked) { return !unlocked && (i == 1 || i == 2); }

void SceneMenu::render()
{
    fb.fillScreen(col::panel);
    gfx_text(30, 20, 2, col::accent, "Menu");

    // sections: Stats = character sheet; Activities = minigame; Settings = game speed etc.
    bool unlocked = app().pet.activitiesUnlocked();
    for (int i = 0; i < OPT_N; i++) {
        int oy = OPT_Y + i * (OPT_H + OPT_G);
        bool locked = opt_locked(i, unlocked);
        Rect{ OPT_X, oy, OPT_W, OPT_H }.button(OPTS[i],
            locked ? rgb565(56, 60, 74) : col::accent, locked ? col::dim : col::black);
    }

    if (!unlocked) {   // explain why Battle/Activities are greyed out
        const char* hint = "Locked until In-Training II";
        int hw = (int)strlen(hint) * 6;
        gfx_text((GAME_W - hw) / 2, OPT_Y + OPT_N * (OPT_H + OPT_G) + 2, 1, col::dim, "%s", hint);
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
    bool unlocked = app().pet.activitiesUnlocked();
    for (int i = 0; i < OPT_N; i++) {
        int oy = OPT_Y + i * (OPT_H + OPT_G);
        if (Rect{ OPT_X, oy, OPT_W, OPT_H }.contains(in)) {
            if (opt_locked(i, unlocked)) return;   // Battle/Activities gated until In-Training II
            if      (i == 0) app().setScene(SceneId::Stats,    Slide::Forward);  // character sheet
            else if (i == 1) app().setScene(SceneId::BattleSelect, Slide::Forward);  // battle mode picker
            else if (i == 2) app().setScene(SceneId::Activities, Slide::Forward);    // minigame picker
            else             app().setScene(SceneId::Settings, Slide::Forward);  // game speed etc.
            return;
        }
    }
}
