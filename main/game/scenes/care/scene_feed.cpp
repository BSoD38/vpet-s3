#include "scene_feed.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "sim/foods.hpp"
#include "sim/economy.hpp"
#include "engine/audio/sfx.hpp"

// layout
static const int ROW_H  = 46;
static const int LIST_Y = 64, LIST_B = 6;
static const int VIEW_H = GAME_H - LIST_Y - LIST_B;   // not LIST_H: that name is a vendor include guard
static const int PAD_X  = 12;
static const int SWATCH_R = 13;

void SceneFeed::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, ROW_H);
    list_.reset();                                     // top of the list, no leftover flick
}

void SceneFeed::render()
{
    const FoodRegistry& foods = app().foods;
    const int n = foods.count();

    fb.fillScreen(col::panel);
    gfx_text(PAD_X, 18, 3, col::accent, "FEED");
    // Rows can buy (see onInput), so the wallet belongs on this screen too -- being told the
    // price of something without being told what you have is the wrong half of the story.
    draw_wallet(GAME_W - PAD_X, 42, app().economy.bits(), 1);

    // Coarse hunger state, so you know whether a snack or a full meal is called for
    // without being handed a number to optimise.
    const float hunger = app().pet.state().hunger;
    const int   tier   = care_tier(hunger);
    gfx_text(PAD_X, 46, 1, col::dim, "Hunger");
    gfx_text(PAD_X + 44, 46, 1, care_tier_color(tier), "%s", hunger_label(hunger));

    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(n); i++) {
        const Food& f = foods.at(i);
        Rect row = list_.rowRect(i);

        Rect card{ PAD_X, row.y, GAME_W - 2 * PAD_X, ROW_H - 6 };
        card.fill(col::card, 8);
        card.outline(col::dim, 8);

        int cy = row.y + (ROW_H - 6) / 2;
        fb.fillCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, f.color);
        fb.drawCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, col::black);

        // Name/desc come from data files (mods included), so their length is unknown at
        // layout time -- fit them to the space left inside the card instead of letting
        // them run past its edge.
        // Stock, per docs/economy-and-inventory.md 3. A FREE food (cost 0 -- kibble, and any
        // mod food that copies it) is unlimited and shows no count at all: the rule is data,
        // not a special case for one id, so the row simply has nothing to say.
        int rightW = 0;
        if (f.cost > 0) {
            const int held = app().economy.count(f.id);
            char tagbuf[16];
            uint16_t tagcol;
            if (held > 0) {
                snprintf(tagbuf, sizeof tagbuf, "x%d", held);
                tagcol = col::white;
            } else if (app().economy.canAfford(f.cost)) {
                // Out of stock but affordable: the row becomes a buy button, so running dry
                // never means a trip to the shop mid-feed.
                snprintf(tagbuf, sizeof tagbuf, "BUY %u", (unsigned)f.cost);
                tagcol = col::accent;
            } else {
                snprintf(tagbuf, sizeof tagbuf, "%u", (unsigned)f.cost);
                tagcol = col::dim;
            }
            rightW = (int)strlen(tagbuf) * 6 + 8;
            gfx_text(card.x + card.w - rightW, cy - 4, 1, tagcol, "%s", tagbuf);
        }

        const bool out = (f.cost > 0 && app().economy.count(f.id) == 0);

        int tx = PAD_X + 8 + SWATCH_R * 2 + 10;
        int textW = (card.x + card.w) - tx - rightW - 6;
        gfx_text_fit(tx, row.y + 6,  textW, 2, out ? col::dim : col::white, "%s", f.name);
        gfx_text_fit(tx, row.y + 25, textW, 1, col::dim,   "%s", f.desc);
    }
    list_.endClip();
    list_.drawScrollbar(n);

    draw_back();
}

void SceneFeed::onInput(const Input& in)
{
    // Back sits above the viewport, so a scroll gesture can never swallow it.
    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }

    const int n = app().foods.count();
    list_.update(in, n);

    int row = list_.tapped();
    if (row < 0 || row >= n) return;

    const Food& f = app().foods.at(row);

    // Out of stock: the tap BUYS one and stays put, so the next tap feeds it. Deliberately
    // two gestures -- spending the player's Bits as a side effect of reaching for the food
    // they always tap is exactly the kind of surprise a shop should never spring.
    if (f.cost > 0 && app().economy.count(f.id) == 0) {
        if (app().economy.buy(f.id, ITEM_FOOD, f.cost)) {
            app().economy.flush();
            sfx::play(sfx::kSelect);
        } else {
            sfx::play(sfx::kDenied);
        }
        return;
    }

    // Ask BEFORE consuming: an egg, a sleeping or a sick creature refuses the meal, and a
    // refused meal must not eat the stock. canEat() arms the "no" wiggle itself.
    if (!app().pet.canEat()) { app().pet.playRefusal(); return; }

    if (f.cost > 0) { app().economy.take(f.id, 1); app().economy.flush(); }
    app().pet.feed(f);
    app().setScene(SceneId::Home, Slide::Back);   // feed and return, one gesture
}
