#pragma once
#include "engine/gfx.hpp"
#include <cstring>

// Reusable horizontal tab bar for paged screens. Draws `n` equal-width tabs across
// [x, x+w], highlighting the active one; tabbar_hit() maps a touch point to a tab index
// (or -1 if outside the bar). Header-only so any scene can use it.
//
// `size` is the label text size. It stays 2 by default, but a bar with four tabs on a
// 240px screen has ~50px per tab, which a five-letter label overruns at size 2 -- those
// callers pass 1 for the whole bar rather than letting one label spill over its pill.

inline void tabbar_draw(int x, int y, int w, int h, const char* const* labels, int n, int active,
                        int size = 2)
{
    if (n <= 0) return;
    int seg = w / n;
    for (int i = 0; i < n; i++) {
        int tx = x + i * seg;
        int tw = (i == n - 1) ? (x + w - tx) : seg;   // last tab absorbs the rounding remainder
        int iw = tw - 4;                              // small gap between tabs
        bool on = (i == active);
        fb.fillRoundRect(tx, y, iw, h, 6, on ? col::accent : rgb565(52, 56, 72));
        const int cw = size * 6, ch = size * 8;      // the default font cell, scaled
        int lw = (int)strlen(labels[i]) * cw;
        gfx_text(tx + (iw - lw) / 2, y + (h - ch) / 2, size, on ? col::black : col::white, "%s", labels[i]);
    }
}

inline int tabbar_hit(int px, int py, int x, int y, int w, int h, int n)
{
    if (n <= 0 || py < y || py >= y + h || px < x || px >= x + w) return -1;
    int i = (px - x) / (w / n);
    return i >= n ? n - 1 : i;
}
