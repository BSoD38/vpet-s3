#pragma once
#include <cstdint>

// Battery gauge: turns the filtered terminal voltage from drivers/BAT_Driver into the
// numbers the UI shows, and infers whether a charger is plugged in.
//
// The board charges over USB but gives the MCU NO charge-status line -- the "charging"
// LED is wired straight to the charger chip, and the only battery signal that reaches the
// ESP32 is the ADC on GPIO8. So charging has to be read out of the voltage itself, which
// is what this module does (see battery.cpp for how, and what it can and cannot see).
//
// It also fixes the gauge jumping the moment you plug in, and reaching 100% on a charger
// whose plateau this board reads as 4.17 V rather than a textbook 4.20 V. Both come from the
// same place: instead of assuming what a full pack reads, the gauge LEARNS it -- once while
// charging (the charger's plateau) and once on battery (the same pack under our own load) --
// and slides the discharge curve to meet whichever applies. That calibrates the divider and
// cancels the charging offset in one step; a rate limit absorbs the rest.
struct BatteryState {
    float volts    = 0.0f;   // filtered terminal voltage
    int   pct      = -1;     // what the UI shows: curve, calibrated, rate-limited
    int   pctRaw   = -1;     // the same value before rate limiting (About screen)
    bool  charging = false;  // a charger appears to be attached
    bool  full     = false;  // the charge has plateaued -- charging is essentially done
    // Learned calibration, for the About screen: what a full pack reads on this board while
    // charging, and while running on battery. `calibrated` is false while these are still
    // the built-in assumptions (no full charge has been observed yet).
    float vFull     = 0.0f;
    float vLoadFull = 0.0f;
    bool  calibrated = false;
};

// Advance the gauge by `dt` seconds. Called from the 100 ms driver task in main.cpp, right
// after BAT_Get_Volts() has refreshed the filtered reading.
void battery_poll(float dt);

// Latest state, safe to call from any task (each field is published atomically).
BatteryState battery_state();
