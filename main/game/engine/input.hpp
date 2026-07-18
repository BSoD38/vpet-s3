#pragma once
#include <cstdint>

// Single-touch input state, sampled once per frame.
struct Input {
    bool    down;      // finger currently on screen
    bool    pressed;   // went down this frame (rising edge)
    bool    released;  // lifted this frame (falling edge)
    int16_t x, y;      // last known coordinates (screen space)
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

// True if (px,py) falls within the rect [x,y,w,h].
inline bool hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
