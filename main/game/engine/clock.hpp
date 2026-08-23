#pragma once
#include <cstdint>
#include "drivers.hpp"   // datetime_t

// Wall-clock seconds derived from the battery-backed RTC (PCF85063). Absolute
// value need not be correct; only deltas matter (used for real-time aging).

// A reading below this year cannot be one the player set: the PCF85063 comes up at
// 1970-01-01 whenever it loses power (no backup battery, or a flat one) and then keeps
// counting from there for as long as the board stays on -- a plausible-looking number
// that is pure fiction. Treating it as a real time is what let a reset clock be written
// into the save as an aging baseline, so the next boot (clock finally set) measured the
// whole multi-decade jump as time the player had spent away.
static const int CLOCK_YEAR_MIN = 2020;

uint32_t clock_now(void);                 // unix-ish seconds, 0 if RTC time looks invalid
uint32_t clock_elapsed(uint32_t since);   // clock_now()-since, clamped to >= 0
datetime_t clock_datetime(void);          // torn-read-safe snapshot of the RTC global

// Seconds for an EXPLICIT datetime (0 if it is not a valid one), for callers that must not
// go through clock_now(): right after writing the RTC, the shared `datetime` global still
// holds the old time until the core-0 driver task refreshes it (~100 ms later).
uint32_t clock_epoch(const datetime_t& t);
