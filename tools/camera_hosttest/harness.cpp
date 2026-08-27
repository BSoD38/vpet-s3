// Host harness for the 2D camera (engine/camera.hpp) and the pinch recognizer
// (engine/pinch.hpp) -- see docs/camera.md.
//
// WHY THIS EXISTS. The camera's guarantees are all MATH guarantees: the mapping inverts,
// a zoom anchored on a point holds that point still, tiles snapped through projectRect
// can never open a seam, and the identity camera is a bit-exact no-op. Every one of those
// is provable here in milliseconds, against the same headers the firmware compiles --
// camera.hpp and pinch.hpp depend on nothing from ESP-IDF or LovyanGFX by design (no shim
// directory needed, unlike the battle harness). A seam or a zoom jump found on the device
// instead costs a flash cycle per hypothesis.
//
// Nothing in the firmware build references this. It is a development tool.
//
// Build (MSYS2 mingw64 g++; any host compiler will do -- put mingw64/bin on PATH first):
//
//   export PATH=/c/msys64/mingw64/bin:$PATH
//   g++ -std=gnu++20 -O2 -I main/game tools/camera_hosttest/harness.cpp -o harness
//
// Run:  ./harness        prints PASS/FAIL per property, exit 0 only if all pass.

#include "engine/camera.hpp"
#include "engine/pinch.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    g_fail++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Deterministic PRNG (the firmware's esp_random is unavailable and unwanted here).
static uint32_t s_rng = 0x1234567u;
static uint32_t rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static float frnd(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() % 10000u) / 9999.0f; }

static const int VW = 240, VH = 320;   // the panel (gfx.hpp GAME_W/GAME_H)

// Random legal camera state: any zoom in range, panned anywhere, then clamped.
static Camera2D random_cam()
{
    Camera2D c(VW, VH, VW, VH);
    c.setZoom(frnd(1.0f, 4.0f));
    c.panScreen(frnd(-500.0f, 500.0f), frnd(-500.0f, 500.0f));
    c.clampToWorld();
    return c;
}

static void test_roundtrip()
{
    for (int k = 0; k < 20000; k++) {
        Camera2D c = random_cam();
        float v = frnd(-100.0f, 400.0f);
        CHECK(fabsf(c.wx(c.sx(v)) - v) < 1e-3f, "wx(sx(%f)) drifted (zoom %f)", v, c.zoom());
        CHECK(fabsf(c.wy(c.sy(v)) - v) < 1e-3f, "wy(sy(%f)) drifted (zoom %f)", v, c.zoom());
    }
    printf("PASS round-trip world<->screen\n");
}

static void test_zoom_anchor()
{
    for (int k = 0; k < 5000; k++) {
        Camera2D c = random_cam();
        float ax = frnd(0.0f, (float)VW), ay = frnd(0.0f, (float)VH);
        float wx0 = c.wx(ax), wy0 = c.wy(ay);
        for (int s = 0; s < 8; s++) {                 // arbitrary zoom sequence, same anchor
            c.zoomAt(ax, ay, frnd(1.0f, 4.0f));
            CHECK(fabsf(c.wx(ax) - wx0) < 1e-2f, "zoomAt moved its anchor in x");
            CHECK(fabsf(c.wy(ay) - wy0) < 1e-2f, "zoomAt moved its anchor in y");
        }
    }
    printf("PASS zoomAt anchor invariance\n");
}

static void test_clamp()
{
    for (int k = 0; k < 20000; k++) {
        Camera2D c = random_cam();                    // random_cam already clamps
        CHECK(c.visX() >= -1e-3f && c.visX() + c.visW() <= VW + 1e-3f, "visible rect left the world in x");
        CHECK(c.visY() >= -1e-3f && c.visY() + c.visH() <= VH + 1e-3f, "visible rect left the world in y");
    }
    // zoom 1 (world == view): clamp must land on the exact center -> identity
    Camera2D c(VW, VH, VW, VH);
    c.setZoom(2.0f); c.panScreen(300, -300); c.clampToWorld();
    c.setZoom(1.0f); c.clampToWorld();
    CHECK(c.identity(), "snap to zoom 1 did not restore identity()");
    printf("PASS clampToWorld containment + zoom-1 identity\n");
}

