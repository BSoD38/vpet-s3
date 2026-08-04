#pragma once
#include "engine/gfx.hpp"
#include <cstring>
#include <cstdarg>
#include <cstdio>

// Shared UI for the training minigames (Run / Mind Maze / Smash / Bulwark / Stance). Every
// one of them shows the same READY-screen stamina line and the same game-over result card;
// these header-only helpers hold that layout in ONE place so each scene only supplies its
// own text. Same spirit as widgets.hpp / tabs.hpp: stateless, rebuilt each frame.

// Standardized game-over card geometry (centered panel with a warn border).
static constexpr int MG_CARD_X = 28, MG_CARD_Y = 108, MG_CARD_W = GAME_W - 56, MG_CARD_H = 100;

// READY-screen stamina line: "Energy N/100" (turns warn + "(tired!)" below 25).
inline void mg_energy_readout(int x, int y, int energy) {
    gfx_text(x, y, 1, energy < 25 ? col::warn : col::dim,
             "Energy %d/100%s", energy, energy < 25 ? "  (tired!)" : "");
}

// A printf-style line horizontally centered within the game-over card.
inline void mg_center(int y, int size, uint16_t c, const char* fmt, ...) {
    char buf[48];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int tw = (int)strlen(buf) * size * 6;   // font cell is size*6 px wide
    gfx_text(MG_CARD_X + (MG_CARD_W - tw) / 2, y, size, c, "%s", buf);
}

// Draw the game-over card: panel + warn border + centered title + the "Tap to exit" footer.
// Callers then add their score/gains lines with mg_center() at MG_CARD_Y + 40 / + 68.
inline void mg_over_card(const char* title) {
    fb.fillRoundRect(MG_CARD_X, MG_CARD_Y, MG_CARD_W, MG_CARD_H, 8, col::panel);
    fb.drawRoundRect(MG_CARD_X, MG_CARD_Y, MG_CARD_W, MG_CARD_H, 8, col::warn);
    mg_center(MG_CARD_Y + 14, 2, col::warn, "%s", title);
    mg_center(MG_CARD_Y + 84, 1, col::dim,  "Tap to exit");
}
