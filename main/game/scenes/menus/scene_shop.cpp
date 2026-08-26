#include "scene_shop.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/clock.hpp"          // clock_now: the day index the stock rotation is seeded from
#include "engine/audio/sfx.hpp"
#include "ui/tabs.hpp"
#include <cstring>
#include <cstdio>
#include <strings.h>                 // strcasecmp

// --- list layout -------------------------------------------------------------------------
// The wallet owns row 2. It cannot share the title row: at six digits the pill runs ~148px
// wide and kBack sits at x 168..228, so on a rich save they would collide.
static const int WALLET_Y = 46;
static const int TAB_Y = 78, TAB_H = 28, TAB_X = 12, TAB_W = GAME_W - 24;
static const int ROW_H  = 46;
// A heading is 8px of text plus breathing room -- 6px below it (so it hugs the group it
// labels) and, for every heading AFTER the first, 14px above it to separate it from the
// previous group. The first heading has nothing above it to separate from, so it gets the
// short version and the list starts flush against the tab bar.
static const int HEAD_H       = 28;
static const int HEAD_H_FIRST = 14;
static const int HEAD_TEXT_UP = 14;   // text sits this far above the row's bottom edge
static const int LIST_Y = TAB_Y + TAB_H + 8, LIST_B = 6;
static const int VIEW_H = GAME_H - LIST_Y - LIST_B;
static const int PAD_X  = 12;
static const int SWATCH_R = 13;
static const float FLASH_SECS = 0.9f;

// --- detail layout -----------------------------------------------------------------------
static const int D_SWATCH_R = 20;
static const int D_TOP      = 82;
static const int D_TEXT_X   = PAD_X + D_SWATCH_R * 2 + 12;
static const int D_DESC_Y   = 142;
static const int D_OWNED_Y  = 196;
static const Rect D_MINUS{ PAD_X,                 218, 46, 40 };
static const Rect D_PLUS { GAME_W - PAD_X - 46,   218, 46, 40 };
static const Rect D_BUY  { PAD_X,                 266, GAME_W - 2 * PAD_X, 40 };

static const char* const TABS[] = { "Shop", "Bag" };

// --- grouping ----------------------------------------------------------------------------
// The order groups appear in, which is deliberately NOT the ItemKind enum order: that is a
// storage detail, this is a reading order -- the things you buy most often first, keepsakes
// (which are never for sale) last.
static uint8_t kind_rank(uint8_t kind)
{
    switch (kind) {
        case ITEM_FOOD:     return 0;
        case ITEM_CARE:     return 1;
        case ITEM_TOY:      return 2;
        case ITEM_DECOR:    return 3;
        case ITEM_SPECIAL:  return 4;
        case ITEM_KEEPSAKE: return 5;
        default:            return 6;
    }
}

// Plural heading for a group ("Food" reads wrong as a heading over six of them).
static const char* kind_heading(uint8_t kind)
{
    switch (kind) {
        case ITEM_FOOD:     return "FOOD";
        case ITEM_CARE:     return "MEDICINE";
        case ITEM_TOY:      return "TOYS";
        case ITEM_DECOR:    return "DECOR";
        case ITEM_SPECIAL:  return "SPECIAL";
        case ITEM_KEEPSAKE: return "KEEPSAKES";
        default:            return "OTHER";
    }
}

// --- one resolver, so a row's registry lives in exactly one place -------------------------

namespace {

struct Line {
    const char* name;
    const char* desc;
    uint16_t    color;
    uint16_t    cost;
    uint8_t     kind;
    const char* id;
    const char* rarity;
};

Line line_of(App& app, bool isFood, int idx)
{
    if (isFood) {
        const Food& f = app.foods.at(idx);
        return Line{ f.name, f.desc, f.color, f.cost, ITEM_FOOD, f.id, f.rarity };
    }
    const Item& it = app.items.at(idx);
    return Line{ it.name, it.desc, it.color, it.cost, it.kind, it.id, it.rarity };
}

// Resolve a bag stack against the registries: items FIRST, then foods -- the same order the
// inventory itself uses, so a name can never come from the wrong table. If neither knows it,
// the mod that supplied it has been uninstalled: show the raw id rather than dropping the
// entry, because the player still owns it and it comes back if the mod does.
Line resolve_id(App& app, const char* id, uint8_t kind)
{
    int ii = app.items.indexOf(id);
    if (ii >= 0) return line_of(app, false, ii);
    int fi = app.foods.indexOf(id);
    if (fi >= 0) return line_of(app, true, fi);
    return Line{ id, "This item's mod is not installed.", col::dim, 0, kind, id, "common" };
}

}  // namespace

