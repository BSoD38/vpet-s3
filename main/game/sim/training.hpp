#pragma once
#include "pet.hpp"       // same-folder include (training moved into sim/ alongside pet)
#include "economy.hpp"   // Bits are paid here too: one funnel, so no game can forget to pay

// Shared training-reward helper for the minigames.
//
// Every stat-training minigame ends the same way: convert its performance into desired
// stat gains, apply the energy gate (see docs/training-and-energy.md), train the stats,
// nudge friendship, and persist. This factors that pattern out of the individual scenes so
// a new minigame is just "its own loop + a small StatGain table + a cost" -- the growth
// *rate* is entirely in the table each game builds, so different games can train one or
// several stats at whatever pace suits them.

// One desired stat gain BEFORE the energy gate. A game fills a small array of these from
// its score/performance. Multiple entries for the same stat are summed.
struct StatGain {
    StatId stat;
    int    amount;   // points to add (pre-gate; negative/zero is ignored)
};

// What actually happened, for the game-over card.
struct TrainingResult {
    bool  tired;                 // energy limited the gains (scale ratio < 1)
    int   granted[STAT_COUNT];   // actual points added, indexed by StatId
    int   friendship;            // friendship added (never energy-gated)
    float energySpent;           // stamina drained
    uint32_t bits;               // Bits earned (scales with the energy actually spent)
};

// Apply a finished training session. `energyCost` is what a full-value session costs; if
// the pet has less stamina than that, EVERY stat gain scales down by have/cost (and only
// what's left is drained) -- a tired pet still trains, just less. `friendshipBonus` is
// added unconditionally (sharing the game always builds the bond). Persists via markSaved.
// Bits are paid through the SAME funnel as the stats, and for the same reason: five games
// each remembering to credit the wallet is five chances to forget one. The payout scales
// with the energy actually spent, so a tired pet earns proportionally less -- which is what
// lets minigame income re-use the energy gate as its limiter instead of needing a daily cap
// (docs/economy-and-inventory.md 1, rule 4).
TrainingResult grant_training(Pet& pet, Economy& econ, float energyCost,
                              const StatGain* gains, int n, int friendshipBonus);
