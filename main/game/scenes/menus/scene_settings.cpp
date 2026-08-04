#include "scene_settings.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/tabs.hpp"
#include "ui/widgets.hpp"
#include <cstring>
#include <cstdio>

static const int SPEEDS[] = { 1, 2, 5, 10, 20, 50, 100 };
static const int SPEED_N  = (int)(sizeof(SPEEDS) / sizeof(SPEEDS[0]));

// tab bar
static const char* const TABS[] = { "GAME", "SYSTEM" };
static const int TAB_N = 2;
static const int TAB_X = 8, TAB_Y = 44, TAB_W = GAME_W - 16, TAB_H = 32;

// GAME page: game-speed grid
static const int GRID_X0 = 7, GRID_Y = 140, BTN_W = 52, BTN_H = 38, BTN_GX = 6, BTN_GY = 10, COLS = 4;

// SYSTEM page: full-width rows
static const int ROW_X = 16, ROW_W = GAME_W - 32, ROW_H = 34;
static const int DBG_Y = 96, TIME_Y = 138, CHEAT_Y = 180;

static Rect speed_btn(int i)
{
    return { GRID_X0 + (i % COLS) * (BTN_W + BTN_GX),
             GRID_Y  + (i / COLS) * (BTN_H + BTN_GY), BTN_W, BTN_H };
}

// SYSTEM page rows share x/width/height; only the y differs.
static Rect sys_row(int y) { return { ROW_X, y, ROW_W, ROW_H }; }

void SceneSettings::onEnter() { page_ = 0; }   // always open on the Game tab

static void render_game(App& app)
{
    unsigned speed = app.pet.state().gameSpeed;
    gfx_text(16, 88,  1, col::dim,  "Game speed (1x = real time)");
    gfx_text(16, 104, 2, col::good, "Now: %ux", speed);
    for (int i = 0; i < SPEED_N; i++) {
        bool sel = ((unsigned)SPEEDS[i] == speed);
        char lbl[8]; snprintf(lbl, sizeof lbl, "%dx", SPEEDS[i]);
        speed_btn(i).button(lbl, sel ? col::accent : rgb565(60, 64, 84), sel ? col::black : col::white);
    }
}

static void render_system(App& app)
{
    bool dbg = app.debugOverlay;
    sys_row(DBG_Y).fill(dbg ? col::good : rgb565(60, 64, 84));
    gfx_text(ROW_X + 12, DBG_Y + 10, 2, dbg ? col::black : col::white, "Debug info");
    gfx_text(ROW_X + ROW_W - 38, DBG_Y + 11, 2, dbg ? col::black : col::dim, dbg ? "ON" : "OFF");

    sys_row(TIME_Y).fill(col::accent);
    gfx_text(ROW_X + 12, TIME_Y + 10, 2, col::black, "Set Time/Date");
    gfx_text(ROW_X + ROW_W - 18, TIME_Y + 11, 2, col::black, ">");

    sys_row(CHEAT_Y).fill(rgb565(120, 90, 150));
    gfx_text(ROW_X + 12, CHEAT_Y + 10, 2, col::white, "Cheats / Debug");
    gfx_text(ROW_X + ROW_W - 18, CHEAT_Y + 11, 2, col::white, ">");

    gfx_text(16, 236, 1, col::dim, "Species: %s", app.pet.speciesName());
}

void SceneSettings::render()
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "Settings");

    draw_back();

    tabbar_draw(TAB_X, TAB_Y, TAB_W, TAB_H, TABS, TAB_N, page_);

    if (page_ == 0) render_game(app());
    else            render_system(app());
}

void SceneSettings::onInput(const Input& in)
{
    if (!in.pressed) return;

    if (kBack.contains(in)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    int tab = tabbar_hit(in.x, in.y, TAB_X, TAB_Y, TAB_W, TAB_H, TAB_N);
    if (tab >= 0) { page_ = tab; return; }

    if (page_ == 0) {
        for (int i = 0; i < SPEED_N; i++) {
            if (speed_btn(i).contains(in)) {
                app().pet.setGameSpeed((uint16_t)SPEEDS[i]);
                return;
            }
        }
    } else {
        if (sys_row(DBG_Y).contains(in)) {
            app().debugOverlay = !app().debugOverlay;
            app().save.storeU8("dbg", app().debugOverlay ? 1 : 0);
            return;
        }
        if (sys_row(TIME_Y).contains(in)) {
            app().setScene(SceneId::TimeSet, Slide::Forward);
            return;
        }
        if (sys_row(CHEAT_Y).contains(in)) {
            app().setScene(SceneId::Cheats, Slide::Forward);
            return;
        }
    }
}
