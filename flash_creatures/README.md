# Creatures

Every creature the pet can be is defined by **data**, not code. This folder is the
**base game** roster; it gets packed into the flash `creatures` partition at build
time and mounted read‑only at `/creatures` on the device.

You can add or replace creatures from an **SD card** without rebuilding — put them
under `/sdcard/creatures/` in the exact same layout. If an SD creature has the
**same `id`** as a base one, the **SD version wins** (so a card can mod the game).

## Layout

One folder per creature. The folder name is only a container; the real identity is
the `id` field inside the JSON.

```
creatures/
  egg/
    creature.json
    sprite.png
  nibbling/
    creature.json
    sprite.png
  ...
```

## `creature.json`

```jsonc
{
  "id": "sparky",            // unique string id (referenced by evolution edges)
  "name": "Sparky",          // display name (shown on the Home screen)
  "tier": 3,                 // life stage, 0..7 (see table below)

  "base": {                  // INNATE base stats. Effective stat = base + trained modifier.
    "hp":  90,               //   Max HP   (cap 99999)
    "str": 10,               //   Strength (cap 9999)
    "end":  8,               //   Endurance / defense
    "agi": 16,               //   Agility
    "int":  9                //   Intellect
  },

  "needs": {                 // how fast this creature's meters move (per REAL hour @ 1x speed)
    "hungerPerHr":   12,     //   hunger drain
    "happyPerHr":    13,     //   happiness drain
    "poopIntervalS": 9000,   //   seconds between poops (awake)
    "sleepStart":    23,     //   sleep window start hour (0..23)
    "sleepEnd":       6      //   sleep window end hour   (may wrap past midnight)
  },

  "minStageSecs": 172800,    // min time as this creature before it may evolve (real seconds @1x)
  "sprite": "sprite.png",    // sprite filename in this folder

  "evolutions": [            // outgoing branches, checked TOP-DOWN; first one that qualifies wins
    { "to": "boltor", "minAgi": 120 },   // needs Agility >= 120
    { "to": "rollo" }                    // fallback: no requirements -> always qualifies
  ]
}
```

Every field is optional except that a creature with no `id` falls back to its folder
name. Missing numbers default to `0` (and `poopIntervalS`/`minStageSecs` default to
"never"). Omit a field to accept its default.

### `tier` values

| tier | stage          |
|------|----------------|
| 0    | Egg            |
| 1    | In‑Training I  |
| 2    | In‑Training II |
| 3    | Child          |
| 4    | Champion       |
| 5    | Ultimate       |
| 6    | Mega           |
| 7    | Mega+          |

`tier` is for display/pacing only — it does **not** gate evolution. What a creature
becomes is decided entirely by its `evolutions` edges.

### Evolution edges

When a creature has spent at least `minStageSecs` in its stage, the game walks its
`evolutions` list **in order** and takes the **first edge whose conditions are all
met**. Put your special branches first and a no‑condition fallback last so there's
always somewhere to go.

Condition fields on an edge (all optional; each defaults to "no requirement"):

| field             | meaning                                             |
|-------------------|-----------------------------------------------------|
| `to`              | target creature `id` (**required**)                 |
| `minHp`           | effective Max HP ≥ value                             |
| `minStr`          | effective Strength ≥ value                           |
| `minEnd`          | effective Endurance ≥ value                          |
| `minAgi`          | effective Agility ≥ value                            |
| `minInt`          | effective Intellect ≥ value                          |
| `minFriendship`   | bond meter ≥ value (0..1000)                         |
| `maxCareMistakes` | care‑mistake count ≤ value (fewer = better care)    |

Stat conditions test the **effective** stat (base + what you've trained). Two
creatures can point their edges at the **same** target (convergence), and one
creature can branch to **several** (divergence) — the tree is just who points where.

Notes:
- **Trained stat modifiers reset to 0 on every evolution**; the new creature's base
  carries the progression. **Friendship persists.**
- If an edge's `to` id isn't found in the registry, that edge is skipped (and logged).

## Sprites

- **PNG**, any size up to 256×256 (48×48 recommended to match the current art).
- Decoded into PSRAM once when the creature loads, then blitted centered.
- **Transparency:** use fully transparent or fully opaque pixels (hard edges).
  Transparent areas show through; soft/anti‑aliased alpha will fringe against the
  background because the panel has no per‑pixel alpha.

## Adding a creature

- **Base game:** drop a new `<id>/` folder here and rebuild — it's repacked into the
  flash image automatically on `idf.py flash`.
- **Mod (no rebuild):** put `<id>/creature.json` + its sprite under
  `/sdcard/creatures/` on the card. Same `id` as a base creature overrides it.

## Current limits

- Registry holds up to **40** creatures and **4** evolution edges per creature.
- **Sprites load lazily** — a creature's PNG is decoded into PSRAM only when it's
  first shown, and kept in a small LRU cache (16 sprites), so only what's on screen
  is resident. The roster can grow large without running out of RAM on sprites.
- Creature **definitions** (the JSON) are still parsed eagerly at boot; that's cheap
  for a modest roster. A very large roster (hundreds of creatures) would also want a
  prebuilt manifest and to keep the bulk of assets on the SD card rather than flash.
