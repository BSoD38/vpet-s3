# Death & Lifespan

Death is the emotional anchor the rest of the care sim points at: every care mistake, every
friendship point, every conversation gains weight because the pet's time is finite. The design
goal, set explicitly, is that **death is saddening — more so the higher the friendship** — and
that a bond can, with luck, *postpone* it. Friendship is deliberately both the grief amplifier
and the grief insurance: the saddest deaths are the rarest, and there is no way to optimise
out of feeling something.

Related: [conversations-and-personality.md](conversations-and-personality.md) (deathbed
conversations, scar drift — this doc un-parks the "Death" trigger listed there),
[battle-system.md](battle-system.md) (injuries; battles stay non-lethal),
[training-and-energy.md](training-and-energy.md) (activity blocks),
[economy-and-inventory.md](economy-and-inventory.md) (economy hooks),
[evolution-pacing.md](evolution-pacing.md) (the ladder is paced against the lifespan set here),
[roadmap.md](roadmap.md) (cemetery).

**Status: design. Nothing below is built. All numbers are first guesses — placed to make the
design concrete, fully expected to change once play-tested** — and live in gamedata JSON, not code.

---

## 1. Design rules

1. **One meter, many slopes.** Every death is vitality reaching zero. Old age, neglect and
   sickness are not separate mechanics — they are different drain rates on the same pool.
2. **Restrictions come from condition, never from age.** A diligent player's reward for a
   14-month companion is never a lockout. Elderly/Twilight pets battle, train and play at full
   capability, with **no stat penalties** — age is *expressed* (walk, sleep, dialogue), never
   *enforced*.
3. **Death is never a surprise and never completes offline.** Every death is preceded by
   visible, actionable states, and the final event only resolves with the player watching.
4. **Fate takes no inputs at the brink.** The miracle roll is luck weighted by the entire
   friendship history. Nothing done during the death event — no dialogue choice, no last-second
   item — changes the outcome, and the roll is persisted *before* the first frame renders, so
   the reset button cannot reroll it.
5. **Saves are never free.** Every survival costs something permanent (ceiling, scar, odds),
   so no bond makes a pet immortal.
6. **Neglect cannot be farmed.** Generational rewards are gated on how the life *ended*;
   churning pets yields nothing.

## 2. Vitality

A hidden pool, `vitality` (0..`VIT_MAX` = 10000), full at hatch, draining continuously in
`Pet::tick()` (inside the care-freeze gate, so freeze pauses aging — vacations stay guilt-free).

```
drain/day = BASE_DRAIN × careMult × speciesMult(reserved, 1.0 in v1)
```

- `BASE_DRAIN` ≈ 33/day → an average-care life of **~10 months**.
- `careMult` comes from a trailing exponential moving average of care quality (care-mistake
  rate, time spent sick, hunger/energy averages): **0.7× for diligent care (~14+ months)** up
  to **~3.5× for sustained neglect (~3 months)**.
- Each care mistake also chips a **flat chunk** immediately — neglect costs twice, the moment
  and the trend.
- **Serious untreated sickness adds a large flat drain** (~1500/day): fatal in **3–6 days** at
  any age. This *is* the neglect death — a cliff on the same meter, not a second mechanism.

Vitality is **never shown as a number**. The player reads it through behaviour and the states
below. (A "checkup" care action giving a qualitative reading is deferred.)

| Care pattern | Lifespan |
| --- | --- |
| Diligent (0.7×) | ~14 months |
| Average (1.0×) | ~10 months |
| Sustained neglect (3.5×) | ~3 months |
| Serious sickness, untreated | days |

## 3. The condition track

`Healthy → Sick (mild → serious) → Critical`, plus `Injured` and `Recovery`.

- **Sick** comes from care neglect (and later, food consequences per food-and-feeding §1.4).
  Mild is cheap to treat; untreated it turns **serious** (the big drain above, urgent signals).
  Treatment is a medicine/care action — the treatment window is *the* agency window.
- **Injured** comes from battle aftermath (catastrophic losses). It heals with rest/treatment;
  ignored for days it converts to Sick. **Battles never kill — only ignoring their aftermath
  does.** That line is what keeps "non-lethal battles" honest.
