#include "scene_settings.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include <cstring>
#include <cstdio>

static const int SPEEDS[] = { 1, 2, 5, 10, 20, 50, 100 };
static const int SPEED_N  = (int)(sizeof(SPEEDS) / sizeof(SPEEDS[0]));

static const int BACK_X = GAME_W - 72, BACK_Y = 12, BACK_W = 60, BACK_H = 30;
static const int GRID_X0 = 7, GRID_Y = 90, BTN_W = 52, BTN_H = 36, BTN_GX = 6, BTN_GY = 8, COLS = 4;

static const int ROW_X = 16, ROW_W = GAME_W - 32, ROW_H = 32;
static const int DBG_Y = 182;    // debug-overlay toggle row
static const int TIME_Y = 222;   // set time/date row

static void btn_rect(int i, int& x, int& y)
{
    x = GRID_X0 + (i % COLS) * (BTN_W + BTN_GX);
    y = GRID_Y  + (i / COLS) * (BTN_H + BTN_GY);
}

void SceneSettings::render()
{
    unsigned speed = app().pet.state().gameSpeed;

    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "Settings");

    gfx_text(16, 46, 1, col::dim,  "Game speed (1x = real time)");
    gfx_text(16, 60, 2, col::good, "Now: %ux", speed);

    for (int i = 0; i < SPEED_N; i++) {
        int bx, by; btn_rect(i, bx, by);
        bool sel = ((unsigned)SPEEDS[i] == speed);
        fb.fillRoundRect(bx, by, BTN_W, BTN_H, 6, sel ? col::accent : rgb565(60, 64, 84));
        char lbl[8]; snprintf(lbl, sizeof lbl, "%dx", SPEEDS[i]);
        int lw = (int)strlen(lbl) * 12;
        gfx_text(bx + (BTN_W - lw) / 2, by + (BTN_H - 14) / 2, 2,
                 sel ? col::black : col::white, "%s", lbl);
    }

    // debug overlay toggle
    bool dbg = app().debugOverlay;
    fb.fillRoundRect(ROW_X, DBG_Y, ROW_W, ROW_H, 6, dbg ? col::good : rgb565(60, 64, 84));
    gfx_text(ROW_X + 12, DBG_Y + 9, 2, dbg ? col::black : col::white, "Debug info");
    gfx_text(ROW_X + ROW_W - 38, DBG_Y + 11, 2, dbg ? col::black : col::dim, dbg ? "ON" : "OFF");

    // set time / date
    fb.fillRoundRect(ROW_X, TIME_Y, ROW_W, ROW_H, 6, col::accent);
    gfx_text(ROW_X + 12, TIME_Y + 9, 2, col::black, "Set Time/Date");
    gfx_text(ROW_X + ROW_W - 18, TIME_Y + 11, 2, col::black, ">");

    gfx_text(16, 268, 1, col::dim, "Species: %s", app().pet.speciesName());

    fb.fillRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 6, col::accent);
    gfx_text(BACK_X + 10, BACK_Y + 8, 2, col::black, "Back");
}

void SceneSettings::onInput(const Input& in)
{
    if (!in.pressed) return;
    if (hit(in.x, in.y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }
    for (int i = 0; i < SPEED_N; i++) {
        int bx, by; btn_rect(i, bx, by);
        if (hit(in.x, in.y, bx, by, BTN_W, BTN_H)) {
            app().pet.setGameSpeed((uint16_t)SPEEDS[i]);
            return;
        }
    }
    if (hit(in.x, in.y, ROW_X, DBG_Y, ROW_W, ROW_H)) {
        app().debugOverlay = !app().debugOverlay;
        app().save.storeU8("dbg", app().debugOverlay ? 1 : 0);
        return;
    }
    if (hit(in.x, in.y, ROW_X, TIME_Y, ROW_W, ROW_H)) {
        app().setScene(SceneId::TimeSet, Slide::Forward);
        return;
    }
}
