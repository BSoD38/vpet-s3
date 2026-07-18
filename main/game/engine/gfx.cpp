#include "gfx.hpp"
#include "esp_log.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <initializer_list>

LGFX_Sprite fb(&display);
static LGFX_Sprite snap(&display);   // frozen snapshot of the outgoing scene (transitions)
static LGFX_Sprite comp(&display);   // off-screen composite target (single atomic panel push)

void gfx_init()
{
    for (LGFX_Sprite* s : { &fb, &snap, &comp }) {
        s->setPsram(true);
        s->setColorDepth(16);
        // ~150 KB PSRAM each; without them every draw would deref a null buffer.
        if (!s->createSprite(GAME_W, GAME_H)) {
            ESP_LOGE("GFX", "createSprite(%d,%d) failed - out of PSRAM", GAME_W, GAME_H);
            abort();   // fail loudly instead of crashing later on a null buffer
        }
    }
    // Our baked RGB565 arrays (tiles, fallback "?" sprite) are authored in native
    // rgb565 (high-byte-first) order. LovyanGFX otherwise reads raw uint16_t image data
    // as byte-swapped (swap565), which scrambles the colors; tell fb to read it natively
    // so pushImage() blits come out right.
    fb.setSwapBytes(true);
}

void gfx_present()
{
    fb.pushSprite(0, 0);
}

void gfx_snapshot()
{
    fb.pushSprite(&snap, 0, 0);   // copy current back-buffer into the snapshot sprite
}

// Both composite into the off-screen buffer and push it ONCE, so the panel
// updates atomically (no flicker from painting two sprites onto the live panel).

// Forward: old scene is the base; the new scene (fb) covers it, sliding in from
// the right and overshooting past 0. Where the overshoot pulls the new sheet's
// right edge in, fill with the incoming background so the OLD scene doesn't peek.
void gfx_present_cover(int fbX, uint16_t gapColor)
{
    snap.pushSprite(&comp, 0, 0);      // old base fills the buffer
    fb.pushSprite(&comp, fbX, 0);      // new scene on top
    if (fbX < 0)                       // overshoot exposed a strip on the right
        comp.fillRect(fbX + GAME_W, 0, -fbX, GAME_H, gapColor);
    comp.pushSprite(0, 0);
}

// Back: new scene (fb) is the base and fills the buffer, so wherever the outgoing
// snapshot slides off to the right, the destination shows through with no gap.
// The anticipation ease first winds the outgoing sheet slightly LEFT, which would
// uncover the destination on the right early; fill that strip with the outgoing bg.
void gfx_present_reveal(int snapX, uint16_t gapColor)
{
    fb.pushSprite(&comp, 0, 0);        // new base fills the buffer
    snap.pushSprite(&comp, snapX, 0);  // old scene slides off on top
    if (snapX < 0)                     // anticipation wound it left, exposing a right strip
        comp.fillRect(snapX + GAME_W, 0, -snapX, GAME_H, gapColor);
    comp.pushSprite(0, 0);
}

// Iris (circular) mask: copy the chosen sheet into the composite buffer, black out
// everything outside a circle of `radius` centred on the screen, then push once.
void gfx_present_iris(bool useSnap, int radius)
{
    (useSnap ? snap : fb).pushSprite(&comp, 0, 0);   // work on a copy; leaves snap/fb intact
    const int cx = GAME_W / 2, cy = GAME_H / 2;
    const int r2 = radius * radius;
    for (int y = 0; y < GAME_H; y++) {
        int dy = y - cy;
        int inside = r2 - dy * dy;
        if (inside <= 0) { comp.fillRect(0, y, GAME_W, 1, 0x0000); continue; }
        int half  = (int)sqrtf((float)inside);
        int left  = cx - half, right = cx + half;
        if (left  > 0)      comp.fillRect(0, y, left, 1, 0x0000);
        if (right < GAME_W) comp.fillRect(right, y, GAME_W - right, 1, 0x0000);
    }
    comp.pushSprite(0, 0);
}

void gfx_text(int x, int y, uint8_t size, uint16_t color, const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fb.setTextColor(color);
    fb.setTextSize(size);
    fb.setCursor(x, y);
    fb.print(buf);
}

void gfx_bar(int x, int y, int w, int h, float frac,
             uint16_t fg, uint16_t bg, uint16_t border)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    fb.fillRect(x, y, w, h, bg);
    fb.fillRect(x, y, (int)(w * frac), h, fg);
    fb.drawRect(x, y, w, h, border);
}

void gfx_blit(const Sprite& s, int cx, int cy)
{
    fb.pushImage(cx - s.w / 2, cy - s.h / 2, s.w, s.h, s.data, s.transp);
}

void gfx_blit_sprite(LGFX_Sprite* s, int cx, int cy, uint16_t transp)
{
    if (!s) return;
    s->pushSprite(&fb, cx - s->width() / 2, cy - s->height() / 2, transp);
}

// Anchor by the sprite's feet: (cx, bottomY) is the bottom-center. Lets creatures of
// any height stand on the same baseline instead of floating/sinking when centered.
void gfx_blit_sprite_bottom(LGFX_Sprite* s, int cx, int bottomY, uint16_t transp)
{
    if (!s) return;
    s->pushSprite(&fb, cx - s->width() / 2, bottomY - s->height(), transp);
}

// Draw centered at (cx, cy), scaled DOWN to fit within maxW x maxH (aspect preserved);
// sprites already within the box are drawn at native size.
void gfx_blit_sprite_fit(LGFX_Sprite* s, int cx, int cy, int maxW, int maxH, uint16_t transp)
{
    if (!s) return;
    int w = s->width(), h = s->height();
    if (w <= maxW && h <= maxH) { gfx_blit_sprite(s, cx, cy, transp); return; }
    float z = (float)maxW / w;
    float zy = (float)maxH / h;
    if (zy < z) z = zy;
    s->setPivot((float)w / 2, (float)h / 2);
    s->pushRotateZoom(&fb, (float)cx, (float)cy, 0.0f, z, z, transp);
}

// Fill rect [x,y,w,h] by repeating `tile` (tw x th) as an opaque texture. scrollX shifts
// the pattern left (for scrolling grounds). Clips to the rect so edge tiles don't overrun.
void gfx_tile_region(int x, int y, int w, int h, const uint16_t* tile, int tw, int th, int scrollX)
{
    fb.setClipRect(x, y, w, h);
    int phase = ((scrollX % tw) + tw) % tw;   // 0..tw-1, wraps cleanly for either sign of scrollX
    for (int ty = y; ty < y + h; ty += th)
        for (int tx = x - phase; tx < x + w; tx += tw)
            fb.pushImage(tx, ty, tw, th, tile);
    fb.clearClipRect();
}
