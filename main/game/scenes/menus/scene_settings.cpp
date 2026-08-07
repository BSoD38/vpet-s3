#include "scene_settings.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/fw_update.hpp"   // fw_current_version (footer)
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

// GAME page: game-speed grid (two rows, ending at y226), then the care-freeze row under it
static const int GRID_X0 = 7, GRID_Y = 140, BTN_W = 52, BTN_H = 38, BTN_GX = 6, BTN_GY = 10, COLS = 4;
static const int FRZ_LBL_Y = 234, FRZ_Y = 248;

// Full-width rows, shared by both pages
static const int ROW_X = 16, ROW_W = GAME_W - 32, ROW_H = 34;
static const int DBG_Y = 96, TIME_Y = 138, CHEAT_Y = 180, UPD_Y = 222, RESET_Y = 264;

// Factory-reset confirm page: the erase button must be HELD for HOLD_S seconds (a tap,
// however unlucky, can't wipe a save). Progress resets the moment the finger leaves.
static const float HOLD_S = 3.0f;
static const Rect  kHold  { 30, 196, GAME_W - 60, 52 };
static const Rect  kCancel{ 30, 266, GAME_W - 60, 40 };

static Rect speed_btn(int i)
{
    return { GRID_X0 + (i % COLS) * (BTN_W + BTN_GX),
             GRID_Y  + (i / COLS) * (BTN_H + BTN_GY), BTN_W, BTN_H };
}

// Full-width rows share x/width/height; only the y differs.
static Rect sys_row(int y) { return { ROW_X, y, ROW_W, ROW_H }; }

void SceneSettings::onEnter()
{
    page_ = 0;                 // always open on the Game tab
    confirmReset_ = false;
    holdT_ = 0.0f;
}

// Hold-to-erase progress. Runs only on the confirm page; leaving the button (or lifting)
// starts over. Reaching the threshold erases NVS and restarts -- factoryReset() never returns.
void SceneSettings::update(float dt)
{
    if (!confirmReset_) return;
    if (down_ && kHold.contains(tx_, ty_)) {
        holdT_ += dt;
        if (holdT_ >= HOLD_S) app().save.factoryReset();
    } else {
        holdT_ = 0.0f;
    }
}

static void render_game(App& app)
{
    unsigned speed  = app.pet.state().gameSpeed;
    bool     frozen = app.pet.frozen();

    gfx_text(16, 88, 1, col::dim, "Game speed (1x = real time)");
    // While frozen the multiplier is moot -- the clock it multiplies isn't running. The
    // buttons stay live, though: picking the speed you want to come back to is a reasonable
    // thing to do while packing.
    if (frozen) gfx_text(16, 104, 2, kFrozenCol, "Now: paused");
    else        gfx_text(16, 104, 2, col::good,  "Now: %ux", speed);
    for (int i = 0; i < SPEED_N; i++) {
        bool sel = ((unsigned)SPEEDS[i] == speed);
        char lbl[8]; snprintf(lbl, sizeof lbl, "%dx", SPEEDS[i]);
        uint16_t bg = sel ? (frozen ? rgb565(70, 92, 112) : col::accent) : rgb565(60, 64, 84);
        speed_btn(i).button(lbl, bg, sel && !frozen ? col::black : col::white);
    }

    // --- care freeze (Pet::setFrozen) ---
    gfx_text(16, FRZ_LBL_Y, 1, col::dim, "Care");
    sys_row(FRZ_Y).fill(frozen ? kFrozenCol : rgb565(60, 64, 84));
    gfx_text(ROW_X + 12, FRZ_Y + 10, 2, frozen ? col::black : col::white, "Freeze care");
    gfx_text(ROW_X + ROW_W - 38, FRZ_Y + 11, 2, frozen ? col::black : col::dim,
             frozen ? "ON" : "OFF");
    // Says what it COSTS as well as what it saves: the player is agreeing to both halves.
    gfx_text_wrap(16, FRZ_Y + ROW_H + 6, GAME_W - 32, 1, frozen ? kFrozenCol : col::dim,
                  frozen ? "Nothing ages or decays. Care is disabled."
                         : "Stops aging and care while you're away.",
                  2, -1, 2);
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

    sys_row(UPD_Y).fill(rgb565(70, 130, 200));
    gfx_text(ROW_X + 12, UPD_Y + 10, 2, col::white, "System Update");
    gfx_text(ROW_X + ROW_W - 18, UPD_Y + 11, 2, col::white, ">");

    sys_row(RESET_Y).fill(col::warn);
    gfx_text(ROW_X + 12, RESET_Y + 10, 2, col::white, "Factory Reset");
    gfx_text(ROW_X + ROW_W - 18, RESET_Y + 11, 2, col::white, ">");

    gfx_text(16, 306, 1, col::dim, "FW v%s", fw_current_version());
}

