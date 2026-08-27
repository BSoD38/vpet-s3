#pragma once
#include <cstdint>

// Touch input state + a motion snapshot, sampled once per frame. Single-touch API
// (down/pressed/released and x/y follow the FIRST finger, as always) plus a second touch
// point for the pinch gesture (engine/pinch.hpp).
struct Input {
    bool    down;      // finger currently on screen
    bool    pressed;   // went down this frame (rising edge)
    bool    released;  // lifted this frame (falling edge)
    int16_t x, y;      // last known coordinates (screen space)

    // Multi-touch: how many fingers are on screen (0..2 -- the poll only asks the
    // controller for two; both boards' controllers track five) and where the second one
    // is. points has NO edge flags on purpose: gestures gate on `points >= 2` because the
    // hardware gives no finger IDs, so "which finger arrived/left" is unknowable -- see
    // engine/pinch.hpp for the gesture built on that constraint.
    uint8_t points;    // deglitched finger count; x2/y2 are valid while it is >= 2
    int16_t x2, y2;    // second finger (screen space, same mapping as x/y)

    // Motion (QMI8658 accelerometer), copied each frame from the background sensor task's
    // global (see main.cpp Driver_Loop / engine/drivers.hpp). In g (±4g range; the
    // down-pointing axis reads ~1.0 at rest -> gives tilt via the gravity vector, and shake
    // via magnitude). Games calibrate thresholds empirically. (Gyro is not plumbed through:
    // no game uses it, so it isn't read/copied -- add it back here if one ever needs it.)
    float   ax, ay, az;   // accelerometer (g)
};

// Polls the CST328 touch controller and produces deglitched/debounced edges.
class InputManager {
    bool    stable_ = false;    // deglitched "finger down" state
    int     emptyCount_ = 0;
    int16_t lastx_ = 0, lasty_ = 0;
    int64_t lastPressUs_ = 0;
    // Second finger, tracked separately: same presence bridge as the primary (see
    // input.cpp for why it is not shorter), and no debounce, because it produces no edges
    // -- only the `points` count.
    bool    stable2_ = false;
    bool    raw2_ = false;       // previous poll also saw two points (rise-side deglitch)
    int     emptyCount2_ = 0;
    int16_t last2x_ = 0, last2y_ = 0;
public:
    void poll(Input& in);
};

// Buttons are hit-tested with a few px of "slop": a flat-thumb press reports a centroid
// that can land just outside a small target, so we grow the rect slightly on every side.
// Kept small (2px) so tightly-spaced controls (e.g. the Home care bar with 5px gaps, the -/+
// steppers, the speed grid) stay unambiguous: two adjacent rects only overlap when their gap
// is < 2*TOUCH_SLOP (4px), and every laid-out control keeps a gap >= that.
inline constexpr int TOUCH_SLOP = 2;

// True if (px,py) falls within the rect [x,y,w,h], expanded by TOUCH_SLOP on each side.
inline bool hit(int px, int py, int x, int y, int w, int h) {
    return px >= x - TOUCH_SLOP && px < x + w + TOUCH_SLOP &&
           py >= y - TOUCH_SLOP && py < y + h + TOUCH_SLOP;
}
