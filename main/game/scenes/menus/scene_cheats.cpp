#include "scene_cheats.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "sim/creatures.hpp"
#include <cstring>
#include <strings.h> // strcasecmp (picker sort)
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

// max/zero, the species button (tapping it opens the scrolling picker), and force-evolve.
// The bottom three rows keep a 4px gap == 2*TOUCH_SLOP, for the same reason the stat rows do:
// slop-expanded hit boxes must not overlap into the row above or a tap lands on the wrong one.
static const Rect MAX_BTN  { 16,  236, 100, 22 };
static const Rect ZERO_BTN { 124, 236, 100, 22 };
static const Rect SP_BTN   { 16,  262, GAME_W - 32, 26 };
static const Rect EVO_BTN  { 16,  292, GAME_W - 32, 24 };

// How long a force-evolve result stays on the button.
static const float EVO_MSG_SECS = 2.5f;

// picker: single-line rows, so a 200-slot modded roster is a couple of flicks tall
static const int PICK_Y     = 52;
static const int PICK_ROW_H = 36;

struct StatRow { StatId id; const char* label; int step; };
static const StatRow SROWS[] = {
    { STAT_STR,   "STR", 250 },
    { STAT_END,   "END", 250 },
    { STAT_AGI,   "AGI", 250 },
    { STAT_INT,   "INT", 250 },
    { STAT_MAXHP, "HP",  2500 },
};
static const int SROW_N = (int)(sizeof(SROWS) / sizeof(SROWS[0]));

// Picker order: evolution stage first, then name (case-insensitive; ids break a name tie
// so two same-named modded creatures still sort deterministically).
static bool species_before(const CreatureRegistry& reg, int a, int b)
{
    const Creature& ca = reg.at(a);
    const Creature& cb = reg.at(b);
    if (ca.tier != cb.tier) return ca.tier < cb.tier;
    int c = strcasecmp(ca.name, cb.name);
    if (c != 0) return c < 0;
    return strcmp(ca.id, cb.id) < 0;
}

void SceneCheats::onEnter()
{
    picking_ = false;   // never re-enter the scene with the picker still open
    evoMsgT_ = 0.0f;    // a stale result from the last visit would be answering nothing
}

void SceneCheats::update(float dt)
{
    if (evoMsgT_ > 0.0f) evoMsgT_ -= dt;
}

void SceneCheats::render()
{
    if (picking_) { renderPicker(); return; }

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

    // --- species (opens the picker) ---
    const Creature& c = app().creatures.at(pet.creatureIndex());
    char sp[48];
    snprintf(sp, sizeof sp, "%s (T%u)", c.name, (unsigned)c.tier);
    SP_BTN.button(sp, col::card, col::white, 1);
    SP_BTN.outline(col::accent);

    // --- force evolve ---
    // Distinct colour from the species button on purpose: they sit next to each other and do
    // very different things (earned branch vs arbitrary morph), so they should not read as a
    // pair of ways to do the same thing.
    if (evoMsgT_ > 0.0f) EVO_BTN.button(evoMsg_, evoMsgCol_, col::black, 1);
    else                 EVO_BTN.button("FORCE EVOLVE", rgb565(150, 110, 60), col::white, 1);
}

