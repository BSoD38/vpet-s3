#pragma once
#include <cstdint>

// Single-touch input state + a motion snapshot, sampled once per frame.
struct Input {
    bool    down;      // finger currently on screen
    bool    pressed;   // went down this frame (rising edge)
    bool    released;  // lifted this frame (falling edge)
    int16_t x, y;      // last known coordinates (screen space)

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
