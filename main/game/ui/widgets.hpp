#pragma once
#include "engine/gfx.hpp"
#include "engine/input.hpp"
#include "esp_timer.h"   // ListView measures its own frame dt for flick physics
#include <cstring>
#include <cstdlib>
#include <cstdio>

// Immediate-mode UI helpers. A Rect is just geometry (x,y,w,h) -- no lifecycle, no heap,
// no scene graph. It unifies the two things every on-screen control needs: somewhere to
// draw and something to hit-test, so a button's geometry lives in ONE place instead of
// being copy-pasted between render() and onInput(). Same spirit as tabs.hpp: header-only,
// stateless, rebuilt each frame.
struct Rect {
    int x, y, w, h;

    // --- hit-testing --------------------------------------------------------------------
    bool contains(int px, int py) const { return hit(px, py, x, y, w, h); }
    bool contains(const Input& in) const { return hit(in.x, in.y, x, y, w, h); }
    bool tapped(const Input& in)   const { return in.pressed && contains(in); }  // pressed this frame, inside

    // --- drawing ------------------------------------------------------------------------
    void fill(uint16_t bg, int radius = 6)    const { fb.fillRoundRect(x, y, w, h, radius, bg); }
    void outline(uint16_t c, int radius = 6)  const { fb.drawRoundRect(x, y, w, h, radius, c); }

    // Filled rounded button with a horizontally + vertically centered label. The default
    // font cell is ~6x8 px at size 1, so it scales linearly with `size`.
    void button(const char* label, uint16_t bg, uint16_t fg, int size = 2, int radius = 6) const {
        fill(bg, radius);
        int cw = size * 6, ch = size * 8;
        int lw = (int)strlen(label) * cw;
        gfx_text(x + (w - lw) / 2, y + (h - ch) / 2, size, fg, "%s", label);
    }
};

// The care-freeze accent (Pet::setFrozen). Shared for the same reason care_tier_color() is:
// the Home pause marker and caption, the Menu lock hint, the Stats note and the Settings
// toggle all describe ONE state, and must not drift into four slightly different blues.
inline constexpr uint16_t kFrozenCol = rgb565(150, 205, 240);

// Colour for a coarse care tier (0..4) from sim/pet.hpp's care_tier(). Lives here rather
// than in the sim so the simulation layer stays free of the renderer's palette.
inline uint16_t care_tier_color(int tier) {
    switch (tier) {
        case 0:  return col::warn;
        case 1:  return rgb565(235, 150, 60);
        case 2:  return rgb565(230, 210, 90);
        default: return col::good;
    }
}

// The Bits readout, right-aligned so it ends at `rightX`. Shared for the same reason
// care_tier_color() and kFrozenCol are: the Shop header and the Feed picker (which can buy
// inline) describe ONE number, and a player who learns where to find it on one screen must
// not have to hunt for it on the other.
//
// Drawn as a filled pill at size 2 with the word spelled out. The first version was a bare
// size-1 "0 B" tucked in the corner and it read as decoration -- on a 240px panel the wallet
// has to look like a control, not a footnote.
// The size argument lets it be a headline on the Shop (which has a row to spare) and a
// footnote on the Feed picker (which does not) -- same pill, colours and wording either way.
//
// MIND THE RIGHT EDGE: at six digits this is ~148px at size 2, while kBack occupies
// x 168..228. They cannot share a row, and the overlap would only ever have shown up on a
// save rich enough to need the extra digits. The Shop gives it its own row for that reason.
inline void draw_wallet(int rightX, int y, uint32_t bits, int size = 2) {
    char buf[20];
    snprintf(buf, sizeof buf, "%u Bits", (unsigned)bits);
    const int cw = size * 6, ch = size * 8;
    const int w = (int)strlen(buf) * cw + 16, h = ch + 8;
    const int x = rightX - w;
    const uint16_t fg = bits ? col::good : col::dim;
    fb.fillRoundRect(x, y, w, h, h / 2, rgb565(40, 44, 58));
    fb.drawRoundRect(x, y, w, h, h / 2, fg);
    gfx_text(x + 8, y + (h - ch) / 2, size, fg, "%s", buf);
}

// The standard top-right "Back" button, identical across the menu scenes.
inline constexpr Rect kBack{ GAME_W - 72, 12, 60, 30 };
inline void draw_back() { kBack.button("Back", col::accent, col::black); }

// "Shop", sitting immediately left of Back. The care pickers (Feed, Heal) show prices and a
// wallet but deliberately do NOT sell -- a picker picks, the shop buys -- so each one needs a
// one-tap way to go and fix an empty shelf. Shared geometry so it lands in the same place on
// every screen that offers it; x 100..160 clears both kBack and a 4-letter size-3 title.
inline constexpr Rect kShopTo{ GAME_W - 140, 12, 60, 30 };
inline void draw_shop_to() { kShopTo.button("Shop", rgb565(74, 92, 132), col::white); }

// A drag-scrollable vertical list of uniform rows, for content whose length isn't known at
// design time (conversation history, a growing minigame roster, a creature list). Same
// immediate-mode spirit as Rect -- the list owns only a scroll offset and drag bookkeeping;
// the SCENE holds the instance as a member (so the offset survives across frames) and draws
// the rows itself. Rows are addressed by index; nothing about their content lives in here.
//
// Per frame:  list.geom(...); list.update(in, count);
//             list.beginClip(); for (i = list.first(); i <= list.last(count); ++i) { ... }
//             list.endClip(); list.drawScrollbar(count);
//             if (int t = list.tapped(); t >= 0) { ... }
// On scene entry: list.reset() (clears scroll AND any still-coasting flick).
struct ListView {
    // Layout, refreshed each frame by the scene.
    int   x = 0, y = 0, w = GAME_W, h = 100;
    int   rowH = 40;

