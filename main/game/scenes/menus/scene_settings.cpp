#include "scene_settings.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"        // clampf
#include "engine/audio/audio.hpp"
#include "engine/power.hpp"       // screen timeout + brightness settings
#include "engine/pakfs.hpp"       // mod-pack count, for the SD Card page
#include "engine/audio/sfx.hpp"
#include "ui/tabs.hpp"
#include "ui/widgets.hpp"
#include <cstring>
#include <cstdio>
#include "esp_system.h"           // esp_restart, after a format
#include "esp_timer.h"            // the post-format restart countdown
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "SD_MMC.h"               // SD_GetCard / SD_MountFailed / SD_Format
}

// tab bar. Four tabs across 224px leave ~52px each, which a size-2 "SYSTEM" (72px) would
// overrun, so the whole bar draws its labels at size 1.
static const char* const TABS[] = { "GAME", "SOUND", "SCREEN", "SYSTEM" };
static const int TAB_N = 4;
static const int TAB_SIZE = 1;
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

// GAME page: the care-freeze row. (Game speed lives on the Cheats screen now, with the
// other testing aids -- it accelerates the whole simulation, which was never a player
// option so much as a way to watch weeks happen.)
static const int FRZ_LBL_Y = 96, FRZ_Y = 110;

// Full-width rows, shared by both pages
static const int ROW_X = 16, ROW_W = GAME_W - 32, ROW_H = 34;
// The SYSTEM page carries seven rows, which only fit at a shorter row height and a tighter
// pitch. It also gave up its "FW vX" footer: the About screen it now links to opens with the
// same version string, alongside everything else worth quoting in a bug report.
static const int SYS_H  = 28;
static const int DBG_Y  =  92, TIME_Y  = 124, CHEAT_Y = 156, UPD_Y = 188,
                 SD_Y   = 220, ABOUT_Y = 252, RESET_Y = 284;   // last row ends at 312
// Label baseline and right-hand marker inside a SYSTEM row, derived from SYS_H rather than
// written out per row: seven copies of a hand-tuned offset is what drifts the next time the
// pitch changes.
static const int SYS_TY = (SYS_H - 16) / 2;        // size-2 text is 16px tall

// SCREEN page: a 4-way picker for the screen timeout and a brightness slider. Both are
// DEVICE settings rather than game ones -- they belong to the board, not to the creature.
static const int SCR_TO_LBL_Y  = 96;
static const int SCR_TO_Y      = 110;   // picker row, ROW_H tall
static const int SCR_TO_HINT_Y = 152;
static const int SCR_BRI_Y     = 210;   // slider top; draw_slider puts its label 15px above
static const int SCR_BRI_HINT_Y= 252;

// One segment of the timeout picker. Same x/width as the full-width rows, cut into
// SCREEN_OFF_N equal pills with the same 4px gap the tab bar uses.
static Rect timeout_seg(int i)
{
    const int seg = ROW_W / SCREEN_OFF_N;
    return { ROW_X + i * seg, SCR_TO_Y, seg - 4, ROW_H };
}
static const Rect kBriSld{ SLD_X, SCR_BRI_Y, SLD_W, SLD_H };

// Finger x -> brightness percent. Maps the WHOLE track to 0..100 and lets the setter clamp,
// rather than mapping it to MIN..100: the fill then shows the true percentage, and the dead
// strip at the left end is an honest picture of the floor instead of a hidden offset.
static uint8_t bright_from_x(int px)
{
    float f = (float)(px - kBriSld.x) / (float)kBriSld.w;
    f = clampf(f, 0.0f, 1.0f);
    return (uint8_t)(f * 100.0f + 0.5f);
}

// Factory-reset confirm page: the erase button must be HELD for HOLD_S seconds (a tap,
// however unlucky, can't wipe a save). Progress resets the moment the finger leaves.
static const float HOLD_S = 3.0f;
static const Rect  kHold  { 30, 196, GAME_W - 60, 52 };
static const Rect  kCancel{ 30, 266, GAME_W - 60, 40 };