// The Line for any row, headings excluded (callers must not ask about those).
static Line row_line(App& app, const SceneShop::Row& r);

// --- construction ------------------------------------------------------------------------

void SceneShop::onEnter()
{
    list_.geom(0, LIST_Y, GAME_W, VIEW_H, 1);   // 1px 'rows': see rowH() in the header
    list_.reset();
    flash_ = 0.0f;
    closeDetail();
    buildRows();
}

void SceneShop::buildRows()
{
    rowCount_ = 0;
    if (tab_ == 0) buildStock(); else buildBag();
}

// Commons are always on the shelf; everything scarcer rotates. The rotation is seeded from
// the RTC DAY INDEX rather than stored, so it is identical across reboots, costs no NVS, and
// cannot be rerolled by power-cycling. A dead RTC yields day 0 -- still deterministic, just
// never changing, which is the right failure mode for a shop.
void SceneShop::buildStock()
{
    int  rare[MAX_ROWS];
    bool rareIsFood[MAX_ROWS];
    int  rareN = 0;
    int  n = 0;

    // Pass 1: commons (and anything with an unrecognised rarity -- an unknown word should
    // leave a mod's item BUYABLE rather than hiding it forever).
    for (int pass = 0; pass < 2; pass++) {
        const int cnt = pass == 0 ? app().foods.count() : app().items.count();
        for (int i = 0; i < cnt; i++) {
            Line l = line_of(app(), pass == 0, i);
            if (l.cost == 0) continue;                 // free (kibble) or not for sale
            bool scarce = strcasecmp(l.rarity, "uncommon") == 0
                       || strcasecmp(l.rarity, "rare") == 0;
            if (scarce) {
                if (rareN < MAX_ROWS) { rare[rareN] = i; rareIsFood[rareN] = (pass == 0); rareN++; }
            } else if (n < MAX_ROWS) {
                rows_[n++] = Row{ ROW_STOCK, l.kind, pass == 0, (int16_t)i };
            }
        }
    }

    // Pass 2: today's slice of the scarce pool.
    if (rareN > 0) {
        const uint32_t day = clock_now() / 86400u;
        const int take = rareN < ROTATING ? rareN : ROTATING;
        for (int k = 0; k < take && n < MAX_ROWS; k++) {
            int pick = (int)((day + (uint32_t)k) % (uint32_t)rareN);
            Line l = line_of(app(), rareIsFood[pick], rare[pick]);
            rows_[n++] = Row{ ROW_STOCK, l.kind, rareIsFood[pick], (int16_t)rare[pick] };
        }
    }

    groupAndSort(n);
}

void SceneShop::buildBag()
{
    int n = 0;
    const int slots = app().economy.slotCount();
    for (int i = 0; i < slots && n < MAX_ROWS; i++) {
        const InvSlot& s = app().economy.slotAt(i);
        rows_[n++] = Row{ ROW_BAG, s.kind, false, (int16_t)i };
    }
    groupAndSort(n);
}

// Sort by group, then by name inside it, then splice in a heading before each group. An
// insertion sort because n is tens at most and the alternative is dragging <algorithm> and a
// comparator into a scene for no measurable gain.
void SceneShop::groupAndSort(int n)
{
    for (int i = 1; i < n; i++) {
        Row key = rows_[i];
        Line kl = row_line(app(), key);
        int j = i - 1;
        while (j >= 0) {
            Line jl = row_line(app(), rows_[j]);
            const uint8_t kr = kind_rank(key.kind), jr = kind_rank(rows_[j].kind);
            const bool after = (jr < kr) || (jr == kr && strcasecmp(jl.name, kl.name) <= 0);
            if (after) break;
            rows_[j + 1] = rows_[j];
            j--;
        }
        rows_[j + 1] = key;
    }

    // Splice headings in, walking backwards so earlier indices stay valid as things shift.
    rowCount_ = n;
    for (int i = n - 1; i >= 0; i--) {
        const bool first = (i == 0) || (rows_[i - 1].kind != rows_[i].kind);
        if (!first || rowCount_ >= MAX_ROWS) continue;
        for (int k = rowCount_; k > i; k--) rows_[k] = rows_[k - 1];
        rows_[i] = Row{ ROW_HEADING, rows_[i + 1].kind, false, 0 };
        rowCount_++;
    }
}

