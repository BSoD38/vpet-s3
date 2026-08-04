#!/usr/bin/env python3
"""Monte-Carlo reachability check for the personality drift model.

Answers the two questions that decide whether the design in
docs/conversations-and-personality.md actually works:

  1. REACHABILITY -- can every Nature/personality be landed on by some plausible
     way of playing?  A trait that never wins is dead content.
  2. STABILITY -- once crystallized, does identity hold, or does it flicker
     between neighbours every few actions?

Model (mirrors the spec):
  * 4 drift axes.  CORE axes decide the Nature (sticky); SURFACE axes decide the
    personality within it (drifty).
  * Each action contributes a nudge VECTOR.  Drift is an exponential moving
    average of those vectors, so the reachable region is the convex hull of the
    action vectors weighted by play frequency -- a trait pointing outside that
    hull is unreachable, which is exactly what this script measures.
  * Selection is a WEIGHTED DIRECTION match: score = sum(w*axis*ideal) normalised
    by the ideal's own norm, so a trait never wins just for having a big ideal,
    and axes a trait omits are ignored (fat, forgiving cells).
  * Switching requires beating the incumbent by MARGIN (hysteresis).

Fidelity: this mirrors the FIRMWARE's model, not an idealized one --
  * crystallization requires CRYSTAL_MIN_MAG drift magnitude (personality.cpp), so a
    playthrough can end "unformed";
  * a challenger trait must stay ahead for DWELL_SECS before identity switches
    (modelled as DWELL_EVENTS consecutive events at an assumed EVENT_GAP_S cadence);
  * per-nudge strength scaling is modelled (the auto-sleep nudge lands at 0.5x);
  * every vector in ACTIONS is CHECKED against the DRIFT_* tables compiled into the
    firmware and the food-drift JSON before simulating -- a PASS against stale numbers
    is worse than no validation at all.

Pure stdlib, no hardware.  Run:  python tools/personality_sim.py
"""

import json
import os
import random
import re
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

AXES = ["brave", "energetic", "social", "wild"]
CORE = ["brave", "social"]        # decide Nature -- slow, sticky
SURFACE = ["energetic", "wild"]   # decide personality -- drifts with play