- **Critical**: vitality has bottomed out via the sickness/neglect path. The pet collapses and
  the sim suspends for it — no further drain, nothing ticks. Critical is a **held state, not a
  timer**: it waits indefinitely (drawer, deep sleep, weekend) until the player is present,
  then the death event fires. Once Critical, treatment no longer works — agency ended in the
  Sick window; what remains is the roll. If the player is watching when the collapse happens,
  Critical lasts a second and flows straight into the event.
- **Recovery** (post-save, §5): several days, wants gentle care.

**Sick, Injured, Critical and Recovery block battle, training and all minigames.** Forward
hook: a *pet-less* minigame lets the player keep earning while the pet is down — the lockout becomes
a loop, not a wall (economy phase E2).

**The agency window now has a decision in it.** Through v1 the sentence above was aspirational:
`Pet::heal()` cost nothing, worked instantly and *paid* health and friendship, so an attentive
player never faced a choice and the condition track was a non-event for anyone actually holding
the device — the danger only reached players who had forgotten the pet entirely. From economy
phase E2, treatment is a **priced item** with tiers matching the condition ladder
([economy-and-inventory.md](economy-and-inventory.md) §4). The real cost of going without is
denominated in vitality: at `sickBadPerDay: 1500` a day spent seriously ill is 15% of a whole
life, so medicine is a way of buying lifespan back. Nobody is ever locked out — the pet-less
minigame is uncapped and always available — but doing nothing now has a price.

## 4. The life track (old age)

Two thresholds, both **presentation-only** (rule 2):

- **Elderly** (< 15% ≈ 5–7 weeks at good care): slower walk cadence, more sleep, aged
  conversation lines. The anticipatory-grief window — fully playable.
- **Twilight** (< 3% ≈ ~2 weeks): farewell-flavoured conversation content unlocks. The player
  knows.

**The aged gait only started working properly in E3.** Stretching the footfall period is meant to
be the one lever that slows the walk coherently — ground travel derives from the step cadence, so
gait and speed slow together. It did not: `CreatureWalk` computed travel from the `ANIM_STEP_SECS`
*constant* while `CreatureAnim` carried a live, stretched period, so an elderly creature moved its
legs slower while covering ground at full speed. That is exactly the skate
[`walk.hpp`](../main/game/engine/walk.hpp)'s invariant exists to prevent, and this document
asserted it was handled from the day D1 shipped. The walk now takes the live period, so anything
that retimes the legs retimes the ground with them — which is also what lets a creature *run*
after a ball ([economy-and-inventory.md §6](economy-and-inventory.md)).

At zero on this track, the pet is at the brink: the **deathbed conversation** (§6) fires if one
is eligible, otherwise the plain brink scene — held for the player's presence exactly like
Critical.

## 5. The miracle

One roll, two flavours, resolved and written to NVS before anything renders:

```
p = 85% × bondFactor × 0.5^savesUsed        (savesUsed persists per lifetime)
bondFactor = clamp((friendship − 4000) / 6000, 0, 1)
```

Below ~40% bond the chance is effectively zero — **the miracle is the friendship**. Max-bond:
85% → 42% → 21% → … Decaying odds guarantee every life ends.

| | Critical save (sickness/neglect) | Old-age reprieve |
| --- | --- | --- |
| Restore | to **30% of max** (~3–4 months) | **+5% of max** (~2–3 weeks) |
| Permanent cost | **max vitality −15%**, scar | none |
| State after | Recovery (days), then normal life | still Twilight |
| Next roll | halved | halved |

- The **restore** is what surviving means — without it the pet would collapse again next tick.
  A Critical save buys a second act; a reprieve buys a longer farewell. Old age is supposed to
  be the end; sickness isn't necessarily.
- The **ceiling cut** does nothing at that moment and costs the whole rest of the life: refills
  and thresholds are percentages of max, drain is absolute, so total lifespan, Elderly onset and
  the next refill all shrink ~15%, compounding per save. Odds halving + pool shrinking converge
  on death from both directions; store a `scars` count and derive `max = VIT_MAX × 0.85^scars`.
