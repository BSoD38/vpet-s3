#include "input.hpp"
#include "gfx.hpp"        // GAME_W / GAME_H (for optional coord flipping)
#include "drivers.hpp"    // tp, esp_lcd_touch_read_data/get_coordinates
#include "esp_timer.h"

// Capacitive touch controllers occasionally report "no touch" for a frame in the
// middle of a real press. Treated naively that looks like release-then-press and
// fires a second "pressed" edge. We deglitch (require several consecutive empty
// reads before calling it released) and rate-limit accepted presses.
static const int     RELEASE_FRAMES = 3;        // consecutive empty reads => really up (~75ms @ 40fps)
static const int64_t DEBOUNCE_US    = 250000;   // min gap between accepted presses

void InputManager::poll(Input& in)
{
    bool raw = false;
    uint16_t x[1] = {0}, y[1] = {0};
    uint8_t n = 0;

    if (tp) {
        esp_lcd_touch_read_data(tp);
        if (esp_lcd_touch_get_coordinates(tp, x, y, nullptr, &n, 1) && n > 0) {
            raw = true;
            lastx_ = (int16_t)x[0];
            lasty_ = (int16_t)y[0];
            // If on-screen buttons respond at the wrong spot, flip here to match
            // the LovyanGFX display orientation:
            //   lastx_ = GAME_W - 1 - lastx_;  lasty_ = GAME_H - 1 - lasty_;
        }
    }

    bool prevStable = stable_;
    if (raw) {
        emptyCount_ = 0;
        stable_ = true;
    } else if (++emptyCount_ >= RELEASE_FRAMES) {
        stable_ = false;   // bridge brief dropouts; only release after N empties
    }

    in.down = stable_;
    in.x = lastx_;
    in.y = lasty_;
    in.released = !stable_ && prevStable;

    in.pressed = false;
    if (stable_ && !prevStable) {                    // rising edge
        int64_t now = esp_timer_get_time();
        if (now - lastPressUs_ >= DEBOUNCE_US) {     // rate-limit accepted presses
            in.pressed = true;
            lastPressUs_ = now;
        }
    }
}
