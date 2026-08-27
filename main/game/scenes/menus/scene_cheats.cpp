#include "scene_cheats.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"       // clampf (vitality slider)
#include "engine/audio/sfx.hpp"  // tap/select/denied feedback (setScene only voices navigation)
#include "ui/widgets.hpp"
#include "sim/creatures.hpp"
#include <cstring>
#include <strings.h> // strcasecmp (picker sort)
#include <cstdio>    // snprintf (labels)

// ---- layout -----------------------------------------------------------------------------
// One scrolling list of uniform rows below the fixed title/Back header. Row content is
// addressed by ROWS[] below; taps resolve to a row via the ListView and to a zone within
// it via the release x, so nothing here owns a screen-absolute rectangle any more.
static const int LIST_Y = 48, ROW_H = 34;
static const int ROW_G  = 3;                          // gutter below each row's button

// Both lists run to the panel's bottom edge, with the row gutter handed to ListView::padB:
// a viewport that stopped short of GAME_H showed its own clip line as a strip of bare panel
// under a half-drawn row. See scene_feed.cpp.
static const int LIST_H_MAIN = GAME_H - LIST_Y;

// x-zones within a row: split rows break at the middle; stat rows put steppers at the
// right edge (same places the old fixed layout kept them, so muscle memory survives).
static const int SPLIT_X = 122;                       // left | right halves
static const int MINUS_X0 = 140, PLUS_X0 = 188;       // [-] then [+] to the row's edge

// picker: single-line rows, so a 200-slot modded roster is a couple of flicks tall
static const int PICK_Y     = 52;
static const int PICK_ROW_H = 36;
static const int PICK_G     = 2;                      // gutter below each picker card
static const int PICK_H     = GAME_H - PICK_Y;

// How long a force-evolve result stays on the button.
static const float EVO_MSG_SECS = 2.5f;

// Sim-speed steps (moved here from Settings: accelerating the whole simulation is a
// testing aid, not a player option).
static const int SPEEDS[] = { 1, 2, 5, 10, 20, 50, 100 };
static const int SPEED_N  = (int)(sizeof(SPEEDS) / sizeof(SPEEDS[0]));

struct StatRow { StatId id; const char* label; int step; };
static const StatRow SROWS[] = {
    { STAT_STR,   "STR", 250 },
    { STAT_END,   "END", 250 },
    { STAT_AGI,   "AGI", 250 },
    { STAT_INT,   "INT", 250 },
    { STAT_MAXHP, "HP",  2500 },
};

// ---- the row table ----------------------------------------------------------------------
// Label, content and behaviour travel together per row; inserting a cheat is one entry
// here plus a case in the render and tap switches (same argument as SceneMenu's table).
enum RowKind : uint8_t {
    ROW_HEADER,    // section label; not tappable
    ROW_RESTORE,   // RESTORE ALL
    ROW_HP_EN,     // Full HP | Full Stamina
    ROW_SPEED,     // sim-speed multiplier: label, value, [-] [+] through SPEEDS[]
    ROW_VIT,       // vitality slider (sets the life track; thresholds ticked on the track)
    ROW_COND,      // condition cycle: OK -> Sick -> Very sick -> Hurt -> OK
    ROW_FRIEND,    // friendship, [-]/[+] in steps of 100 (drives miracle odds + farewells)
    ROW_BRINK,     // straight to the death event: Critical | Old age (the roll is REAL)
    ROW_STAT,      // one battle stat: label, value, [-] [+]   (arg = SROWS index)
    ROW_MAXZERO,   // MAX STATS | ZERO STATS
    ROW_SPECIES,   // current species; opens the picker
    ROW_EVO,       // force the earned evolution
    ROW_BITS,      // wallet, [-]/[+] in steps of 500 (there is no other way to test the Shop)
};
struct CheatRow { RowKind kind; int8_t arg; const char* label; };
static const CheatRow ROWS[] = {
    { ROW_HEADER,  0, "RESTORE" },
    { ROW_RESTORE, 0, nullptr },
    { ROW_HP_EN,   0, nullptr },
    { ROW_HEADER,  0, "SIM (test levers)" },
    { ROW_SPEED,   0, nullptr },
    { ROW_VIT,     0, nullptr },
    { ROW_COND,    0, nullptr },
    { ROW_FRIEND,  0, nullptr },
    { ROW_BRINK,   0, nullptr },
    { ROW_HEADER,  0, "STATS" },
    { ROW_STAT,    0, nullptr },
    { ROW_STAT,    1, nullptr },
    { ROW_STAT,    2, nullptr },
    { ROW_STAT,    3, nullptr },
    { ROW_STAT,    4, nullptr },
    { ROW_MAXZERO, 0, nullptr },
    { ROW_HEADER,  0, "SPECIES" },
    { ROW_SPECIES, 0, nullptr },
    { ROW_EVO,     0, nullptr },
    { ROW_HEADER,  0, "ECONOMY" },
    { ROW_BITS,    0, nullptr },
};
static const int ROW_N = (int)(sizeof(ROWS) / sizeof(ROWS[0]));