- **Scar drift**: a Critical survival is the **strongest single personality-drift event in the
  game**, pushed hard toward the wary/timid (neglect-flavoured) axis ends — even a saved pet
  comes back changed. Exact axis mapping and interaction with crystallised natures is decided
  against `personality.cpp` at implementation.
- A reprieved pet stays old: no un-frailing, no Recovery, no ceiling cost (meaningless when it
  will never refill). During borrowed time the player knows every goodbye might be the last —
  each reprieve is joy with worse odds attached.

## 6. Deathbed conversations

At the old-age brink, the death event *is* a conversation, friendship-gated — the bond
produces both the words and the odds, together:

| Tier (reuses conversation bands) | At the brink |
| --- | --- |
| below Trusted (< 5000) | No conversation — the plain, quiet passing. |
| Trusted / Close (5000–8499) | A modest farewell. |
| Bonded / Soulbound (8500+) | The profound one — history callbacks, journal references. |

- **The roll selects the ending; the conversation reveals it.** Player choices colour the
  exchange and must never influence the outcome (rule 4): a losing dialogue path would mean
  players believe a fumbled menu killed their pet, and a winning one is a solved puzzle instead
  of a miracle.
- **Reprieve count is a conversation gate**, so the second farewell can acknowledge the first
  ("You're sitting with me again…"). Decaying odds cap this at ~2–3 conversations per pet —
  small content budget, disproportionate payoff.
- **Critical deaths deliberately get none of this.** Neglect death stays sudden and
  near-wordless — collapse, roll, done. Old age earns a spoken goodbye because the player was
  there for all of it; neglect takes the pet before anything can be said. Grief with words,
  guilt without them.

### The `@fate` fork

One conversation file per farewell, **not** a death-copy and a reprieve-copy. The format is a
flat node list with string-id jumps, so the fork is one reserved jump target plus two top-level
entry points:

```json
{
  "id": "farewell_bonded_1",
  "trigger": "deathbed",
  "minFriendship": 8500,
  "reprieves": 0,
  "nodes": [
    { "id": "open",       "text": "…", "to": "last_words" },
    { "id": "last_words", "text": "…", "to": "@fate" },
    { "id": "goodbye",    "text": "…", "to": "end" },
    { "id": "stay",       "text": "…I think I'd like to stay a little longer.", "to": "end" }
  ],
  "onDeath": "goodbye",
  "onReprieve": "stay"
}
```

When traversal hits `@fate`, the runtime jumps to `onDeath` or `onReprieve` per the persisted
roll. Loader work: the `deathbed` trigger, the `reprieves` gate, `@fate` resolution, and
load-time validation that both entry points exist.

Why single-file: the design *requires* everything before the fork to be outcome-invariant —
if the early lines differed by outcome, players would learn the tell and the roll would leak
from line one. Two hand-mirrored copies make that invariant a hope; one file makes it
structural, halves the authoring matrix (tiers × reprieve variants), and keeps mods
drift-proof. It also structurally enforces rule 4: fate has exactly one door. Cost: both
endings share the 32-node cap (~20-node body + two ~6-node endings fits; the cap is a PSRAM
content decision — one resident conversation — and can be raised for this trigger if the
Bonded farewell needs room).

## 7. The death event & memorial

1. Dedicated scene, no UI chrome. **One piece of music that plays here and nowhere else in the
   game** — never reused, so it stays sacred.
2. The sequence visibly *begins* every time; a passed roll interrupts it on-screen. Survivors
   witness the stakes — a near-death is a story beat, not a log line.
3. On death: the farewell (conversation or brief scene per tier), then a quiet screen. **No
   "new egg" button here.** The next interaction shows the memorial; only then the offer to
   begin again.
4. **A structured lineage record is persisted at every death from day one** — species, stage
   reached, generation, age, cause, final friendship, saves used, death date. A few bytes each.
   The journal memorial renders from it now; the future cemetery scene renders from it later,
   so gen-1 pets are not lost to that feature arriving late.
5. Memorial is **journal-only** in v1 (permanent entry). Cemetery scene: deferred.

