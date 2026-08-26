#pragma once
#include <cstdint>
// Host shim for sim/creatures.hpp. The real header pulls in engine/display.hpp and with it
// all of LovyanGFX, none of which the battle core touches -- it reads the attribute enum and
// the base-stat fields, and that is the whole dependency.
enum Attribute : uint8_t {
    ATTR_FREE = 0,
    ATTR_VACCINE,
    ATTR_DATA,
    ATTR_VIRUS,
};

struct Creature {
    char     id[24];
    char     name[24];
    uint8_t  tier;
    uint8_t  attribute;
    uint32_t baseHp;
    uint16_t baseStr, baseEnd, baseAgi, baseInt;
};