// One kind's row index in ROWS[] (first hit). For rows something outside the render loop
// needs to locate -- the vitality slider's input intercept hit-tests against its track.
static int row_index(RowKind k)
{
    for (int i = 0; i < (int)(sizeof(ROWS) / sizeof(ROWS[0])); i++)
        if (ROWS[i].kind == k) return i;
    return -1;
}

// The neutral button grey every plain control here wears.
static const uint16_t kBtnBg = rgb565(60, 64, 84);

// Which stepper zone an x lands in: -1 for [-], +1 for [+], 0 for neither (label/value).
// Shared by every row with steppers, so the zones can't drift apart between them.
static int stepper_dir(int x)
{
    return (x >= MINUS_X0 && x < PLUS_X0) ? -1 : (x >= PLUS_X0) ? 1 : 0;
}

// Row-local button rects (x is screen-absolute; y comes from the row).
static Rect row_full(const Rect& r)  { return { 12, r.y + 3, GAME_W - 24, ROW_H - 6 }; }
static Rect row_left(const Rect& r)  { return { 12, r.y + 3, SPLIT_X - 16, ROW_H - 6 }; }
static Rect row_right(const Rect& r) { return { SPLIT_X + 2, r.y + 3, GAME_W - SPLIT_X - 14, ROW_H - 6 }; }
static Rect stat_minus(const Rect& r) { return { MINUS_X0 + 2, r.y + 4, PLUS_X0 - MINUS_X0 - 6, ROW_H - 8 }; }
static Rect stat_plus (const Rect& r) { return { PLUS_X0 + 2,  r.y + 4, GAME_W - PLUS_X0 - 16, ROW_H - 8 }; }

