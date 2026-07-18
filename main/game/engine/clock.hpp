#pragma once
#include <cstdint>
#include "drivers.hpp"   // datetime_t

// Wall-clock seconds derived from the battery-backed RTC (PCF85063). Absolute
// value need not be correct; only deltas matter (used for real-time aging).
uint32_t clock_now(void);                 // unix-ish seconds, 0 if RTC time looks invalid
uint32_t clock_elapsed(uint32_t since);   // clock_now()-since, clamped to >= 0
datetime_t clock_datetime(void);          // torn-read-safe snapshot of the RTC global
