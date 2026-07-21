# Training & Energy — Design Doc

Status: **design captured; Phase 0 plumbing implemented** (field + regen + accessors).
The gate itself (energy cost on the training minigames) lands with the battle system's
Phase 3. See also: [battle-system.md](battle-system.md), [roadmap.md](roadmap.md).

## Purpose

Stat growth needs a brake, or a player maxes a baby's stats on day one and the real-time
raising loop stops mattering. "Rest" is repurposed into an **energy/stamina** resource that
gates *training* — you spend energy to train, then rest to recover it, so growth spreads
across days and the RTC-based aging carries weight.

This is deliberately **separate from combat**: battles are gated by HP, not energy. Energy
only limits stat *training*.

## Model

- `energy` — 0–100, stored in `PetState` ([`pet.hpp`](../main/game/sim/pet.hpp)); starts full
  at birth.
- **Regenerates over real time** on the same RTC/offline-aging path as hunger/health, so it
  refills while the device is off (advanced during `Pet::boot()` catch-up):
  - awake — `ENERGY_REGEN_HR` (~8h to refill)
  - asleep — `ENERGY_REGEN_SLEEP_HR` (faster; rest is the efficient way to recover)
- **Training costs energy.** The minigames already grant stats (the runner trains Agi+HP,
  [`scene_run.hpp`](../main/game/scenes/minigames/scene_run.hpp)); they become energy-gated —
  no energy, no gains. `Pet::spendEnergy(amount)` deducts if available and returns false when
  there isn't enough.

## Implementation status

- **Done (Phase 0):** `energy` field + `PET_VERSION` 5→6 bump; regen in `Pet::tick()`; init in
  `Pet::newEgg()`; `Pet::energy()` / `Pet::spendEnergy()` accessors.
- **Pending (battle Phase 3):** charge energy on the training minigames and gate their
  rewards; show an energy bar on the Stats screen.

## Open tuning knobs

- Energy max and regen rates (awake vs. asleep).
- Energy cost per training session, and whether cost scales with the size of the stat gain.
- Whether a fully-drained pet blocks training entirely or just yields diminished gains.
