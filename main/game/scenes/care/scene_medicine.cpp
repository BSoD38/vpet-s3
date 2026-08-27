#include "scene_medicine.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/audio/sfx.hpp"
#include "ui/widgets.hpp"
#include "sim/items.hpp"
#include <cstring>
#include <cstdio>

// layout
static const int ROW_H  = 46;
static const int CARD_G = 6;                     // gutter below each card, inside its row
static const int LIST_Y = 76;
// Full height to the panel's bottom edge; the gutter is ListView::padB. See scene_feed.cpp.
static const int VIEW_H = GAME_H - LIST_Y;
static const int PAD_X  = 12;
static const int SWATCH_R = 13;

void SceneMedicine::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, ROW_H, CARD_G);
    list_.reset();
    build();
}

void SceneMedicine::build()
{
    rowCount_ = 0;
    const int slots = app().economy.slotCount();
    for (int i = 0; i < slots && rowCount_ < MAX_ROWS; i++) {
        const InvSlot& s = app().economy.slotAt(i);
        if (s.kind != ITEM_CARE) continue;
        // A stack whose mod has been uninstalled has no registry entry, so nothing could be
        // applied from it. Leave it in the bag (the Bag tab still shows it) but not here.
        if (app().items.indexOf(s.id) < 0) continue;
        rows_[rowCount_++] = (int16_t)i;
    }
}

void SceneMedicine::render()
{
    Pet& pet = app().pet;

    fb.fillScreen(col::panel);
    gfx_text(PAD_X, 18, 3, col::accent, "HEAL");   // matches the Home button that opened it
    draw_shop_to();
    draw_wallet(GAME_W - PAD_X, 42, app().economy.bits(), 1);

    // What is actually wrong, and whether a dose would land right now. Both decide the
    // outcome of every tap below, so neither should have to be discovered by being refused.
    const char* marker = pet.conditionMarker();
    gfx_text(PAD_X, 46, 1, col::dim, "Condition");
    gfx_text(PAD_X + 62, 46, 1, marker ? col::warn : col::good, "%s", marker ? marker : "Healthy");

    const uint32_t cd = pet.treatCooldownLeft();
    if (cd > 0) {
        // Rounded UP: "ready in 0h" while still refusing would be the worst of both.
        const unsigned mins = (unsigned)((cd + 59) / 60);
        if (mins >= 60) gfx_text(PAD_X, 60, 1, col::warn, "Next dose in %uh %um", mins / 60, mins % 60);
        else            gfx_text(PAD_X, 60, 1, col::warn, "Next dose in %um", mins);
    }

    if (rowCount_ == 0) {
        const char* a = "No medicine in the bag.";
        const char* b = "The Shop stocks remedies.";
        gfx_text((GAME_W - (int)strlen(a) * 6) / 2, LIST_Y + 30, 1, col::white, "%s", a);
        gfx_text((GAME_W - (int)strlen(b) * 6) / 2, LIST_Y + 46, 1, col::dim,   "%s", b);
        draw_back();
        return;
    }

    list_.beginClip();
    for (int i = list_.first(); i <= list_.last(rowCount_); i++) {
        const InvSlot& s = app().economy.slotAt(rows_[i]);
        const Item& it = app().items.at(app().items.indexOf(s.id));
        Rect row = list_.rowRect(i);

        Rect card{ PAD_X, row.y, GAME_W - 2 * PAD_X, ROW_H - CARD_G };
        card.fill(col::card, 8);
        card.outline(col::dim, 8);

        int cy = row.y + (ROW_H - CARD_G) / 2;
        fb.fillCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, it.color);
        fb.drawCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, col::black);

        char cnt[8];
        snprintf(cnt, sizeof cnt, "x%u", (unsigned)s.count);
        int cw = (int)strlen(cnt) * 12;
        gfx_text(card.x + card.w - cw - 8, cy - 8, 2, col::white, "%s", cnt);

        int tx = PAD_X + 8 + SWATCH_R * 2 + 10;
        int textW = (card.x + card.w) - tx - cw - 16;
        gfx_text_fit(tx, row.y + 6,  textW, 2, col::white, "%s", it.name);
        gfx_text_fit(tx, row.y + 25, textW, 1, col::dim,   "%s", it.desc);
    }
    list_.endClip();
    list_.drawScrollbar(rowCount_);

    draw_back();
}

void SceneMedicine::onInput(const Input& in)
{
    if (in.pressed && kBack.contains(in)) {
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }
    if (in.pressed && kShopTo.contains(in)) {
        app().shop.setReturn(SceneId::Medicine);
        app().setScene(SceneId::Shop, Slide::Forward);
        return;
    }

    list_.update(in, rowCount_);

    int row = list_.tapped();
    if (row < 0 || row >= rowCount_) return;

    const InvSlot& s = app().economy.slotAt(rows_[row]);
    const int ii = app().items.indexOf(s.id);
    if (ii < 0) return;
    const Item& it = app().items.at(ii);

    // Pet::treat voices its own refusal and consumes nothing when the dose cannot land, so
    // the item only leaves the bag on a true return. Doing it the other way round would
    // spend a remedy on a healthy creature.
    if (!app().pet.treat(it.treats, it.potency, it.health)) return;

    char id[24];
    strncpy(id, s.id, sizeof id - 1);          // `s` dangles once the stack is removed
    id[sizeof id - 1] = '\0';
    app().economy.take(id, 1);
    app().economy.flush();

    build();                                    // the stack may have just emptied
    app().setScene(SceneId::Home, Slide::Back); // treat and return, one gesture, like Feed
}