static Line row_line(App& app, const SceneShop::Row& r)
{
    if (r.type == SceneShop::ROW_BAG) {
        const InvSlot& s = app.economy.slotAt(r.idx);
        return resolve_id(app, s.id, s.kind);
    }
    return line_of(app, r.isFood, r.idx);
}

// --- row geometry ------------------------------------------------------------------------

int SceneShop::rowH(int i) const
{
    if (rows_[i].type != ROW_HEADING) return ROW_H;
    return (i == 0) ? HEAD_H_FIRST : HEAD_H;
}

int SceneShop::rowY(int i) const
{
    int y = 0;
    for (int k = 0; k < i; k++) y += rowH(k);
    return y;
}

int SceneShop::totalH() const
{
    int y = 0;
    for (int k = 0; k < rowCount_; k++) y += rowH(k);
    return y;
}

int SceneShop::rowAt(int contentY) const
{
    if (contentY < 0) return -1;
    int y = 0;
    for (int k = 0; k < rowCount_; k++) {
        const int h = rowH(k);
        if (contentY < y + h) return k;
        y += h;
    }
    return -1;
}

// --- list page ---------------------------------------------------------------------------

void SceneShop::drawHeadingRow(int i, const Rect& row)
{
    // Deliberately light: a heading is a separator, not a card. It sits low in its row so it
    // reads as attached to the group beneath it rather than floating between two.
    const char* h = kind_heading(rows_[i].kind);
    const int ty = row.y + rowH(i) - HEAD_TEXT_UP;
    gfx_text(PAD_X, ty, 1, col::accent, "%s", h);
    const int lx = PAD_X + (int)strlen(h) * 6 + 8;
    fb.drawFastHLine(lx, ty + 3, GAME_W - PAD_X - lx, rgb565(60, 64, 80));
}

void SceneShop::drawStockRow(int i, const Rect& row)
{
    Line l = line_of(app(), rows_[i].isFood, rows_[i].idx);
    const bool afford = app().economy.canAfford(l.cost);

    Rect card{ PAD_X, row.y, GAME_W - 2 * PAD_X, ROW_H - 6 };
    card.fill(col::card, 8);
    card.outline(col::dim, 8);

    int cy = row.y + (ROW_H - 6) / 2;
    fb.fillCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, l.color);
    fb.drawCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, col::black);

    char price[16];
    snprintf(price, sizeof price, "%u", (unsigned)l.cost);
    int pw = (int)strlen(price) * 12;
    gfx_text(card.x + card.w - pw - 8, row.y + 6, 2,
             afford ? col::accent : col::dim, "%s", price);

    int held = app().economy.count(l.id);
    if (held > 0) {
        char have[16];
        snprintf(have, sizeof have, "have %d", held);
        int hw = (int)strlen(have) * 6;
        gfx_text(card.x + card.w - hw - 8, row.y + 27, 1, col::dim, "%s", have);
    }

    // Name and desc are still elided here -- that is what the detail page is for.
    int tx = PAD_X + 8 + SWATCH_R * 2 + 10;
    int textW = (card.x + card.w) - tx - pw - 16;
    gfx_text_fit(tx, row.y + 6,  textW, 2, afford ? col::white : col::dim, "%s", l.name);
    gfx_text_fit(tx, row.y + 25, textW, 1, col::dim, "%s", l.desc);
}

void SceneShop::drawBagRow(int i, const Rect& row)
{
    const InvSlot& s = app().economy.slotAt(rows_[i].idx);
    Line l = resolve_id(app(), s.id, s.kind);

    Rect card{ PAD_X, row.y, GAME_W - 2 * PAD_X, ROW_H - 6 };
    card.fill(col::card, 8);
    card.outline(col::dim, 8);

    int cy = row.y + (ROW_H - 6) / 2;
    fb.fillCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, l.color);
    fb.drawCircle(PAD_X + 8 + SWATCH_R, cy, SWATCH_R, col::black);

    char cnt[12];
    snprintf(cnt, sizeof cnt, "x%u", (unsigned)s.count);
    int cw = (int)strlen(cnt) * 12;
    gfx_text(card.x + card.w - cw - 8, cy - 8, 2, col::white, "%s", cnt);

    // The kind is the group heading now, so the row doesn't repeat it.
    int tx = PAD_X + 8 + SWATCH_R * 2 + 10;
    int textW = (card.x + card.w) - tx - cw - 16;
    gfx_text_fit(tx, row.y + 6,  textW, 2, col::white, "%s", l.name);
    gfx_text_fit(tx, row.y + 25, textW, 1, col::dim,   "%s", l.desc);
}

