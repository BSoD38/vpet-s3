#pragma once
#include <cstdint>
#include "display.hpp"   // LGFX, display, LGFX_Sprite
#include "camera.hpp"    // Camera2D (pure math; used by the *_world draw variants below)

// Logical game resolution (matches the panel, portrait).
static constexpr int GAME_W = 240;
static constexpr int GAME_H = 320;

// RGB565 helper (same formula LovyanGFX uses; verified on-screen with the current
// display config, so colors come out correct without any swap).
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Blend two RGB565 colors channel-wise: t=0 -> a, t=1 -> b (t is clamped).
uint16_t mix565(uint16_t a, uint16_t b, float t);

// Palette
namespace col {
    static constexpr uint16_t black  = rgb565(15, 15, 20);
    static constexpr uint16_t white  = rgb565(245, 245, 250);
    static constexpr uint16_t sky    = rgb565(90, 170, 235);
    static constexpr uint16_t ground = rgb565(95, 75, 50);
    static constexpr uint16_t grass  = rgb565(70, 180, 95);
    static constexpr uint16_t panel  = rgb565(30, 34, 52);
    static constexpr uint16_t card   = rgb565(52, 58, 84);   // raised card on a panel background
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
// Darken the WHOLE frame toward black: k=0 leaves it, k>=1 blacks it. The panel stack has
// no alpha compositing, so this is the engine's substitute for a fade overlay -- a
// per-channel multiply over the back-buffer (a few ms; fine for a fade's worth of frames).
// Call at the END of a scene's render, after everything else is drawn.
void gfx_fade(float k);
void gfx_present_cover(int fbX, uint16_t gapColor);     // old base; new (fb) covers/overshoots in from right
void gfx_present_reveal(int snapX, uint16_t gapColor);  // new (fb) base; old (snap) slides off (anticipation)
void gfx_present_iris(bool useSnap, int radius);        // circular iris on snap (old) or fb (new)
void gfx_text(int x, int y, uint8_t size, uint16_t color, const char* fmt, ...);

// Single-line text truncated to fit `maxW` px, ending in ".." when it doesn't. Use this for
// any DATA-DRIVEN string drawn inside a fixed box (food names/descriptions, creature names,
// mod content): the author's text length isn't known at layout time, and it must never spill
// past its card. Nested clip rects aren't an option here (LovyanGFX setClipRect replaces
// rather than intersects, so it would break an enclosing ListView clip).
void gfx_text_fit(int x, int y, int maxW, uint8_t size, uint16_t color, const char* fmt, ...);

// --- Word-wrapped text ------------------------------------------------------------------
// Greedy word wrap into a box `w` px wide (the default font cell is 6x8 at size 1, so both
// axes scale linearly with `size`). Breaks at spaces, honours explicit '\n', and hard-breaks
// any single word longer than the line. `lineGap` is extra spacing between baselines.
//
// `reveal` caps how many source characters are drawn, which is all a typewriter effect needs:
// advance a float counter and pass it in (-1 = draw everything). The return value is always
// the FULL line count regardless of `reveal`, so layout stays put while text types itself out.
// `maxLines` (0 = unlimited) caps the block's height and elides the last line with ".." when
// text remains. Essential for DATA-DRIVEN text in a fixed box: without it a long modded string
// grows the block until it overlaps whatever sits below. Pass the same cap to the measure
// calls so layout and rendering agree.
int gfx_text_wrap(int x, int y, int w, uint8_t size, uint16_t color, const char* s,
                  int lineGap = 2, int reveal = -1, int maxLines = 0);
int gfx_text_wrap_lines(int w, uint8_t size, const char* s, int maxLines = 0);
int gfx_text_wrap_height(int w, uint8_t size, const char* s, int lineGap = 2, int maxLines = 0);
void gfx_bar(int x, int y, int w, int h, float frac,    // stat bar (0..1)
             uint16_t fg, uint16_t bg, uint16_t border);
// `mirror` flips the sprite left-to-right (creature facing, and poses like the DMC
// refusal head-shake that animate by alternating flipped/unflipped).
void gfx_blit(const Sprite& s, int cx, int cy);         // draw sprite centered at (cx,cy)
void gfx_blit_sprite(LGFX_Sprite* s, int cx, int cy, uint16_t transp, bool mirror = false);           // PSRAM sprite, centered
void gfx_blit_sprite_bottom(LGFX_Sprite* s, int cx, int bottomY, uint16_t transp, bool mirror = false);  // anchored by its feet (bottom-center)
void gfx_blit_sprite_fit(LGFX_Sprite* s, int cx, int cy, int maxW, int maxH, uint16_t transp, bool mirror = false);  // scaled to fit a box, centered
void gfx_blit_sprite_fit_bottom(LGFX_Sprite* s, int cx, int bottomY, int maxW, int maxH, uint16_t transp, bool mirror = false);  // feet-anchored + scaled DOWN to fit
void gfx_invalidate_scaled(const void* src);           // drop cached scaled copies of src; call BEFORE freeing src
void gfx_tile_region(int x, int y, int w, int h, const uint16_t* tile,   // fill a rect by repeating an opaque
                     int tw, int th, int scrollX = 0);                   // tile; scrollX shifts it left (scrolling ground)

// --- Camera-aware world drawing -----------------------------------------------------------
// Variants of the primitives above that take WORLD coordinates and a Camera2D (camera.hpp).
// All of them share one skeleton: CULL against the camera's visible rect (an off-screen
// draw is skipped entirely, not clipped), fall through to the exact pre-camera code path
// when the camera is at identity() (so an idle camera renders pixel-identically and costs
// one comparison), and otherwise project through the camera's single rounding policy.
// Zoomed sprites go through LovyanGFX's nearest-neighbour rotate-zoom -- deliberately no
// anti-aliasing (crisp pixel art, and no per-frame resample cost), and deliberately NOT the
// scaled-copy LRU cache above, whose integer-box keys a continuous zoom would thrash.
// Distinct names rather than overloads, so camera-aware call sites stay greppable.
void gfx_fill_rect_world(const Camera2D& c, float x, float y, float w, float h, uint16_t col);
void gfx_fill_circle_world(const Camera2D& c, float cx, float cy, float r, uint16_t col);
void gfx_draw_circle_world(const Camera2D& c, float cx, float cy, float r, uint16_t col);
void gfx_fill_arc_world(const Camera2D& c, float cx, float cy, float r0, float r1,
                        int a0, int a1, uint16_t col);
void gfx_fill_triangle_world(const Camera2D& c, float x0, float y0, float x1, float y1,
                             float x2, float y2, uint16_t col);
void gfx_blit_world(const Camera2D& c, const Sprite& s, float wcx, float wcy);   // centered
void gfx_blit_sprite_world(const Camera2D& c, LGFX_Sprite* s, float wcx, float wcy,
                           uint16_t transp, bool mirror = false);                // centered
void gfx_blit_sprite_bottom_world(const Camera2D& c, LGFX_Sprite* s, float wcx, float wBottom,
                                  uint16_t transp, bool mirror = false);         // feet-anchored
void gfx_tile_region_world(const Camera2D& c, float x, float y, float w, float h,
                           const uint16_t* tile, int tw, int th);