// The seam property, matching gfx_tile_region_world's arithmetic EXACTLY: tile i's right
// edge floor(sx(x + i*tw + tw)) must equal tile i+1's left edge floor(sx(x + (i+1)*tw))
// -- and every snapped tile must keep a positive width so none is skipped.
static void test_tile_seams()
{
    const int tw = 16;
    for (int zi = 1000; zi <= 4000; zi++) {           // zoom 1.000 .. 4.000 in 0.001 steps
        float z = zi / 1000.0f;
        Camera2D c(VW, VH, VW, VH);
        c.setZoom(z);
        c.panScreen(frnd(-200.0f, 200.0f), frnd(-200.0f, 200.0f));   // random pan each step
        c.clampToWorld();
        for (int i = 0; i < VW / tw - 1; i++) {
            float wx0  = 0.0f + i * (float)tw;
            float wx0n = 0.0f + (i + 1) * (float)tw;
            int X1  = (int)floorf(c.sx(wx0 + tw));
            int X0n = (int)floorf(c.sx(wx0n));
            CHECK(X1 == X0n, "seam at zoom %f tile %d (%d vs %d)", z, i, X1, X0n);
            int Wd = X1 - (int)floorf(c.sx(wx0));
            CHECK(Wd >= (int)tw, "zoomed tile narrower than source at zoom %f", z);  // zoom >= 1
        }
    }
    printf("PASS tile corner-snap seam sweep (3001 zooms x 14 edges)\n");
}

static void test_identity_exact()
{
    Camera2D c(VW, VH, VW, VH);
    CHECK(c.identity(), "fresh camera not identity()");
    for (int k = 0; k < 5000; k++) {
        int x = (int)(rnd() % 240), y = (int)(rnd() % 320);
        int w = 1 + (int)(rnd() % 240), h = 1 + (int)(rnd() % 320);
        int X, Y, W, H;
        c.projectRect((float)x, (float)y, (float)w, (float)h, X, Y, W, H);
        CHECK(X == x && Y == y && W == w && H == h,
              "identity projectRect changed (%d,%d,%d,%d)->(%d,%d,%d,%d)", x, y, w, h, X, Y, W, H);
        CHECK(c.sx((float)x) == (float)x && c.sy((float)y) == (float)y, "identity mapping not exact");
    }
    printf("PASS identity exactness\n");
}

// --- pinch scripts -------------------------------------------------------------------------

static Input two(float x1, float y1, float x2, float y2)
{
    Input in{};
    in.down = true; in.points = 2;
    in.x = (int16_t)x1; in.y = (int16_t)y1; in.x2 = (int16_t)x2; in.y2 = (int16_t)y2;
    return in;
}
static Input one(float x, float y) { Input in{}; in.down = true; in.points = 1; in.x = (int16_t)x; in.y = (int16_t)y; return in; }
static Input none() { return Input{}; }