// SD Card sub-page: card status, and the one place in the firmware that formats the card.
// Hold-to-confirm for the same reason the factory reset has one. It shares HOLD_S and the
// scene's holdT_ with that page, which is safe because only one sub-page is ever open --
// both are entered from a SYSTEM row and both clear holdT_ on the way in and out.
static const Rect kFmtHold  { 30, 214, GAME_W - 60, 52 };
static const Rect kFmtCancel{ 30, 276, GAME_W - 60, 36 };

// Only offer the format where it could actually do something: a mounted card (wipe it and
// start again) or a card that answered on the bus but would not mount -- which is precisely
// the case SD_Init used to silently handle by reformatting. With no card there is nothing to
// write to, and SD_Format() would only come back with an error.
static bool sd_can_format() { return SD_GetCard() != nullptr || SD_MountFailed(); }

// Mod packs that came off the CARD. pakfs also carries the base game's own base.pak, which
// is mounted from the flash data partition and has nothing to do with this screen.
static int sd_pack_count()
{
    int n = 0;
    for (int i = 0; i < pakfs_count(); i++) {
        const char* s = pakfs_source(i);
        if (s && strncmp(s, "/sdcard", 7) == 0) n++;
    }
    return n;
}

// Formatting is destructive AND, on a mounted card, force-unmounts the volume: every open
// handle on it dies, and each mounted mod pack holds one. There is no way back to a running
// game from here even when it succeeds, because everything the card contributes (creatures,
// foods, mods, update images) is boot-time state that is now wrong. So this never returns --
// it says what happened and restarts, exactly like a card pulled out mid-session.
[[noreturn]] static void format_sd_and_restart(App& app)
{
    fb.fillScreen(col::panel);
    gfx_text(16, 120, 2, col::warn, "Formatting...");
    gfx_text_wrap(16, 152, GAME_W - 32, 1, col::white,
                  "Don't remove the card or switch the device off. On a large card this "
                  "can take a while.", 2, -1, 3);
    gfx_present();

    // The mixer streams from the card and holds file handles open on it, and the format pulls
    // the filesystem out from under both. Same first move, for the same reason, as
    // halt_for_card_change() in app.cpp.
    audio::shutdown();

    const esp_err_t e = SD_Format();
    const bool ok = (e == ESP_OK);

    const Rect    kRestart{ 30, 210, GAME_W - 60, 52 };
    const float   AUTO_S = 10.0f;
    const int64_t t0 = esp_timer_get_time();
    Input in{};
    while (true) {
        const float left = AUTO_S - (float)(esp_timer_get_time() - t0) / 1e6f;
        if (left <= 0.0f) break;

        fb.fillScreen(col::panel);
        gfx_text(16, 56, 2, ok ? col::good : col::warn, ok ? "Card formatted" : "Format failed");
        gfx_text_wrap(16, 96, GAME_W - 32, 1, col::white, ok
            ? "The card is empty and ready to use. Copy your mods back onto it from a "
              "computer.\n\nThe device restarts now to pick it up."
            : "The card has been left unusable, and the device has to restart either way. "
              "If it keeps failing, the card itself is probably gone.", 2, -1, 6);
        if (!ok) gfx_text(16, 186, 1, col::dim, "error: %s", esp_err_to_name(e));
        kRestart.button("RESTART NOW", col::accent, col::black);
        gfx_text(16, 280, 1, col::dim, "Restarting automatically in %ds", (int)left + 1);
        gfx_present();

        app.input.poll(in);
        if (in.pressed && kRestart.contains(in)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
}

// Full-width rows share x/width/height; only the y differs.
static Rect wide_row(int y) { return { ROW_X, y, ROW_W, ROW_H }; }   // GAME / SOUND
static Rect sys_row(int y)  { return { ROW_X, y, ROW_W, SYS_H  }; }   // SYSTEM (six of them)

void SceneSettings::onEnter()
{
    page_ = 0;                 // always open on the Game tab
    confirmReset_ = false;
    sdPage_ = false;
    holdT_ = 0.0f;
    dragSlider_ = -1;
    dragBright_ = false;
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

    // Brightness follows the finger and lands in NVS once, on release -- same reasoning as
    // the volume sliders above. It re-applies through the pet rather than calling the driver
    // directly, so a creature whose lights are off stays dimmed while the setting moves.
    if (dragBright_) {
        if (down_) {
            set_screen_brightness(bright_from_x(tx_));
            app().pet.refreshBacklight();
        } else {
            screen_settings_store(app().save);
            sfx::play(sfx::kSelect);
            dragBright_ = false;
        }
    }

    // Both sub-pages commit on a hold, and only one of them is ever open, so they share
    // holdT_ rather than carrying a timer each.
    if (sdPage_) {
        if (down_ && sd_can_format() && kFmtHold.contains(tx_, ty_)) {
            holdT_ += dt;
            if (holdT_ >= HOLD_S) format_sd_and_restart(app());   // no return
        } else {
            holdT_ = 0.0f;
        }
        return;
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
    bool frozen = app.pet.frozen();

    // --- care freeze (Pet::setFrozen) ---
    gfx_text(16, FRZ_LBL_Y, 1, col::dim, "Care");
    wide_row(FRZ_Y).fill(frozen ? kFrozenCol : rgb565(60, 64, 84));
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

    wide_row(MUTE_Y).fill(m ? col::warn : rgb565(60, 64, 84));
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

// SCREEN page. The timeout is a row of discrete buttons rather than a slider: there are only
// four values, and a slider would invite dragging for a precision the setting doesn't have.
static void render_screen()
{
    gfx_text(16, SCR_TO_LBL_Y, 1, col::dim, "Screen timeout");

    const uint16_t cur = screen_off_s();
    for (int i = 0; i < SCREEN_OFF_N; i++) {
        const uint16_t s  = (uint16_t)(SCREEN_OFF_MIN_S + i * SCREEN_OFF_STEP_S);
        const bool     on = (s == cur);
        char lbl[8];
        snprintf(lbl, sizeof lbl, "%us", (unsigned)s);
        timeout_seg(i).button(lbl, on ? col::accent : rgb565(60, 64, 84),
                              on ? col::black : col::white);
    }
    gfx_text_wrap(16, SCR_TO_HINT_Y, GAME_W - 32, 1, col::dim,
                  "Screen turns off after this long with nothing touched. A tap or the "
                  "PWR button wakes it.", 2, -1, 3);

    // Live value, not a cached one: dragging this slider changes the panel on the spot.
    draw_slider(kBriSld, "Brightness", screen_brightness() / 100.0f,
                rgb565(240, 200, 90), false);
    gfx_text_wrap(16, SCR_BRI_HINT_Y, GAME_W - 32, 1, col::dim,
                  "Never below 20%, so it can't go dark enough to hide this slider. "
                  "Dims further while your pet's lights are off.", 2, -1, 4);
}

// A SYSTEM row that opens another screen: label left, ">" right. Six of the seven rows are
// this, and writing the offsets once is what stops them drifting apart the next time the
// page has to be re-pitched to fit another entry.
static void sys_link(int y, uint16_t bg, uint16_t fg, const char* label)
{
    sys_row(y).fill(bg);
    gfx_text(ROW_X + 12, y + SYS_TY, 2, fg, "%s", label);
    gfx_text(ROW_X + ROW_W - 18, y + SYS_TY, 2, fg, ">");
}

static void render_system(App& app)
{
    // The odd one out: a toggle rather than a link, so it draws its own state on the right.
    bool dbg = app.debugOverlay;
    sys_row(DBG_Y).fill(dbg ? col::good : rgb565(60, 64, 84));
    gfx_text(ROW_X + 12, DBG_Y + SYS_TY, 2, dbg ? col::black : col::white, "Debug info");
    gfx_text(ROW_X + ROW_W - 38, DBG_Y + SYS_TY, 2, dbg ? col::black : col::dim, dbg ? "ON" : "OFF");

    sys_link(TIME_Y,  col::accent,          col::black, "Set Time/Date");
    sys_link(CHEAT_Y, rgb565(120, 90, 150), col::white, "Cheats / Debug");
    sys_link(UPD_Y,   rgb565(70, 130, 200), col::white, "System Update");
    sys_link(SD_Y,    rgb565(90, 110, 145), col::white, "SD Card");
    sys_link(ABOUT_Y, rgb565(70, 120, 130), col::white, "About");
    sys_link(RESET_Y, col::warn,            col::white, "Factory Reset");
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

// SD Card page. Says which of three states the card is in, because they need three different
// things from the player, and only one of them is something this screen can fix.
static void render_sd(float holdT)
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "SD Card");
    draw_back();

    const bool mounted = (SD_GetCard() != nullptr);

    if (mounted) {
        const int packs = sd_pack_count();
        gfx_text(16, 56, 2, col::good, "Card ready");
        gfx_text(16, 84, 1, col::dim, "%u MB", (unsigned)SDCard_Size);
        gfx_text(16, 100, 1, col::dim, packs == 1 ? "%d mod pack loaded from it"
                                                  : "%d mod packs loaded from it", packs);
    } else if (SD_MountFailed()) {
        gfx_text(16, 56, 2, col::warn, "Card unreadable");
        gfx_text_wrap(16, 84, GAME_W - 32, 1, col::white,
                      "A card is inserted, but it carries no filesystem this device can "
                      "read -- either it was never formatted, or the one on it is damaged.",
                      2, -1, 4);
    } else {
        gfx_text(16, 56, 2, col::dim, "No card");
        gfx_text_wrap(16, 84, GAME_W - 32, 1, col::white,
                      "Nothing is inserted, or the card is not answering. Mod packs, extra "
                      "creatures and system updates all come from the card.", 2, -1, 4);
    }

    gfx_text_wrap(16, 150, GAME_W - 32, 1, col::dim,
                  "Formatting erases everything on the card: your mod packs, any loose "
                  "creature or food files, and any update image. Your pet lives in the "
                  "device, not on the card, so it is not touched.", 2, -1, 4);

    if (sd_can_format()) {
        // Hold button: the fill grows with hold progress inside the button frame, the same
        // control the factory reset uses, because it is the same size of decision.
        kFmtHold.fill(rgb565(90, 24, 24));
        float p = holdT / HOLD_S;
        if (p > 1.0f) p = 1.0f;
        if (p > 0.0f)
            fb.fillRoundRect(kFmtHold.x, kFmtHold.y, (int)(kFmtHold.w * p), kFmtHold.h, 8, col::warn);
        fb.drawRoundRect(kFmtHold.x, kFmtHold.y, kFmtHold.w, kFmtHold.h, 8, col::white);
        gfx_text(kFmtHold.x + 6, kFmtHold.y + 12, 2, col::white, "HOLD TO FORMAT");
        gfx_text(kFmtHold.x + 6, kFmtHold.y + 34, 1, rgb565(255, 190, 180),
                 "keep pressing for %d seconds", (int)HOLD_S);
    } else {
        // Greyed out rather than hidden. A player who came here to fix a card should be able
        // to see that the tool exists and that it needs a card in the slot, instead of
        // finding a page that appears to offer nothing.
        kFmtHold.button("HOLD TO FORMAT", rgb565(46, 48, 60), rgb565(92, 96, 112));
        // Above the button, not below it: below is where the Back button lives.
        gfx_text(16, kFmtHold.y - 20, 1, col::dim, "Insert a card to enable this.");
    }

    kFmtCancel.button("Back", col::accent, col::black);
}

void SceneSettings::render()
{
    if (confirmReset_) { render_confirm_reset(holdT_); return; }
    if (sdPage_)       { render_sd(holdT_);            return; }

    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "Settings");

    draw_back();

    tabbar_draw(TAB_X, TAB_Y, TAB_W, TAB_H, TABS, TAB_N, page_, TAB_SIZE);

    if      (page_ == 0) render_game(app());
    else if (page_ == 1) render_sound(app());
    else if (page_ == 2) render_screen();
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
            sfx::play(sfx::kBack);              // an in-scene page, so setScene can't voice it
        }
        return;
    }

    if (sdPage_) {                              // same shape as the confirm page above
        if (kBack.contains(in) || kFmtCancel.contains(in)) {
            sdPage_ = false;
            holdT_ = 0.0f;
            sfx::play(sfx::kBack);
        }
        return;
    }

    if (kBack.contains(in)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    int tab = tabbar_hit(in.x, in.y, TAB_X, TAB_Y, TAB_W, TAB_H, TAB_N);
    if (tab >= 0) {
        if (tab != page_) sfx::play(sfx::kTap);   // re-tapping the open tab changes nothing
        page_ = tab; dragSlider_ = -1; dragBright_ = false;
        return;
    }

    if (page_ == 1) {
        for (int i = 0; i < SLIDER_N; i++) {
            const Rect r = vol_slider(i);
            if (!r.contains(in)) continue;
            dragSlider_ = (int8_t)i;              // update() takes it from here
            slider_set(i, (float)(in.x - r.x) / (float)r.w);
            return;
        }
        if (wide_row(MUTE_Y).contains(in)) {
            audio::set_muted(!audio::muted());
            audio::settings_store(app().save);
            // Deliberately after the toggle: unmuting is confirmed by hearing it, and
            // muting is confirmed by NOT hearing it.
            sfx::play(sfx::kSelect);
            return;
        }
        return;
    }

    if (page_ == 2) {
        for (int i = 0; i < SCREEN_OFF_N; i++) {
            if (!timeout_seg(i).contains(in)) continue;
            const uint16_t s = (uint16_t)(SCREEN_OFF_MIN_S + i * SCREEN_OFF_STEP_S);
            if (s == screen_off_s()) { sfx::play(sfx::kTap); return; }   // already the choice
            set_screen_off_s(s);
            screen_settings_store(app().save);   // one key, one tap: no drag to batch
            sfx::play(sfx::kSelect);
            return;
        }
        if (kBriSld.contains(in)) {
            dragBright_ = true;                  // update() takes it from here
            set_screen_brightness(bright_from_x(in.x));
            app().pet.refreshBacklight();
            return;
        }
        return;
    }

    if (page_ == 0) {
        // A plain toggle, no hold-to-confirm: freezing costs nothing and thawing undoes it
        // exactly, so the factory-reset ceremony would only make the mode annoying to use.
        if (wide_row(FRZ_Y).contains(in)) {
            app().pet.setFrozen(!app().pet.frozen());
            sfx::play(sfx::kSelect);           // a commitment, not a browse: the firmer click
            return;
        }
    } else {
        if (sys_row(DBG_Y).contains(in)) {
            app().debugOverlay = !app().debugOverlay;
            app().save.storeU8("dbg", app().debugOverlay ? 1 : 0);
            sfx::play(sfx::kTap);
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
        if (sys_row(SD_Y).contains(in)) {
            sdPage_ = true;
            holdT_ = 0.0f;
            sfx::play(sfx::kTap);              // in-scene page swap; setScene isn't involved
            return;
        }
        if (sys_row(ABOUT_Y).contains(in)) {
            app().setScene(SceneId::About, Slide::Forward);
            return;
        }
        if (sys_row(RESET_Y).contains(in)) {
            confirmReset_ = true;
            holdT_ = 0.0f;
            sfx::play(sfx::kTap);              // in-scene page swap; setScene isn't involved
            return;
        }
    }
}
