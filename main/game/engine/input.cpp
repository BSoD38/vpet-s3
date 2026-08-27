#include "input.hpp"
#include "gfx.hpp"        // GAME_W / GAME_H (for optional coord flipping)
#include "drivers.hpp"    // tp, esp_lcd_touch_read_data/get_coordinates
#include "esp_timer.h"

// Capacitive touch controllers occasionally report "no touch" for a frame in the
// middle of a real press. Treated naively that looks like release-then-press and
// fires a second "pressed" edge. We deglitch (require several consecutive empty
// reads before calling it released) and rate-limit accepted presses.
static const int     RELEASE_FRAMES = 2;        // consecutive empty reads => really up (bridges 1-frame dropouts)
static const int64_t DEBOUNCE_US    = 70000;    // min gap between accepted presses (~just kills contact chatter;
                                                //  low enough that deliberate re-taps / button switches register)
// The second finger's own bridge. Deliberately the SAME two-frame length as the primary's
// (1 with `>` is 2 with `>=`): a shorter one would drop `points` on any single-frame
// dropout of the second contact, and PinchZoom treats that as a release -- which below
// SNAP_BELOW snaps the zoom to exactly 1x, right where every pinch-out starts. The frame
// of stale x2/y2 this bridge exports at a real release is handled where it lands, by the
// degenerate-pair hold in engine/pinch.hpp.
static const int     RELEASE_FRAMES_2 = 1;

void InputManager::poll(Input& in)
{
    bool raw = false;
    uint16_t x[2] = {0, 0}, y[2] = {0, 0};
    uint8_t n = 0;

    if (tp) {
        esp_lcd_touch_read_data(tp);
        // Exactly ONE get_coordinates per poll: the driver invalidates its stored points
        // after every read, so a second call in the same frame would see nothing.
        if (esp_lcd_touch_get_coordinates(tp, x, y, nullptr, &n, 2) && n > 0) {
            raw = true;
            lastx_ = (int16_t)x[0];
            lasty_ = (int16_t)y[0];
            // If on-screen buttons respond at the wrong spot, flip here to match
            // the LovyanGFX display orientation:
            //   lastx_ = GAME_W - 1 - lastx_;  lasty_ = GAME_H - 1 - lasty_;
        }
        if (n >= 2) {
            emptyCount2_ = 0;
            last2x_ = (int16_t)x[1];
            last2y_ = (int16_t)y[1];
            // The second finger is deglitched on the way UP as well, which the primary is
            // not: a lone frame reporting two points is usually a ghost (palm edge, contact
            // chatter), and scenes stand their single-finger gestures down on `points >= 2`
            // -- so acting on one frame of it would cancel an in-progress rub and swallow
            // taps for the rest of the touch. One frame (~23 ms) of latency before a pinch
            // engages is imperceptible; a rub dying at random is not. Only the false->true
            // edge is gated, so a mid-pinch dropout still rides the release bridge below.
            if (raw2_) stable2_ = true;
            raw2_ = true;
        } else {
            raw2_ = false;
            // Latched: the counter stops once it has released, so it cannot run away
            // over a long uninterrupted uptime.
            if (emptyCount2_ <= RELEASE_FRAMES_2 && ++emptyCount2_ > RELEASE_FRAMES_2)
                stable2_ = false;
        }
    }

    bool prevStable = stable_;
    if (raw) {
        emptyCount_ = 0;
        stable_ = true;
    } else if (emptyCount_ < RELEASE_FRAMES && ++emptyCount_ >= RELEASE_FRAMES) {
        stable_ = false;   // bridge brief dropouts; only release after N empties (the
    }                      // count latches there, so it can't run away while idle)

    in.down = stable_;
    in.x = lastx_;
    in.y = lasty_;
    in.released = !stable_ && prevStable;

    // Two fingers only count while the primary is (deglitched-)down too: a lone "second"
    // point with no first is controller noise.
    in.points = stable_ ? (uint8_t)(stable2_ ? 2 : 1) : (uint8_t)0;
    in.x2 = last2x_;
    in.y2 = last2y_;

    in.pressed = false;
    if (stable_ && !prevStable) {                    // rising edge
        int64_t now = esp_timer_get_time();
        if (now - lastPressUs_ >= DEBOUNCE_US) {     // rate-limit accepted presses
            in.pressed = true;
            lastPressUs_ = now;
        }
    }

    // Motion snapshot. Accel is kept fresh (~10Hz) by the background sensor task
    // (main.cpp Driver_Loop -> QMI8658_Loop); we just copy the latest values so scenes get
    // a per-frame snapshot consistent with the touch state. A torn read across x/y/z is at
    // worst a one-frame blip -- harmless for game input, so no locking needed.
    in.ax = Accel.x; in.ay = Accel.y; in.az = Accel.z;
}
