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

void gfx_fade(float k)
{
    if (k <= 0.0f) return;
    uint16_t* px = (uint16_t*)fb.getBuffer();
    if (!px) return;
    const int n = GAME_W * GAME_H;
    if (k >= 1.0f) { memset(px, 0, (size_t)n * 2); return; }

    // The sprite's IN-MEMORY byte order isn't promised by the API (see the scaled-copy
    // cache, which hit the same wall): detect it once by round-tripping a known color
    // through drawPixel and reading the raw buffer back.
    static int swapped = -1;
    if (swapped < 0) {
        uint16_t keep = px[0];
        fb.drawPixel(0, 0, (uint16_t)0xF800u);
        swapped = (px[0] != 0xF800u) ? 1 : 0;
        px[0] = keep;
    }

    // Per-channel multiply via two tiny LUTs (5-bit red/blue share one). Rebuilt per call:
    // 96 multiplies against 76,800 pixel transforms is noise.
    const int m = (int)((1.0f - k) * 256.0f + 0.5f);
    uint8_t l5[32], l6[64];
    for (int i = 0; i < 32; i++) l5[i] = (uint8_t)((i * m) >> 8);
    for (int i = 0; i < 64; i++) l6[i] = (uint8_t)((i * m) >> 8);

    if (swapped) {
        for (int i = 0; i < n; i++) {
            uint16_t v = (uint16_t)__builtin_bswap16(px[i]);
            v = (uint16_t)((l5[(v >> 11) & 31] << 11) | (l6[(v >> 5) & 63] << 5) | l5[v & 31]);
            px[i] = (uint16_t)__builtin_bswap16(v);
        }
    } else {
        for (int i = 0; i < n; i++) {
            uint16_t v = px[i];
            px[i] = (uint16_t)((l5[(v >> 11) & 31] << 11) | (l6[(v >> 5) & 63] << 5) | l5[v & 31]);
        }
    }
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

// Mirrored (left-to-right flipped) push of s with its top-left at (x, y). Rotate-zoom
// with zoom_x = -1 / angle 0 is an exact 1:1 reversed copy (nearest sampling on integer
// steps), so pixel art stays crisp. The pivot is set explicitly: the transform maps the
// pivot to the destination point, and (w-1)/2 is the geometric center of the pixel grid.
static void push_mirrored(LGFX_Sprite* s, int x, int y, uint16_t transp)
{
    float px = (s->width() - 1) * 0.5f, py = (s->height() - 1) * 0.5f;
    s->setPivot(px, py);
    s->pushRotateZoom(&fb, x + px, y + py, 0.0f, -1.0f, 1.0f, transp);
}

void gfx_blit_sprite(LGFX_Sprite* s, int cx, int cy, uint16_t transp, bool mirror)
{
    if (!s) return;
    int x = cx - s->width() / 2, y = cy - s->height() / 2;
    if (mirror) push_mirrored(s, x, y, transp);
    else        s->pushSprite(&fb, x, y, transp);
}

// Anchor by the sprite's feet: (cx, bottomY) is the bottom-center. Lets creatures of
// any height stand on the same baseline instead of floating/sinking when centered.
void gfx_blit_sprite_bottom(LGFX_Sprite* s, int cx, int bottomY, uint16_t transp, bool mirror)
{
    if (!s) return;
    int x = cx - s->width() / 2, y = bottomY - s->height();
    if (mirror) push_mirrored(s, x, y, transp);
    else        s->pushSprite(&fb, x, y, transp);
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

// Whether a 16bpp sprite's raw buffer stores rgb565 byte-swapped. Calibrated once by
// round-tripping a known color through a scratch sprite (same class/config as fb, so
// the layout matches). Same trick scaled_copy uses for readPixelValue.
static bool fb565_swapped()
{
    static int swapped = -1;
    if (swapped < 0) {
        LGFX_Sprite t(&display);
        t.setColorDepth(16);
        if (t.createSprite(1, 1)) {
            t.drawPixel(0, 0, (uint16_t)0xF800u);
            swapped = (*(const uint16_t*)t.getBuffer() != 0xF800u) ? 1 : 0;
            t.deleteSprite();
        } else {
            swapped = 0;                       // can't calibrate; assume unswapped
        }
    }
    return swapped == 1;
}

// Mirrored equivalent of fb.pushAlphaImage: composite an argb8888 sprite onto fb with
// its top-left at (x, y), reading each source row right-to-left. LovyanGFX's alpha
// blit only reads its buffer forward, so the flip is done here in OUR loop rather
// than by patching the library or caching a reversed copy -- a mirrored draw costs
// the same as an unmirrored one and never touches a cache slot. Straight (non-
// premultiplied) alpha over rgb565, same semantics as pushAlphaImage.
static void push_alpha_mirrored(LGFX_Sprite* sc, int x, int y)
{
    const uint32_t* src = (const uint32_t*)sc->getBuffer();
    int w = sc->width(), h = sc->height();
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;               // clip to fb
    int x1 = (x + w > GAME_W) ? GAME_W - x : w;
    int y1 = (y + h > GAME_H) ? GAME_H - y : h;
    if (x0 >= x1 || y0 >= y1) return;

    const bool swap = fb565_swapped();
    uint16_t* dst = (uint16_t*)fb.getBuffer();
    for (int sy = y0; sy < y1; sy++) {
        const uint32_t* srow = src + sy * w;
        uint16_t*       drow = dst + (y + sy) * GAME_W + x;
        for (int sx = x0; sx < x1; sx++) {
            uint32_t p = srow[w - 1 - sx];                      // <- the mirror
            uint32_t a = p >> 24;
            if (a == 0) continue;
            uint32_t sr = (p >> 16) & 0xFF, sg = (p >> 8) & 0xFF, sb = p & 0xFF;
            uint32_t out;
            if (a == 255) {
                out = ((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3);
            } else {
                uint16_t d = drow[sx];
                if (swap) d = (uint16_t)((d >> 8) | (d << 8));
                uint32_t dr = ((d >> 11) & 0x1F) << 3, dg = ((d >> 5) & 0x3F) << 2, db = (d & 0x1F) << 3;
                uint32_t ia = 255 - a;
                uint32_t r = (sr * a + dr * ia + 127) / 255;
                uint32_t g = (sg * a + dg * ia + 127) / 255;
                uint32_t b = (sb * a + db * ia + 127) / 255;
                out = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
            drow[sx] = swap ? (uint16_t)((out >> 8) | (out << 8)) : (uint16_t)out;
        }
    }
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
void gfx_blit_sprite_fit(LGFX_Sprite* s, int cx, int cy, int maxW, int maxH, uint16_t transp, bool mirror)
{
    if (!s) return;
    LGFX_Sprite* sc = scaled_copy(s, maxW, maxH);
    if (!sc) { gfx_blit_sprite(s, cx, cy, transp, mirror); return; }
    int x = cx - sc->width() / 2, y = cy - sc->height() / 2;
    if (mirror) push_alpha_mirrored(sc, x, y);   // flip applied while drawing; cache stays orientation-free
    else fb.pushAlphaImage(x, y, sc->width(), sc->height(), (const lgfx::argb8888_t*)sc->getBuffer());
}

// Like gfx_blit_sprite_fit but anchored by the feet: (cx, bottomY) is the bottom-center, so
// a scaled-down big creature still stands on the given baseline. Native size if it already fits.
void gfx_blit_sprite_fit_bottom(LGFX_Sprite* s, int cx, int bottomY, int maxW, int maxH, uint16_t transp, bool mirror)
{
    if (!s) return;
    LGFX_Sprite* sc = scaled_copy(s, maxW, maxH);
    if (!sc) { gfx_blit_sprite_bottom(s, cx, bottomY, transp, mirror); return; }
    int x = cx - sc->width() / 2, y = bottomY - sc->height();
    if (mirror) push_alpha_mirrored(sc, x, y);   // flip applied while drawing; cache stays orientation-free
    else fb.pushAlphaImage(x, y, sc->width(), sc->height(), (const lgfx::argb8888_t*)sc->getBuffer());
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

// --- Camera-aware world drawing (see gfx.hpp for the shared skeleton) ---------------------
//
// Rects and circles need no identity() special case: Camera2D::projectRect and the rounded
// center/radius reproduce the plain calls exactly at identity, so only the paths where the
// DRAW MECHANISM changes (sprites: pushSprite vs pushRotateZoom; tiles: pushImage vs
// per-tile rotate-zoom) branch on it -- there the fallback also guarantees pixel parity.

void gfx_fill_rect_world(const Camera2D& c, float x, float y, float w, float h, uint16_t col)
{
    if (!c.visible(x, y, w, h)) return;
    int X, Y, W, H;
    c.projectRect(x, y, w, h, X, Y, W, H);
    fb.fillRect(X, Y, W, H, col);
}

void gfx_fill_circle_world(const Camera2D& c, float cx, float cy, float r, uint16_t col)
{
    if (!c.visible(cx - r, cy - r, r * 2, r * 2)) return;
    int R = (int)lroundf(c.scale(r));
    fb.fillCircle((int)lroundf(c.sx(cx)), (int)lroundf(c.sy(cy)), R < 1 ? 1 : R, col);
}

void gfx_draw_circle_world(const Camera2D& c, float cx, float cy, float r, uint16_t col)
{
    if (!c.visible(cx - r, cy - r, r * 2, r * 2)) return;
    int R = (int)lroundf(c.scale(r));
    fb.drawCircle((int)lroundf(c.sx(cx)), (int)lroundf(c.sy(cy)), R < 1 ? 1 : R, col);
}

void gfx_fill_arc_world(const Camera2D& c, float cx, float cy, float r0, float r1,
                        int a0, int a1, uint16_t col)
{
    float r = r0 > r1 ? r0 : r1;
    if (!c.visible(cx - r, cy - r, r * 2, r * 2)) return;
    fb.fillArc((int)lroundf(c.sx(cx)), (int)lroundf(c.sy(cy)),
               (int)lroundf(c.scale(r0)), (int)lroundf(c.scale(r1)), a0, a1, col);
}

void gfx_fill_triangle_world(const Camera2D& c, float x0, float y0, float x1, float y1,
                             float x2, float y2, uint16_t col)
{
    float minX = fminf(x0, fminf(x1, x2)), maxX = fmaxf(x0, fmaxf(x1, x2));
    float minY = fminf(y0, fminf(y1, y2)), maxY = fmaxf(y0, fmaxf(y1, y2));
    if (!c.visible(minX, minY, maxX - minX, maxY - minY)) return;
    fb.fillTriangle((int)lroundf(c.sx(x0)), (int)lroundf(c.sy(y0)),
                    (int)lroundf(c.sx(x1)), (int)lroundf(c.sy(y1)),
                    (int)lroundf(c.sx(x2)), (int)lroundf(c.sy(y2)), col);
}

void gfx_blit_world(const Camera2D& c, const Sprite& s, float wcx, float wcy)
{
    if (!c.visible(wcx - s.w * 0.5f, wcy - s.h * 0.5f, s.w, s.h)) return;
    if (c.identity()) { gfx_blit(s, (int)lroundf(wcx), (int)lroundf(wcy)); return; }
    float z = c.zoom();
    fb.pushImageRotateZoom(c.sx(wcx), c.sy(wcy), (s.w - 1) * 0.5f, (s.h - 1) * 0.5f,
                           0.0f, z, z, s.w, s.h, s.data, s.transp);
}

// Zoomed sprite: the same pivot-center rotate-zoom trick push_mirrored uses, with the zoom
// factors doing double duty -- magnitude is the camera zoom, the x sign is the mirror. The
// pivot maps to the projected world CENTER, and since the rendered extent is zoom*size
// centered there, the sprite's world-space bottom lands exactly on the projected baseline.
static void push_world(LGFX_Sprite* s, const Camera2D& c, float wcx, float wcy,
                       uint16_t transp, bool mirror)
{
    float z = c.zoom();
    s->setPivot((s->width() - 1) * 0.5f, (s->height() - 1) * 0.5f);
    s->pushRotateZoom(&fb, c.sx(wcx), c.sy(wcy), 0.0f, mirror ? -z : z, z, transp);
}

void gfx_blit_sprite_world(const Camera2D& c, LGFX_Sprite* s, float wcx, float wcy,
                           uint16_t transp, bool mirror)
{
    if (!s) return;
    float w = (float)s->width(), h = (float)s->height();
    if (!c.visible(wcx - w * 0.5f, wcy - h * 0.5f, w, h)) return;
    if (c.identity()) { gfx_blit_sprite(s, (int)lroundf(wcx), (int)lroundf(wcy), transp, mirror); return; }
    push_world(s, c, wcx, wcy, transp, mirror);
}

void gfx_blit_sprite_bottom_world(const Camera2D& c, LGFX_Sprite* s, float wcx, float wBottom,
                                  uint16_t transp, bool mirror)
{
    if (!s) return;
    float w = (float)s->width(), h = (float)s->height();
    if (!c.visible(wcx - w * 0.5f, wBottom - h, w, h)) return;
    if (c.identity()) { gfx_blit_sprite_bottom(s, (int)lroundf(wcx), (int)lroundf(wBottom), transp, mirror); return; }
    push_world(s, c, wcx, wBottom - h * 0.5f, transp, mirror);
}

// Overscale applied to every zoomed tile, in destination pixels. Each tile is rendered
// into its corner-snapped rect; the rasterizer samples pixel CENTERS, so a rect covering
// [X0, X1) owns exactly the pixel columns X0..X1-1, and a hair of overscale (spread half
// to each side, so +-0.25px) guarantees those edge columns are inside the rendered extent
// despite float rounding -- while staying too small to ever reach a neighbour's first
// column. No per-tile clipping needed; seams and double-draws are both impossible.
static const float TILE_SNAP_PAD = 0.51f;

void gfx_tile_region_world(const Camera2D& c, float x, float y, float w, float h,
                           const uint16_t* tile, int tw, int th)
{
    // floorf, not a truncating cast: the zoomed path below floors every edge (via
    // projectRect and the per-tile snap), and the two paths have to agree on a negative
    // or fractional coordinate or the region would shift a pixel the instant a pinch starts.
    if (c.identity()) {
        int IX = (int)floorf(x), IY = (int)floorf(y);
        gfx_tile_region(IX, IY, (int)floorf(x + w) - IX, (int)floorf(y + h) - IY, tile, tw, th);
        return;
    }
    if (!c.visible(x, y, w, h)) return;

    // Clip to the projected region rect: edge tiles overrun it (same as the plain path),
    // and the last row/column of a region that isn't a tile multiple must crop.
    int RX, RY, RW, RH;
    c.projectRect(x, y, w, h, RX, RY, RW, RH);
    fb.setClipRect(RX, RY, RW, RH);

    // Walk ONLY the tiles overlapping the visible part of the region -- this is the tile
    // path's culling: at 4x zoom, three quarters of the ground never enters the loop.
    float wxB = x + w, wyB = y + h;
    if (c.visX() + c.visW() < wxB) wxB = c.visX() + c.visW();
    if (c.visY() + c.visH() < wyB) wyB = c.visY() + c.visH();
    float wxA = x > c.visX() ? x : c.visX();
    float wyA = y > c.visY() ? y : c.visY();
    int i0 = (int)floorf((wxA - x) / (float)tw);
    int j0 = (int)floorf((wyA - y) / (float)th);

    const float pivX = (tw - 1) * 0.5f, pivY = (th - 1) * 0.5f;
    for (int j = j0; y + j * (float)th < wyB; j++) {
        float wy0 = y + j * (float)th;
        int Y0 = (int)floorf(c.sy(wy0));
        int Hd = (int)floorf(c.sy(wy0 + th)) - Y0;
        if (Hd <= 0) continue;
        for (int i = i0; x + i * (float)tw < wxB; i++) {
            float wx0 = x + i * (float)tw;
            int X0 = (int)floorf(c.sx(wx0));
            int Wd = (int)floorf(c.sx(wx0 + tw)) - X0;
            if (Wd <= 0) continue;
            // Scale each tile to exactly its own snapped rect: neighbours share edges by
            // construction (both floor the same world coordinate), so per-tile scales that
            // differ in the third decimal cannot open a seam between them.
            fb.pushImageRotateZoom(X0 + (Wd - 1) * 0.5f, Y0 + (Hd - 1) * 0.5f,
                                   pivX, pivY, 0.0f,
                                   (Wd + TILE_SNAP_PAD) / (float)tw,
                                   (Hd + TILE_SNAP_PAD) / (float)th,
                                   tw, th, tile);
        }
    }
    fb.clearClipRect();
}
