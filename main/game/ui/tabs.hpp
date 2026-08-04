#pragma once
#include "engine/gfx.hpp"
#include <cstring>

// Reusable horizontal tab bar for paged screens. Draws `n` equal-width tabs across
// [x, x+w] (size-2 labels), highlighting the active one; tabbar_hit() maps a touch point
// to a tab index (or -1 if outside the bar). Header-only so any scene can use it.

inline void tabbar_draw(int x, int y, int w, int h, const char* const* labels, int n, int active)
{
    if (n <= 0) return;
    int seg = w / n;
    for (int i = 0; i < n; i++) {
        int tx = x + i * seg;
        int tw = (i == n - 1) ? (x + w - tx) : seg;   // last tab absorbs the rounding remainder
        int iw = tw - 4;                              // small gap between tabs
        bool on = (i == active);
        fb.fillRoundRect(tx, y, iw, h, 6, on ? col::accent : rgb565(52, 56, 72));
        int lw = (int)strlen(labels[i]) * 12;         // size-2 text is ~12px/char
        gfx_text(tx + (iw - lw) / 2, y + (h - 16) / 2, 2, on ? col::black : col::white, "%s", labels[i]);
    }
}

inline int tabbar_hit(int px, int py, int x, int y, int w, int h, int n)
{
    if (n <= 0 || py < y || py >= y + h || px < x || px >= x + w) return -1;
    int i = (px - x) / (w / n);
    return i >= n ? n - 1 : i;
}
