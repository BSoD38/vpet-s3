#include "scene_activities.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/widgets.hpp"
#include <cstring>

// ---- minigame registry -----------------------------------------------------------------
// One row per training minigame. `available(pet)` gates species/stage-exclusive games
// (nullptr = always available once Activities is unlocked). Add a row here when a new
// minigame scene is wired into App/SceneId. `trains` is the short stat tagline shown on
// the card. Order = display order.
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

// layout
static const int CARD_X = 20, CARD_W = 200, CARD_H = 42, CARD_G = 8;
static const int LIST_Y = 64;
static const int MAX_VISIBLE = 5;       // rows that fit above the bottom edge (320px screen)

static inline int  card_y(int row)    { return LIST_Y + row * (CARD_H + CARD_G); }
static inline Rect card_rect(int row) { return { CARD_X, card_y(row), CARD_W, CARD_H }; }

static bool act_available(const Activity& a, const Pet& pet)
{
    return a.available == nullptr || a.available(pet);
}

void SceneActivities::render()
{
    fb.fillScreen(col::panel);
    gfx_text(20, 18, 3, col::accent, "TRAIN");

    int shown = ACT_N < MAX_VISIBLE ? ACT_N : MAX_VISIBLE;
    for (int i = 0; i < shown; i++) {
        const Activity& a = ACTS[i];
        bool ok = act_available(a, app().pet);
        int y = card_y(i);

        Rect card = card_rect(i);
        card.fill(ok ? rgb565(52, 58, 84) : rgb565(44, 46, 56), 8);
        card.outline(ok ? col::accent : col::dim, 8);
        gfx_text(CARD_X + 12, y + 6,  2, ok ? col::white : col::dim, "%s", a.name);
        gfx_text(CARD_X + 12, y + 26, 1, ok ? col::dim : rgb565(120, 120, 130),
                 ok ? "trains %s" : "locked - %s", a.trains);
    }

    // honest overflow notice (no silent truncation) until a scroll pass exists
    if (ACT_N > MAX_VISIBLE) {
        gfx_text(CARD_X, card_y(MAX_VISIBLE) + 2, 1, col::warn, "(+%d more)", ACT_N - MAX_VISIBLE);
    }

    // energy readout (training spends stamina; a tired pet trains at reduced rates). Sits in
    // the header, not the bottom, so it never collides with the last (5th) activity card.
    int en = (int)app().pet.energy();
    gfx_text(20, 46, 1, en < 25 ? col::warn : col::dim,
             "Energy %d/100%s", en, en < 25 ? "  (tired)" : "");

    draw_back();
}

void SceneActivities::onInput(const Input& in)
{
    if (!in.pressed) return;

    if (kBack.contains(in)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    int shown = ACT_N < MAX_VISIBLE ? ACT_N : MAX_VISIBLE;
    for (int i = 0; i < shown; i++) {
        if (card_rect(i).contains(in)) {
            const Activity& a = ACTS[i];
            if (!act_available(a, app().pet)) return;   // locked entry: ignore the tap
            app().setScene(a.scene, a.slide);
            return;
        }
    }
}
