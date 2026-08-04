#include "gfx.hpp"
#include "esp_log.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    // We position all text manually; never let it wrap to a new line (clip at the edge instead).
    fb.setTextWrap(false);
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

void gfx_text_fit(int x, int y, int maxW, uint8_t size, uint16_t color, const char* fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const int cw = 6 * size;                       // default font cell, scaled
    const int cols = (cw > 0) ? maxW / cw : 0;
    if (cols <= 0) return;

    if ((int)strlen(buf) > cols) {                 // too long: cut and mark the elision
        if (cols >= 3) { buf[cols - 2] = '.'; buf[cols - 1] = '.'; }
        buf[cols] = '\0';
    }
    fb.setTextColor(color);
    fb.setTextSize(size);
    fb.setCursor(x, y);
    fb.print(buf);
}

// Shared wrap engine for gfx_text_wrap / _lines / _height: walks the string once, laying out
// one line per iteration. Always runs to the end of the string (even when `reveal` has already
// been exhausted) so the returned line count reflects the whole text, not just the visible part.
static int wrap_core(int x, int y, int w, uint8_t size, uint16_t color, const char* s,
                     int lineGap, int reveal, bool draw, int maxLines)
{
    if (!s || !*s || size == 0) return 0;
    const int cw = 6 * size, ch = 8 * size;
    int cols = w / cw;
    if (cols < 1) cols = 1;

    if (draw) { fb.setTextColor(color); fb.setTextSize(size); }

    int lines = 0, consumed = 0;
    for (const char* p = s; *p; ) {
        // Longest prefix that fits, remembering the last space we could break at.
        int take = 0, brk = -1;
        while (p[take] && take < cols && p[take] != '\n') {
            if (p[take] == ' ') brk = take;
            take++;
        }
        int skip = 0;                                  // consumed but not drawn (the break char)
        if      (p[take] == '\n') skip = 1;            // explicit line break
        else if (p[take] == ' ')  skip = 1;            // fits exactly, break on the space
        else if (p[take] != '\0' && brk >= 0) { take = brk; skip = 1; }   // back up to the space
        // (no space to back up to -> hard-break a too-long word at `cols`)

        // On the last permitted line, if text still follows, end it in ".." so the reader can
        // tell it was cut. Only when the line is fully typed, so the typewriter doesn't show
        // an ellipsis before it has got there.
        const bool lastAllowed = (maxLines > 0 && lines == maxLines - 1);
        const bool moreAfter   = (p[take + skip] != '\0');

        if (draw && take > 0) {
            int n = take;
            if (reveal >= 0) {
                int room = reveal - consumed;
                n = (room <= 0) ? 0 : (room < n ? room : n);
            }
            const bool elide = lastAllowed && moreAfter && n == take;
            if (elide && n > 2) n -= 2;
            if (n > 0) {
                char buf[48];
                if (n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
                memcpy(buf, p, (size_t)n);
                buf[n] = '\0';
                if (elide) { buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '\0'; }
                fb.setCursor(x, y + lines * (ch + lineGap));
                fb.print(buf);
            }
        }
        consumed += take + skip;
        lines++;
        p += take + skip;
        if (maxLines > 0 && lines >= maxLines) break;
    }
    return lines;
}

int gfx_text_wrap(int x, int y, int w, uint8_t size, uint16_t color, const char* s,
                  int lineGap, int reveal, int maxLines)
{
    return wrap_core(x, y, w, size, color, s, lineGap, reveal, true, maxLines);
}

int gfx_text_wrap_lines(int w, uint8_t size, const char* s, int maxLines)
{
    return wrap_core(0, 0, w, size, 0, s, 0, -1, false, maxLines);
}

int gfx_text_wrap_height(int w, uint8_t size, const char* s, int lineGap, int maxLines)
{
    int n = wrap_core(0, 0, w, size, 0, s, lineGap, -1, false, maxLines);
    return n <= 0 ? 0 : n * (8 * size + lineGap) - lineGap;
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

uint16_t mix565(uint16_t a, uint16_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
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

// LRU cache of downscaled copies of source sprites. Per-frame AA resampling looked great but
// tanked the framerate (~25fps); the scaled bitmap depends only on (source, box), so we run
// LovyanGFX's high-quality AA resampler ONCE and per-frame just alpha-composite the result.
//
// LovyanGFX's AA blends against the destination and won't emit an alpha channel, so caching
// it needs a recovered alpha (a color key can't hold the soft edges, and they must survive
// over ANY background -- e.g. Stance's tilting beam behind the pet). We recover it: render
// the AA scale twice, over white and over black. Per pixel, (white - black) == (1-coverage)*255
// gives the alpha, and (over-black / coverage) recovers the straight color -> an argb8888
// sprite blitted with pushAlphaImage (true per-pixel compositing).
//
// Multiple entries so several scaled sprites coexist without thrashing -- needed for sprite
// animations (all frames stay cached across a cycle) and multiple creatures on screen at once.
// Slots are lazily allocated (no PSRAM used until actually cached) and evicted least-recently-used.
struct ScaledSlot {
    LGFX_Sprite* spr = nullptr;    // argb8888 result (allocated on first use of this slot)
    const void*  src = nullptr;    // key: source sprite
    int          maxW = 0, maxH = 0;   // key: fit box
    uint32_t     tick = 0;         // LRU stamp
};
static const int SCALED_CACHE = 16;    // max distinct scaled sprites kept resident
static ScaledSlot s_scaledSlots[SCALED_CACHE];
static uint32_t   s_scaledClock = 0;

// Cached AA-downscaled copy of s for the maxW x maxH box, or nullptr if s already fits (draw
// it natively) or allocation failed.
static LGFX_Sprite* scaled_copy(LGFX_Sprite* s, int maxW, int maxH)
{
    int sw0 = s->width(), sh0 = s->height();
    if (sw0 <= maxW && sh0 <= maxH) return nullptr;             // fits: no scaling needed

    // cache hit?
    for (int i = 0; i < SCALED_CACHE; i++) {
        ScaledSlot& e = s_scaledSlots[i];
        if (e.spr && e.src == s && e.maxW == maxW && e.maxH == maxH) {
            e.tick = ++s_scaledClock;
            return e.spr;
        }
    }
    // miss: take a free slot, else evict the least-recently-used one
    int pick = 0;
    uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < SCALED_CACHE; i++) {
        if (!s_scaledSlots[i].spr) { pick = i; break; }
        if (s_scaledSlots[i].tick < best) { best = s_scaledSlots[i].tick; pick = i; }
    }
    ScaledSlot& e = s_scaledSlots[pick];

    float z = (float)maxW / sw0, zy = (float)maxH / sh0;
    if (zy < z) z = zy;                                         // uniform scale (aspect preserved)
    int dw = (int)(sw0 * z + 0.5f), dh = (int)(sh0 * z + 0.5f);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    // Two AA renders of the scaled sprite: one over a white field, one over black.
    LGFX_Sprite tw(&display), tb(&display);
    tw.setPsram(true); tw.setColorDepth(16);
    tb.setPsram(true); tb.setColorDepth(16);
    if (!tw.createSprite(dw, dh) || !tb.createSprite(dw, dh)) {
        tw.deleteSprite(); tb.deleteSprite(); return nullptr;   // OOM -> native (slot untouched)
    }
    // readPixelValue may return raw (byte-swapped) or logical rgb565; detect by round-tripping
    // a known color (all same-format sprites share the order), then read back consistently.
    tb.drawPixel(0, 0, (uint16_t)0xF800u);
    bool rdswap = ((tb.readPixelValue(0, 0) & 0xFFFFu) != 0xF800u);
    tw.fillScreen((uint16_t)0xFFFFu);                           // white field
    tb.fillScreen((uint16_t)0x0000u);                           // black field
    s->setPivot((float)sw0 / 2, (float)sh0 / 2);
    s->pushRotateZoomWithAA(&tw, dw / 2.0f, dh / 2.0f, 0.0f, z, z, SPRITE_TRANSP);
    s->pushRotateZoomWithAA(&tb, dw / 2.0f, dh / 2.0f, 0.0f, z, z, SPRITE_TRANSP);

    // (re)allocate the slot's argb8888 result at the new size
    if (!e.spr) e.spr = new LGFX_Sprite(&display);
    else        e.spr->deleteSprite();
    e.spr->setPsram(true);
    e.spr->setColorDepth(lgfx::argb8888_4Byte);
    if (!e.spr->createSprite(dw, dh)) {
        tw.deleteSprite(); tb.deleteSprite();
        delete e.spr; e.spr = nullptr;                          // slot back to free
        return nullptr;
    }

    auto dec = [&](LGFX_Sprite& t, int x, int y, int& R, int& G, int& B) {
        uint32_t v = t.readPixelValue(x, y) & 0xFFFF;
        if (rdswap) v = ((v >> 8) | (v << 8)) & 0xFFFF;
        int r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
        R = (r5 << 3) | (r5 >> 2); G = (g6 << 2) | (g6 >> 4); B = (b5 << 3) | (b5 >> 2);
    };

    uint32_t* out = (uint32_t*)e.spr->getBuffer();              // argb8888: 0xAARRGGBB per pixel
    for (int dy = 0; dy < dh; dy++) {
        for (int dx = 0; dx < dw; dx++) {
            int rw, gw, bw, rb, gb, bb;
            dec(tw, dx, dy, rw, gw, bw);
            dec(tb, dx, dy, rb, gb, bb);
            int d = (rw - rb) + (gw - gb) + (bw - bb);          // == 3*(1-cov)*255
            if (d < 0) d = 0;
            if (d > 765) d = 765;
            float cov = 1.0f - (float)d / 765.0f;
            uint32_t argb = 0;                                  // default fully transparent
            if (cov > 0.004f) {
                float ic = 1.0f / cov;
                int R = (int)(rb * ic + 0.5f);
                int G = (int)(gb * ic + 0.5f);
                int B = (int)(bb * ic + 0.5f);
                if (R > 255) R = 255;
                if (G > 255) G = 255;
                if (B > 255) B = 255;
                int A = (int)(cov * 255.0f + 0.5f);
                if (A > 255) A = 255;
                argb = ((uint32_t)A << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
            }
            out[dy * dw + dx] = argb;
        }
    }
    tw.deleteSprite(); tb.deleteSprite();

    e.src = s; e.maxW = maxW; e.maxH = maxH; e.tick = ++s_scaledClock;
    return e.spr;
}

// Drop any cached scaled copies whose source is `src`, freeing their slots. MUST be called
// before a source sprite is freed: the cache keys on the raw LGFX_Sprite* pointer, and a
// freed heap address can be reused for a DIFFERENT sprite (e.g. the creature registry's LRU
// evicts + reallocates), so a stale entry would otherwise alias the wrong image.
void gfx_invalidate_scaled(const void* src)
{
    if (!src) return;
    for (int i = 0; i < SCALED_CACHE; i++) {
        ScaledSlot& e = s_scaledSlots[i];
        if (e.spr && e.src == src) {
            e.spr->deleteSprite();
            delete e.spr;
            e.spr = nullptr;
            e.src = nullptr; e.maxW = 0; e.maxH = 0; e.tick = 0;
        }
    }
}

// Centered at (cx, cy), scaled DOWN to fit maxW x maxH (aspect preserved, AA via a cached
// alpha copy); sprites already within the box are drawn at native size.
void gfx_blit_sprite_fit(LGFX_Sprite* s, int cx, int cy, int maxW, int maxH, uint16_t transp)
{
    if (!s) return;
    LGFX_Sprite* sc = scaled_copy(s, maxW, maxH);
    if (!sc) { gfx_blit_sprite(s, cx, cy, transp); return; }
    fb.pushAlphaImage(cx - sc->width() / 2, cy - sc->height() / 2, sc->width(), sc->height(),
                      (const lgfx::argb8888_t*)sc->getBuffer());
}

// Like gfx_blit_sprite_fit but anchored by the feet: (cx, bottomY) is the bottom-center, so
// a scaled-down big creature still stands on the given baseline. Native size if it already fits.
void gfx_blit_sprite_fit_bottom(LGFX_Sprite* s, int cx, int bottomY, int maxW, int maxH, uint16_t transp)
{
    if (!s) return;
    LGFX_Sprite* sc = scaled_copy(s, maxW, maxH);
    if (!sc) { gfx_blit_sprite_bottom(s, cx, bottomY, transp); return; }
    fb.pushAlphaImage(cx - sc->width() / 2, bottomY - sc->height(), sc->width(), sc->height(),
                      (const lgfx::argb8888_t*)sc->getBuffer());
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