    // Persistent state.
    float scroll = 0;          // px scrolled down from the top

    // A press only counts as a row tap if the finger never travelled far enough to be a drag.
    static constexpr int   DRAG_SLOP = 6;      // px before a press becomes a scroll gesture
    static constexpr float FRICTION  = 9.0f;   // flick decay (higher = stops sooner)

    void geom(int x_, int y_, int w_, int h_, int rowH_) {
        x = x_; y = y_; w = w_; h = h_; rowH = rowH_;
    }

    // Scene entry / tab switch: back to the top with ALL transient input state cleared.
    // (scroll = 0 alone left a previous visit's flick velocity coasting, so the fresh
    // list visibly scrolled itself away from the top.)
    void reset() {
        scroll = 0; vel_ = 0;
        held_ = dragging_ = tapCancel_ = false;
        tapped_ = -1;
        lastUs_ = 0;
    }

    float maxScroll(int count) const {
        float m = (float)(count * rowH - h);
        return m > 0 ? m : 0;
    }

    // Advance drag/flick physics and latch a row tap. `count` = current row count.
    // Frame dt is measured internally, so scenes don't have to stash update()'s dt
    // just to feed the scroll physics.
    void update(const Input& in, int count) {
        const int64_t now = esp_timer_get_time();
        float dt = lastUs_ ? (float)(now - lastUs_) / 1e6f : 0.0f;
        if (dt > 0.1f) dt = 0.1f;                     // clamp gaps (scene was away)
        lastUs_ = now;

        tapped_ = -1;
        const bool inside = in.x >= x && in.x < x + w && in.y >= y && in.y < y + h;

        if (in.pressed && inside) {
            held_ = true; dragging_ = false;
            // A press that lands on a COASTING list is a stop, not a choice: without this,
            // tapping to halt a flick instantly activated whatever row slid under the finger.
            tapCancel_ = (vel_ != 0);
            grabX_ = in.x; grabY_ = in.y; grabScroll_ = scroll; lastY_ = in.y; vel_ = 0;
        } else if (in.down && held_) {
            if (!dragging_ && abs(in.y - grabY_) > DRAG_SLOP) dragging_ = true;
            // Sideways travel is no scroll gesture, but it isn't a tap either -- a
            // horizontal swipe across the list must not activate the row it ends on.
            if (abs(in.x - grabX_) > DRAG_SLOP) tapCancel_ = true;
            if (dragging_) {
                scroll = grabScroll_ - (float)(in.y - grabY_);
                if (dt > 0) vel_ = -(float)(in.y - lastY_) / dt;   // px/s, for the release flick
            }
            lastY_ = in.y;
        } else if (in.released && held_) {
            if (!dragging_ && !tapCancel_ && inside) {
                int row = (int)((in.y - y + scroll) / rowH);
                if (row >= 0 && row < count) tapped_ = row;
            }
            held_ = false; dragging_ = false; tapCancel_ = false;
        }

        if (!held_ && vel_ != 0) {                    // coast after a flick
            scroll += vel_ * dt;
            vel_ -= vel_ * (dt * FRICTION < 1.0f ? dt * FRICTION : 1.0f);
            if (vel_ > -8 && vel_ < 8) vel_ = 0;
        }

        const float m = maxScroll(count);             // hard clamp (no rubber-banding)
        if (scroll > m) { scroll = m; vel_ = 0; }
        if (scroll < 0) { scroll = 0; vel_ = 0; }
    }

    int  tapped()   const { return tapped_; }         // row tapped this frame, or -1
    bool dragging() const { return dragging_; }

    // Visible row range (clamped); draw with rowRect(i) and let the clip cut the edges.
    int first() const { int f = (int)(scroll / rowH); return f > 0 ? f : 0; }
    int last(int count) const {
        int l = (int)((scroll + h - 1) / rowH);
        return l >= count ? count - 1 : l;
    }
    Rect rowRect(int i) const { return Rect{ x, y + i * rowH - (int)scroll, w, rowH }; }

    void beginClip() const { fb.setClipRect(x, y, w, h); }
    void endClip()   const { fb.clearClipRect(); }

    // Thin right-edge indicator; drawn only when the content actually overflows. Takes the
    // count explicitly (like last(count)) -- a cached copy went stale whenever a scene
    // rendered before its first update() ran (e.g. during the entry slide).
    void drawScrollbar(int count, uint16_t c = col::dim, int barW = 3) const {
        const float m = maxScroll(count);
        if (m <= 0) return;
        int trackH = h, knobH = (int)((float)h / (count * rowH) * trackH);
        if (knobH < 12) knobH = 12;
        int knobY = y + (int)((scroll / m) * (trackH - knobH));
        fb.fillRoundRect(x + w - barW - 1, knobY, barW, knobH, barW / 2, c);
    }

private:
    bool    held_ = false, dragging_ = false;
    bool    tapCancel_ = false;   // gesture disqualified from being a tap (coast-stop / sideways)
    int     grabX_ = 0, grabY_ = 0, lastY_ = 0;
    float   grabScroll_ = 0, vel_ = 0;
    int     tapped_ = -1;
    int64_t lastUs_ = 0;          // esp_timer timestamp of the previous update()
};