static void draw_steppers(const Rect& r)
{
    stat_minus(r).button("-", kBtnBg, col::white, 2);
    stat_plus(r).button ("+", kBtnBg, col::white, 2);
}

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
    main_.geom(0, LIST_Y, GAME_W, LIST_H_MAIN, ROW_H, ROW_G);
    main_.reset();
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
    gfx_text(16, 18, 2, col::accent, "Cheats");
    draw_back();

    main_.beginClip();
    for (int i = main_.first(); i <= main_.last(ROW_N); i++) {
        Rect r = main_.rowRect(i);
        switch (ROWS[i].kind) {
            case ROW_HEADER:
                gfx_text(14, r.y + (ROW_H - 8) / 2, 1, col::dim, "%s", ROWS[i].label);
                break;
            case ROW_RESTORE:
                row_full(r).button("RESTORE ALL", col::good, col::black, 1);
                break;
            case ROW_HP_EN:
                row_left(r).button ("Full HP",      rgb565(70, 120, 90), col::white, 1);
                row_right(r).button("Full Stamina", rgb565(70, 120, 90), col::white, 1);
                break;
            case ROW_SPEED:
                gfx_text(14, r.y + (ROW_H - 8) / 2, 1, col::white, "Speed");
                // While frozen the multiplier is moot -- the clock it multiplies isn't
                // running -- but the steppers stay live: picking the speed to come back
                // to is a reasonable thing to do while the sim is paused.
                if (pet.frozen())
                    gfx_text(58, r.y + (ROW_H - 8) / 2, 1, kFrozenCol, "paused");
                else
                    gfx_text(58, r.y + (ROW_H - 8) / 2, 1, col::accent, "%ux",
                             (unsigned)pet.state().gameSpeed);
                draw_steppers(r);
                break;
            case ROW_VIT: {
                // Filled-track slider (same read as the Settings volume sliders). The fill
                // is the pool as a fraction of the EFFECTIVE ceiling (scars shrink it), the
                // ticks mark the Twilight/Elderly thresholds, and the centred text names the
                // stage the position lands in. While dragging, the PENDING value is drawn;
                // it commits (one NVS write) on release -- and 0 is a real destination: the
                // pool empties and the next tick enters the death event, roll and all.
                const VitalsTuning& T = vitals_tuning();
                float frac = vitDrag_ ? vitFrac_ : pet.vitalityFrac();
                const char* stage; uint16_t fg;   // the stage the position lands in
                if      (frac <= T.twilightFrac) { stage = "TWILIGHT"; fg = rgb565(120, 80, 110); }
                else if (frac <= T.elderlyFrac)  { stage = "ELDERLY";  fg = rgb565(140, 120, 70); }
                else                             { stage = "PRIME";    fg = col::good; }
                Rect tr = row_full(r);
                tr.fill(rgb565(38, 42, 58), 8);
                int w = (int)(tr.w * frac + 0.5f);
                if (w > 0) {
                    if (w < 10) w = 10;              // a sliver still reads as a rounded end
                    fb.fillRoundRect(tr.x, tr.y, w, tr.h, 8, fg);
                }
                tr.outline(col::dim, 8);
                fb.fillRect(tr.x + (int)(tr.w * T.twilightFrac), tr.y + 2, 1, tr.h - 4, col::dim);
                fb.fillRect(tr.x + (int)(tr.w * T.elderlyFrac),  tr.y + 2, 1, tr.h - 4, col::dim);
                char lbl[28];
                snprintf(lbl, sizeof lbl, "Vitality %d%%  %s", (int)(frac * 100.0f + 0.5f), stage);
                gfx_text(tr.x + (tr.w - (int)strlen(lbl) * 6) / 2, tr.y + (tr.h - 8) / 2, 1,
                         col::white, "%s", lbl);
                break;
            }
            case ROW_COND: {
                gfx_text(14, r.y + (ROW_H - 8) / 2, 1, col::white, "Condition");
                const char* m = pet.conditionMarker();
                row_right(r).button(m ? m : "OK", m ? rgb565(140, 80, 70) : kBtnBg, col::white, 1);
                break;
            }
            case ROW_BITS:
                gfx_text(14, r.y + (ROW_H - 8) / 2, 1, col::white, "Bits");
                gfx_text_fit(46, r.y + (ROW_H - 8) / 2, MINUS_X0 - 50, 1, col::good,
                             "%u", (unsigned)app().economy.bits());
                draw_steppers(r);
                break;
            case ROW_FRIEND:
                gfx_text(14, r.y + (ROW_H - 8) / 2, 1, col::white, "Bond");
                // Number AND tier: the tier is what the death system's gates actually read
                // (miracle floor 4000, farewells at 5000/8500), so it saves the arithmetic.
                gfx_text_fit(46, r.y + (ROW_H - 8) / 2, MINUS_X0 - 50, 1, col::accent,
                             "%u %s", (unsigned)pet.friendship(), pet.friendshipTier());
                draw_steppers(r);
                break;
            case ROW_BRINK:
                // Not simulations of the event -- entrances to it. The roll they trigger
                // is as binding as a natural death's (see Pet::cheatTriggerBrink).
                row_left(r).button ("DIE: CRITICAL", rgb565(150, 60, 60),  col::white, 1);
                row_right(r).button("DIE: OLD AGE",  rgb565(110, 80, 130), col::white, 1);
                break;
            case ROW_STAT: {
                const StatRow& s = SROWS[(int)ROWS[i].arg];
                gfx_text(14, r.y + (ROW_H - 16) / 2, 2, col::white, "%s", s.label);
                gfx_text(58, r.y + (ROW_H - 8) / 2, 1, col::accent, "%lu", (unsigned long)pet.stat(s.id));
                draw_steppers(r);
                break;
            }
            case ROW_MAXZERO:
                row_left(r).button ("MAX STATS",  rgb565(120, 90, 150), col::white, 1);
                row_right(r).button("ZERO STATS", rgb565(90, 70, 80),   col::white, 1);
                break;
            case ROW_SPECIES: {
                const Creature& c = app().creatures.at(pet.creatureIndex());
                char sp[48];
                snprintf(sp, sizeof sp, "%s (T%u)", c.name, (unsigned)c.tier);
                Rect b = row_full(r);
                b.button(sp, col::card, col::white, 1);
                b.outline(col::accent);
                break;
            }
            case ROW_EVO:
                // The result is shown ON the button for a couple of seconds: the interesting
                // outcomes are the ones where nothing visibly happens ("no gate met yet",
                // "this form is terminal"), and an unlabelled dead button reads as broken.
                if (evoMsgT_ > 0.0f) row_full(r).button(evoMsg_, evoMsgCol_, col::black, 1);
                else                 row_full(r).button("FORCE EVOLVE", rgb565(150, 110, 60), col::white, 1);
                break;
        }
    }
    main_.endClip();
    main_.drawScrollbar(ROW_N);
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
        Rect card { 12, row.y + PICK_G, GAME_W - 24, PICK_ROW_H - 2 * PICK_G };
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
    if (in.pressed && kBack.contains(in)) {
        picking_ = false;
        sfx::play(sfx::kBack);      // in-scene mode switch; setScene isn't involved
        return;
    }

    const int n = app().creatures.count();
    list_.update(in, n);

    int row = list_.tapped();
    if (row >= 0 && row < n) {
        app().pet.cheatSetSpecies(order_[row]);
        picking_ = false;           // morph and return, one gesture
        sfx::play(sfx::kSelect);    // committing a choice
    }
}

