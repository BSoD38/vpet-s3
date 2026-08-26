#pragma once
#include <cstdint>

class SaveStore;   // sim/save.hpp

// Device power state. This is DISTINCT from the pet's own day/night "lightsOff":
// the pet can be asleep on its schedule while the device is fully Active, and vice
// versa. Deep sleep is not a mode we sit in here (the CPU is off) — it's an action.
enum class PowerMode : uint8_t { Active, Light };

// Result of PowerManager::update(): App applies the display/pet side effects so the
// manager stays free of scene/rendering knowledge. EnterDeep/PowerOff do not return.
enum class PowerAction : uint8_t { None, EnterLight, ExitLight, EnterDeep, PowerOff };

// Periodic deep-sleep wake cadence: the CPU wakes this often to advance care +
// timekeeping and test the wake triggers, then either surfaces or drops back to sleep.
constexpr uint32_t POWER_DEEP_POLL_S = 15 * 60;

// ---- screen settings (Settings -> SCREEN; live values backed by NVS) ----

// How long the device sits with no touch and no button before the screen turns off.
// Four choices, 15s..60s. Kept here rather than in the scene because the inactivity
// timer that consumes it lives in PowerManager::update() below.
constexpr uint16_t SCREEN_OFF_MIN_S  = 15;
constexpr uint16_t SCREEN_OFF_MAX_S  = 60;
constexpr uint16_t SCREEN_OFF_STEP_S = 15;
constexpr uint16_t SCREEN_OFF_DEF_S  = 60;
constexpr int      SCREEN_OFF_N =
    (SCREEN_OFF_MAX_S - SCREEN_OFF_MIN_S) / SCREEN_OFF_STEP_S + 1;   // 4 steps

// Panel brightness, in percent. The floor is the whole reason this setting needs one: a
// player who drags to 0 blacks the screen out, and the control that would undo it is ON
// that screen. Anything at or above the floor is still readable in a dark room.
constexpr uint8_t SCREEN_BRIGHT_MIN = 20;
constexpr uint8_t SCREEN_BRIGHT_MAX = 100;
constexpr uint8_t SCREEN_BRIGHT_DEF = 70;

// Read both settings out of NVS into the live values. Call from app_main BEFORE
// LCD_Init(), so Backlight_Init() lights the panel at the player's level instead of
// flashing the default and then correcting itself.
void screen_settings_load(const SaveStore& save);
void screen_settings_store(const SaveStore& save);   // both keys, one commit

uint16_t screen_off_s();
void     set_screen_off_s(uint16_t s);   // clamped to the range and snapped to the step

uint8_t  screen_brightness();
// Records the player's brightness -- the level Pet::applyBacklight() treats as "lights
// on". It deliberately does NOT drive the panel itself: the pet owns the final level,
// because it also owns the night dim. Callers apply it via Pet::refreshBacklight().
void     set_screen_brightness(uint8_t pct);

// ---- boot-path entry points (called from app_main, before the game task) ----

// Release any pads frozen by the deep-sleep hold and re-assert the power latch so the
// board stays powered. Call FIRST in app_main.
void power_early_init();

// True if this boot is a periodic RTC-timer wake from deep sleep (vs. a PWR-key wake,
// a fresh power-on, or a normal reset).
bool power_woke_from_timer();

// Headless periodic-wake service: replays the pet's offline catch-up over the elapsed
// sleep interval (same code as a normal boot) and tests the wake triggers (evolution /
// low hunger / low happiness / low HP, edge-detected). If nothing fired it persists and
// re-enters deep sleep (never returns); if something fired it returns so app_main can
// do a full display wake. Requires NVS + I2C + RTC already initialised.
void power_service_timer_wake(SaveStore& save);

// ---- sleep/backlight actions (safe whether or not the display is up) ----
[[noreturn]] void power_enter_deep_sleep(uint32_t interval_s = POWER_DEEP_POLL_S);
[[noreturn]] void power_off();          // release latch (battery: off); USB fallback: deep sleep
void power_enter_light();               // panel dark, screen off (sim keeps running)
void power_exit_light();                // panel + backlight back on
void power_mark_display_ready();        // App tells us the LovyanGFX panel is initialised

// ---- active-loop power manager: mode + PWR-button gestures + inactivity timeout ----
class PowerManager {
public:
    void begin(int64_t nowUs);

    // One iteration. touchActivity = a touch occurred this frame; sleepAllowed = the
    // current scene permits sleeping (false in minigame/battle). Returns the action App
    // must apply. Power-off (long hold) is allowed regardless of sleepAllowed.
    PowerAction update(bool touchActivity, bool sleepAllowed, int64_t nowUs);

    PowerMode mode() const { return mode_; }

private:
    PowerMode mode_      = PowerMode::Active;
    bool    btnPrev_     = false;   // debounced button level last frame
    bool    btnConsumed_ = false;   // this press already used (e.g. to wake) -> ignore on release
    int64_t btnDownUs_   = 0;       // timestamp the current press began
    int64_t lastActUs_   = 0;       // last user activity (touch or button)
};
