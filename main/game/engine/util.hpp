#pragma once
#include <cstdint>
#include "esp_random.h"

// Small shared numeric helpers (kept header-only so any layer can use them cheaply).

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Uniform random float in [0,1). Hardware RNG backed (non-deterministic) — fine for
// cosmetic variation; the battle core keeps its own seeded RNG for reproducible logic.
inline float randf() { return (float)(esp_random() & 0xFFFF) / 65535.0f; }
