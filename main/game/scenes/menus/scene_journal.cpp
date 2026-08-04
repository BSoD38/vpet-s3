#include "scene_journal.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/clock.hpp"
#include "ui/widgets.hpp"
#include "ui/tabs.hpp"
#include <cstdio>

// layout
static const int TAB_Y = 64, TAB_H = 28;
static const int LIST_Y = 100, LIST_B = 8;
static const int VIEW_H = GAME_H - LIST_Y - LIST_B;   // not LIST_H (vendor include guard)
static const int PAD_X  = 12;

static const int ROW_MEM  = 30;   // one memory: title + when
static const int ROW_FACT = 40;   // one fact: up to two wrapped lines

static const char* const TABS[2] = { "Memories", "About You" };

static int row_h(int tab) { return tab == 0 ? ROW_MEM : ROW_FACT; }

// "just now" / "4h ago" / "3d ago" -- a diary wants elapsed time, not a timestamp.
static void ago_str(uint32_t when, char* out, int n)
{
    uint32_t now = clock_now();
    uint32_t d = (now > when) ? (now - when) : 0;    // guard a re-set RTC
    if      (d < 60)      snprintf(out, n, "just now");
    else if (d < 3600)    snprintf(out, n, "%um ago", (unsigned)(d / 60));
    else if (d < 86400)   snprintf(out, n, "%uh ago", (unsigned)(d / 3600));
    else                  snprintf(out, n, "%ud ago", (unsigned)(d / 86400));
}

void SceneJournal::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, row_h(tab_));
    list_.reset();                             // top of the list, no leftover flick
}

void SceneJournal::render()
{
    const ConversationSystem& cv = app().conversations;
    const int n = (tab_ == 0) ? cv.journalCount() : cv.factCount();

    fb.fillScreen(col::panel);
    gfx_text(PAD_X, 14, 3, col::accent, "JOURNAL");

    // Identity: what all of this has added up to.
    char plbl[40];
    gfx_text(PAD_X, 46, 1, col::white, "%s", app().drift.label(plbl, sizeof plbl));

    tabbar_draw(PAD_X, TAB_Y, GAME_W - 2 * PAD_X, TAB_H, TABS, 2, tab_);

    if (n == 0) {
        const char* msg = (tab_ == 0) ? "Nothing to remember yet."
                                      : "They haven't learned anything\nabout you yet.";
        gfx_text_wrap(PAD_X + 8, LIST_Y + 24, GAME_W - 2 * PAD_X - 16, 1, col::dim, msg, 6);
        draw_back();
        return;
    }

    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(n); i++) {
        Rect row = list_.rowRect(i);
        Rect card{ PAD_X, row.y, GAME_W - 2 * PAD_X, row_h(tab_) - 6 };
        card.fill(col::card, 6);

        if (tab_ == 0) {
            const ConvJournalEntry& e = cv.journalAt(i);
            char when[16];
            ago_str(e.when, when, sizeof when);
            // Title is data-driven, so fit it to the space left beside the timestamp.
            gfx_text_fit(card.x + 8, row.y + 7, card.w - 70, 1, col::white, "%s", e.title);
            gfx_text(card.x + card.w - 58, row.y + 7, 1, col::dim, "%s", when);
        } else {
            const ConvFact& f = cv.factAt(i);
            // Prefer the writer's phrasing; fall back to the raw pair so a fact set without a
            // note is still visible rather than silently blank.
            if (f.note[0])
                gfx_text_wrap(card.x + 8, row.y + 6, card.w - 16, 1, col::white, f.note, 3, -1, 2);
            else
                gfx_text_fit(card.x + 8, row.y + 6, card.w - 16, 1, col::dim, "%s: %s", f.key, f.val);
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
        int t = tabbar_hit(in.x, in.y, PAD_X, TAB_Y, GAME_W - 2 * PAD_X, TAB_H, 2);
        if (t >= 0 && t != tab_) {
            tab_ = t;
            list_.geom(0, LIST_Y, GAME_W, VIEW_H, row_h(tab_));   // row height differs per page
            list_.reset();
            return;
        }
    }

    const ConversationSystem& cv = app().conversations;
    const int n = (tab_ == 0) ? cv.journalCount() : cv.factCount();
    list_.update(in, n);   // scrollable; rows aren't tappable (nothing to open yet)
}
