# Evolution Pacing

Status: **re-paced** — In-Training shortened, tier 4 and 5 stretched. Mega+ designed, content pending.

Evolution is the most visible progression the game has — the creature literally becomes
something else. This doc is about *when* that happens, because until now it happened almost
entirely in the first week of a life that runs for months.

Related: [death-and-lifespan.md](death-and-lifespan.md) sets the lifespan this is measured
against; [economy-and-inventory.md](economy-and-inventory.md) sells an item that skips a timer,
which only makes sense at the pacing below.

---

## 1. The problem

The ladder as originally shipped, against the lifespan it sits inside:

| Stage | `minStageSecs` | Cumulative |
| --- | --- | --- |
| Egg | 120 | 2 min |
| In-Training I | 3,600 | 1 h |
| In-Training II | 43,200 | 13 h |
| Child | 172,800 | 2.5 d |
| Champion | 259,200 | 5.5 d |
| Ultimate | 345,600 | **9.5 d → Mega** |

A life runs **86 days at worst care, ~300 average, ~428 at best**
(`vitMax 10000 / baseDrainPerDay 33.3` × the care multipliers in
[`vitals.json`](../flash_gamedata/config/vitals.json)).

So form progression finished inside **2–11% of the creature's life**, and the remaining 90%+
had nothing change shape at all. For a game pitched on a 14-month companion, that is the hole.

## 2. The second problem it exposed

`minWins` is **15** at Champion and **40** at Ultimate, and `wins` is cumulative across
evolutions ([`pet.hpp`](../main/game/sim/pet.hpp)). At two or three battles a day those gates
bound *harder* than the timers — so the real pacing was set by how fast the player ground
battles, not by any number anyone chose.

That is backwards for a care game: **the fastest route through it was to stop caring and grind
battles.** Long timers fix this as a side effect — the win gates stop being targets to rush and
become "have you engaged with battle at all" checks.

## 3. Design rules

1. **The opening stays fast — genuinely fast.** A newcomer must get a real, recognisable
   creature within an evening, not a day. The In-Training stages were originally 1 h + 12 h,
   which put the first proper form 13 hours out: a player who started after breakfast had a
   featureless blob until bedtime. They are now 30 min + 90 min, so **Child arrives about two
   hours in**. Child upward is where the stretching happens, once the player is invested.
2. **Timers pace, gates qualify.** The stage timer decides *when*; the stat/bond/win gates decide
   *which branch*. If a gate binds harder than the timer, pacing has quietly moved to whichever
   activity feeds that gate.
3. **The ladder is a fraction of the life, not the whole of it.** Progression should end well
   before old age, leaving a long adulthood where the creature stops changing shape and starts
   changing character (bond tiers, conversations, personality crystallizing).
4. **Rushing is self-limiting.** Vitality drains on a wall clock regardless of stage, so hurrying
   evolutions only means longer spent at the terminal form. No extra balancing needed.

## 4. The re-paced ladder

| Stage | Was | Now | Cumulative |
| --- | --- | --- | --- |
| Egg | 120 | *unchanged* | 2 min |
| In-Training I | 3,600 (1 h) | **1,800 (30 min)** | ~32 min |
| In-Training II | 43,200 (12 h) | **5,400 (90 min)** | **~2 h → Child** |
| Child | 172,800 | *unchanged* | ~2 d |
| Champion | 259,200 (3 d) | **1,209,600 (2 weeks)** | ~2.5 wk |
| Ultimate | 345,600 (4 d) | **3,628,800 (6 weeks)** | **~2 months → Mega** |

Two months to Mega is roughly **20% of an average life**. The win gates now work out at
15 wins over two weeks and 40 over six — comfortably under one a day, so they qualify rather
than grind.

Applied to the In-Training pair and to every tier-4/5 creature across [`flash_creatures/`](../flash_creatures/) and
[`sdcard_mod/creatures/`](../sdcard_mod/creatures/), plus both roster tables in
[`tools/rosters/`](../tools/rosters/) which are the source of truth. Terminal creatures
(`minStageSecs: 1000000000`) are untouched by definition.

## 5. Mega+ — the long tail

Two months of ladder still leaves months of terminal Mega. `STAGE_MEGA_PLUS` is already in the
`LifeStage` enum ([`pet.hpp`](../main/game/sim/pet.hpp)) and nothing uses it. It is the answer:

- **Gated on time as a Mega** (months, not weeks) **plus a very high bond**, and a clean care
  record — the things only a long, well-raised life produces.
- Most players never see it. That is the point: it gives the elderly era a goal without
  stretching the early ladder into tedium, and it rewards exactly the play the rest of the game
  is built to encourage.
- Needs **roster content** — a tier-7 form for each Mega line, with sprites. The mechanism
  works today; the creatures don't exist yet.

## 6. Consequences worth naming

- **A badly-raised pet never reaches Mega.** An 86-day life ends during Ultimate. This is
  intended — it makes reaching Mega an achievement of care rather than of patience — but it is a
  real consequence, and generation 2 will feel different because of it.
- **Offline progress is unaffected.** `boot()`'s catch-up replays `stageSecs` through `tick()`,
  so a device in a drawer keeps advancing; the care freeze correctly suspends it.
- **Mods set their own timers.** `minStageSecs` is per-creature data, so a mod pack can pace its
  own line however it likes. Nothing here is enforced by the engine.

## 7. Deferred

- **Mega+ roster content** (§5) — the tier-7 forms themselves.
- **Lifespan-relative timers** — expressing `minStageSecs` as a fraction of expected lifespan so
  it auto-scales when vitals are retuned. Cleaner in theory, but it makes a modded creature much
  harder to reason about, so absolute seconds stay.
- **A pacing readout** — the Stats screen shows `stageProgress()` but says nothing about *which*
  gates are unmet. Worth revisiting alongside the evolution catalyst.