static void test_pinch()
{
    // Spread from 100px to 200px -> zoom 2, and a further spread to 500px clamps at 4.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(70, 160, 170, 160), c);          // engage, d0 = 100
        CHECK(pz.engaged(), "did not engage on two fingers");
        pz.update(two(20, 160, 220, 160), c);          // d = 200
        CHECK(fabsf(c.zoom() - 2.0f) < 1e-3f, "zoom did not track ratio (z=%f)", c.zoom());
        pz.update(two(-130, 160, 370, 160), c);        // d = 500 -> would be 5x
        CHECK(c.zoom() == 4.0f, "zoom not clamped at max (z=%f)", c.zoom());
    }
    // Release near 1x snaps to exactly 1 and identity; release at 2x persists.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(70, 160, 170, 160), c);
        pz.update(two(65, 160, 175, 160), c);          // d = 110 -> z 1.10 (< SNAP_BELOW)
        pz.update(one(65, 160), c);                    // finger lifts
        CHECK(c.zoom() == 1.0f && c.identity(), "near-1x release did not snap to identity");
        CHECK(pz.engaged(), "tail ended before full release");
        pz.update(none(), c);
        CHECK(!pz.engaged(), "did not disengage on full release");

        pz.update(two(70, 160, 170, 160), c);
        pz.update(two(20, 160, 220, 160), c);          // z = 2
        pz.update(one(20, 160), c);
        pz.update(none(), c);
        CHECK(fabsf(c.zoom() - 2.0f) < 1e-3f, "2x did not persist through release (z=%f)", c.zoom());
    }
    // Finger-order swap between frames: distance and midpoint are unchanged, so nothing moves.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(70, 100, 170, 220), c);
        pz.update(two(40, 80, 200, 240), c);           // zoomed in a bit
        float z = c.zoom(), wx = c.wx(120), wy = c.wy(160);
        pz.update(two(200, 240, 40, 80), c);           // SAME fingers, swapped slots
        CHECK(fabsf(c.zoom() - z) < 1e-4f, "slot swap changed zoom");
        CHECK(fabsf(c.wx(120) - wx) < 1e-3f && fabsf(c.wy(160) - wy) < 1e-3f, "slot swap panned");
    }
    // Two-finger drag: the world point under the midpoint follows the fingers exactly
    // (pan-then-zoom composition), while clamping stays out of the way mid-world.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(70, 160, 170, 160), c);
        pz.update(two(20, 160, 220, 160), c);          // z = 2, mid (120,160)
        float grabbed = c.wx(120.0f);
        pz.update(two(30, 160, 230, 160), c);          // same spread, both +10px right
        CHECK(fabsf(c.wx(130.0f) - grabbed) < 1e-2f, "grabbed world point slipped off the midpoint");
    }
    // The tail's leftover finger is inert: no pan, no zoom, whatever it does.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(70, 160, 170, 160), c);
        pz.update(two(20, 160, 220, 160), c);          // z = 2
        float z = c.zoom(), wx = c.wx(0), wy = c.wy(0);
        pz.update(one(20, 160), c);
        pz.update(one(200, 40), c);                    // drags around...
        pz.update(one(10, 300), c);
        CHECK(pz.engaged(), "tail released early");
        CHECK(c.zoom() == z && c.wx(0) == wx && c.wy(0) == wy, "tail finger moved the camera");
    }
    // Re-grab from the tail re-baselines: no jump on contact, then scales from the current zoom.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(70, 160, 170, 160), c);
        pz.update(two(20, 160, 220, 160), c);          // z = 2
        pz.update(one(20, 160), c);                    // tail
        pz.update(two(70, 160, 170, 160), c);          // re-grab at d = 100
        CHECK(fabsf(c.zoom() - 2.0f) < 1e-3f, "re-grab jumped the zoom (z=%f)", c.zoom());
        pz.update(two(20, 160, 220, 160), c);          // d = 200 again
        CHECK(c.zoom() == 4.0f, "re-grab did not scale from the zoom it found (z=%f)", c.zoom());
    }
    // Two points closer than MIN_PINCH_DIST never engage (one finger + a ghost).
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(120, 160, 130, 160), c);
        CHECK(!pz.engaged(), "engaged on a sub-threshold pair");
        CHECK(c.identity(), "ghost pair moved the camera");
    }
    // REGRESSION: a coincident pair must HOLD the zoom, not read as "pinched all the way
    // out". This is the real release frame, not a hypothetical: when the finger the driver
    // put in slot 0 lifts first, the survivor is re-reported in slot 0 while x2/y2 still
    // hold its stale position for the one frame engine/input.cpp bridges -- so the two
    // reported points collapse onto each other. Clamping d and dividing by d0_ turned that
    // into ZOOM_MIN, and the release then snapped it to exactly 1x, wiping the zoom on
    // roughly half of all pinch releases.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(20, 160, 220, 160), c);          // engage, d0 = 200
        pz.update(two(-70, 160, 310, 160), c);         // d = 380 -> 1.9x
        float z = c.zoom();
        CHECK(z > 1.5f, "setup did not zoom in (z=%f)", (double)z);
        pz.update(two(-70, 160, -70, 160), c);         // the bridged frame: d = 0
        CHECK(c.zoom() == z, "degenerate pair moved the zoom (%f -> %f)", (double)z, (double)c.zoom());
        CHECK(pz.engaged(), "degenerate pair ended the gesture");
        pz.update(one(-70, 160), c);                   // ...then the bridge expires
        pz.update(none(), c);
        CHECK(fabsf(c.zoom() - z) < 1e-3f,
              "zoom lost across a one-finger-first release (%f -> %f)", (double)z, (double)c.zoom());
    }
    // Fingers genuinely converging below MIN_PINCH_DIST mid-gesture: same hold, and the
    // gesture stays live, so spreading them again resumes from the zoom it held.
    {
        Camera2D c(VW, VH, VW, VH); PinchZoom pz;
        pz.update(two(20, 160, 220, 160), c);          // engage, d0 = 200
        pz.update(two(-70, 160, 310, 160), c);         // 1.9x
        float z = c.zoom();
        pz.update(two(115, 160, 125, 160), c);         // d = 10 -> too close to be a pinch
        CHECK(c.zoom() == z, "converging fingers moved the zoom");
        pz.update(two(-70, 160, 310, 160), c);         // spread back out to the same d
        CHECK(fabsf(c.zoom() - z) < 1e-3f, "resuming the spread did not return to the held zoom");
    }
    printf("PASS pinch scripts\n");
}

