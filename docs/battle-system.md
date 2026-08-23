# Battle System — Design Doc

Status: **core v1 implemented & flashed — Phases 0–4 done** (data model, headless core,
animated battle scene, stakes/rewards/energy gate, Quick Battle + Tower modes). Remaining:
Phase 5 playtest tuning + event-driven SFX/sprite hooks, then the deferred backlog below.
Formulas and numbers here are *design intent and tuning knobs*, not final constants.

See also: [training-and-energy.md](training-and-energy.md) (the training gate that battles
deliberately don't use) and [roadmap.md](roadmap.md) (non-battle future systems — economy,
multi-creature ownership, wild encounters, save migration).

## Vision

Turn the virtual pet into something you also *fight* with. Battles are the natural sink for
the RPG stats the pet already carries ([`pet.hpp`](../main/game/sim/pet.hpp)) and the natural
source of the stat/friendship growth that drives evolution. Combat is **player-active** (you
lean in and use your thumbs), single-player PvE first, with device-to-device PvP over BLE as a
long-term north star.

Battle and care **feed each other**: HP is spent in fights and recovered through the care loop
(feed / heal / sleep). A rough fight sends you back to caring for the pet before the next one.

## Decisions (locked)

| Area | Decision |
|---|---|
| Feel | Timed-strike, **ATB charge bars**, real-time, player-active |
| Offense | Pre-armed **Strike** (STR) / **Special** (INT), executed via a timing minigame |
| Defense | Reactive **parry** window (AGI). *Not* a menu action |
| HP | **One HP system**: `health` (0–100) is HP%; battle pool = `health% × effective MAXHP` |
| HP recovery | The existing care loop (feed/heal/sleep/time). **No separate battle meter** |
| Battle gate | **HP only** (training is gated separately by energy — [training-and-energy.md](training-and-energy.md)). Backed by a future heal-frequency limiter |
| Stakes (win) | Exit with remaining HP%; then a sickness/injury roll scaled by HP% |
| Stakes (loss) | Friendship ↓, happiness ↓, **HP → 1%**; sickness roll then near-certain |
| Types | **Vaccine → Data → Virus → Vaccine** triangle, ~±25% |
| Balance | Moderate skill band; **hard tier wall** via stat-differential + MAXHP range |
| Rewards (win) | Small stat gain, friendship ↑, `wins++` (evolution gate). Bits: phase E1 of [economy-and-inventory.md](economy-and-inventory.md) |
| Modes (v1) | **Quick Battle** (random similar-tier foe) + **Tower** (climbing ladder) |
| Architecture | Event-emitting core over a `Combatant` abstraction |

## Moment-to-moment combat

Screen (240×320 portrait): enemy up top, your pet at the bottom. Each has an **HP bar** and an
**ATB charge bar**; your pet also shows a **Special meter**.

1. **Both ATB bars charge concurrently, live.** Fill rate is set by **AGI** — no pausing.
2. **Offense is pre-armed.** The two attack buttons (**Strike** / **Special**) are on screen
   the whole time. **Strike is armed by default.** You tap **Special** to switch your queued
   move (selectable only when its meter is full), and you can flip your choice anytime while
   charging. The armed button stays highlighted. As your gauge nears full, the buttons ramp up
   (brighten/pulse) to cue "execution's coming."
3. **When your bar fills, the armed move fires instantly** into a **timing minigame** — a ring
   closes toward a target. Tap on the beat:
   - *perfect* → crit (~1.75×)
   - *good* → normal (~1.0×)
   - *early / late* → glancing (~0.6×)

   Landing a hit also builds the Special meter.
4. **When the enemy's bar fills, it telegraphs** (wind-up/flash) and you get a **parry window**
   on your pet — a shrinking ring. Nail it → big mitigation (and a little meter/counter
   reward); miss → full hit. Parry window size scales with **AGI**.
5. Repeat until someone reaches 0 HP → `faint` → victory/defeat → exit with remaining HP →
   rewards or the sickness roll.