void SceneShop::renderList()
{
    tabbar_draw(TAB_X, TAB_Y, TAB_W, TAB_H, TABS, 2, tab_);

    if (rowCount_ == 0) {
        const char* msg = (tab_ == 0) ? "Nothing for sale today" : "Your bag is empty";
        int mw = (int)strlen(msg) * 6;
        gfx_text((GAME_W - mw) / 2, LIST_Y + 40, 1, col::dim, "%s", msg);
        return;
    }

    const int scroll = (int)list_.scroll;
    list_.beginClip();
    for (int i = 0; i < rowCount_; i++) {
        const int y = LIST_Y + rowY(i) - scroll;
        if (y + rowH(i) <= LIST_Y) continue;          // fully above the viewport
        if (y >= LIST_Y + VIEW_H) break;              // ...and everything after is below it
        Rect row{ 0, y, GAME_W, rowH(i) };
        switch (rows_[i].type) {
            case ROW_HEADING: drawHeadingRow(i, row); break;
            case ROW_STOCK:   drawStockRow(i, row);   break;
            default:          drawBagRow(i, row);     break;
        }
    }
    list_.endClip();
    list_.drawScrollbar(totalH());
}

// --- detail page -------------------------------------------------------------------------

int SceneShop::maxQty()
{
    if (tab_ != 0 || detailRow_ < 0 || detailRow_ >= rowCount_) return 1;
    Line l = row_line(app(), rows_[detailRow_]);
    if (l.cost == 0) return 1;
    uint32_t affordable = app().economy.bits() / l.cost;
    if (affordable < 1) return 1;                 // "1" stays selectable so BUY can refuse
    return affordable > QTY_MAX ? QTY_MAX : (int)affordable;
}

void SceneShop::renderDetail()
{
    const bool shopping = (rows_[detailRow_].type == ROW_STOCK);
    Line l = row_line(app(), rows_[detailRow_]);

    // Swatch + name. The name WRAPS to two lines at size 2 rather than being elided: on the
    // detail page there is no excuse for still not showing what the thing is called.
    fb.fillCircle(PAD_X + D_SWATCH_R, D_TOP + D_SWATCH_R, D_SWATCH_R, l.color);
    fb.drawCircle(PAD_X + D_SWATCH_R, D_TOP + D_SWATCH_R, D_SWATCH_R, col::black);

    const int textW = GAME_W - D_TEXT_X - PAD_X;
    gfx_text_wrap(D_TEXT_X, D_TOP, textW, 2, col::white, l.name, 2, -1, 2);

    char sub[48];
    if (shopping) snprintf(sub, sizeof sub, "%s   %s", kind_heading(l.kind), l.rarity);
    else          snprintf(sub, sizeof sub, "%s", kind_heading(l.kind));
    gfx_text(D_TEXT_X, D_TOP + 42, 1, col::dim, "%s", sub);

    // Full description, full width, wrapped. This is the whole reason the page exists.
    gfx_text_wrap(PAD_X, D_DESC_Y, GAME_W - 2 * PAD_X, 1, col::white, l.desc, 4, -1, 3);

    if (!shopping) {
        gfx_text(PAD_X, D_OWNED_Y, 2, col::white, "You have %u",
                 (unsigned)app().economy.slotAt(rows_[detailRow_].idx).count);
        gfx_text_wrap(PAD_X, D_OWNED_Y + 28, GAME_W - 2 * PAD_X, 1, col::dim,
                      "Using items arrives with the kinds that need it.", 2, -1, 2);
        return;
    }

    gfx_text(PAD_X, D_OWNED_Y, 1, col::dim, "In bag: %d", app().economy.count(l.id));

    // --- quantity ---
    const int mq = maxQty();
    const bool canLess = qty_ > 1;
    const bool canMore = qty_ < mq;
    D_MINUS.button("-", canLess ? col::accent : rgb565(56, 60, 74),
                        canLess ? col::black  : col::dim, 3);
    D_PLUS .button("+", canMore ? col::accent : rgb565(56, 60, 74),
                        canMore ? col::black  : col::dim, 3);

    char q[8];
    snprintf(q, sizeof q, "x%d", qty_);
    int qw = (int)strlen(q) * 18;
    gfx_text((GAME_W - qw) / 2, D_MINUS.y + 8, 3, col::white, "%s", q);

    // --- total + buy ---
    const uint32_t total = (uint32_t)l.cost * (uint32_t)qty_;
    const bool afford = app().economy.canAfford(total);

    if (flash_ > 0) {
        D_BUY.button("BOUGHT", col::good, col::black, 2);
    } else if (afford) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, "BUY  %u", (unsigned)total);
        D_BUY.button(lbl, col::accent, col::black, 2);
    } else {
        D_BUY.button("NOT ENOUGH BITS", rgb565(56, 60, 74), col::dim, 1);
    }
}

