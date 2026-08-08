#include "scene_settings.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"        // clampf
#include "engine/fw_update.hpp"   // fw_current_version (footer)
#include "engine/audio/audio.hpp"
#include "engine/audio/sfx.hpp"
#include "ui/tabs.hpp"
#include "ui/widgets.hpp"
#include <cstring>
#include <cstdio>

static const int SPEEDS[] = { 1, 2, 5, 10, 20, 50, 100 };
static const int SPEED_N  = (int)(sizeof(SPEEDS) / sizeof(SPEEDS[0]));

// tab bar
static const char* const TABS[] = { "GAME", "SOUND", "SYSTEM" };
static const int TAB_N = 3;
static const int TAB_X = 8, TAB_Y = 44, TAB_W = GAME_W - 16, TAB_H = 32;

// SOUND page: three volume sliders and a mute row.
static const int SLD_X = 16, SLD_W = GAME_W - 32, SLD_H = 30;
static const int VOL_MASTER_Y = 104, VOL_MUSIC_Y = 166, VOL_SFX_Y = 228;
static const int MUTE_Y = 274;
static const int SLIDER_N = 3;

static Rect vol_slider(int i)
{
    static const int Y[SLIDER_N] = { VOL_MASTER_Y, VOL_MUSIC_Y, VOL_SFX_Y };
    return { SLD_X, Y[i], SLD_W, SLD_H };
}

static float slider_value(int i)
{
    switch (i) {
        case 1:  return audio::bus_gain(audio::Bus::Music);
        case 2:  return audio::bus_gain(audio::Bus::Sfx);
        default: return audio::master();
    }
}

static void slider_set(int i, float v)
{
    v = clampf(v, 0.0f, 1.0f);
    switch (i) {
        case 1: audio::set_bus_gain(audio::Bus::Music, v); break;
        // UI sounds ride the effects slider rather than getting a fourth control: they are
        // the same category of "noise the game makes at me", and a player who turns effects
        // down has already said what they want. The engine resolves Bus::Ui to the effects
        // gain itself, so there is nothing to set twice here.
        case 2: audio::set_bus_gain(audio::Bus::Sfx, v); break;
        default: audio::set_master(v); break;
    }
}

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
    dragSlider_ = -1;
}

// Hold-to-erase progress, and volume-slider dragging. Both need the LIVE touch position
// rather than a press event, which is why they live here rather than in onInput().
void SceneSettings::update(float dt)
{
    // A slider follows the finger anywhere horizontally once grabbed, and commits to NVS
    // exactly once when it is let go -- dragging across the track would otherwise write
    // flash on every frame of the gesture.
    if (dragSlider_ >= 0) {
        if (down_) {
            const Rect r = vol_slider(dragSlider_);
            slider_set(dragSlider_, (float)(tx_ - r.x) / (float)r.w);
        } else {
            audio::settings_store(app().save);
            // Confirmation at the new level, so setting a volume tells you what it sounds
            // like. Skipped for the music slider: music is already playing at it.
            if (dragSlider_ != 1) sfx::play(sfx::kSelect);
            dragSlider_ = -1;
        }
    }

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

// A volume track with the level filled in behind a centred percentage. Drawn dimmed while
// muted, because a slider that still reads 80% next to a silent speaker is a bug report.
static void draw_slider(const Rect& r, const char* label, float v, uint16_t fg, bool dimmed)
{
    gfx_text(r.x, r.y - 15, 1, col::dim, "%s", label);

    r.fill(rgb565(38, 42, 58), 8);
    int w = (int)(r.w * v + 0.5f);
    if (w > 0) {
        if (w < 10) w = 10;                       // a sliver still reads as a rounded end
        fb.fillRoundRect(r.x, r.y, w, r.h, 8, dimmed ? rgb565(70, 74, 92) : fg);
    }
    r.outline(dimmed ? rgb565(70, 74, 92) : col::dim, 8);

    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", (int)(v * 100.0f + 0.5f));
    const int tw = (int)strlen(pct) * 12;         // size-2 cell is 12 px wide
    gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 16) / 2, 2,
             dimmed ? col::dim : col::white, "%s", pct);
}

static void render_sound(App& app)
{
    const bool m = audio::muted();

    draw_slider(vol_slider(0), "Master volume", slider_value(0), col::accent, m);
    draw_slider(vol_slider(1), "Music",         slider_value(1), rgb565(120, 170, 240), m);
    draw_slider(vol_slider(2), "Effects",       slider_value(2), col::good, m);

    sys_row(MUTE_Y).fill(m ? col::warn : rgb565(60, 64, 84));
    gfx_text(ROW_X + 12, MUTE_Y + 10, 2, m ? col::black : col::white, "Mute");
    gfx_text(ROW_X + ROW_W - 38, MUTE_Y + 11, 2, m ? col::black : col::dim, m ? "ON" : "OFF");

    // The debug overlay already reports frame time; underruns are the audio equivalent and
    // the one number that distinguishes "the card can't keep up" from "that file is broken".
    if (app.debugOverlay) {
        const audio::Stats s = audio::stats();
        gfx_text(16, 84, 1, col::dim, "%u voices  %u str  %u under  %uus",
                 s.activeVoices, s.activeStreams, s.underruns, s.mixPeakUs);
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

    if (page_ == 0)      render_game(app());
    else if (page_ == 1) render_sound(app());
    else                 render_system(app());
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
    if (tab >= 0) { page_ = tab; dragSlider_ = -1; return; }

    if (page_ == 1) {
        for (int i = 0; i < SLIDER_N; i++) {
            const Rect r = vol_slider(i);
            if (!r.contains(in)) continue;
            dragSlider_ = (int8_t)i;              // update() takes it from here
            slider_set(i, (float)(in.x - r.x) / (float)r.w);
            return;
        }
        if (sys_row(MUTE_Y).contains(in)) {
            audio::set_muted(!audio::muted());
            audio::settings_store(app().save);
            // Deliberately after the toggle: unmuting is confirmed by hearing it, and
            // muting is confirmed by NOT hearing it.
            sfx::play(sfx::kSelect);
            return;
        }
        return;
    }

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
