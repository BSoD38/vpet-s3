#pragma once
#include <cstdint>
// Host stand-in for the ESP hardware RNG. Only engine/util.hpp's cosmetic randf() uses it;
// the battle core's own logic runs off its seeded xorshift, so nothing measurable here
// depends on this being any good.
static inline uint32_t esp_random()
{
    static uint32_t s = 0x9E3779B9u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
