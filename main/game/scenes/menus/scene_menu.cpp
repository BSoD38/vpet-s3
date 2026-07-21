#include "scene_menu.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include <cstring>

// "Other" menu: navigation to the stats sheet, activities (minigames), and settings.
// (Everyday care actions live on the Home screen; the live stat readout is on Stats.)
static const int BACK_X = GAME_W - 72, BACK_Y = 12, BACK_W = 60, BACK_H = 30;
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
        fb.fillRoundRect(OPT_X, oy, OPT_W, OPT_H, 6, locked ? rgb565(56, 60, 74) : col::accent);
        int tw = (int)strlen(OPTS[i]) * 12;   // center the size-2 label (12px/char)
        gfx_text(OPT_X + (OPT_W - tw) / 2, oy + (OPT_H - 16) / 2, 2, locked ? col::dim : col::black, "%s", OPTS[i]);
    }

    if (!unlocked) {   // explain why Battle/Activities are greyed out
        const char* hint = "Locked until In-Training II";
        int hw = (int)strlen(hint) * 6;
        gfx_text((GAME_W - hw) / 2, OPT_Y + OPT_N * (OPT_H + OPT_G) + 2, 1, col::dim, "%s", hint);
    }

    fb.fillRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 6, col::accent);
    gfx_text(BACK_X + 10, BACK_Y + 8, 2, col::black, "Back");
}

void SceneMenu::onInput(const Input& in)
{
    if (!in.pressed) return;

    if (hit(in.x, in.y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }
    bool unlocked = app().pet.activitiesUnlocked();
    for (int i = 0; i < OPT_N; i++) {
        int oy = OPT_Y + i * (OPT_H + OPT_G);
        if (hit(in.x, in.y, OPT_X, oy, OPT_W, OPT_H)) {
            if (opt_locked(i, unlocked)) return;   // Battle/Activities gated until In-Training II
            if      (i == 0) app().setScene(SceneId::Stats,    Slide::Forward);  // character sheet
            else if (i == 1) app().setScene(SceneId::BattleSelect, Slide::Iris);  // battle mode picker
            else if (i == 2) app().setScene(SceneId::Run,      Slide::Iris);     // hurdle runner (iris)
            else             app().setScene(SceneId::Settings, Slide::Forward);  // game speed etc.
            return;
        }
    }
}
