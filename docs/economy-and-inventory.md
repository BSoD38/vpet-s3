# Economy & Inventory

Status: **E1 and E2 built** (wallet, shop, bag, items, medicine, the pet-less shift). Formerly the "Economy — money / shop / items" entry
in [roadmap.md](roadmap.md).

The currency is **Bits**. This doc covers where they come from, what they buy, the inventory
that holds it, and the wish/gift loop that ties the bag to the conversation system.

Related: [food-and-feeding.md](food-and-feeding.md) (foods become stocked),
[conversations-and-personality.md](conversations-and-personality.md) (item rewards, gates),
[evolution-pacing.md](evolution-pacing.md) (why the catalyst item is worth its price),
[death-and-lifespan.md](death-and-lifespan.md) (the pet-less minigame; the memorial keepsake).

---

## 1. Design rules

1. **Money never removes the player's agency.** Basic kibble is free and unlimited, and from any
   *treatable* condition there is always a path back at zero Bits (§4). Money buys speed and
   expression — and where treatment is concerned, **speed is lifespan**, because a seriously ill
   creature loses vitality at 45× the base rate. An earlier draft of this rule said money must
   never gate *survival* at all, which sounded principled but left `Pet::heal()` a free tap that
   quietly cancelled the whole condition system. See §4.
2. **`cost: 0` means free and unlimited.** Not a kibble special-case in code — a data rule, so any
   mod food gets it for free. Kibble already ships with `cost: 0`
   ([`kibble/food.json`](../flash_gamedata/foods/kibble/food.json)); every other base food is
   15–30. The content encoded this rule before the system existed.
3. **Sinks are mostly surplus, not tax.** The big-ticket purchases (decor, toys) are things you
   want, not things you need. A currency that gates necessity creates chores and anxiety; one that
   funds generosity creates aspiration.
4. **Re-use the limiters already in the game.** Minigames cost energy and energy regenerates on a
   clock, so minigame income is *already* metered — no new daily cap. Rare sources (battle wins,
   tower floors) pay uncapped, exactly as `BOND_MILESTONE` bypasses the daily bond allowance.
5. **Effects still read from theme, never from numbers.** Toys carry hidden personality drift the
   same way foods do (personality §2.6). Prices are visible; drift is not.
6. **Bits are the player's, not the pet's.** They survive death, like the lineage ledger. The
   successor inherits the bag and the room you built.

## 2. Bits

### Sources

| Source | Pays | Metered by |
| --- | --- | --- |
| Training minigame | 15–25, scaled by score | **Energy** — already metered |
| Battle win | 40–60 | Rare by nature |
| Tower floor cleared | ~100 | Rare |
| Pet-less shift (§4, §14) | 2–25, scaled by score | **Uncapped in frequency**, capped per shift |
| The creature finds something | an *item*, not Bits | Walk cadence |

A casual day (one session, ~4 minigames) is about **80 Bits**; an engaged day with battles is
about **300**.

### Sinks

| Sink | Price | Reads as |
| --- | --- | --- |
| Flavoured food | 15–30 (already authored) | pocket change |
| Medicine (§4) | 40–200 | pocket change — the tension is preparedness, not price |
| Toy | 300–600 | a couple of days' saving |
| Decor piece | 500–2,500 | a week or two — the room fills across *generations* |
| Evolution catalyst | ~5,000 | weeks; the flagship purchase |
| Token of Amends (§10) | ~6,000 | the most expensive thing in the game; once per creature |

**The wallet shows nowhere on Home.** Shop, Bag and Stats only. The care screen stays about the
creature, consistent with the coarse-readout rule in personality §2.8. The wallet caps at 999,999
so the UI never has to reflow.

## 3. Food becomes stocked (kibble excepted)

You buy flavoured food into the bag and feed from stock. The picker shows a count per row and
greys out zeros — [food-and-feeding.md §5](food-and-feeding.md) was laid out for exactly this.

Stock is what makes §8 possible at all: with pay-at-feed-time there would be nothing *in* the bag
to give away.

**A picker picks; the shop buys.** An earlier version let a zero-stock feed row buy one inline,
which read as a convenience but made one control do two jobs — the row you tap every day to feed
would sometimes spend Bits instead. An empty row now simply refuses, and both care pickers (Feed
and Heal) carry a **Shop shortcut** in the header that returns you where you came from. Same rule
on both screens: they show prices and your wallet, and neither one sells.