void SceneCheats::renderPicker()
{
    const CreatureRegistry& reg = app().creatures;
    const int n   = reg.count();
    const int cur = app().pet.creatureIndex();

    fb.fillScreen(col::panel);
    gfx_text(16, 18, 2, col::accent, "Species");
    draw_back();

    // Text-only rows on purpose: decoding a sprite per visible row would churn the
    // 16-entry LRU sprite cache on every flick and stutter the scroll.
    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(n); i++) {
        const Creature& c = reg.at(order_[i]);
        const bool isCur  = order_[i] == cur;
        Rect row  = list_.rowRect(i);
        Rect card { 12, row.y + 2, GAME_W - 24, PICK_ROW_H - 4 };
        card.fill(col::card);
        card.outline(isCur ? col::accent : col::dim);

        // Right-aligned tier + attribute tag; the name gets whatever width is left.
        char tag[16];
        snprintf(tag, sizeof tag, "T%u %s", (unsigned)c.tier, attr_short(c.attribute));
        int tagX = card.x + card.w - (int)strlen(tag) * 6 - 8;
        gfx_text(tagX, row.y + (PICK_ROW_H - 8) / 2, 1, attr_color(c.attribute), "%s", tag);
        gfx_text_fit(card.x + 8, row.y + (PICK_ROW_H - 16) / 2, tagX - card.x - 16, 2,
                     isCur ? col::accent : col::white, "%s", c.name);
    }
    list_.endClip();
    list_.drawScrollbar(n);
}

void SceneCheats::inputPicker(const Input& in)
{
    // Back sits above the viewport, so a scroll gesture can never swallow it.
    if (in.pressed && kBack.contains(in)) { picking_ = false; return; }

    const int n = app().creatures.count();
    list_.update(in, n);

    int row = list_.tapped();
    if (row >= 0 && row < n) {
        app().pet.cheatSetSpecies(order_[row]);
        picking_ = false;           // morph and return, one gesture
    }
}

void SceneCheats::onInput(const Input& in)
{
    // The picker needs the full press/drag/release stream (scroll gestures), so it
    // branches off before the pressed-only gate below.
    if (picking_) { inputPicker(in); return; }

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

    // force the earned evolution (skips only the stage timer -- see Pet::cheatForceEvolve)
    if (EVO_BTN.contains(in)) {
        char name[24] = {0};
        switch (pet.cheatForceEvolve(name, sizeof name)) {
            case Pet::ForceEvo::Evolved:
                snprintf(evoMsg_, sizeof evoMsg_, "-> %s", name);
                evoMsgCol_ = col::good;
                break;
            case Pet::ForceEvo::Terminal:
                snprintf(evoMsg_, sizeof evoMsg_, "FINAL FORM");
                evoMsgCol_ = col::dim;
                break;
            case Pet::ForceEvo::NotEligible:
                // Not a failure: the pet has edges but has not met a gate, so there is nothing
                // it has earned yet. Raise a stat or the bond and press again.
                snprintf(evoMsg_, sizeof evoMsg_, "NO GATE MET");
                evoMsgCol_ = col::warn;
                break;
        }
        evoMsgT_ = EVO_MSG_SECS;
        return;
    }

    // open the species picker, scrolled so the CURRENT species starts mid-view (with a
    // long modded roster, "where am I" matters more than "what's first in the order").
    if (SP_BTN.contains(in)) {
        const CreatureRegistry& reg = app().creatures;
        const int n = reg.count();

        // Sorted on every open, not once: cheap (n <= 200, a handful of ms at worst)
        // and immune to ever going stale against a future registry reload.
        for (int i = 0; i < n; i++) order_[i] = (int16_t)i;
        for (int i = 1; i < n; i++) {                       // insertion sort
            int16_t v = order_[i];
            int j = i;
            while (j > 0 && species_before(reg, v, order_[j - 1])) {
                order_[j] = order_[j - 1];
                j--;
            }
            order_[j] = v;
        }

        int pos = 0;                                        // display row of the current species
        for (int i = 0; i < n; i++)
            if (order_[i] == pet.creatureIndex()) { pos = i; break; }

        list_.geom(0, PICK_Y, GAME_W, GAME_H - PICK_Y - 6, PICK_ROW_H);
        list_.reset();
        float want = (float)(pos * PICK_ROW_H) - (float)(list_.h - PICK_ROW_H) / 2;
        float m    = list_.maxScroll(n);
        list_.scroll = want < 0 ? 0 : (want > m ? m : want);
        picking_ = true;
        return;
    }
}
