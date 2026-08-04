# Food & Feeding

Feeding is the action players perform *most*, so turning it from a single button into a **choice**
converts the game's biggest duty into a channel for self-expression — with no extra grind. Food types
exist primarily to give the personality system ([conversations-and-personality.md](conversations-and-personality.md))
something to read, and secondarily to make the care loop less mechanical.

Related: the deferred economy that will eventually gate food supply lives in [roadmap.md](roadmap.md).

---

## 1. Design rules

1. **Sidegrades, never tiers.** If one food restores more hunger with no downside, everyone picks it
   and we are back to a single strategy. Every food trades something off (fills less, costs health,
   skews temperament).
2. **Basic kibble is neutral.** A player who only wants the gauge full picks it and stays unskewed.
   Flavoured foods are *opt-in* self-expression, which is what keeps the duty-neutrality rule in
   personality §2.3 intact.
3. **Effects read from theme, never from numbers.** A spicy pod obviously reads as fiery; a warm
   herbal mash reads as calming. Drift is deliberately hidden (personality §2.6) — printing "+brave"
   would let players min-max their creature's identity directly and kill the mystery.
4. **Self-limiting instead of scarce.** Until the economy lands, foods are unlimited; overusing one
   is punished by the *simulation* (sweets → weight and sickness pressure; no variety → poorer health)
   rather than by stock. You still cannot spam the best-feeling food.

## 2. The starter set

| Food | Fills | Reads as | Drift | Side effect |
| --- | --- | --- | --- | --- |
| Basic kibble | high | plain, dependable | — | — |
| Meat / protein | high | hearty, bold | `brave +0.5`, `wild +0.3` | — |
| Greens | medium | wholesome, disciplined | `wild −0.5` | health + |
| Sweet treat | low | indulgent | `social +0.4`, `energetic +0.2`, `wild +0.6` | happiness ++, health − |
| Spicy pod | medium | fiery | `energetic +0.7`, `wild +0.3` | — |
| Herbal mash | medium | placid, restorative | `energetic −0.7`, `wild −0.3` | health +, calming |

Not every food needs a unique temperament vector — greens and herbal mash land in similar drift
territory and differ mainly in their *other* effects. That is fine; variety of consequence counts too.

## 3. Data format

Foods are data-driven and SD-moddable, exactly like creatures:

```
/gamedata/foods/<id>/food.json + sprite.png     # base set, read-only flash partition
/sdcard/foods/<id>/food.json + sprite.png       # additive; overrides base on id collision
```

```json
{
  "id": "spicy_pod",
  "name": "Spicy Pod",
  "tags": ["spicy", "plant"],
  "fills": 25,
  "happiness": 4,
  "health": 0,
  "drift": { "energetic": 0.7, "wild": 0.3 },
  "sprite": "sprite.png",

  "cost": 20,
  "rarity": "common"
}
```

`cost` and `rarity` are **reserved and ignored in v1**. They exist now so that gating food behind the
economy later is a behaviour change only — no schema break, and no mod file has to be rewritten.

**`tags` matter for modding robustness.** Preferences may reference a food id *or* a tag, so a creature
that "likes sweets" automatically likes a sweet food added by a mod it has never heard of. Without
tags, every preference would be a hardcoded id list that mods silently fall outside of.

Unlike conversations, the food set is inherently **bounded** (tens, not thousands), so the registry can
eager-load the whole table the way `CreatureRegistry` does — no streaming index needed. Sprites should
still be lazy/LRU like creature sprites.

## 4. Species preferences

`creature.json` gains an optional block, matching ids or tags:

```json
"food": { "likes": ["berry_mash", "spicy"], "dislikes": ["greens"] }
```

- **Favourite** → bonus happiness + a little friendship, and the drift lands harder (it made an
  impression).
- **Disliked** → refused, reusing the existing `Pet::refused_` "no" wiggle, or eaten with a happiness
  penalty.
- Preferences are per-species, so they **change on evolution** — the new form may want different food,
  which is a natural prompt to re-learn your creature.

**Discovery is gameplay, and it is where this meets the conversation system.** Rather than reading
preferences off a screen, the creature *tells* you ("I'd do anything for a spicy pod!"), which records
a fact the dialogue system already supports. Feeding a favourite it previously told you about is the
kind of small callback the whole conversation feature exists to enable.

## 5. UI

The single Feed action becomes a small food picker (built with `Rect`/`ListView` per the widget
preference). It must be laid out so a **stock count per row** can appear later without a redesign —
that is the only forward-compatibility the UI needs for the economy.

Feeding while hunger is already high remains an **overfeeding deviation** regardless of which food is
chosen, carrying the indulgent drift from personality §2.3.

## 6. Validation

Added to [`tools/personality_sim.py`](../tools/personality_sim.py) as `food/*` archetypes — identical
play in every respect *except* which food is chosen. Results:

| Archetype | Lands on |
| --- | --- |
| `food/meat` | Bold / Fierce (73%), Proud (23%) |
| `food/spicy` | Gentle / Sweet (99%) |
| `food/treats` | Gentle / Sweet (99%) |
| `food/greens` | Gentle / Shy |
| `food/herbal` | Gentle / Shy |

Food choice alone moves the creature across **natures**, not merely traits — so it is a genuine
expression channel. It also repaired the roster's weakest spot: **Sweet went from 2.2% to 8.6%** of
outcomes, since two distinct foods now reach it reliably.

## 7. Build phases

| Phase | Work |
| --- | --- |
| **F0** | `FoodRegistry` (data-driven load, flash + SD, tags), food picker UI replacing the Feed button, per-food fills/happiness/health applied in `Pet`. |
| **F1** | Drift hooks (feed the chosen food's `drift` into the personality axes) — lands with personality Phase 1. |
| **F2** | Species preferences (`creature.json` block, favourite/disliked handling, refusal reuse). |
| **F3** | Self-limiting consequences: sweet-tooth weight/sickness pressure, variety/boredom diminishing returns. |
| **F4** | Conversation hooks — the creature reveals its favourite; facts record it. Lands with conversation Phase 3. |

## 8. Deferred

- **Economy gating** — cost, stock, shops, drops (`cost`/`rarity` already reserved above).
- Cooking / recipes / combining ingredients.
- Food-driven evolution branches (a diet-gated `EvoEdge` condition).
- Per-food eating animations and sounds.
