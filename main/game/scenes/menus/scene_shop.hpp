#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"
#include "sim/economy.hpp"

// Shop and Bag, as two tabs of one screen. They are two views of the same thing -- what you
// can have and what you do have -- and pairing them means the menu grows by one entry rather
// than two, which matters on a 240x320 panel where the menu was already at its layout limit.
//
// NEVER GATED (docs/economy-and-inventory.md 12): this screen works while the pet is frozen,
// sick or asleep. It is the player's screen, not the creature's -- and once treatment is a
// priced item (E2) it is the screen a sick pet's owner most needs to reach.
//
// TWO PAGES, one scene. The list is a browsing view; tapping a row opens a DETAIL page for
// that entry. The detail page exists because a list row has ~120px left after the swatch and
// the price, which elides most 24-char names and nearly every 40-char description -- the list
// alone could not actually tell you what you were buying. Quantity lives there too: buying
// ten of something one tap at a time is not a purchase, it's a chore.
//
// The Bag shares that page (minus the buy controls), because its rows truncate just as badly.
class SceneShop : public Scene {
public:
    // Where Back goes. The Shop is reachable from the Menu and from either care picker, and
    // dumping a player who came from Feed back at the Menu strands them away from the pet.
    void setReturn(SceneId s) { returnTo_ = s; }

    void onEnter() override;
    void render() override;
    void onInput(const Input& in) override;

    // The list is built as a flat array of rows that INCLUDES its own group headings, rather
    // than the scene tracking groups separately and mapping indices around them. ListView
    // addresses rows by index, so anything that is not a row in the array is an index the
    // scroll, the hit-test and the detail page would each have to correct for independently
    // -- which is exactly how an off-by-one gets into one of the three and not the others.
    //
    // Public only so the .cpp's row-resolving helper can take one; nothing outside builds these.
    enum RowType : uint8_t { ROW_HEADING, ROW_STOCK, ROW_BAG };

    struct Row {
        uint8_t type;     // RowType
        uint8_t kind;     // ItemKind -- the group this row belongs to
        bool    isFood;   // ROW_STOCK: which registry `idx` indexes
        int16_t idx;      // ROW_STOCK: registry index. ROW_BAG: inventory slot.
    };

private:
    // Foods (32) + items (48) + a heading per kind. Sized to the registries so the list can
    // never be the thing that truncates a modded roster.
    static const int MAX_ROWS = 80 + ITEM_KIND_COUNT;
    // How many non-common lines are offered at once. The rest of that pool waits for
    // another day (see buildStock).
    static const int ROTATING = 3;
    // Quantity ceiling per purchase. Well above any sane single buy; it exists so a stuck
    // "+" (or a very rich player) cannot run the counter somewhere silly.
    static const int QTY_MAX = 99;

    ListView list_;
    Row      rows_[MAX_ROWS];
    int      rowCount_ = 0;
    int      tab_       = 0;      // 0 = Shop, 1 = Bag
    bool     detail_    = false;  // showing the detail page for detailRow_
    int      detailRow_ = -1;     // index into rows_
    int      qty_       = 1;      // quantity on the detail page
    float    flash_     = 0.0f;   // >0 = a purchase just landed (confirmation fades)
    SceneId  returnTo_  = SceneId::Menu;

    void buildRows();             // rebuild rows_ for the active tab (grouped + sorted)
    void buildStock();            // Shop tab: what is on the shelf today
    void buildBag();              // Bag tab: what is held
    void groupAndSort(int n);     // sort rows_[0..n) by group then name, then insert headings

    // --- variable row heights ------------------------------------------------------------
    // A heading needs a fraction of a card's height, and giving it a whole 46px slot put a
    // block of dead space above the very first group. ListView only does uniform rows, so it
    // is driven in PIXELS instead: geom() gets rowH = 1 and update()/drawScrollbar() get the
    // total pixel height as the "count". Its scroll, clamping and flick physics then work
    // unchanged, tapped() hands back a pixel offset, and this scene owns the row layout.
    int  rowH(int i) const;       // height of row i
    int  rowY(int i) const;       // content-space top of row i
    int  totalH() const;          // content height, in px
    int  rowAt(int contentY) const;  // row containing a content-space y, or -1

    void renderList();
    void renderDetail();
    void drawHeadingRow(int i, const Rect& row);
    void drawStockRow(int i, const Rect& row);
    void drawBagRow(int i, const Rect& row);

    void openDetail(int row);
    void closeDetail();
    int  maxQty();                // what the wallet (and the stack cap) actually allow
    void buyQty();
};
