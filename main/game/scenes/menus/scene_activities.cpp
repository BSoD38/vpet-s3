#include "scene_activities.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include <cstring>

// ---- minigame registry -----------------------------------------------------------------
// One row per training minigame. `available(pet)` gates species/stage-exclusive games
// (nullptr = always available once Activities is unlocked). Add a row here when a new
// minigame scene is wired into App/SceneId. `trains` is the short stat tagline shown on
// the card. Order = display order. The list scrolls, so the roster can grow freely.
struct Activity {
    const char* name;
    const char* trains;                 // e.g. "AGI + Max HP"
    SceneId     scene;
    Slide       slide;                  // transition into the game (Iris = the minigame wipe)
    bool      (*available)(const Pet&); // nullptr => always available
};

static const Activity ACTS[] = {
    { "Run",       "AGI + Max HP", SceneId::Run,      Slide::Iris, nullptr },
    { "Mind Maze", "INT",          SceneId::MindMaze, Slide::Iris, nullptr },
    { "Smash",     "STR + Max HP", SceneId::Smash,    Slide::Iris, nullptr },
    { "Bulwark",   "END + AGI",    SceneId::Bulwark,  Slide::Iris, nullptr },
    { "Stance",    "END + Max HP", SceneId::Stance,   Slide::Iris, nullptr },
};
static const int ACT_N = (int)(sizeof(ACTS) / sizeof(ACTS[0]));

// layout: cards are inset inside full-width list rows; the row's spare CARD_G is the gap.
static const int CARD_X = 20, CARD_W = 200, CARD_H = 42, CARD_G = 8;
static const int ROW_H  = CARD_H + CARD_G;
static const int LIST_Y = 64;                              // viewport top
// Down to the panel's bottom edge, with CARD_G handed to ListView::padB: a viewport that
// stopped short of GAME_H showed its own clip line, and the last card stopped a gutter above
// the screen edge on top of that. See scene_feed.cpp.
// NB: not LIST_H -- that name is an include guard in one of the vendor driver headers.
static const int VIEW_H = GAME_H - LIST_Y;

static bool act_available(const Activity& a, const Pet& pet)
{
    return a.available == nullptr || a.available(pet);
}

void SceneActivities::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, ROW_H, CARD_G);
    list_.reset();                                          // top of the list, no leftover flick
}

void SceneActivities::render()
{
    fb.fillScreen(col::panel);
    gfx_text(20, 18, 3, col::accent, "TRAIN");

    // energy readout (training spends stamina; a tired pet trains at reduced rates). Sits in
    // the header, above the scrolling viewport, so a card can never overlap it.
    int en = (int)app().pet.energy();
    gfx_text(20, 46, 1, en < 25 ? col::warn : col::dim,
             "Energy %d/100%s", en, en < 25 ? "  (tired)" : "");

    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(ACT_N); i++) {
        const Activity& a = ACTS[i];
        bool ok = act_available(a, app().pet);
        Rect row = list_.rowRect(i);
        Rect card{ CARD_X, row.y, CARD_W, CARD_H };

        card.fill(ok ? col::card : rgb565(44, 46, 56), 8);
        card.outline(ok ? col::accent : col::dim, 8);
        gfx_text(CARD_X + 12, row.y + 6,  2, ok ? col::white : col::dim, "%s", a.name);
        gfx_text(CARD_X + 12, row.y + 26, 1, ok ? col::dim : rgb565(120, 120, 130),
                 ok ? "trains %s" : "locked - %s", a.trains);
    }
    list_.endClip();
    list_.drawScrollbar(ACT_N);

    draw_back();
}

void SceneActivities::onInput(const Input& in)
{
    // Back sits above the viewport, so it can't be swallowed by a scroll gesture.
    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    list_.update(in, ACT_N);

    // A tap anywhere on the row (including the gap and the margins beside the card) selects
    // it -- deliberately forgiving, since a flat-thumb centroid often lands off a small target.
    int row = list_.tapped();
    if (row >= 0 && act_available(ACTS[row], app().pet)) {
        app().setScene(ACTS[row].scene, ACTS[row].slide);
    }
}