## 8. Generations & bond momentum

- A **qualifying death grants a permanent bonus to future friendship *gains*** (never losses):
  **+10%** for an old-age death, **+2.5%** for any other death with high bond (≥ 6000 — a
  beloved pet lost to one bad week isn't worth nothing). **Cap +50%.** A neglect death grants
  nothing (rule 6).
- Every new pet **starts at friendship 0**. Every relationship arc plays in full — momentum is
  the player having learned to love, not the pet pre-loving them. No absolute carry-over, so
  conversation pacing and `minFriendship` evolution gates stay intact each generation.
- **Echoes**, independent of momentum: lineage-gated conversation lines ("…was there someone
  before me?"), occasional predecessor mannerisms, memorial references — conversation data
  entries gated on generation/lineage, nearly free.

## 9. Data & persistence

- **Gamedata JSON** (tunable without reflash, moddable via paks): base drain, care-multiplier
  curve, mistake chip, sickness drain, thresholds, miracle constants, restore/ceiling numbers,
  momentum steps/cap. `speciesMult` reserved in `creature.json` for hardy/short-lived lines
  (ignored in v1, so no schema break later — same trick as food `cost`/`rarity`).
- **NVS, per pet**: vitality, condition state, savesUsed, scars. **NVS, persistent across
  generations**: generation counter, momentum, lineage records.
- **Migration**: versioned save-struct bump (the friendship-rescale precedent). Existing pets
  initialise `vitality = VIT_MAX − ageSecs × average drain`, with a generous floor — nobody's
  current companion turns Elderly on flash day.
- Death events, rolls and lineage records are written **before** their scenes render (rule 4);
  a power cut mid-farewell refires the same conversation with the same persisted outcome.

## 10. Build phases

| Phase | Work |
| --- | --- |
| **D0** | Vitality pool + care-multiplier EMA in `Pet::tick()`, condition track (Sick/Injured/Critical/Recovery), treatment action, activity blocks, offline hold semantics, save migration. |
| **D1** | Life track: Elderly/Twilight presentation (walk cadence, sleep, conversation gating), no mechanical effects. |
| **D2** | Miracle rolls + persistence, Critical death event + plain brink scene, Recovery, scar (ceiling + drift hook), lineage record write, journal memorial, the one piece of music. |
| **D3** | Deathbed conversations: `deathbed` trigger, `reprieves` gate, `@fate` fork, validation; farewell content for both tiers. |
| **D4** | Generations: momentum, new-egg flow through memorial, lineage echo gates. |

## 11. Tuning numbers — first guesses (recap)

| Knob | Value |
| --- | --- |
| `VIT_MAX` / base drain | 10000 / ~33 per day (~10-month average life) |
| Care multiplier range | 0.7× … 3.5× |
| Serious-sickness drain | ~1500/day (fatal in 3–6 days) |
| Elderly / Twilight | 15% / 3% of max |
| Miracle max / bond floor / decay | 85% / 4000 / ×0.5 per save |
| Critical save | restore to 30%, ceiling −15%, Recovery days |
| Reprieve | +5% of max (~2–3 weeks) |
| Momentum | +10% old-age, +2.5% high-bond (≥ 6000) other, cap +50% |
| Deathbed tiers | ≥ 5000 modest, ≥ 8500 profound |

## 12. Deferred

- **Cemetery scene** — browse fallen pets from the lineage records (§7.4 keeps the data ready).
- **Pet-less economy minigame** — the one income source that works while the pet is Sick, asleep or
  frozen; economy phase E2 ([economy-and-inventory.md](economy-and-inventory.md)).
- **Checkup action** — qualitative vitality reading ("she's in great shape").
- **Species lifespan modifiers** — `speciesMult` is reserved but 1.0 everywhere in v1.
- **Memorial keepsake** — a free, unpurchasable item minted from the lineage record on death
  ([economy-and-inventory.md](economy-and-inventory.md) §10). Room decor bought with Bits also
  persists across generations, so the room becomes part of the family history.
- Keepsakes, lineage-gated evolution edges, grave/portrait on the home scene.
