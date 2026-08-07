# Conversations & Personality

Two interlocking systems that give the **care loop** meaning and turn a high bond into a real
payoff:

- **Personality** — an emergent identity that drifts from how you actually play, expressed as a
  named trait inside a sticky "way of being".
- **Conversations** — moddable dialogue trees the creature initiates, selected by its personality,
  its species, and what it remembers about *you*.

Battle-specific systems live in [battle-system.md](battle-system.md); the energy gate in
[training-and-energy.md](training-and-energy.md); deferred cross-cutting systems in
[roadmap.md](roadmap.md).

---

## 1. Design principles

1. **RAM is O(1) in the number of installed conversations.** A player with 20 conversations and a
   player with 20,000 modded ones use the same RAM. This is the hard constraint the loader is
   designed around — see [§5](#5-scaling-architecture). Only the *active* conversation is resident.
2. **Authoring stays drop-in.** A mod is a `.json` file in a folder. Modders never hand-write an
   index, manifest, or id registry. Everything derived is derived by the game or the build.
3. **Moddable from SD, additively.** SD content *adds* to the pools; it only replaces a base
   conversation on an exact id collision (the [creatures](../main/game/sim/creatures.cpp) precedent).
4. **Never hitch the frame.** Directory scans are time-sliced off the render path and only run when
   something that affects eligibility actually changed.
5. **No save wipe.** All new state lives in out-of-blob NVS keys, so `PET_VERSION` stays put and
   existing pets survive (the same trick as the nickname and the `"dbg"` flag).
6. **Degrade, never crash.** Oversized, malformed, or missing files are logged and skipped —
   matching how a dangling evolution target is handled today.

---

## 2. Personality

### 2.1 Natures and personalities

A **Nature** is the "way of being" — sticky, rarely changed. A **Personality** is the specific trait
within it, which drifts freely. Starter roster (all data-declared, all extensible):

| Nature | Core position | Personalities |
| --- | --- | --- |
| **Gentle** | timid + attached | Sweet · Dreamy · Shy |
| **Bold** | brave + attached | Brash · Proud · Fierce |
| **Clever** | brave + independent | Curious · Sly · Inventive |
| **Aloof** | timid + independent | Grumpy · Lazy · Stoic |

Natures need not be the same size, and a mod can add either a personality to an existing Nature or
an entirely new Nature, by dropping in a file.

### 2.2 Drift axes

Four hidden, signed axes (each roughly −1..+1), split into two roles:

| Axis | − | + | Role |
| --- | --- | --- | --- |
| `brave` | Timid | Brave | **core** → Nature |
| `social` | Independent | Attached | **core** → Nature |
| `energetic` | Calm | Energetic | **surface** → personality |
| `wild` | Disciplined | Wild | **surface** → personality |

**Core** axes place the creature in one of the four Nature quadrants; **surface** axes decide which
personality inside it. This is the literal reading of the 2-D grid: drifting sideways within a Nature
is easy because it only takes the surface axes, while changing Nature means moving the core axes,
which only the §2.5 events do. It also means each Nature only has to cover a *2-D* space with three
traits, which is what makes coverage tractable.

Each personality declares an **ideal direction** in the surface plane plus its parent Nature. Weights
let a trait emphasise or ignore an axis:

```json
{ "id": "curious", "name": "Curious", "nature": "clever",
  "ideal":  { "energetic": 0.707, "wild": 0.707 },
  "weight": { "energetic": 1.0,   "wild": 1.0 } }
```

Selection is a **weighted direction match** — `score = Σ(w·axis·ideal) / ‖w·ideal‖`, highest wins.
Because that is a linear functional of the drift vector, matching depends on the *direction* the
creature has drifted, not how far: you never have to max an axis to qualify for a trait. Nature is
*not* re-evaluated by proximity — it only changes on the events in §2.5.

### 2.3 What nudges the axes

Existing actions gain a second meaning, with no new gameplay. Values are per-event nudge vectors;
drift is an exponential moving average of them, so **the reachable region is the convex hull of these
vectors** weighted by how often the player does each.

**Duty actions are personality-neutral.** Feeding on time, cleaning poop and healing sickness are
what *every* competent player does constantly, so they say nothing about the player and carry **zero**
drift. Only **deviation** from the expected thing is signal. (Neutral events are harmless: they shrink
the drift vector's magnitude without rotating it, and matching only reads direction.) See §2.8 for why
this matters so much.

| Action | brave | energetic | social | wild |
| --- | --- | --- | --- | --- |
| **Feed basic food on time / clean / heal** | · | · | · | · |
| Feed a *flavoured* food | see [food-and-feeding.md](food-and-feeding.md) | | | |
| Overfeed (regardless of food) | −0.25 | · | +0.4 | +0.7 |
| Let hunger bottom out | −0.4 | · | −0.6 | +0.8 |
| Care mistake | −0.3 | · | −0.5 | +0.5 |
| Long idle / neglect | −0.5 | −0.5 | −0.6 | +0.8 |
| Pet (sustained affection) | −0.4 | −0.3 | +1.0 | −0.2 |
| Poke (rough play) | +0.1 | +1.0 | +0.5 | +0.6 |
| Run | +0.3 | +1.0 | −0.3 | −0.2 |
| Mind Maze | −0.3 | −0.6 | −0.3 | −0.8 |
| Smash | +0.8 | +0.8 | −0.3 | +1.0 |
| Bulwark | +1.0 | +0.6 | −0.3 | −1.0 |
| Stance | · | −1.0 | −0.3 | −0.6 |
| Fight a battle (any style) | +0.7 | +0.4 | −0.1 | +0.3 |
| Win a battle | +0.8 | +0.3 | +0.5 | +0.2 |
| Lose a battle | −0.9 | · | · | · |
| Respect the sleep schedule | −0.15 | −0.2 | · | −0.7 |
| Override the sleep schedule | +0.2 | −0.3 | · | +1.0 |
| **Conversation choices** | authored per choice — the strongest, most intentional nudges | | | |

Three non-obvious rules this table has to obey, each learned the hard way from the simulator (§2.7):

1. **`social` means interaction *style*, not care *quality*.** Feeding and cleaning are duty and
   barely move it; affection moves it a lot; solo minigames and battle grinding move it *down*. If
   care quality drove it, then `social−` would be a synonym for "bad owner" and the two independent
   Natures would be gated behind mistreating the creature.
2. **Every axis needs a strong negative side.** Care actions push `brave` *down* (being provided for
   breeds dependence) — without that, almost everything pushed `brave` up and Bold swallowed 57% of
   all outcomes while Aloof was unreachable.
3. **No two axes may always co-move.** Each pair needs actions that oppose them, or a whole quadrant
   dies: Bulwark is the deliberate brave+/wild− decorrelator, overriding sleep is the calm/wild+ one,
   and winning battles is the social+/brave+ one.
4. **Every axis needs a non-neglectful driver on both sides.** Since duty care is neutral, `brave−`
   now comes from *elective* affection (cuddling far more than needed breeds dependence) and from Mind
   Maze (quiet and cerebral). Without those, `brave−` would only come from starving/neglect/losing, and
   Gentle + Aloof would be locked behind mistreatment.
5. **No aggressive-vs-defensive split, deliberately.** Combat as built offers no genuine style
   choice — Special is strictly better than Strike, and parry is *reactive* rather than elective — so
   every player fights essentially the same way. Splitting drift by "style" would attribute signal to
   a decision nobody makes, which is rule 1's mistake in another costume. *Choosing to fight at all*
   is a real divergence point, so that carries drift; the style within a fight does not. **If combat
   later gains a real trade-off** (a stance/aggression toggle, risk-reward options, fleeing), this
   becomes worth revisiting — the rows were removed, not lost. Verified: collapsing style cost no
   trait its reachability.

Nudges are small and rate-limited so a single afternoon can't whiplash the identity; the intended
timescale is days.

### 2.4 Crystallization and hysteresis

- Egg / In-Training I have **no Nature** (displayed as *Unformed*). The Nature crystallizes at
  **In-Training II** from whatever drift has accumulated — the first "who is this creature" beat.
- Personality changes only when a challenger beats the incumbent's distance by a **margin** and
  holds it for a sustained period, evaluated at a checkpoint (waking up, evolving) rather than
  mid-frame. Prevents flip-flopping and creates a noticeable "they seem different lately…" moment,
  which is itself a conversation trigger.

### 2.5 Nature changes

| Trigger | Status |
| --- | --- |
| **Evolution** — if accumulated drift strongly contradicts the current Nature, it shifts | **v1** |
| Special items (nature-shift keepsakes) | Deferred with the item/economy system |
| Special one-shot story events | Later |
| Death | Parked — no death mechanic exists in the game |

Evolution is therefore the practical Nature-jump window in v1: it rewards a long, consistent way of
playing rather than a single action.

### 2.6 Visibility

The Stats/Journal screen names it — **"Curious (Clever)"** — but never exposes the axis values.
Identity is legible; steering it stays mysterious.

### 2.7 Validation

Reachability and overlap are quantitative, so they get measured rather than guessed:
[`tools/personality_sim.py`](../tools/personality_sim.py) Monte-Carlos ~19 player archetypes
(doting carer, battle addict, night trainer, hermit, chaos gremlin, …) through the nudge table and
reports which personality each lands on, how often identity flips, and — the metric that matters
most — **what share of each trait's landings came from owners who actually mistreat the creature**.
Re-run it after any change to the table or the roster; it is also the basis of the mod lint tool.

Current results: **all 4 natures and all 12 personalities reachable**, and ~0.3 personality changes per
playthrough (identity is sticky, not flickering). Only **Grumpy** and **Lazy** are reachable solely
through neglect, which is deliberate — an ignored creature *should* turn irritable or listless.
Everything aspirational, including both independent Natures, is reachable by owners who feed, clean and
heal properly but simply aren't cuddly (`diligent trainer`, `quiet keeper`, `solo athlete`,
`night trainer`). **Sly is currently the thinnest at ~2%** — its region (calm, unruly, brave *and*
independent) is inherently sparse, since being undisciplined and calm mostly comes from idling, which
also makes a creature timid and drags it toward Aloof.

Two placement rules fell out of this and are worth keeping in mind when adding traits:

- **Aim ideals at the reachable fan, not at even spacing.** Play does not distribute drift evenly, so
  a naively 120°-spaced ideal can point somewhere no playstyle ever produces. The script prints a
  per-Nature direction histogram for exactly this; three traits were nearly dead until re-aimed at it.
- **Keep sibling traits on the same axis set.** A trait that omits an axis loses the shared region to
  a sibling that aligns with two axes at once (this is what held Proud at 0.1%).

### 2.8 Care readout: why precise gauges were a problem

A numeric hunger/happiness bar is an **optimization target**: players fill it. The simulator confirms
the consequence — an archetype that just keeps every meter full landed *Gentle/Shy 100% of the time,
with zero variance*. Worse, the duty actions used to carry drift (`wild −0.4` for feeding, `−0.8` for
cleaning), so a visible gauge applied that as a **constant bias to every competent player**, quietly
compressing the whole personality space into one corner.

Two changes fix it without hiding information the player needs:

1. **Coarse care readout.** Hunger and happiness show as a few readable states (*starving / peckish /
   content / full*, or a mood face) rather than percentages — enough to look after the creature fairly,
   not enough to min-max. **HP and Energy stay exact**, because those are explicit game resources with
   published rules (battle gating, training costs) and precision there is fairness, not min-maxing.
2. **Multiple routes to a full meter, each drifting differently.** Happiness rises through affection
   (timid + attached), rough play (energetic + wild), or winning battles together (brave + attached).
   "Keep it happy" stops being one strategy and becomes a *parenting style*, so even a determined
   optimizer still ends up somewhere distinctive.

Measured result — three archetypes that all pin every gauge to full, differing only in *how*:

| Optimizer | Route | Lands on |
| --- | --- | --- |
| `optimizer/cuddle` | affection | Gentle / Shy |
| `optimizer/play` | rough play | Bold / Fierce |
| `optimizer/battle` | winning together | Bold / Brash |

The general principle, worth applying to any future system that feeds drift: **personality should be
driven by choices where players genuinely diverge, never by actions everyone performs identically.**

---

## 3. Conversation data model

A conversation is a **flat list of nodes** (not nested), so mods stay easy to author and patch.

```json
{
  "id": "shy/first_gift",
  "priority": 10,
  "repeatable": false,
  "when": {
    "minFriendship": 300,
    "minStage": 3,
    "personality": "shy",
    "seen": ["shy/intro"],
    "fact": { "knows_player_name": true }
  },
  "start": "n0",
  "nodes": [
    { "id": "n0", "text": "Hey... can I ask you something?",
      "choices": [
        { "text": "Of course",   "to": "n1",  "effects": { "friendship": 3, "drift": { "social": 0.05 } } },
        { "text": "Maybe later", "to": "end", "effects": { "happiness": -2 } }
      ] },
    { "id": "n1", "text": "What do you love doing most?",
      "choices": [
        { "text": "Exploring", "to": "end", "effects": { "setFact": { "player_likes": "explore" } } },
        { "text": "Resting",   "to": "end", "effects": { "setFact": { "player_likes": "rest" } } }
      ] }
  ]
}
```

**`when`** (all optional, all ANDed): `minFriendship` / `maxFriendship`, `minStage`, `personality`,
`nature`, `attribute`, `species`, `seen` / `notSeen` (conversation ids), `fact` (equality tests),
`minWins`, `sick` / `hungry` / `justEvolved`, `hourRange`.

**`effects`**: `friendship`, `happiness`, `drift` (axis nudges), `setFact`, `stat` boosts, and
`unlock` (marks a fact that gates a later conversation).

**Facts** are the memory of your choices — a small key→value store, and the mechanism behind
"conditional conversations selected by previous player choices".

Player answers are **multiple choice** in v1. Free-typed answers (and interpolating them back into
dialogue) are deferred — choices deliver almost all of the intimacy for a fraction of the work.

### Writing rules

Two conventions the linter enforces, both from playtesting:

1. **The creature must react to what the player said.** A choice that jumps straight to `"end"`
   stops the exchange on the player's line, which reads as not being heard. Route each choice to
   its **own** reply node — a shared reply is nearly as bad, since the creature then says the same
   thing whatever you chose. A choiceless node can chain with a node-level `"to"`, so a reaction
   can run to two beats without a meaningless `"..."` button in between.
2. **Spoken replies are written plainly; unspoken ones go in parentheses.** `"I'll stay a while"`
   is said aloud; `"(sit with them)"` or `"(say nothing)"` is something the player *does*. Without
   the brackets an action reads as dialogue, and a bare `"..."` reads as the player saying nothing
   out loud when they meant to be silent.
3. **Depth scales with the bond.** Early conversations are short and light; late ones are longer,
   more searching, and carry real emotional stakes. The node cap is 32 precisely so the late ones
   can branch properly.

| Bond tier | Gate | Shape |
| --- | --- | --- |
| Stranger / Acquaintance | 0–1499 | 1–3 nodes. Small talk, a single observation, one reaction. |
| Familiar / Friend | 1500–4999 | 4–10 nodes. Real questions about the player; the creature volunteers something of its own. Rifts become possible. |
| Trusted / Close | 5000–8499 | 10–20 nodes. Multi-step, branching, callbacks to earlier answers. |
| Bonded / Soulbound | 8500+ | Up to 32 nodes. The rare, profound ones — these are the payoff for months. |

Because the bond takes ~2.5 months to top out, most conversations must live in the first two rows.
Write the deep ones knowing they're for the players who stayed.

### Rifts (mood)

A choice may set `setMood` to `hurt` or `angry`. While upset the creature **refuses petting and
poking** (food still works — it won't be touched, but it will accept a gift, and feeding softens
the mood), and Home shows a rain-cloud or a steam puff instead of the attention badge. A
conversation gated on `mood: "upset"` (matching hurt *or* angry) with high `priority` is the way
back, and setting `setMood: "ok"` mends it — reconciliation is worth a big friendship gain, since
making up is one of the strongest bonding beats there is.

**A rift with no written way out is a broken care loop, not a sad story**, so the linter treats
"something can upset the creature but nothing gated on an upset mood clears it" as an error.

---

## 4. Pools and folder layout

Four pools, in increasing specificity. A conversation from a more specific pool outranks a generic
one when both are eligible.

Each pool directory is **flat** — one `.json` per conversation, no subdirectories. The scan is a
single time-sliced cursor, so recursing would mean carrying a stack of open directory handles
across frames for what is only an organisational nicety; the `when` gate already scopes a
conversation to its nature or trait. Name files `<scope>_<slug>.json` to keep them sorted, and
note that **a nested folder is invisible on device** — [`tools/conv_lint.py`](../tools/conv_lint.py)
treats one as an error for exactly that reason.

```
/gamedata/conversations/                   # flash partition (read-only, built at build time)
    natures/*.json                         #   broad: "a Gentle creature would say…"
    personalities/*.json                   #   finer: "specifically a Shy one…"
    player/*.json                          #   about YOU: hopes, likes, dreams
/sdcard/conversations/                     # SD: same layout, additive (+ id-collision override)
    natures/… personalities/… player/…

/creatures/<id>/conversations/*.json           # species-specific, ships with the creature
/sdcard/creatures/<id>/conversations/*.json    # species-specific, from a modded creature
```

Species conversations need no new partition — they ride along in the creature folder that already
exists. The generic pools live in a single new **`gamedata` FAT partition (2 MB)** mounted at
`/gamedata`, shared with `foods/` ([food-and-feeding.md](food-and-feeding.md)) and any future data
system, rather than adding a partition per feature. The flash map currently uses ~4 MB of 16 MB, so
there is ample room.

---

## 5. Scaling architecture

The invariant from §1.1 comes from splitting selection from loading into three layers.

### 5.1 Selection — streaming, constant RAM

The library is **never** held in memory. Selection streams candidates and keeps only the best few
in a fixed-size reservoir:

```
for each pool:
    for each candidate record:
        evaluate `when` against current pet/personality/facts state
        if eligible: offer it to a fixed K-slot reservoir (K = 8), ranked by
                     pool specificity, then priority, then unseen-one-shot first
pick weighted-randomly among the survivors   → variety, no "always the same greeting"
```

RAM cost: `K × sizeof(candidate)` ≈ a few hundred bytes, **independent of library size**. A
repeatable small-talk pool guarantees the reservoir is never empty, so the pet is never mute.

### 5.2 Index — an I/O optimization, never a requirement

Streaming per-file would mean one `fopen` per candidate. To keep that cheap at scale, each pool
directory may carry a derived **`_index.bin`**: fixed-size records holding the id hash, the `when`
gate fields, priority/flags, and the body's filename — everything selection needs, and nothing else.
It is read in **chunks through a small static buffer** (~32 records at a time), so even a
10,000-entry index costs 1–2 KB of scratch.

| Root | How the index is produced |
| --- | --- |
| Flash (`/conversations`) | Generated at **build time** by a Python tool and packed into the FAT image — the partition is mounted read-only. Mirrors `gen_creatures.py`. |
| SD (`/sdcard/…`) | Generated by the **game** on first scan and cached on the card; invalidated by a cheap dirty check (entry count + sizes). If the card is read-only or generation fails, fall back to §5.1. |
| Absent | Streaming per-file scan. Correct, just slower. |

So the index is always derived and always optional — modders keep dropping in `.json` files.

### 5.3 Body — one conversation resident, ever

Only the **chosen** conversation is parsed: read the file into a shared static scratch buffer,
`cJSON_Parse`, marshal into a fixed `ActiveConversation` struct (capped nodes/choices/text), then
free the cJSON document immediately. cJSON therefore only ever holds one small document, which also
keeps heap fragmentation bounded. Buffers live in PSRAM alongside the framebuffers.

### 5.4 Time-slicing

A scan processes a budgeted number of records per frame and is triggered only when eligibility could
have changed — friendship tier crossed, stage/personality changed, a new fact set, day rollover, or
the bubble cooldown expiring. Never per-frame, never blocking, so the frame rate is untouched.

### 5.5 Override resolution stays O(1)

Resolution happens for the **winner only**: when loading conversation `X`, try the SD path before the
flash path. During scanning, a base conversation that an SD file overrides may briefly occupy two
reservoir slots — harmless, since loading resolves to the SD copy either way.

### 5.6 State that grows with *play* (bounded on purpose)

| State | Representation | Cap |
| --- | --- | --- |
| Seen-set | 32-bit FNV hashes of ids, ring buffer | ~768 entries (~3 KB) |
| Player facts | key→value table, global NVS namespace | ~48 facts |
| Journal | conversation id + timestamp, newest-first | ~64 entries |

Hash collisions in the seen-set produce a rare, benign "already seen". The journal stores **ids, not
text** — titles and bodies are re-rendered from the data files, so history can't grow unbounded and
stays correct when a mod is updated. A removed mod degrades its entries to *"a forgotten memory"*.

---

## 6. Delivery

### 6.1 Pet-initiated bubbles

The creature starts conversations itself. A wanted conversation appears as a small **persistent,
tappable bubble** beside it — it does *not* time out and vanish, so the payoff can't be missed.

- Frequency scales with friendship: a Soulbound creature is chattier than a stranger. Another payoff
  axis for the bond.
- Suppressed while asleep and during minigames/battle, following the existing
  `Scene::allowsSleep()` precedent.
- Contextual pools fall out for free: sick, starving, and just-evolved dialogue.
- **Nothing is offered below `CONV_MIN_STAGE` (= `STAGE_IN_TRAINING_2`).** An egg and the form it
  first hatches into never talk, whatever the content says: speech is something the creature grows
  into, and it arrives with the same stage that unlocks Activities. This is a floor in
  `ConversationSystem::update()`, not a `minStage` gate per file, so it holds for mod content and
  for the nature/trait pools (which gate on personality, not on stage). A file may still ask for a
  later stage; `conv_lint.py` warns about one asking for an earlier one.

### 6.2 Journal

A scene reachable from the Menu:

- **History** — past conversations, newest first, re-rendered from data.
- **"What they know about you"** — your recorded facts, the emotional payoff made visible.
- **Identity** — the current `Personality (Nature)`.

Both pages need a scrolling list (§8, Phase 0).

---

## 7. Balance

Conversations are a **payoff**, not a friendship farm — mostly happiness and small friendship, so
they don't undercut the evolution gates and battle-reward pacing that also spend friendship.
**One-shot story beats** are the exception: bigger friendship, occasional stat boosts, and unlocking
further conversations. Item rewards are deferred with the economy system.

---

## 8. Build phases

| Phase | Work |
| --- | --- |
| **0** | **Engine prerequisites** — word-wrapped text + typewriter reveal in `engine/gfx`; a reusable scrolling list in `ui/widgets.hpp` (also retires the Activities picker's `(+N more)` limit). |
| **1** | **Personality core** — axes, data-loaded natures/personalities, drift hooks into existing actions, crystallization, hysteresis, evolution jump window, `Personality (Nature)` on the Stats screen. Also the §2.8 care changes: **coarse hunger/happiness readout** (Home HUD + Stats sheet; HP/Energy stay exact) and **multi-route happiness** (poking and battle wins raise it meaningfully, not just affection). *Ships value alone: care starts meaning something before any dialogue exists.* |
| **2** | **Conversation loader** — partition, pools, index + streaming scan, facts/seen persistence. |
| **3** | **`SceneConversation`** — bubble trigger, dialogue UI, choices, effects. |
| **4** | **Journal scene.** |
| **5** | **Content authoring + tuning.** |

---

## 9. Deferred

- Free-typed player answers + interpolating them into dialogue.
- Item rewards and nature-shift items (with the economy system).
- Death as a Nature-change trigger (no death mechanic exists).
- Voice/SFX per personality; per-species conversation art.
- ~~A Python lint tool for mod authors~~ — **built**: [`tools/conv_lint.py`](../tools/conv_lint.py)
  validates schema and every cap against the firmware's own constants, cross-references
  `to`/`start`/`requireSeen`/nature/trait ids, flags facts nothing sets and facts with no journal
  phrasing, and **replays the renderer's word-wrap** to warn when text will be visibly elided
  on screen. Run it before flashing content.

---

## 10. Tuning knobs

Axis nudge magnitudes and rate limits, the personality-switch margin and dwell time, the
crystallization stage, the Nature-jump contradiction threshold, bubble cadence vs. friendship, the
reservoir size `K`, per-frame scan budget, and all state caps in §5.6. Every number here is a first
pass, expected to move during playtesting.