static void test_follow()
{
    // Converges onto a mid-world target at zoom 2 (clamp inactive there).
    {
        Camera2D c(VW, VH, VW, VH);
        c.setZoom(2.0f); c.clampToWorld();
        for (int f = 0; f < 300; f++) c.follow(80.0f, 200.0f, 1.0f / 60.0f, 5.0f);
        CHECK(fabsf(c.wx(VW * 0.5f) - 80.0f) < 0.1f, "follow did not center target x (at %f)", c.wx(VW * 0.5f));
        CHECK(fabsf(c.wy(VH * 0.5f) - 200.0f) < 0.1f, "follow did not center target y");
    }
    // A target near the world edge parks the view AT the edge (never outside the world).
    {
        Camera2D c(VW, VH, VW, VH);
        c.setZoom(4.0f); c.clampToWorld();
        for (int f = 0; f < 300; f++) c.follow(5.0f, 5.0f, 1.0f / 60.0f, 5.0f);
        CHECK(c.visX() >= -1e-3f && c.visY() >= -1e-3f, "follow chased a target out of the world");
        CHECK(fabsf(c.visX()) < 0.1f && fabsf(c.visY()) < 0.1f, "follow did not park at the edge");
    }
    // At zoom 1 the clamp re-centers every frame: a follow camera at rest stays identity.
    {
        Camera2D c(VW, VH, VW, VH);
        c.follow(30.0f, 40.0f, 1.0f / 60.0f, 5.0f);
        CHECK(c.identity(), "zoom-1 follow broke identity()");
    }
    printf("PASS follow (converge, edge clamp, zoom-1 identity)\n");
}

// zoomOnly mode (Home): the pinch sets the level about the camera's center; the midpoint
// neither anchors nor pans, so a follow target owns the framing.
static void test_pinch_zoom_only()
{
    Camera2D c(VW, VH, VW, VH); PinchZoom pz; pz.zoomOnly = true;
    c.setZoom(2.0f); c.clampToWorld();                 // zoomed, so the clamp allows off-center
    for (int f = 0; f < 300; f++) c.follow(80.0f, 200.0f, 1.0f / 60.0f, 5.0f);   // settle on a pet
    pz.update(two(70, 160, 170, 160), c);              // engage, d0 = 100 at z0 = 2
    pz.update(two(45, 100, 245, 100), c);              // d = 200, midpoint moved a lot
    CHECK(c.zoom() == 4.0f, "zoomOnly did not track ratio to the cap (z=%f)", c.zoom());
    CHECK(fabsf(c.wx(VW * 0.5f) - 80.0f) < 0.5f && fabsf(c.wy(VH * 0.5f) - 200.0f) < 0.5f,
          "zoomOnly moved the center (midpoint must not anchor or pan)");
    pz.update(two(95, 160, 145, 160), c);              // d = 50 -> z = 2*0.5 = 1 (min-clamped)
    pz.update(one(95, 160), c);                        // release under SNAP_BELOW
    CHECK(c.zoom() == 1.0f && c.identity(), "zoomOnly release near 1x did not snap to identity");
    printf("PASS pinch zoomOnly mode\n");
}

static void test_culling()
{
    Camera2D c(VW, VH, VW, VH);
    c.zoomAt(120, 160, 4.0f);                          // centered 4x: visible = (90,120)-(150,200)
    c.clampToWorld();
    CHECK(!c.visible(10, 10, 20, 20), "off-view sprite reported visible at 4x");
    CHECK(c.visible(115, 155, 10, 10), "center sprite reported hidden");
    CHECK(c.visible(85, 155, 10, 10), "edge-straddling sprite culled");   // overlaps at x 90
    Camera2D id(VW, VH, VW, VH);
    CHECK(id.visible(0, 0, 1, 1) && id.visible(239, 319, 1, 1), "identity culled on-screen content");
    printf("PASS culling\n");
}

int main()
{
    test_roundtrip();
    test_zoom_anchor();
    test_clamp();
    test_tile_seams();
    test_identity_exact();
    test_pinch();
    test_follow();
    test_pinch_zoom_only();
    test_culling();
    if (g_fail) { printf("%d FAILURE(S)\n", g_fail); return 1; }
    printf("all camera/pinch properties hold\n");
    return 0;
}