**The risk, stated plainly:** flavoured foods are the personality expression channel, so pricing
them puts a price on expression, and a broke player drifts neutral on kibble. Mitigated by keeping
food *pocket change* relative to income — the real sinks sit above it. If playtesting shows drift
flattening, food prices are the first knob to drop.

## 4. Medicine and the Sick window

[death-and-lifespan.md](death-and-lifespan.md) §3 already declares the Sick states to be the
window where the player still has agency: *"Once Critical, treatment no longer works — agency
ended in the Sick window."* But `Pet::heal()` costs nothing, is instant, and *pays* health and
friendship — so there was no agency in the agency window, and the condition system was a
non-event for anyone actually holding the device. **Treatment is priced to make that sentence
true.**

### What sickness actually costs

| | |
| --- | --- |
| `COND_SICK` | Blocks activities, food and touch. **No vitality drain.** Escalates after `sickEscalateHrs: 12` |
| `COND_SICK_BAD` | `sickBadPerDay: 1500` — **45× the base drain** of 33.3/day, i.e. 15% of a whole life per day |
| `COND_INJURED` | Festers into sickness after `injuryFesterHrs: 24` |

That 1500/day is why medicine tiers honestly: **curing a serious sickness a day sooner saves 15%
of the creature's life.** The expensive dose pays for itself in weeks of companion, which is a
real value proposition rather than an arbitrary tier ladder.

Note the scale, though: 1500/day is about **1 vitality per minute**. The punishment lands on
leaving a creature ill for *hours and days*, never on the few minutes spent earning a cure.

### The doses

| Item | ~Price | Does | Note |
| --- | --- | --- | --- |
| Bandage | 40 | clears `INJURED` | Stops the fester before it becomes sickness |
| Remedy | 60 | sickness track, **one step** | `SICK_BAD → SICK`, or `SICK → healthy` |
| Strong medicine | 200 | sickness track, **full** | `SICK_BAD → healthy` outright |
| Tonic | **500** | +40 HP | Buys back battle access early. See below |
| — | — | `CRITICAL` | Still past treating, unchanged (death §3) |

**A treatment cooldown of 4 hours** separates condition doses — the *"future heal-frequency
limiter"* the battle doc anticipated. Without it two Remedies (120) would cure a serious illness
instantly for less than one Strong Medicine (200), and the premium tier would be dead content.
Stepping `SICK_BAD → SICK` stops the 1500/day drain immediately, so the wait is paid in blocked
activities rather than in vitality — four hours of playable time is what the expensive dose is
really selling. The cooldown does not apply to the Tonic, which is limited by costing Bits.

**Why the Tonic is 500 and not 35.** HP regenerates on its own at 25/hour, and it gates only
*battle* (`MIN_BATTLE_HP` = 20); minigames are gated by condition and energy instead. So +40 HP
buys back about **1.6 hours** of battle access, for the price of **ten battle wins**. That is
deliberately a bad trade: a cheap HP restore would make battling costless, which is the one thing
the post-battle HP cost exists to prevent. It is a safety valve for "I want to fight *now*", not a
staple — and it stays `common` rarity precisely so it is always on the shelf when that moment
comes, rather than subject to the daily rotation.

These map straight onto the one-step-per-dose `switch` already in `Pet::heal()`
([`pet.cpp`](../main/game/sim/pet.cpp)); the change is that a dose now comes out of the bag.
Heal becomes "use a medicine", greyed with a count like the feed picker.

A `care` item declares what it treats, so the set is data-driven and moddable like everything
else — a mod can add its own remedy without the engine knowing its name:

```json
{ "id": "soothing_salve", "kind": "care", "treats": "injured", "cost": 45, ... }
```

`treats` takes a `Condition` id (`sick` / `sick_bad` / `injured`). An unknown or absent value is
skipped with a warning rather than silently doing nothing, the same contract every other loader
follows. `critical` is deliberately not accepted: it is past treating by design.

These prices are **ordinary pocket change** against ~80 Bits on a casual day. That is deliberate:
the tension is *preparedness*, not affordability. **Stocking medicine before anything is wrong is
the strategic layer** — and it gives the shop a reason to matter on a day when nothing is wrong.

A **starter remedy** ships in the bag at game start and at each new generation, so the mechanic is
learned before it is ever an emergency.

### The floor

