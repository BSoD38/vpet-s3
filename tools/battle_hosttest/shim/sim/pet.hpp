#pragma once
#include <cstdint>
#include "sim/creatures.hpp"
// Host shim for sim/pet.hpp. battle/combatant.hpp needs StatId/STAT_COUNT, and battle.cpp's
// combatant_from_pet() needs the five accessors below -- nothing else of the live sim.
enum StatId : uint8_t {
    STAT_MAXHP = 0,
    STAT_STR,
    STAT_END,
    STAT_AGI,
    STAT_INT,
    STAT_COUNT
};

struct PetState { float health = 100.0f; };

class Pet {
public:
    const char*     displayName()   const { return name_; }
    const Creature* creature()      const { return cr_; }
    int             creatureIndex() const { return idx_; }
    uint32_t        stat(StatId s)  const { return stat_[s]; }
    const PetState& state()         const { return st_; }

    char      name_[24] = "Host";
    const Creature* cr_ = nullptr;
    int       idx_ = -1;
    uint32_t  stat_[STAT_COUNT] = {};
    PetState  st_;
};
