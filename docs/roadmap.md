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

**Moved out — designed in [economy-and-inventory.md](economy-and-inventory.md).** Currency
(**Bits**), a shop, an inventory, toys, room decor and the wish/gift loop that ties the bag to
the conversation system. Battle rewards stay scoped to *stat gain + friendship + win count*;
Bits are added to them in phase E1.

## Multi-creature ownership / teams

Owning more than one creature, and (later) battle teams with switching. The **attribute
triangle** in the battle system is the payoff — type coverage turns "which do I bring?" into a
real decision. The battle core's `Combatant` abstraction is built so a team/switch mechanic
slots in without a combat rewrite.

## Audio — more formats, and sound as content

The [sound engine](sound-engine.md) reads WAV (PCM + IMA ADPCM), MP3 and tracker modules
(MOD/XM/S3M/IT via vendored libxmp-lite), and has a chip synth. What is left open behind the
`Decoder` seam in [`decoder.hpp`](../main/game/engine/audio/decoder.hpp) — a subclass plus one
line in `decoder_open()`:

- **Ogg Vorbis** — skipped because Tremor costs ~100 KB of code and 30–40% of a core for an
  advantage over MP3 (licensing) that is moot with Helix already vendored.

The **soundtrack itself** is the bigger gap: the base game ships RTTTL chip tunes because they
cost 4 KB, and now that modules play, an actual composed score is a content job rather than an
engine one. Note that redistributing someone else's module in `base.pak` needs permission —
anything a player drops in `/sdcard/sounds/` is their business, not the project's.

On the content side, [creature voices](sound-engine.md#creature-voices) already spend that
data-driven bank — `voicePitch`, a voice family, and an optional `<creature dir>/sounds/` set
per species — but the base game only ships the four families. Authored cries for the roster,
per-food eating sounds and mood-driven pitch on the pet's voice are all still reachable without
engine changes.

## Wild encounters (IMU pedometer)

Random encounters whose frequency ties to real-world activity via a step counter on the
QMI8658 IMU (the M1 plan already sketches a pedometer) — a more organic opponent source
alongside the Quick Battle / Tower modes.