The pet-less minigame (§14) is a **sorting shift**: parcels ride a belt, you tap the bin that
takes the shape at the front, the bins swap as the pace climbs, and three dropped parcels end it.
It is **uncapped in how often you can work** and always available, but one shift pays between 2
and 25 Bits depending on how well you sorted. From zero, a remedy is a shift or two.

An earlier version ran a flat forty seconds with no score. That made every shift identical and
gave nothing to get better at, so it is now endless with a difficulty ramp: skill decides the pay,
inside a ceiling. The ceiling is what keeps it a floor rather than a career -- even a superb shift
is worse per minute than a battle win, so nobody rationally grinds it in preference to playing the
game. It is there for fun, or for when there is no other choice.

Uncapped × small is still unbounded given enough time, and that is fine **because nothing in this
economy is power**: stat and training items were rejected (§10), decor is cosmetic by decision
(§7), toys are expression (§6). The worst a grinder achieves is a nicer room, sooner. The only
item with real mechanical weight is the evolution catalyst, and at a handful of Bits a round that
is 12+ hours of deliberate tedium. If grinding ever shows up in playtesting the fix is the rate,
a knob in §13 — not a cap, and not a redesign.

No cap also means no edge case: there is no state in which a player with a treatable creature
cannot act.

## 5. Items

```
/gamedata/items/<id>/item.json + sprite.png     # base set, read-only flash partition
/gamedata/items/items.json                      # or one pack holding an array
/sdcard/items/...                               # additive; overrides base on id collision
```

`ItemRegistry` mirrors `FoodRegistry` exactly — three-layer load (flash → `.pak` → loose SD),
id-collision override, tag matching, eager-loaded because the set is bounded. Drift blocks go
through `parse_drift()` ([`personality.hpp`](../main/game/sim/personality.hpp)), the same one parser
foods use — so items are limited to the four real axes (`brave`, `energetic`, `social`, `wild`) and
cannot invent a fifth. A new `GD_ITEMS`
entry in the `GdSystem` enum ([`gamedata.hpp`](../main/game/sim/gamedata.hpp)) so the About screen
counts modded items alongside everything else.

```json
{
  "id": "puzzle_box",
  "name": "Puzzle Box",
  "kind": "toy",
  "desc": "Rattles when it wants attention.",
  "tags": ["toy", "quiet"],
  "cost": 450,
  "rarity": "uncommon",
  "drift": { "energetic": -0.5, "wild": -0.3 },
  "happiness": 6,
  "sprite": "sprite.png"
}
```

| `kind` | Consumed? | What it does |
| --- | --- | --- |
| `toy` | no | Drift + happiness when played with (§6) |
| `decor` | no | Occupies a room slot (§7) |
| `care` | yes | Medicine and the like — the priced treatment path (§4) |
| `special` | yes | One-off effects (§10) |
| `keepsake` | no | Memorial items; not purchasable |

**Inventory resolves an id against `ItemRegistry` first, then `FoodRegistry`** — so no existing
mod file has to change, and foods keep their own schema and folder.

**No selling.** Bits flow one way. Buy/sell arbitrage and bag tidy-up are both chores nobody asked
for.

## 5.1 The modding ceiling — deferred to E6

**As built, an item mod can only add more of what the engine already understands.** `kind` is a
closed enum, and each value's behaviour is compiled in: a `toy` nudges drift and happiness, a
`care` item cures one `Condition`, `decor` fills a slot. So a mod can ship a hundred new toys and
not one new *idea*. It cannot:

- add a kind the engine has never heard of, with behaviour to match;
- give one item **more than one** effect (a snack that also cures, a toy that pays Bits);
- touch anything outside its bucket — stats, energy, vitality, bond, the wish;
- gate an item on anything ("only on a sick pet", "only at night", "only past Champion");
- express uses, cooldowns, or durability.

That is a poor deal for a game whose creatures, foods, conversations, natures, sounds and packs
are all fully data-driven. Items are currently the one system where the data is a lookup table
rather than a description of behaviour.

### The shape of the fix

**Borrow the conversation system's, rather than inventing a second one.** It already solved
exactly this problem — moddable, gated, data-driven behaviour with a lint tool
([`conv_lint.py`](../tools/conv_lint.py)) that validates against the firmware's own constants. An
item should read the same way a conversation choice does:

