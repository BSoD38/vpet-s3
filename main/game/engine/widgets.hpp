#pragma once
#include "engine/gfx.hpp"
#include "engine/input.hpp"
#include <cstring>

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

// The standard top-right "Back" button, identical across the menu scenes.
inline constexpr Rect kBack{ GAME_W - 72, 12, 60, 30 };
inline void draw_back() { kBack.button("Back", col::accent, col::black); }
