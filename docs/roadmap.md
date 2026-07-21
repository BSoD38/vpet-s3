# Roadmap — Deferred / Future Systems (non-battle)

Cross-cutting features that surfaced while designing the battle system but are **their own
systems**, not battle mechanics. Parked here so the feature docs stay focused. Battle-specific
future work (classic spectate mode, BLE PvP, extra battle modes, the heal-frequency limiter)
lives in [battle-system.md](battle-system.md); the training/energy gate lives in
[training-and-energy.md](training-and-energy.md).

## Save-migration system

While the game is early WIP, destructive save changes are acceptable — a `PET_VERSION` bump
wipes to a new egg (`Pet::boot()` accepts a save only on an exact version match,
[`pet.cpp`](../main/game/sim/pet.cpp)). **Once the systems stabilize**, add a real versioned
migration path so updates preserve players' pets instead of resetting them.

## Economy — money / shop / items

Currency earned from battles (and other activities), a shop, and item drops. Battle rewards
were deliberately scoped to *stat gain + friendship + win count* for v1; money is deferred to
this system.

## Multi-creature ownership / teams

Owning more than one creature, and (later) battle teams with switching. The **attribute
triangle** in the battle system is the payoff — type coverage turns "which do I bring?" into a
real decision. The battle core's `Combatant` abstraction is built so a team/switch mechanic
slots in without a combat rewrite.

## Wild encounters (IMU pedometer)

Random encounters whose frequency ties to real-world activity via a step counter on the
QMI8658 IMU (the M1 plan already sketches a pedometer) — a more organic opponent source
alongside the Quick Battle / Tower modes.