```json
{
  "id": "night_tonic", "name": "Night Tonic", "group": "tonics",
  "cost": 90, "uses": 3,
  "when":    { "condition": "sick", "hourAfter": 20 },
  "effects": [ { "cure": "sick" }, { "happiness": 4 }, { "drift": { "energetic": -0.4 } } ]
}
```

Three changes carry most of the value:

1. **`effects` as a list**, replacing the single-purpose `treats` / `happiness` / `drift` fields.
   Items become composable instead of single-purpose. The op vocabulary is the engine's existing
   verbs — `cure`, `hunger`, `happiness`, `health`, `energy`, `bond`, `drift`, `bits`, `stat`,
   `setWish`, `forgiveMistake`, `evolve` — each already a method on `Pet` or `Economy`.
2. **`when` gates on use**, reusing the `ConvContext` shape so one gate vocabulary covers
   conversations and items both.
3. **`kind` becomes presentation only** — a free-text `group` with a display label, so a mod can
   add a "TONICS" heading to the Shop without an enum change. Sorting falls back to declaration
   order for unknown groups.

### Why it is E6 and not now

An effect interpreter is a real piece of work: a parser, a bounded op table, a use-time gate
evaluator, and a lint tool, plus the migration of the five built-in kinds onto it. Doing it before
the kinds actually *exist* would be designing an abstraction over one example — medicine (E2),
toys (E3) and decor (E5) are the three cases that will show what the vocabulary needs to cover.
E6 is the first point where all of them are real.

**The schema stays forward-compatible in the meantime.** `kind` survives as the group label, and
today's `treats` / `happiness` / `drift` become sugar for single-entry `effects` lists — so the
fixtures and any mod written against the current schema keep working.

## 6. Toys — the play channel

Durable, one "out" at a time, visible in the room. Home's play action uses whatever is out, which
gives `PLAY_AFFECTION` / `PLAY_ROUGH` ([`pet.hpp`](../main/game/sim/pet.hpp)) a real vocabulary
instead of two verbs.

| Toy | Reads as | Drift |
| --- | --- | --- |
| Ball | boisterous | `energetic +`, `wild +` |
| Plush | comforting | `social +`, `energetic −` |
| Puzzle box | thoughtful | `energetic −`, `wild −` |
| Chew rope | rough-and-tumble | `brave +`, `wild +` |

Toys **never wear out**. The sink is wanting several for different drifts; wear-out would turn an
expression channel into a maintenance chore.

## 7. Room decor

**Purely cosmetic.** Decor changes how the room looks and nothing else — no vitality bonus, no
sleep modifier. Zero balance risk, zero coupling to vitals, and the emotional payoff (a room that
accumulates across generations) is the entire point. Sim effects can always be added later; they
cannot easily be removed.

Fixed slot set — **floor, wall, window, bed, one feature object** — so the home-scene render stays
bounded. Decor persists across death with everything else in §11.

## 8. The wish → gift loop

The piece with the most in it, and the reason the bag and the conversation system belong in the
same doc.

```
conversation:  "I'd do anything for a spicy pod."   ->  setWish("spicy_pod")
shop:          that row gets a small marker
bag:           you're carrying one
give:          creature accepts  ->  milestone bond, hard drift, a permanent fact
journal:       "You brought her a spicy pod when she asked."
weeks later:   a conversation gates on that fact and brings it up unprompted
```

- **One active wish at a time.** Bounded RAM, and it stays precious instead of becoming a
  checklist.
- **Its own small NVS key**, per-creature: `wantId[24]` + `wantSetAt`. Deliberately *not* a fact —
  facts are global, permanent and capped at 32
  ([`conversation.hpp`](../main/game/sim/conversation.hpp)), whereas a wish is per-creature and
  expires. **Fulfilling** it writes a permanent fact; that fact is the callback.
- **Authored wishes win, derived wishes are the fallback.** A conversation can `setWish`
  explicitly; if none has, the game derives one from the species' `food.likes` block, so an
  unmodded creature still wishes for things.
- **Wishes lapse silently** after a few days. No guilt-trip on a device you left in a drawer.
- **Anti-farm, re-using the existing mechanism**: an ordinary gift goes through `BOND_ROUTINE`
  (daily allowance); only wish fulfilment gets `BOND_MILESTONE`. The same split conversations
  already make.
- **Refusal re-uses `refused_`** — gifting a disliked item gets the "no" wiggle, exactly as
  disliked food does.

## 9. Conversation hooks