void SceneCheats::onInput(const Input& in)
{
    // Both views need the full press/drag/release stream (scroll gestures), so neither
    // branch gates on in.pressed the way the old fixed layout did.
    if (picking_) { inputPicker(in); return; }

    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Settings, Slide::Back);
        return;
    }

    Pet& pet = app().pet;

    // Vitality slider: a press on its track grabs the whole gesture from the list (so
    // dragging the knob can't scroll or tap), follows the finger, and commits exactly one
    // NVS write on release -- the same contract as the Settings volume sliders. rowRect()
    // gives the row's true on-screen position at the current scroll, and the viewport
    // check keeps a scrolled-out track from ghost-catching presses on the header.
    {
        const Rect tr = row_full(main_.rowRect(row_index(ROW_VIT)));
        if (!vitDrag_ && in.pressed && in.y >= LIST_Y && tr.contains(in)) {
            vitDrag_ = true;
            vitFrac_ = clampf((float)(in.x - tr.x) / (float)tr.w, 0.0f, 1.0f);
            return;
        }
        if (vitDrag_) {
            if (in.down) {
                vitFrac_ = clampf((float)(in.x - tr.x) / (float)tr.w, 0.0f, 1.0f);
            } else {
                pet.cheatSetVitality(vitFrac_);
                vitDrag_ = false;
                sfx::play(sfx::kSelect);   // commit click on release, like the volume sliders
            }
            return;
        }
    }

    main_.update(in, ROW_N);
    int row = main_.tapped();
    if (row < 0) return;

    // Feedback is decided ONCE, after the dispatch -- the same centralising argument as
    // App::setScene's navigation sounds: no case can forget to be audible. A resolved tap
    // defaults to the plain click; a case overrides only when it deviates (kDenied when
    // nothing could change, nullptr when the tap hit dead space or the sim voices the
    // outcome itself).
    const char* fb = sfx::kTap;
    switch (ROWS[row].kind) {
        case ROW_RESTORE:
            pet.cheatRestore();
            break;
        case ROW_HP_EN:
            if (in.x < SPLIT_X) pet.cheatSetHealth(100);
            else                pet.cheatSetEnergy(100);
            break;
        case ROW_SPEED: {
            unsigned cur = pet.state().gameSpeed;
            int idx = 0;
            for (int i = 0; i < SPEED_N; i++)
                if ((unsigned)SPEEDS[i] == cur) { idx = i; break; }
            int dir = stepper_dir(in.x);
            if (dir == 0) { fb = nullptr; break; }      // tapped the label, not a stepper
            int ni = idx + dir;
            if (ni < 0 || ni >= SPEED_N) fb = sfx::kDenied;   // end of range
            else pet.setGameSpeed((uint16_t)SPEEDS[ni]);
            break;
        }
        // condition cycle. Critical and Recovery are deliberately absent: neither is
        // reachable until the death event exists (D2), and Critical refuses treatment by
        // design -- a cheat that strands the pet in it would be a trap, not a lever.
        case ROW_COND:
            switch (pet.condition()) {
                case COND_HEALTHY:  pet.cheatSetCondition(COND_SICK);     break;
                case COND_SICK:     pet.cheatSetCondition(COND_SICK_BAD); break;
                case COND_SICK_BAD: pet.cheatSetCondition(COND_INJURED);  break;
                default:            pet.cheatSetCondition(COND_HEALTHY);  break;
            }
            break;
        case ROW_BITS: {
            // Bits are otherwise earned only by playing, so without this the Shop cannot be
            // exercised at all on a fresh save -- and a spend-side bug would be invisible.
            int dir = stepper_dir(in.x);
            if (dir == 0) { fb = nullptr; break; }      // tapped the label, not a stepper
            Economy& e = app().economy;
            uint32_t before = e.bits();
            if (dir > 0) e.earn(500);
            else         e.spend(e.bits() < 500 ? e.bits() : 500);
            if (e.bits() == before) fb = sfx::kDenied;  // pinned at 0 / the cap
            else                    e.flush();
            break;
        }
        case ROW_FRIEND: {
            int dir = stepper_dir(in.x);
            if (dir == 0) { fb = nullptr; break; }      // tapped the label, not a stepper
            uint16_t before = pet.friendship();
            pet.cheatSetFriendship((int)before + dir * 100);
            if (pet.friendship() == before) fb = sfx::kDenied;   // pinned at 0 / the cap
            break;
        }
        case ROW_BRINK:
            // The runLoop's brink routing takes the screen on the very next frame; the
            // denied click is for an egg (unborn things can't die) or an event already
            // under way.
            if (!pet.cheatTriggerBrink(in.x < SPLIT_X ? BRINK_CRITICAL : BRINK_OLDAGE))
                fb = sfx::kDenied;
            else
                fb = nullptr;          // the event's own dusk is the feedback
            break;
        case ROW_STAT: {
            const StatRow& s = SROWS[(int)ROWS[row].arg];
            int dir = stepper_dir(in.x);
            if (dir == 0) { fb = nullptr; break; }      // tapped the label, not a stepper
            uint32_t before = pet.stat(s.id);
            pet.cheatAdjustStat(s.id, dir * s.step);
            // Already pinned at the cap (or zero): a click would lie about a change.
            if (pet.stat(s.id) == before) fb = sfx::kDenied;
            break;
        }
        case ROW_MAXZERO:
            if (in.x < SPLIT_X) for (const StatRow& s : SROWS) pet.cheatMaxStat(s.id);
            else                for (const StatRow& s : SROWS) pet.cheatAdjustStat(s.id, -2000000000);
            break;
        // open the species picker, scrolled so the CURRENT species starts mid-view (with a
        // long modded roster, "where am I" matters more than "what's first in the order").
        case ROW_SPECIES: {
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

            list_.geom(0, PICK_Y, GAME_W, PICK_H, PICK_ROW_H, PICK_G);
            list_.reset();
            float want = (float)(pos * PICK_ROW_H) - (float)(list_.h - PICK_ROW_H) / 2;
            float m    = list_.maxScroll(n);
            list_.scroll = want < 0 ? 0 : (want > m ? m : want);
            picking_ = true;
            break;
        }
        // force the earned evolution (skips only the stage timer -- see Pet::cheatForceEvolve)
        case ROW_EVO: {
            char name[24] = {0};
            switch (pet.cheatForceEvolve(name, sizeof name)) {
                case Pet::ForceEvo::Evolved:
                    snprintf(evoMsg_, sizeof evoMsg_, "-> %s", name);
                    evoMsgCol_ = col::good;
                    fb = nullptr;              // the evolution fanfare is the feedback
                    break;
                case Pet::ForceEvo::Terminal:
                    snprintf(evoMsg_, sizeof evoMsg_, "FINAL FORM");
                    evoMsgCol_ = col::dim;
                    fb = sfx::kDenied;
                    break;
                case Pet::ForceEvo::NotEligible:
                    // Not a failure: the pet has edges but has not met a gate, so there is
                    // nothing it has earned yet. Raise a stat or the bond and press again.
                    snprintf(evoMsg_, sizeof evoMsg_, "NO GATE MET");
                    evoMsgCol_ = col::warn;
                    fb = sfx::kDenied;
                    break;
            }
            evoMsgT_ = EVO_MSG_SECS;
            break;
        }
        default:       // headers and other dead space
            fb = nullptr;
            break;
    }
    if (fb) sfx::play(fb);
}
