#pragma once
#include <cstdint>
#include "sim/pet.hpp"   // StatId, STAT_COUNT

// A battle participant: an effective-stats snapshot decoupled from the live Pet/sim,
// plus the mutable per-fight state (HP, ATB charge, special meter). The battle core
// only ever touches Combatants, so the same code drives the player, an AI enemy, or
// (later) a BLE peer — see docs/battle-system.md.
struct Combatant {
    char     name[24];
    uint8_t  attribute;          // Attribute (type triangle); ATTR_FREE = neutral
    uint32_t stat[STAT_COUNT];   // EFFECTIVE stats snapshot (indexed by StatId)
    int      spriteIdx;          // registry index for rendering (Phase 2); -1 = none

    // --- per-fight mutable state (atb/meter reset by Battle::begin; hp/maxHp seeded here) ---
    int64_t  hp;                 // current battle HP (0..maxHp)
    uint32_t maxHp;              // pool size = effective MAXHP (the HP-bar denominator)
    float    atb;                // 0..1 ATB charge
    float    meter;              // 0..1 special meter
    float    aiSkill;            // 0..1 timing skill for auto/AI control (player = tap-driven)
};

class Pet;
struct Creature;

// Build a Combatant from the player's Pet: effective stats, and current health as the
// starting fraction of the MAXHP pool (the unified-HP model). aiSkill is set to 1.0 but
// the player's real taps override synthesized timing in a manual-controlled battle.
Combatant combatant_from_pet(const Pet& pet);

// Build an AI enemy from a registry creature. statScale multiplies the creature's base
// stats (tune difficulty / tier); aiSkill sets how well the AI times its strikes/parries.
Combatant combatant_from_creature(const Creature& c, int idx, float statScale, float aiSkill);