void SceneShop::openDetail(int row)
{
    if (rows_[row].type == ROW_HEADING) { sfx::play(sfx::kTap); return; }
    detail_ = true;
    detailRow_ = row;
    qty_ = 1;
    flash_ = 0.0f;
    sfx::play(sfx::kTap);
}

void SceneShop::closeDetail()
{
    detail_ = false;
    detailRow_ = -1;
    qty_ = 1;
}

void SceneShop::buyQty()
{
    Line l = row_line(app(), rows_[detailRow_]);
    if (!app().economy.buy(l.id, l.kind, l.cost, qty_)) {
        sfx::play(sfx::kDenied);                 // can't afford it, or the bag is full
        return;
    }
    app().economy.flush();                       // a purchase is rare and deliberate: persist now
    sfx::play(sfx::kSelect);
    flash_ = FLASH_SECS;
    // Back to one, so a second deliberate tap cannot repeat a ten-item purchase by accident.
    qty_ = 1;
}

// --- shared ------------------------------------------------------------------------------

void SceneShop::render()
{
    if (flash_ > 0) flash_ -= 1.0f / 60.0f;      // render-rate fade; precision is irrelevant here

    fb.fillScreen(col::panel);
    gfx_text(PAD_X, 18, 3, col::accent, "%s", tab_ == 0 ? "SHOP" : "BAG");

    // The wallet appears HERE and on the Feed picker, never on Home: the care screen stays
    // about the creature (docs/economy-and-inventory.md 2).
    draw_wallet(GAME_W - PAD_X, WALLET_Y, app().economy.bits());

    if (detail_ && detailRow_ >= 0 && detailRow_ < rowCount_) renderDetail();
    else                                                     renderList();

    draw_back();
}

void SceneShop::onInput(const Input& in)
{
    // Back sits above the viewport, so a scroll gesture can never swallow it. On the detail
    // page it returns to the list rather than leaving the shop -- one Back, one step.
    if (in.pressed && kBack.contains(in)) {
        if (detail_) { closeDetail(); sfx::play(sfx::kBack); }
        else         app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    if (detail_) {
        // The row list is rebuilt on tab switches and re-entry; if anything ever leaves the
        // detail page pointing past its end, fall back to the list rather than reading off
        // the array.
        if (detailRow_ < 0 || detailRow_ >= rowCount_) { closeDetail(); return; }
        if (!in.pressed) return;
        if (rows_[detailRow_].type != ROW_STOCK) return;   // the Bag detail is read-only in E1

        if (D_MINUS.contains(in)) {
            if (qty_ > 1) { qty_--; sfx::play(sfx::kTap); } else sfx::play(sfx::kDenied);
            return;
        }
        if (D_PLUS.contains(in)) {
            if (qty_ < maxQty()) { qty_++; sfx::play(sfx::kTap); } else sfx::play(sfx::kDenied);
            return;
        }
        if (D_BUY.contains(in)) { buyQty(); return; }
        return;
    }

    if (in.pressed) {
        int t = tabbar_hit(in.x, in.y, TAB_X, TAB_Y, TAB_W, TAB_H, 2);
        if (t >= 0) {
            if (t != tab_) { tab_ = t; list_.reset(); buildRows(); sfx::play(sfx::kTap); }
            return;
        }
    }

    list_.update(in, totalH());

    // tapped() is a pixel offset here (rowH is 1), so it maps through rowAt() rather than
    // being a row index already.
    int px = list_.tapped();
    if (px < 0) return;
    int row = rowAt(px);
    if (row >= 0) openDetail(row);
}