ALPHA = 0.02      # EMA rate: ~50-event memory, so identity tracks sustained behaviour
MARGIN = 0.08     # hysteresis: how much a challenger must beat the incumbent by
EVENTS = 600      # actions per simulated playthrough
RUNS = 300        # playthroughs per archetype
CRYSTALLIZE = 120 # earliest event the Nature may set (stands in for In-Training II)
# Mirrors personality.cpp: no identity until the drift vector has real magnitude, and a
# challenger must hold its lead for an hour of sim time before the trait flips.
CRYSTAL_MIN_MAG = 0.15
EVENT_GAP_S     = 300              # assumed sim-seconds between notable actions
DWELL_SECS      = 3600.0
DWELL_EVENTS    = max(1, int(DWELL_SECS // EVENT_GAP_S))

# Per-action nudge strength (PersonalityTracker::nudge's `strength`); unlisted = 1.0.
# The scheduled-sleep nudge is passive, so pet.cpp emits it at half strength.
STRENGTH = {"sleep_respect": 0.5}

# ---------------------------------------------------------------------------
# Action nudge table.  Deliberately DECORRELATED: for every pair of axes there
# must be actions moving them together AND in opposition, otherwise a whole
# quadrant becomes unreachable.  The decorrelators are marked.
# ---------------------------------------------------------------------------
ACTIONS = {
    # --- care.  CRUCIAL: `social` measures INTERACTION STYLE (time spent together vs
    # self-directed), NOT care quality.  Feeding/cleaning/healing are duty, so they
    # barely move it; affection moves it a lot.  Otherwise `social-` becomes a synonym
    # for "bad owner" and half the natures are gated behind neglect.
    # These also push `brave` NEGATIVE: being provided for breeds dependence, which is
    # what gives the brave axis a real negative side (without it Gentle/Aloof die).
    # DUTY actions are personality-NEUTRAL. Every competent player performs these
    # constantly (a visible gauge makes sure of it), so they carry no information
    # about the player -- and a constant bias term would drag everyone into the same
    # corner. Only DEVIATION from the expected thing is signal. Note that neutral
    # events are genuinely harmless here: they shrink the drift vector's magnitude
    # without rotating it, and matching only looks at direction.
    "feed_prompt":   {},          # basic kibble: the neutral duty default
    "clean_prompt":  {},
    "heal":          {},
    # Food TYPES. Feeding is the most frequent action in the game, so making it a
    # CHOICE converts the biggest duty into a divergence channel with no added grind.
    # Basic kibble stays neutral so a player who just wants the bar full is unskewed;
    # flavoured foods are opt-in self-expression. Effects must read from THEME, never
    # from displayed numbers, or players min-max personality directly (see doc 2.6).
    "feed_meat":     {"brave": +0.5, "wild": +0.3},
    "feed_greens":   {"wild": -0.5},
    "feed_treat":    {"social": +0.4, "energetic": +0.2, "wild": +0.6},
    "feed_spicy":    {"energetic": +0.7, "wild": +0.3},
    "feed_herbal":   {"energetic": -0.7, "wild": -0.3},
    # overfeeding / treats: indulgent rather than disciplined. The social+ AND wild+
    # decorrelator -- without it there is no "warm but chaotic" region at all.
    "feed_spoil":    {"brave": -0.25, "social": +0.4, "wild": +0.7},
    # The firmware's only "care mistake" drift IS the starve episode (DRIFT_STARVE); a
    # separate care_mistake vector existed here once, validating a nudge the device
    # never emits.
    "starve":        {"brave": -0.4, "social": -0.6, "wild": +0.8},
    # Affection is ELECTIVE, not duty -- cuddling far more than needed is a choice,
    # and it is now the main non-neglectful source of `brave-` (dependence).
    "pet":           {"brave": -0.4, "social": +1.0, "energetic": -0.3, "wild": -0.2},
    "poke":          {"brave": +0.1, "social": +0.5, "energetic": +1.0, "wild": +0.6},
    # idling / neglect: timid, low energy, no discipline. Makes Aloof/Lazy reachable
    # and is the E- W+ decorrelator (everything else pairs calm WITH discipline).
    "idle":          {"brave": -0.5, "social": -0.6, "energetic": -0.5, "wild": +0.8},

    # --- minigames: solo, self-directed activity, so mildly social-. This is what
    # lets a DILIGENT owner (feeds, cleans, heals) still raise an independent-natured
    # creature -- Clever/Aloof no longer require neglecting it.
    "run":           {"brave": +0.3, "social": -0.3, "energetic": +1.0, "wild": -0.2},
    # Mind Maze is the non-neglectful route to timid+independent (Aloof): quiet,
    # solitary, cerebral. Without it, `brave-` paired with `social-` would only be
    # reachable through mistreatment again.
    "mindmaze":      {"brave": -0.3, "social": -0.3, "energetic": -0.6, "wild": -0.8},
    "smash":         {"brave": +0.8, "social": -0.3, "energetic": +0.8, "wild": +1.0},
    # Bulwark = disciplined bravery: the brave+ / wild- decorrelator, and also
    # energetic+ / wild- so the surface axes don't co-move either.
    "bulwark":       {"brave": +1.0, "social": -0.3, "energetic": +0.6, "wild": -1.0},
    "stance":        {"social": -0.3, "energetic": -1.0, "wild": -0.6},

    # --- battle.  There is deliberately NO aggressive-vs-defensive split: combat today
    # offers no real style choice (Special is strictly better than Strike, and parry is
    # reactive rather than elective), so every player fights the same way. Splitting drift
    # by "style" would attribute signal to a decision nobody actually makes -- the same
    # mistake as letting duty care shape personality.
    # NOTE: the firmware nudges ONLY on the outcome (DRIFT_WIN / DRIFT_LOSS); there is no
    # per-fight vector, so this table must not invent one ("battle_fight" used to, and the
    # sim then validated drift the device never emitted). Winning TOGETHER is the bonding
    # event (social+), the social+/brave+ decorrelator keeping Bold reachable.
    "battle_win":    {"brave": +0.8, "social": +0.5, "energetic": +0.3, "wild": +0.2},
    "battle_loss":   {"brave": -0.9},

    # --- sleep discipline. Overriding the schedule leaves the creature unruly but
    # also GROGGY (energetic-), which is what opens up the "calm yet undisciplined"
    # direction for brave creatures -- otherwise that region is only reachable via
    # idling, which makes them timid and drags them into Aloof instead.
    "sleep_respect": {"brave": -0.15, "energetic": -0.2, "wild": -0.7},
    "sleep_override":{"brave": +0.2, "energetic": -0.3, "wild": +1.0},
}

# ---------------------------------------------------------------------------
# Roster.  Natures split the CORE plane into quadrants; within each, three
# personalities spread across the SURFACE plane.  `w` omits axes the trait is
# indifferent to.
# ---------------------------------------------------------------------------
NATURES = {
    "gentle": {"ideal": {"brave": -1, "social": +1}},
    "bold":   {"ideal": {"brave": +1, "social": +1}},
    "clever": {"ideal": {"brave": +1, "social": -1}},
    "aloof":  {"ideal": {"brave": -1, "social": -1}},
}

# Within a Nature the three ideals are unit vectors spaced 120 degrees apart in the
# (energetic, wild) plane. Because the score is a linear functional of the drift
# vector, evenly spaced ideals carve the plane into three EQUAL 120-degree wedges --
# so no sibling can be geometrically squeezed out, and reachability reduces to the
# single question of whether play can point the drift in that direction at all.
# (Mixing ideals that omit an axis with ideals that don't breaks this fairness: the
# two-axis one wins the shared region. Keep siblings on the same axis set.)
def _dir(deg, axes=("energetic", "wild")):
    import math
    r = math.radians(deg)
    return {axes[0]: round(math.cos(r), 3), axes[1]: round(math.sin(r), 3)}


_W2 = {"energetic": 1.0, "wild": 1.0}

# Angles are aimed at each nature's REACHABLE fan (see the direction histogram this
# script prints), not spaced a naive 120 apart: play doesn't distribute drift evenly,
# so an evenly-spaced ideal can point at a direction no playstyle ever produces.
PERSONALITIES = {
    # gentle: playful / dreamy / quiet
    "sweet":      {"nature": "gentle", "ideal": _dir(20),  "w": _W2},
    "dreamy":     {"nature": "gentle", "ideal": _dir(115), "w": _W2},
    "shy":        {"nature": "gentle", "ideal": _dir(265), "w": _W2},
    # bold: reckless / dignified / controlled aggression
    "brash":      {"nature": "bold", "ideal": _dir(70),  "w": _W2},
    "proud":      {"nature": "bold", "ideal": _dir(190), "w": _W2},
    "fierce":     {"nature": "bold", "ideal": _dir(330), "w": _W2},
    # clever: explorer / calculating / methodical tinkerer
    "curious":    {"nature": "clever", "ideal": _dir(45),  "w": _W2},
    "sly":        {"nature": "clever", "ideal": _dir(215), "w": _W2},
    "inventive":  {"nature": "clever", "ideal": _dir(300), "w": _W2},
    # aloof: irritable / idle / impassive
    "grumpy":     {"nature": "aloof", "ideal": _dir(45),  "w": _W2},
    "lazy":       {"nature": "aloof", "ideal": _dir(115), "w": _W2},
    "stoic":      {"nature": "aloof", "ideal": _dir(240), "w": _W2},
}

# ---------------------------------------------------------------------------
# Player archetypes: relative frequency of each action.  These stand in for
# "plausible ways someone actually plays", including lopsided ones.
# ---------------------------------------------------------------------------
ARCHETYPES = {
    # --- GAUGE OPTIMIZERS: all three keep every meter pinned full, and differ ONLY in
    # which route they use to do it. If the multi-route design works they must land on
    # three different creatures -- that is the whole defence against visible gauges
    # flattening the personality space.
    "optimizer/cuddle": {"feed_prompt": 8, "clean_prompt": 6, "heal": 2, "pet": 10,
                         "sleep_respect": 4},
    "optimizer/play":   {"feed_prompt": 8, "clean_prompt": 6, "heal": 2, "poke": 10,
                         "sleep_respect": 4},
    # (battle_fight events are now expressed as a win/loss mix -- the firmware only
    # nudges on outcomes, so "fights a lot" means "wins and loses a lot".)
    "optimizer/battle": {"feed_prompt": 8, "clean_prompt": 6, "heal": 2, "battle_win": 11,
                         "battle_loss": 3, "sleep_respect": 3},

    # --- FOOD-ONLY DIVERGENCE: identical play in every respect except which food they
    # choose. If these land differently, food type alone is a real expression channel.
    "food/meat":        {"feed_meat": 10, "clean_prompt": 6, "heal": 2, "pet": 4,
                         "sleep_respect": 4, "run": 2},
    "food/greens":      {"feed_greens": 10, "clean_prompt": 6, "heal": 2, "pet": 4,
                         "sleep_respect": 4, "run": 2},
    "food/spicy":       {"feed_spicy": 10, "clean_prompt": 6, "heal": 2, "pet": 4,
                         "sleep_respect": 4, "run": 2},
    "food/herbal":      {"feed_herbal": 10, "clean_prompt": 6, "heal": 2, "pet": 4,
                         "sleep_respect": 4, "run": 2},
    "food/treats":      {"feed_treat": 10, "clean_prompt": 6, "heal": 2, "pet": 4,
                         "sleep_respect": 4, "run": 2},

    "doting carer":     {"feed_prompt": 8, "pet": 10, "clean_prompt": 6, "heal": 3,
                         "sleep_respect": 5, "run": 1, "mindmaze": 1},
    "playful friend":   {"poke": 10, "pet": 6, "feed_prompt": 4, "run": 5,
                         "clean_prompt": 3},
    "indulgent":        {"feed_spoil": 10, "pet": 6, "sleep_override": 5,
                         "clean_prompt": 2},
    "neglectful":       {"idle": 12, "starve": 9, "feed_prompt": 2,
                         "sleep_override": 2},
    # --- Dutiful-but-not-cuddly owners. These exist to prove the independent
    # natures (Clever / Aloof) are reachable WITHOUT mistreating the creature:
    # every one of them feeds, cleans and heals properly.
    "diligent trainer": {"feed_prompt": 6, "clean_prompt": 5, "heal": 2, "bulwark": 8,
                         "stance": 6, "sleep_respect": 5},
    "solo athlete":     {"feed_prompt": 5, "clean_prompt": 4, "heal": 2, "run": 10,
                         "smash": 4, "sleep_respect": 3},
    "night owl":        {"feed_prompt": 4, "clean_prompt": 3, "heal": 2,
                         "battle_win": 6, "battle_loss": 3, "sleep_override": 9},
    "quiet keeper":     {"feed_prompt": 6, "clean_prompt": 5, "heal": 2, "mindmaze": 6,
                         "stance": 5, "sleep_respect": 6},
    # affectionate AND drills hard: the energetic-but-disciplined bold player
    "loyal athlete":    {"run": 10, "bulwark": 5, "pet": 6, "feed_prompt": 4,
                         "clean_prompt": 3, "battle_win": 2},
    # plays late, keeps it up past bedtime, trains solo -- but feeds and cleans
    # properly. The groggy/unruly corner, reached without any mistreatment.
    "night trainer":    {"sleep_override": 9, "mindmaze": 5, "smash": 3,
                         "feed_prompt": 5, "clean_prompt": 4, "heal": 2},
    "battle addict":    {"battle_win": 11, "battle_loss": 5, "smash": 5,
                         "feed_prompt": 3, "heal": 2, "sleep_override": 3, "poke": 2},
    # was "defensive duelist" -- with no in-combat style, a defensive player is really
    # someone who DRILLS defence (Bulwark) and fights: same shape, honest label.
    "guardian":         {"bulwark": 8, "battle_win": 6, "battle_loss": 2,
                         "feed_prompt": 3, "clean_prompt": 3, "heal": 2},
    "zen warrior":      {"stance": 9, "battle_win": 5, "battle_loss": 2, "pet": 4,
                         "feed_prompt": 4, "sleep_respect": 5},
    "puzzle solver":    {"mindmaze": 10, "stance": 5, "feed_prompt": 3, "pet": 2,
                         "sleep_respect": 4},
    "hermit":           {"mindmaze": 8, "sleep_respect": 8, "stance": 4,
                         "starve": 5, "idle": 3},
    "athlete":          {"run": 10, "smash": 4, "poke": 3, "feed_prompt": 3,
                         "sleep_respect": 2},
    "disciplinarian":   {"bulwark": 8, "stance": 5, "clean_prompt": 5, "feed_prompt": 4,
                         "sleep_respect": 8, "mindmaze": 3},
    "chaos gremlin":    {"poke": 10, "smash": 6, "sleep_override": 8, "idle": 3,
                         "starve": 3},
    "pesterer":         {"poke": 10, "idle": 7, "starve": 5, "sleep_override": 4,
                         "feed_prompt": 1},
    "balanced":         {k: 1 for k in ACTIONS},
    "rough player":     {"poke": 6, "smash": 4, "battle_win": 2, "battle_loss": 2,
                         "idle": 4, "starve": 3, "feed_prompt": 2},
}

# Archetypes that actually mistreat the creature (starving, care mistakes, long
# neglect).  A trait reachable ONLY from these is content you have to be a bad owner
# to see -- fine for Aloof/Lazy by design, a bug for anything aspirational.
NEGLECTFUL = {"neglectful", "pesterer", "chaos gremlin", "rough player"}


def score(axes, ideal, weights):
    """Weighted direction match, normalised by the ideal's own norm."""
    num = 0.0
    den = 0.0
    for a, iv in ideal.items():
        w = weights.get(a, 1.0)
        num += w * axes[a] * iv
        den += w * iv * iv
    return num / (den ** 0.5) if den > 0 else 0.0


def pick(axes, candidates):
    """Best-scoring candidate: [(name, ideal, weights), ...]"""
    best, best_s = None, -1e9
    for name, ideal, w in candidates:
        s = score(axes, ideal, w)
        if s > best_s:
            best, best_s = name, s
    return best, best_s


def nature_candidates():
    return [(n, d["ideal"], {a: 1.0 for a in CORE}) for n, d in NATURES.items()]


def trait_candidates(nature):
    return [(p, d["ideal"], d["w"])
            for p, d in PERSONALITIES.items() if d["nature"] == nature]


def simulate(weights, rng):
    """One playthrough. Returns (nature, final trait, switch count, surface angle).
    nature is None when the drift never reached CRYSTAL_MIN_MAG (device: 'Unformed')."""
    axes = {a: 0.0 for a in AXES}
    names = list(weights.keys())
    probs = [weights[n] for n in names]

    nature = None
    trait = None
    challenger, dwell = None, 0
    switches = 0

    for i in range(EVENTS):
        act = rng.choices(names, weights=probs, k=1)[0]
        vec = ACTIONS[act]
        a = ALPHA * STRENGTH.get(act, 1.0)      # per-nudge strength, like the firmware
        for ax in AXES:
            axes[ax] += (vec.get(ax, 0.0) - axes[ax]) * a

        if nature is None:
            # Crystallize once developed enough AND the play has said something --
            # mirrors CRYSTAL_MIN_MAG (an all-zero vector must not be handed an identity,
            # where every trait ties and the first would win).
            if i >= CRYSTALLIZE:
                mag = sum(v * v for v in axes.values()) ** 0.5
                if mag >= CRYSTAL_MIN_MAG:
                    nature, _ = pick(axes, nature_candidates())
                    trait, _ = pick(axes, trait_candidates(nature))
            continue

        # Challenger + dwell, mirroring PersonalityTracker::tick: a better-scoring trait
        # must STAY ahead by MARGIN for DWELL_EVENTS in a row before identity shifts.
        cands = trait_candidates(nature)
        cur = next(c for c in cands if c[0] == trait)
        best, best_s = pick(axes, cands)
        if best == trait or best_s <= score(axes, cur[1], cur[2]) + MARGIN:
            challenger, dwell = None, 0
        else:
            if challenger != best:
                challenger, dwell = best, 0
            dwell += 1
            if dwell >= DWELL_EVENTS:
                trait = best
                switches += 1
                challenger, dwell = None, 0

    import math
    angle = math.degrees(math.atan2(axes["wild"], axes["energetic"])) % 360
    return nature, trait, switches, angle


def verify_firmware_vectors():
    """Refuse to validate against stale numbers: every vector in ACTIONS must match the
    DRIFT_* table actually compiled into the firmware (pet.cpp + the minigame scenes) and
    the food-drift JSON shipped in flash_gamedata. Returns a list of mismatches."""
    problems = []

    mapping = {   # DRIFT_<name> in C++  ->  ACTIONS key here
        "OVERFEED": "feed_spoil", "STARVE": "starve", "IDLE": "idle",
        "AFFECTION": "pet", "ROUGH": "poke", "WIN": "battle_win", "LOSS": "battle_loss",
        "SLEEP_OK": "sleep_respect", "SLEEP_NO": "sleep_override",
        "RUN": "run", "MAZE": "mindmaze", "SMASH": "smash",
        "BULWARK": "bulwark", "STANCE": "stance",
    }

    pat = re.compile(r"DRIFT_(\w+)\[AX_COUNT\]\s*=\s*\{([^}]*)\}")
    found = {}
    for base, _dirs, files in os.walk(os.path.join(ROOT, "main", "game")):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            with open(os.path.join(base, fn), encoding="utf-8") as f:
                text = f.read()
            for m in pat.finditer(text):
                found[m.group(1)] = [float(x.strip().rstrip("fF"))
                                     for x in m.group(2).split(",")]

    for cname, action in mapping.items():
        if cname not in found:
            problems.append(f"firmware vector DRIFT_{cname} not found under main/game/")
            continue
        sim = ACTIONS.get(action)
        if sim is None:
            problems.append(f"ACTIONS['{action}'] missing (mirrors DRIFT_{cname})")
            continue
        for i, ax in enumerate(AXES):           # AXES order == the firmware's DriftAxis
            if abs(found[cname][i] - sim.get(ax, 0.0)) > 1e-6:
                problems.append(f"DRIFT_{cname}.{ax} = {found[cname][i]} in firmware, but "
                                f"ACTIONS['{action}'] says {sim.get(ax, 0.0)}")

    # Food drift ships as data, not code -- compare against the JSON itself.
    food_map = {"meat": "feed_meat", "greens": "feed_greens", "treat": "feed_treat",
                "spicy_pod": "feed_spicy", "herbal_mash": "feed_herbal",
                "kibble": "feed_prompt"}
    for food, action in food_map.items():
        path = os.path.join(ROOT, "flash_gamedata", "foods", food, "food.json")
        if not os.path.isfile(path):
            problems.append(f"{path} missing (mirrors ACTIONS['{action}'])")
            continue
        with open(path, encoding="utf-8") as f:
            drift = json.load(f).get("drift") or {}
        sim = ACTIONS.get(action, {})
        for ax in AXES:
            if abs(drift.get(ax, 0.0) - sim.get(ax, 0.0)) > 1e-6:
                problems.append(f"foods/{food} drift.{ax} = {drift.get(ax, 0.0)}, but "
                                f"ACTIONS['{action}'] says {sim.get(ax, 0.0)}")
    return problems


def main():
    problems = verify_firmware_vectors()
    if problems:
        print("FIRMWARE MIRROR CHECK FAILED -- this simulator would validate numbers the")
        print("device does not run. Fix ACTIONS or the firmware/data tables first:")
        for p in problems:
            print(f"  {p}")
        return 1
    print("firmware mirror check OK: ACTIONS matches pet.cpp, the minigame scenes, "
          "and the food JSON\n")

    rng = random.Random(20260803)
    all_traits = Counter()
    all_natures = Counter()
    kind_traits = Counter()        # landings from NON-neglectful archetypes only
    angles = {n: [] for n in NATURES}
    total_switches = 0
    total_unformed = 0

    print(f"{'archetype':<20} {'nature':<8} {'personality':<12} {'switches/run':>12}")
    print("-" * 56)

    for arch, weights in ARCHETYPES.items():
        traits = Counter()
        natures = Counter()
        switches = 0
        unformed = 0
        for _ in range(RUNS):
            n, t, sw, ang = simulate(weights, rng)
            if n is None:                      # never crystallized (mag < CRYSTAL_MIN_MAG)
                unformed += 1
                continue
            traits[t] += 1
            natures[n] += 1
            switches += sw
            angles[n].append(ang)
        all_traits.update(traits)
        all_natures.update(natures)
        if arch not in NEGLECTFUL:
            kind_traits.update(traits)
        total_switches += switches
        total_unformed += unformed

        top_n = natures.most_common(1)[0] if natures else ("unformed", RUNS)
        top_t = traits.most_common(1)[0] if traits else ("-", 0)
        tag = " (neglect)" if arch in NEGLECTFUL else ""
        print(f"{arch + tag:<20} {top_n[0]:<8} {top_t[0]:<12} {switches / RUNS:>12.2f}")
        if len(traits) > 1:
            spread = ", ".join(f"{t} {100*c//RUNS}%" for t, c in traits.most_common())
            print(f"{'':<20} -> {spread}")
        if unformed:
            print(f"{'':<20} -> UNFORMED in {100*unformed//RUNS}% of runs "
                  f"(drift magnitude stayed under {CRYSTAL_MIN_MAG})")

    runs_total = RUNS * len(ARCHETYPES)
    print("\n=== NATURE COVERAGE ===")
    for n in NATURES:
        c = all_natures[n]
        print(f"  {n:<10} {100*c/runs_total:5.1f}%  {'DEAD' if c == 0 else ''}")

    # Where does play actually point the surface axes?  Ideals should be placed inside
    # each nature's reachable fan; a trait aimed at an empty bin can never win.
    print("\n=== REACHABLE SURFACE DIRECTIONS (30-degree bins, per nature) ===")
    for n in NATURES:
        if not angles[n]:
            continue
        bins = Counter(int(a // 30) * 30 for a in angles[n])
        row = "  ".join(f"{b:>3}:{100*bins[b]//len(angles[n]):>2}%"
                        for b in sorted(bins))
        ideals = ", ".join(
            f"{p}@{int(round(__import__('math').degrees(__import__('math').atan2(d['ideal']['wild'], d['ideal']['energetic'])) % 360))}"
            for p, d in PERSONALITIES.items() if d["nature"] == n)
        print(f"  {n:<7} {row}")
        print(f"  {'':<7} ideals: {ideals}")

    print("\n=== PERSONALITY COVERAGE ===")
    dead, neglect_only = [], []
    for p, d in PERSONALITIES.items():
        c, kc = all_traits[p], kind_traits[p]
        if c == 0:
            dead.append(p)
        elif kc * 200 < runs_total:          # <0.5% of runs from decent owners
            neglect_only.append(p)
        print(f"  {p:<11} ({d['nature']:<6}) total {100*c/runs_total:5.1f}%"
              f"   kind-owner {100*kc/runs_total:5.1f}%"
              f"  {'<-- UNREACHABLE' if c == 0 else ''}")

    print(f"\nmean personality switches per playthrough: "
          f"{total_switches / runs_total:.2f}")
    if total_unformed:
        print(f"unformed playthroughs (never crystallized): "
              f"{100 * total_unformed / runs_total:.1f}%")
    if dead:
        print(f"\nFAIL: unreachable -> {', '.join(dead)}")
        return 1
    print("\nPASS: every nature and personality is reachable")
    if neglect_only:
        print(f"NOTE: only reachable by mistreating the creature -> "
              f"{', '.join(neglect_only)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