// Confirmation page for the factory reset. Deliberately its own screen (not a small
// modal): the player should read what they're about to lose, and the erase control is
// a hold-to-confirm with a visible progress fill.
static void render_confirm_reset(float holdT)
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::warn, "Factory Reset");
    draw_back();

    gfx_text_wrap(16, 64, GAME_W - 32, 1, col::white,
                  "This erases EVERYTHING on the device:\n"
                  "your pet, its stats and bond, journal,\n"
                  "conversation memories, tower progress\n"
                  "and settings.\n\n"
                  "The game restarts with a new egg.\n"
                  "This cannot be undone.", 3);

    // Hold button: the fill grows with hold progress inside the button frame.
    kHold.fill(rgb565(90, 24, 24));
    float p = holdT / HOLD_S;
    if (p > 1.0f) p = 1.0f;
    if (p > 0.0f)
        fb.fillRoundRect(kHold.x, kHold.y, (int)(kHold.w * p), kHold.h, 8, col::warn);
    fb.drawRoundRect(kHold.x, kHold.y, kHold.w, kHold.h, 8, col::white);
    gfx_text(kHold.x + 18, kHold.y + 12, 2, col::white, "HOLD TO ERASE");
    gfx_text(kHold.x + 18, kHold.y + 34, 1, rgb565(255, 190, 180), "keep pressing for %d seconds", (int)HOLD_S);

    kCancel.button("Cancel", col::accent, col::black);
}

void SceneSettings::render()
{
    if (confirmReset_) { render_confirm_reset(holdT_); return; }

    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "Settings");

    draw_back();

    tabbar_draw(TAB_X, TAB_Y, TAB_W, TAB_H, TABS, TAB_N, page_);

    if (page_ == 0) render_game(app());
    else            render_system(app());
}

void SceneSettings::onInput(const Input& in)
{
    down_ = in.down; tx_ = in.x; ty_ = in.y;   // live state for the hold-to-erase gesture

    if (!in.pressed) return;

    if (confirmReset_) {                        // confirm page: only Cancel/Back leave it
        if (kBack.contains(in) || kCancel.contains(in)) {
            confirmReset_ = false;
            holdT_ = 0.0f;
        }
        return;
    }

    if (kBack.contains(in)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    int tab = tabbar_hit(in.x, in.y, TAB_X, TAB_Y, TAB_W, TAB_H, TAB_N);
    if (tab >= 0) { page_ = tab; return; }

    if (page_ == 0) {
        // A plain toggle, no hold-to-confirm: freezing costs nothing and thawing undoes it
        // exactly, so the factory-reset ceremony would only make the mode annoying to use.
        if (sys_row(FRZ_Y).contains(in)) {
            app().pet.setFrozen(!app().pet.frozen());
            return;
        }
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
        if (sys_row(UPD_Y).contains(in)) {
            app().setScene(SceneId::Update, Slide::Forward);
            return;
        }
        if (sys_row(RESET_Y).contains(in)) {
            confirmReset_ = true;
            holdT_ = 0.0f;
            return;
        }
    }
}
