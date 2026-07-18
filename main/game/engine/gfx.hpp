#pragma once
#include <cstdint>
#include "display.hpp"   // LGFX, display, LGFX_Sprite

// Logical game resolution (matches the panel, portrait).
static constexpr int GAME_W = 240;
static constexpr int GAME_H = 320;

// RGB565 helper (same formula LovyanGFX uses; verified on-screen with the current
// display config, so colors come out correct without any swap).
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Palette
namespace col {
    static constexpr uint16_t black  = rgb565(15, 15, 20);
    static constexpr uint16_t white  = rgb565(245, 245, 250);
    static constexpr uint16_t sky    = rgb565(90, 170, 235);
    static constexpr uint16_t ground = rgb565(95, 75, 50);
    static constexpr uint16_t grass  = rgb565(70, 180, 95);
    static constexpr uint16_t panel  = rgb565(30, 34, 52);
    static constexpr uint16_t accent = rgb565(250, 190, 60);
    static constexpr uint16_t good   = rgb565(90, 210, 110);
    static constexpr uint16_t warn   = rgb565(235, 90, 80);
    static constexpr uint16_t dim    = rgb565(120, 125, 140);
}

// Transparent color key (magenta) shared by all sprite blitting: sprites are drawn on a
// fillScreen(SPRITE_TRANSP) canvas and these pixels blit as clear. PNG creatures rely on
// it (sim/creatures.cpp); baked fallback bitmaps use it too (assets/sprites.hpp).
static constexpr uint16_t SPRITE_TRANSP = 0xF81F;

// A blittable sprite (RGB565 with a transparent color key).
struct Sprite {
    const uint16_t* data;
    int16_t w, h;
    uint16_t transp;
};

// Shared full-screen back-buffer. Scenes draw into `fb`, then game loop presents it.
extern LGFX_Sprite fb;

void gfx_init();                                        // create back-buffer (after Display_Init)
void gfx_present();                                     // push back-buffer to the panel
void gfx_snapshot();                                    // copy fb -> transition buffer (freeze outgoing scene)
void gfx_present_cover(int fbX, uint16_t gapColor);     // old base; new (fb) covers/overshoots in from right
void gfx_present_reveal(int snapX, uint16_t gapColor);  // new (fb) base; old (snap) slides off (anticipation)
void gfx_present_iris(bool useSnap, int radius);        // circular iris on snap (old) or fb (new)
void gfx_text(int x, int y, uint8_t size, uint16_t color, const char* fmt, ...);
void gfx_bar(int x, int y, int w, int h, float frac,    // stat bar (0..1)
             uint16_t fg, uint16_t bg, uint16_t border);
void gfx_blit(const Sprite& s, int cx, int cy);         // draw sprite centered at (cx,cy)
void gfx_blit_sprite(LGFX_Sprite* s, int cx, int cy, uint16_t transp);           // PSRAM sprite, centered
void gfx_blit_sprite_bottom(LGFX_Sprite* s, int cx, int bottomY, uint16_t transp);  // anchored by its feet (bottom-center)
void gfx_blit_sprite_fit(LGFX_Sprite* s, int cx, int cy, int maxW, int maxH, uint16_t transp);  // scaled to fit a box, centered
void gfx_tile_region(int x, int y, int w, int h, const uint16_t* tile,   // fill a rect by repeating an opaque
                     int tw, int th, int scrollX = 0);                   // tile; scrollX shifts it left (scrolling ground)