All six mirror the shape `setFact` already has, so mod authors learn nothing new:

| Hook | Kind | Meaning |
| --- | --- | --- |
| `when.hasItem: "spicy_pod"` | gate | The creature notices what's in your bag |
| `when.wish: "pending" / "fulfilled" / "none"` | gate | React to its own asking |
| `when.minBits: 500` | gate | Rare — "you could afford it, you know" |
| `effects.giveItem: "shell"` | effect | The item rewards deferred in personality §9 |
| `effects.takeItem: "spicy_pod"` | effect | The gift actually leaves the bag |
| `effects.setWish: "spicy_pod"` | effect | The ask |

Engine side: two new fields on `ConvContext` (a `hasItem` resolver and the wish state) and the
matching cases in `gatePasses()`. [`tools/conv_lint.py`](../tools/conv_lint.py) gains
cross-referencing of item ids against the registry, the same way it already checks nature and
trait ids.

## 10. Special items

**Evolution Catalyst — ~5,000 Bits, consumable.** Skips the stage timer, nothing else.

`Pet::cheatForceEvolve()` ([`pet.hpp`](../main/game/sim/pet.hpp)) already has exactly the right
semantics, and its own comment already argues the case: it *"exercises the evolution PATH rather
than bypassing it"* — walking the real edge list, so stats, bond, wins and care mistakes still
decide which branch you get. Promoting it from cheat to item is close to a rename.

