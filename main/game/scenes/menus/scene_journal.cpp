#include "scene_journal.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/clock.hpp"
#include "sim/lineage.hpp"   // the In Memory page renders the lineage ledger
#include "ui/widgets.hpp"
#include "ui/tabs.hpp"
#include <cstdio>

// layout
static const int TAB_Y = 64, TAB_H = 28;
static const int LIST_Y = 100;
// Full height to the panel's bottom edge; CARD_G goes to ListView::padB. See scene_feed.cpp.
static const int VIEW_H = GAME_H - LIST_Y;            // not LIST_H (vendor include guard)
static const int PAD_X  = 12;

static const int ROW_MEM  = 30;   // one memory: title + when
static const int ROW_FACT = 40;   // one fact: up to two wrapped lines
static const int ROW_LIN  = 40;   // one predecessor: name/gen + lived/stage/bond
static const int CARD_G   = 6;    // gutter below each card, inside its row (any page)

static const char* const TABS[3] = { "Memories", "About You", "In Memory" };
static const int TAB_N = 3;

static int row_h(int tab) { return tab == 0 ? ROW_MEM : tab == 1 ? ROW_FACT : ROW_LIN; }

// "just now" / "4h ago" / "3d ago" -- a diary wants elapsed time, not a timestamp.
static void ago_str(uint32_t when, char* out, int n)
{
    // Stamped while the RTC had no trustworthy time (clock_now() returns 0 then), so a
    // difference against a clock that has since been set would be decades of nonsense.
    if (when == 0) { snprintf(out, n, "earlier"); return; }
    uint32_t now = clock_now();
    uint32_t d = (now > when) ? (now - when) : 0;    // guard a re-set RTC
    if      (d < 60)      snprintf(out, n, "just now");
    else if (d < 3600)    snprintf(out, n, "%um ago", (unsigned)(d / 60));
    else if (d < 86400)   snprintf(out, n, "%uh ago", (unsigned)(d / 3600));
    else                  snprintf(out, n, "%ud ago", (unsigned)(d / 86400));
}

void SceneJournal::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, row_h(tab_), CARD_G);
    list_.reset();                             // top of the list, no leftover flick
}

void SceneJournal::render()
{
    const ConversationSystem& cv = app().conversations;
    const int n = (tab_ == 0) ? cv.journalCount()
                : (tab_ == 1) ? cv.factCount()
                              : lineage_count(app().save);

    fb.fillScreen(col::panel);
    gfx_text(PAD_X, 14, 3, col::accent, "JOURNAL");

    // Identity: what all of this has added up to.
    char plbl[40];
    gfx_text(PAD_X, 46, 1, col::white, "%s", app().drift.label(plbl, sizeof plbl));

    tabbar_draw(PAD_X, TAB_Y, GAME_W - 2 * PAD_X, TAB_H, TABS, TAB_N, tab_);

    if (n == 0) {
        const char* msg = (tab_ == 0) ? "Nothing to remember yet."
                        : (tab_ == 1) ? "They haven't learned anything\nabout you yet."
                                      : "No one has been lost.";
        gfx_text_wrap(PAD_X + 8, LIST_Y + 24, GAME_W - 2 * PAD_X - 16, 1, col::dim, msg, 6);
        draw_back();
        return;
    }

    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(n); i++) {
        Rect row = list_.rowRect(i);
        Rect card{ PAD_X, row.y, GAME_W - 2 * PAD_X, row_h(tab_) - CARD_G };
        card.fill(col::card, 6);

        if (tab_ == 0) {
            const ConvJournalEntry& e = cv.journalAt(i);
            char when[16];
            ago_str(e.when, when, sizeof when);
            // Title is data-driven, so fit it to the space left beside the timestamp.
            gfx_text_fit(card.x + 8, row.y + 7, card.w - 70, 1, col::white, "%s", e.title);
            gfx_text(card.x + card.w - 58, row.y + 7, 1, col::dim, "%s", when);
        } else if (tab_ == 1) {
            const ConvFact& f = cv.factAt(i);
            // Prefer the writer's phrasing; fall back to the raw pair so a fact set without a
            // note is still visible rather than silently blank.
            if (f.note[0])
                gfx_text_wrap(card.x + 8, row.y + 6, card.w - 16, 1, col::white, f.note, 3, -1, 2);
            else
                gfx_text_fit(card.x + 8, row.y + 6, card.w - 16, 1, col::dim, "%s: %s", f.key, f.val);
        } else {
            LineageRecord rec{};
            if (!lineage_get(app().save, i, &rec)) continue;
            // Species id -> display name while the species is still installed; the raw id
            // is an honest fallback for a mod creature whose pack has since been removed.
            int ci = app().creatures.indexOf(rec.speciesId);
            const char* species = ci >= 0 ? app().creatures.at(ci).name : rec.speciesId;
            char days[16];
            snprintf(days, sizeof days, "%ud", (unsigned)(rec.ageSecs / 86400u));
            gfx_text_fit(card.x + 8, row.y + 6, card.w - 60, 1, col::white, "Gen %u  %s",
                         (unsigned)rec.generation,
                         rec.nickname[0] ? rec.nickname : species);
            gfx_text(card.x + card.w - 8 - (int)strlen(days) * 6, row.y + 6, 1, col::dim,
                     "%s", days);
            gfx_text_fit(card.x + 8, row.y + 20, card.w - 16, 1, col::dim, "%s - %s%s",
                         stage_name(rec.stage), friendship_tier_name(rec.friendship),
                         rec.cause == (uint8_t)BRINK_OLDAGE ? ", a full life" : "");
        }
    }
    list_.endClip();
    list_.drawScrollbar(n);

    draw_back();
}

void SceneJournal::onInput(const Input& in)
{
    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    if (in.pressed) {
        int t = tabbar_hit(in.x, in.y, PAD_X, TAB_Y, GAME_W - 2 * PAD_X, TAB_H, TAB_N);
        if (t >= 0 && t != tab_) {
            sfx::play(sfx::kTap);
            tab_ = t;
            list_.geom(0, LIST_Y, GAME_W, VIEW_H, row_h(tab_), CARD_G);   // row height differs per page
            list_.reset();
            return;
        }
    }

    const ConversationSystem& cv = app().conversations;
    const int n = (tab_ == 0) ? cv.journalCount()
                : (tab_ == 1) ? cv.factCount()
                              : lineage_count(app().save);
    list_.update(in, n);   // scrollable; rows aren't tappable (nothing to open yet)
}