**Readability guard — concurrent charging, serialized resolution.** Bars fill at the same time
(you feel the enemy's creeping up on you), but only **one** prompt (timing ring *or* parry) is
ever on screen at once. If both bars fill together, higher **AGI** acts first and the other
holds at full, waiting its turn. No colliding prompts on a small panel.

**Graceful degradation = free classic mode.** A player who never touches the screen just leaves
Strike armed; it auto-fires on neutral timing and no parries happen. That is exactly the future
**"classic" spectate mode** — same engine, zero extra combat code. The later classic mode just
drives the same core with synthetic "average timing" input.

## Stats → combat mapping

Uses the existing `StatId` enum verbatim ([`pet.hpp`](../main/game/sim/pet.hpp)):

- **MAXHP** — the *size* of the one HP pool (see below). Wide cap (99999) is the primary tier
  wall: training it makes you tankier without adding another bar.
- **STR** — Strike (physical) attack power.
- **END** — defense; reduces incoming damage.
- **AGI** — ATB fill rate (turn frequency) *and* parry window size (dodge). High-value stat.
- **INT** — Special (tech) attack power.

Combat reads **effective** stats (`Pet::stat()` = innate base + trained modifier).

## HP — one unified system

There is exactly **one** HP value, and the `STAT_MAXHP` stat is just its magnitude.

- The pet's existing `health` (0–100) **is** its HP, shown as one bar on the home screen *and*
  in battle. No second bar, no separate battle pool to reason about.
- In a fight, the working pool = **`(health / 100) × effective MAXHP`**. Damage is dealt in
  absolute points against that pool; at battle end, convert what's left back to a percentage
  and store it as `health`.
- Consequence — **the tier wall is automatic**: a baby (tiny MAXHP) and a Mega (huge MAXHP)
  both read "100%" when fresh, but the Mega's absolute pool dwarfs the baby's. Training MAXHP
  increases how much a "%" is worth.
- **Recovery is the care loop you already have** — feed/heal/sleep/passive regen in
  `Pet::tick()` ([pet.cpp:263-269](../main/game/sim/pet.cpp#L263)). Battle damage and care
  neglect (starve/sick) drain the *same* bar. One number, everywhere.

Code shape: keep `health` as the 0–100 "% full"; express battle damage as points against
effective `MAXHP`, converting to/from the percentage at the battle boundary. Minimal
disruption to the existing sim.

## Damage model (sketch)

Three independent dials produce "skill matters, but tier gaps are walls":

- **Skill band (moderate):** timing quality multiplies the hit ~**0.6× … 1.75×**. Clean play
  wins fair matchups; it can't fabricate damage from nothing.
- **Type modifier:** advantage ~**±25%** (see triangle below). A real edge, never a trump.
- **The wall:** damage keys off attacker **STR/INT vs. defender END**. A large *negative*
  differential floors damage near zero while the stronger side chunks you; layered on top of
  the **MAXHP** pool magnitude, the ceiling enforces itself. **Skill lives inside a tier; it
  does not jump tiers.** A perfectly-played baby cannot out-DPS an endgame wall.

Rough shape (to be tuned):

```
raw     = base(STR or INT) * skillMult(timing) * typeMult(attacker, defender)
mitig   = raw * defenseFactor(END, defenderParry)
damage  = max(floorChip, mitig - defenderEND * k)   // floorChip keeps hits from being 0 in fair fights
```

## Attribute triangle

Classic Digimon: **Vaccine beats Data, Data beats Virus, Virus beats Vaccine.** Plus a neutral
**Free** (no advantage either way) for creatures that shouldn't participate.

- Field on `Creature` ([`creatures.hpp`](../main/game/sim/creatures.hpp)): `uint8_t attribute`
  (enum `ATTR_FREE, ATTR_VACCINE, ATTR_DATA, ATTR_VIRUS`), parsed from an optional `"attribute"`
  key in `creature.json` (default Free). *(Implemented in Phase 0.)*
- Hitting the type you beat applies the +25% (and taking the disadvantage, −25%).
- **This is the hook for the future multi-creature feature** (see [roadmap.md](roadmap.md)):
  once you own several pets, type coverage turns "which do I bring?" into a real decision.

## Stakes & rewards

- **On battle exit** (win or loss), roll sickness/injury with odds that climb as HP% drops —
  trivial near full, high near 1%. The roll pokes the existing care fields (`sick`, `health`)
  rather than inventing a parallel system.
- **On loss:** friendship ↓, happiness ↓, **HP → 1%** → the exit roll is then near-certain to
  fire → you're dumped into feed/heal/rest recovery. That's the intended "hurts, but not
  catastrophic" landing: no permadeath, a real recovery tax.
- **Rewards mirror the existing minigame pattern** (`SceneRun::award()`, granted once on
  finish): **win** → small stat gain via `Pet::trainStat()`, a `Pet::addFriendship()` bump,
  `recordWin()` (feeds evolution gates). **Loss** → friendship ↓, happiness ↓, HP→1%,
  `recordLoss()`. **Bits** are paid on a win in phase E1 of [economy-and-inventory.md](economy-and-inventory.md)
  (40–60, uncapped — battle wins are rare enough to need no daily limit). Item drops stay deferred.

## Modes (v1)

Both are thin wrappers over one battle core — an *opponent source* + a *reward rule*.

### Quick Battle
Random match against a creature near your tier. Opponents are drawn from the existing
`CreatureRegistry` species (filtered by `LifeStage` tier), with an AI stat profile generated
and scaled to the target tier. Doubles as a **discovery hook** — you meet species you haven't
raised, teasing the future collection feature.

### Tower
A fixed climb (target ~20–50 floors), **boss every ~5 floors**, escalating rewards,
**checkpoints** so a loss doesn't reset the whole run. Difficulty scales per floor.

### PvE difficulty dial
Enemy strength is not only its stats — it's **how well the AI plays**: how accurately it times
its own strikes and how often/precisely it parries. Sloppy at low tiers/early floors,
approaching frame-perfect at the top. This is a single knob per opponent.

## Architecture

- **`Combatant` abstraction.** The battle core operates on a snapshot —
  `{ stats, attribute, hp, maxHp, meter, aiSkill, spriteRef }` — never on `Pet` directly. Your
  active pet becomes Combatant #1 (its `hp` seeded from `health% × MAXHP`); the enemy is the
  same struct, AI-filled. A future team/switch, and eventually a **BLE peer**, are just "a
  Combatant from elsewhere." No rewrite.
- **Event-emitting core.** The battle logic *emits events* rather than drawing:
  `attack_start, hit, crit, hurt, faint, victory, defeat, parry_ok, parry_miss, …`. The scene
  renders in response; a future **sprite state machine** and **audio** just subscribe. This
  keeps combat logic ignorant of presentation and makes classic/auto mode and PvP trivial to
  layer on.
- **Input abstraction.** Player taps, "average timing" (classic mode), AI, and a BLE peer are
  all just sources of timing-quality + move-choice into the same resolver.

## Data model changes

- **`Creature.attribute`** (`uint8_t`) + `creature.json` parse (default Free). ✅ Phase 0.
- **`EvoEdge.minWins`** (`uint32_t`, 0 = ignore), checked in `Pet::pickEvolution()`. ✅ Phase 0.
- **`PetState`**: `wins`, `losses`, `energy` added; `PET_VERSION` 5→6. ✅ Phase 0. *(Energy's
  semantics live in [training-and-energy.md](training-and-energy.md).)*
  - **Persistence route chosen: B** — fields added to `PetState` with a version bump. Because
    `Pet::boot()` accepts a save only on an exact version match
    ([pet.cpp:282](../main/game/sim/pet.cpp#L282)), this is a **one-time wipe to a fresh egg**,
    acceptable while WIP. The eventual migration system is tracked in [roadmap.md](roadmap.md).
- Battle "current HP" needs no separate persistence — it *is* `health`, which already persists.

## Integration with existing code

- **New scene** `SceneBattle` under `main/game/scenes/battle/`. Wire like the others: add to
  `SceneId`, add an `App` member, `bind()` it in `App::init()`, add a `case` in
  `App::setScene()`, and add the source to `main/CMakeLists.txt`
  ([`app.hpp`](../main/game/core/app.hpp), [`app.cpp`](../main/game/core/app.cpp)).
- **Entry point:** a menu option — a new top-level **"Battle"** entry or nested under
  **"Activities"** in [`scene_menu.cpp`](../main/game/scenes/menus/scene_menu.cpp). Launch with
  the `Slide::Iris` transition (the minigame's cartoon wipe).
- **Mode select:** a small pre-battle screen (Quick Battle / Tower).
- **Rewards** reuse `Pet::trainStat()` / `Pet::addFriendship()`, following `SceneRun::award()`.
- **Stats screen** ([`scene_stats.cpp`](../main/game/scenes/menus/scene_stats.cpp)) gains a
  **W/L** readout (and, per the training doc, an energy bar).
- **Fixed-timestep loop** already exists in `App::runLoop()`; the battle scene lives inside it
  like any other scene (`update(dt)` / `render()`).

## Implementation plan

Sequenced so **every phase builds, flashes, and shows something verifiable on the board** (you
play on hardware in parallel).

**Phase 0 — Data model & persistence. ✅ (implemented; pending on-device verify.)**
- `Creature.attribute` + JSON parse; `EvoEdge.minWins` + gate in `pickEvolution()`;
  `wins`/`losses`/`energy` in `PetState` (route B, `PET_VERSION`→6); `Pet` accessors
  (`recordWin/recordLoss/wins/losses`, `energy`/`spendEnergy`); energy regen in `tick()`.
  Roster seeded with attributes.
- *Verify:* builds/flashes; boots to a fresh egg (expected wipe from the version bump).

**Phase 1 — Battle core (headless).**
- `battle/combatant.hpp` (`Combatant`), `battle/battle.{hpp,cpp}` (ATB, damage model, special
  meter, parry, faint/win/lose, event queue, `PlayerIntent`). Build a player Combatant from the
  `Pet` and an enemy from a scaled registry creature.
- *Verify:* auto-vs-auto battle logged over serial — fair fights competitive, tier gaps walls.

**Phase 2 — Battle scene (live loop UI).**
- `scenes/battle/scene_battle.{hpp,cpp}` — bars (`gfx_bar`), pre-armed Strike/Special, timing
  ring, parry ring, damage popups, telegraphs, result overlay; feed taps → `PlayerIntent`;
  serialized prompts. Wire into `App`; launch from the menu with `Slide::Iris`. One hardcoded
  test opponent.
- *Verify:* play a full fight on the board.

**Phase 3 — Stakes, rewards & energy gate.**
- End-of-battle: unified-HP write-back (health% ← remaining pool), win/loss rewards, exit-HP
  sickness roll; **energy gate** applied to the training minigames (see training doc); W/L on
  the Stats screen.
- *Verify:* win bumps stats/friendship/wins; loss → HP 1% + likely sickness; training blocked
  when energy is spent; power-cycle persists.

**Phase 4 — Modes.**
- Mode-select screen; **Quick Battle** (tier-filtered random foe, scaled stats + `aiSkill`);
  **Tower** (persisted floor + checkpoint, boss every ~5, per-floor scaling).
- *Verify:* both launch, scale correctly, persist across power-cycle.

**Phase 5 — Tuning & hooks (ongoing).**
- Playtest the skill band / type % / stat curve / sickness odds / reward amounts; wire
  event-driven SFX; leave sprite-state-machine hook points.

## Later / out of scope for v1 (battle-specific)

- **Heal-frequency limiter** — the pressure valve that keeps HP-only battle-gating honest
  (stops "heal and immediately re-fight"). Agreed to design *later*; nothing depends on it
  before then.
- **BLE PvP** device-to-device (the `Combatant`/event architecture is built to accommodate it).
- **Explicit "classic" spectate mode** UI (the engine already supports it via idle input).
- **Additional modes:** boss raids, tournaments, survival/endless, daily challenges.
- **Richer movesets / elemental sub-types** on top of the attribute triangle.

*The economy is designed in [economy-and-inventory.md](economy-and-inventory.md). Other non-battle
future systems — multi-creature ownership, wild encounters, and the save-migration system — live
in [roadmap.md](roadmap.md).*

## Open tuning knobs / TBD

- **Heal-frequency limiter design** (deferred, see above).
- Exact skill-band multipliers, type %, and the stat-differential curve (playtest).
- Special-meter fill rate and what Special actually does (bigger hit vs. effect).
- Tower length, boss cadence, checkpoint rules, and per-floor scaling curve.
- Sickness/injury roll probabilities vs. exit-HP%.
- Stat-gain amount per win (flat vs. scaled by opponent strength).
