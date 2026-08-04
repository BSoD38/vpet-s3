#include "scene_cheats.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "sim/creatures.hpp"
#include <cstring>
#include <cstdio>    // snprintf (species label)

// ---- layout -----------------------------------------------------------------------------
static const Rect BACK_BTN { GAME_W - 66, 10, 56, 28 };

// restore row A (two halves) + row B (full width)
static const Rect HP_BTN  { 16,  40, 100, 28 };            // Full HP
static const Rect EN_BTN  { 124, 40, 100, 28 };            // Full Stamina
static const Rect ALL_BTN { 16,  72, GAME_W - 32, 26 };    // Restore All

// stat rows: label + value + [-]/[+] steppers. Row height leaves a 4px gap to the next row
// (== 2*TOUCH_SLOP) so slop-expanded steppers never overlap vertically into the wrong row.
static const int STAT_Y0 = 116, STAT_PITCH = 24, STAT_BH = 20;
static const int MINUS_X = 150, PLUS_X = 190, SBW = 34;
static int  stat_row_y(int i) { return STAT_Y0 + i * STAT_PITCH; }
static Rect stat_minus(int i) { return { MINUS_X, stat_row_y(i), SBW, STAT_BH }; }
static Rect stat_plus (int i) { return { PLUS_X,  stat_row_y(i), SBW, STAT_BH }; }

// max/zero + species cycler
static const Rect MAX_BTN  { 16,  236, 100, 24 };
static const Rect ZERO_BTN { 124, 236, 100, 24 };
static const Rect SP_PREV  { 16,         268, 30, 28 };
static const Rect SP_NEXT  { GAME_W - 46, 268, 30, 28 };

struct StatRow { StatId id; const char* label; int step; };
static const StatRow SROWS[] = {
    { STAT_STR,   "STR", 250 },
    { STAT_END,   "END", 250 },
    { STAT_AGI,   "AGI", 250 },
    { STAT_INT,   "INT", 250 },
    { STAT_MAXHP, "HP",  2500 },
};
static const int SROW_N = (int)(sizeof(SROWS) / sizeof(SROWS[0]));

void SceneCheats::render()
{
    Pet& pet = app().pet;
    fb.fillScreen(col::panel);
    gfx_text(16, 12, 2, col::accent, "Cheats");

    BACK_BTN.button("Back", col::accent, col::black, 1);

    // --- restore ---
    HP_BTN.button ("Full HP",      rgb565(70, 120, 90), col::white, 1);
    EN_BTN.button ("Full Stamina", rgb565(70, 120, 90), col::white, 1);
    ALL_BTN.button("RESTORE ALL",  col::good,           col::black, 2);

    // --- stats ---
    gfx_text(16, 102, 1, col::dim, "STATS");
    for (int i = 0; i < SROW_N; i++) {
        int y = stat_row_y(i);
        gfx_text(16, y + 6, 2, col::white, "%s", SROWS[i].label);
        gfx_text(58, y + 7, 1, col::accent, "%lu", (unsigned long)pet.stat(SROWS[i].id));
        stat_minus(i).button("-", rgb565(60, 64, 84), col::white, 2);
        stat_plus(i).button ("+", rgb565(60, 64, 84), col::white, 2);
    }

    MAX_BTN.button ("MAX STATS",  rgb565(120, 90, 150), col::white, 1);
    ZERO_BTN.button("ZERO STATS", rgb565(90, 70, 80),   col::white, 1);

    // --- species cycler ---
    SP_PREV.button("<", col::accent, col::black, 2);
    SP_NEXT.button(">", col::accent, col::black, 2);
    const Creature& c = app().creatures.at(pet.creatureIndex());
    char sp[40];
    snprintf(sp, sizeof sp, "%s (T%u)", c.name, (unsigned)c.tier);
    int tw = (int)strlen(sp) * 6;
    gfx_text((GAME_W - tw) / 2, SP_PREV.y + (SP_PREV.h - 8) / 2, 1, col::white, "%s", sp);
}

void SceneCheats::onInput(const Input& in)
{
    if (!in.pressed) return;
    Pet& pet = app().pet;

    if (BACK_BTN.contains(in)) {
        app().setScene(SceneId::Settings, Slide::Back);
        return;
    }

    // restore
    if (HP_BTN.contains(in))  { pet.cheatSetHealth(100); return; }
    if (EN_BTN.contains(in))  { pet.cheatSetEnergy(100); return; }
    if (ALL_BTN.contains(in)) { pet.cheatRestore();      return; }

    // per-stat -/+
    for (int i = 0; i < SROW_N; i++) {
        if (stat_minus(i).contains(in)) { pet.cheatAdjustStat(SROWS[i].id, -SROWS[i].step); return; }
        if (stat_plus(i).contains(in))  { pet.cheatAdjustStat(SROWS[i].id,  SROWS[i].step); return; }
    }

    // max / zero all stats
    if (MAX_BTN.contains(in)) {
        for (int i = 0; i < SROW_N; i++) pet.cheatMaxStat(SROWS[i].id);
        return;
    }
    if (ZERO_BTN.contains(in)) {
        for (int i = 0; i < SROW_N; i++) pet.cheatAdjustStat(SROWS[i].id, -2000000000);   // clamps to 0
        return;
    }

    // species cycler (wraps)
    int n = app().creatures.count();
    if (n > 0) {
        int cur = pet.creatureIndex();
        if (SP_PREV.contains(in)) { pet.cheatSetSpecies((cur - 1 + n) % n); return; }
        if (SP_NEXT.contains(in)) { pet.cheatSetSpecies((cur + 1) % n);     return; }
    }
}