- It returns `Evolved` / `Terminal` / `NotEligible`. **If it returns anything but `Evolved`, do not
  consume the item.** Zero-risk purchase, honest message ("It isn't ready — something's still
  missing"), and no need to expose the gate list.
- It **cannot choose the branch**. Branch choice is the whole design; an item that picks the form
  would flatten it.
- The price only makes sense at the re-paced ladder — skipping a six-week Ultimate timer is worth
  5,000; skipping the old four-day one was not. See [evolution-pacing.md](evolution-pacing.md).

**Memorial keepsake — free, not purchasable.** Granted on death from the lineage record. The one
item money cannot touch.

**Two doors are closed:**

- **No fate or revival items.** [death-and-lifespan.md](death-and-lifespan.md) rule 4 already says
  it in writing: *"nothing done during the death event — no dialogue choice, no last-second item —
  changes the outcome."*
- **No nature-shift keepsake.** Considered and dropped: an item that buys a Nature contradicts
  personality §2.5, which makes evolution the jump window precisely because it *"rewards a long,
  consistent way of playing rather than a single action."* The catalyst replaced it.

**Token of Amends — ~6,000 Bits, consumable, once per creature.** Forgives **two** care mistakes.

Every evolution gate is earnable by playing except `maxCareMistakes`, which only ever counts up: a
player who neglected their creature in week one is locked out of the good branches
(`maxCareMistakes: 3` on the best edges) for that creature's whole life, with nothing they can do
about it. This is the one unlock not reachable by grinding something else, which is why it is the
most expensive thing in the game.

Three guards keep it from becoming absolution:

- **Once per creature.** Not once per purchase — the limit is on the life, not the wallet. So a
  neglectful player can soften one stretch of it and no more, and death rule 6 (*"neglect cannot be
  farmed"*) still holds exactly. Tracked in the wish blob's neighbourhood: a per-creature flag,
  cleared with a new egg.
- **Two mistakes, not a reset.** Enough to reopen a branch you narrowly lost; nowhere near enough
  to erase a pattern.
- **It never touches vitality.** Scars, the drain multiplier and the trailing care average are
  untouched — the creature's *body* remembers what its evolution table has forgiven.

### It should be earned, not bought

The purchasable version is a **placeholder for the version that matters**: amends made by actually
doing better. The obvious shape is a sustained stretch of clean care — some weeks with no new
mistakes — granting one Token outright.

That is worth building because it is self-justifying (you earn forgiveness by being forgivable) and
because the game already does exactly this one scale down: `Pet::mendMood()`
([`pet.hpp`](../main/game/sim/pet.hpp)) already tracks *"care shown since being upset"* and softens
the creature's mood a step when it fills. The Token is that mechanic at the timescale of a life
rather than a sulk.

Once the earned route exists, the shop version becomes a shortcut for the impatient rather than the
intended path — and could reasonably be removed, or repriced upward, at that point. Deferred (§15).

## 11. Persistence and data layout

Everything gets **its own NVS key**, never `PetState` — adding a field there bumps `PET_VERSION`
and wipes the player's pet. Same discipline as the bond allowance, mood and vitals.

| State | Key | Survives death? |
| --- | --- | --- |
| Wallet (Bits) | own `u32` | **yes** |
| Inventory | own versioned blob | **yes** |
| Room layout | own blob | **yes** |
| Active wish | own blob | no — per-creature |

`InvSlot { char id[24]; uint8_t kind; uint16_t count; }` × 48 slots ≈ 1 KB. Durables are
`count: 1`.

## 12. UI

- **`SceneShop`** — a menu entry, **never gated**. It works while the pet is frozen, sick or
  asleep; it is the player's screen, not the pet's. Commons are always stocked; 2–3 rotating slots
  carry uncommon/rare, seeded from the **RTC day index** so rotation is deterministic, survives
  reboots and needs *zero* persistence. The wished-for item's row gets a marker.
- **`SceneBag`** — inventory, with Use / Give / Set-out per item kind.
- **Home's Heal action becomes "use a medicine"** — a small picker like Feed, showing a count per
  dose and greying out at zero. With no medicine at all it points at the shop, and at the pet-less
  minigame when the player cannot afford one.
- Both built with `ListView` and `Rect`, like the feed picker.
- **E1 needs no art at all** — items reuse the food approach (colour swatch, name, flavour line).
  Sprites land with decor in E5.

## 13. Tuning

`flash_gamedata/config/economy.json`, with the same per-key override as
[`vitals.json`](../flash_gamedata/config/vitals.json): a mod or a loose
`/sdcard/config/economy.json` that sets one key overrides just that key. Prices live on the items;
this file holds the rates — minigame/battle/tower payouts, the pet-less daily cap, wish lifetime,
shop rotation size.

Every number in this doc is a first guess and expects to move with playtesting.

## 14. Build phases

| Phase | Work |
| --- | --- |
| **E1** | Wallet, `ItemRegistry`, `SceneShop` + `SceneBag`, food stock + inline buy, income from minigames and battles. *The loop closes here.* |
| **E2** | **Medicine (§4) *and* the pet-less minigame, in the same phase** — priced treatment never ships without its floor. See below. |
| **E3** | Toys — drift channel, one toy out, play-with-toy on Home. |
| **E4** | Wish→gift loop, the six conversation hooks, `conv_lint` item cross-referencing. |
| **E5** | Room decor — slots, home-scene background layers, sprites. |
| **E6** | Evolution catalyst; Token of Amends; memorial keepsake on death. **Plus the modding rework in §5.1** — the item schema stops being five fixed buckets and becomes composable effects. |

**Why E2 is indivisible.** `Pet::conditionBlocked()` blocks battle, training and *all* minigames
while anything is wrong, and its own comment already names the exception:

> *"Battle, training and ALL minigames are blocked while anything is wrong. The pet-less 'earn
> medicine money' game planned for the economy is the deliberate future exception."*
> — [`pet.hpp`](../main/game/sim/pet.hpp)

So a sick creature blocks **every** income source except the pet-less one. Shipping priced
treatment before that floor exists would be a genuine hard lock — sick pet, no Bits, no way to earn
any, no way to treat it. Splitting E2 across two releases reintroduces exactly the failure mode
rule 1 exists to prevent.

It matters for a second reason: **the care loop earns nothing.** Income comes only from minigames,
battles and the tower, so a player who just feeds and pets has no Bits at all. Their only route to
medicine is the pet-less game. It is load-bearing, not a bonus.

## 15. Deferred

- **Earning the Token of Amends** (§10) — a sustained stretch of clean care granting one outright,
  instead of buying it. This is the *intended* form; the purchasable version is a placeholder that
  should be repriced or removed once this exists. `Pet::mendMood()` is the same idea at the
  timescale of a sulk and is the obvious model to follow.
- **Cooking / recipes**, inherited from [food-and-feeding.md](food-and-feeding.md) §8.
- **Battle items.** The battle core has no item slot; adding one is a combat change, not an economy
  one.
- **Cosmetic accessories on the creature sprite** — needs sprite compositing the gfx layer does not
  do yet.
- **A second currency** for prestige sinks. One currency plus a few earned-only items covers the
  same ground with less to explain.
- **Passive income.** Deliberately absent — it makes play optional, which is the opposite of the
  point.
