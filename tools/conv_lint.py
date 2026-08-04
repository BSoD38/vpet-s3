#!/usr/bin/env python3
"""Validate conversation content against what the firmware can actually load.

Hand-authored JSON degrades quietly on device: an over-long line is silently elided, a typo'd
`to:` target ends the conversation early, a gate naming a trait that doesn't exist can never
match, an oversized file is skipped entirely. With a handful of files that's survivable; across a
full roster it's a menace -- and mods have no other safety net at all. So this checks, before
flashing:

  * schema + types, and every field against the FIRMWARE'S caps (mirrored below)
  * file size against the firmware's read cap (over it, the file is silently skipped)
  * text that will be visibly truncated on screen, by replaying the renderer's wrap
  * cross-references: node `to`, `start`, `requireSeen`, nature/personality ids
  * duplicate ids, unreachable nodes, facts nothing ever sets, facts with no journal phrasing
  * the two writing rules: the creature must react to a choice, and unspoken replies are
    parenthesised
  * that a creature which can be upset can always be un-upset again
  * files the scanner will never see (subdirectories -- the pool scan is deliberately flat)

Handles both layouts: a file may be ONE conversation (object) or a PACK of them (array).

Pure stdlib.  Run:  python tools/conv_lint.py
Exit code is non-zero if there are errors (warnings alone don't fail).
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMEDATA = os.path.join(ROOT, "flash_gamedata")
CREATURES = os.path.join(ROOT, "flash_creatures")

# --- caps, mirrored from main/game/sim/conversation.hpp ---------------------------------
# These are the STORABLE maxima: the firmware buffer size minus the NUL that jstr()'s
# strncpy(dst, s, n-1) always reserves. Checking against the raw buffer size passed
# exactly-at-cap content that was then silently truncated on device -- and a truncated
# conversation ID breaks requireSeen chains forever (the gate hashes the full string,
# markSeen hashes the truncation; they can never match).
ID_MAX, TITLE_MAX, TEXT_MAX, CHOICE_MAX = 40 - 1, 32 - 1, 128 - 1, 96 - 1
KEY_MAX, NOTE_MAX, NODE_ID_MAX, TO_MAX = 24 - 1, 48 - 1, 16 - 1, 16 - 1
MAX_NODES, MAX_CHOICES = 32, 3
MOODS = ("ok", "hurt", "angry")
MOOD_GATES = MOODS + ("upset",)          # gates may also ask for "upset" (hurt OR angry)
FRIENDSHIP_MAX = 10000                   # sim/pet.hpp: a gate above this can never pass
# read_file()'s cap in conversation.cpp. An oversized file is skipped on device (with only a
# log line nobody sees), which is precisely the failure this script exists to make visible.
MAX_FILE_BYTES = 32768

# --- render widths, mirrored from scenes/care/scene_conversation.cpp --------------------
# Speech: (GAME_W-24) - 2*TEXT_PAD = 196px at 6px/char.  Choice: 240-24-2*CHOICE_PAD = 200px.
SPEECH_COLS, SPEECH_LINES = 196 // 6, 5
CHOICE_COLS, CHOICE_LINES = 200 // 6, 2

POOLS = ("natures", "personalities", "player")
AXES = ("brave", "energetic", "social", "wild")

errors, warnings = [], []


def err(where, msg):
    errors.append(f"{where}: {msg}")


def warn(where, msg):
    warnings.append(f"{where}: {msg}")


def wrapped_lines(text, cols):
    """Greedy wrap matching gfx.cpp wrap_core(), so the count agrees with the device."""
    lines, i, n = 0, 0, len(text)
    while i < n:
        take, brk = 0, -1
        while i + take < n and take < cols and text[i + take] != "\n":
            if text[i + take] == " ":
                brk = take
            take += 1
        skip = 0
        at = i + take
        if at < n and text[at] == "\n":
            skip = 1
        elif at < n and text[at] == " ":
            skip = 1
        elif at < n and brk >= 0:
            take, skip = brk, 1
        lines += 1
        i += take + skip
    return max(1, lines)


def check_str(where, field, value, cap):
    if not isinstance(value, str):
        err(where, f"{field} must be a string")
        return False
    # BYTES, not code points: the firmware buffers count bytes. (The 6px font is ASCII-only
    # anyway, so non-ASCII would also render as garbage -- flag it.)
    blen = len(value.encode("utf-8"))
    if blen > cap:
        err(where, f"{field} is {blen} bytes, firmware stores at most {cap}")
        return False
    if not value.isascii():
        warn(where, f"{field} contains non-ASCII characters -- the 6px font renders ASCII only")
    return True


def load_ids(path, label):
    """natures/base.json + personalities/base.json declare the ids gates may reference."""
    out = set()
    d = os.path.join(GAMEDATA, path)
    if not os.path.isdir(d):
        warn(label, f"missing directory {d}")
        return out
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".json"):
            continue
        try:
            with open(os.path.join(d, fn), encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:                                    # noqa: BLE001
            err(f"{label}/{fn}", f"unreadable: {e}")
            continue
        if not isinstance(data, list):
            err(f"{label}/{fn}", "must be a JSON array of entries")
            continue
        for e in data:
            if isinstance(e, dict) and e.get("id"):
                out.add(e["id"])
    return out


def collect_files():
    """Every file the device's scanner will actually read, with its pool."""
    found = []
    for pool in POOLS:
        d = os.path.join(GAMEDATA, "conversations", pool)
        if not os.path.isdir(d):
            warn(f"conversations/{pool}", "pool directory does not exist")
            continue
        for entry in sorted(os.listdir(d)):
            p = os.path.join(d, entry)
            if os.path.isdir(p):
                # The scan reads directory entries as files and skips what won't open, so a
                # nested folder is invisible on device rather than an error. Flag it loudly.
                err(f"conversations/{pool}/{entry}",
                    "is a DIRECTORY -- the pool scan is flat, so nothing inside it will ever "
                    "load. Move the files up and prefix the name instead.")
                continue
            if entry.endswith(".json"):
                found.append((f"{pool}/{entry}", p, pool))
    if os.path.isdir(CREATURES):
        for cid in sorted(os.listdir(CREATURES)):
            d = os.path.join(CREATURES, cid, "conversations")
            if not os.path.isdir(d):
                continue
            for entry in sorted(os.listdir(d)):
                if entry.endswith(".json"):
                    found.append((f"{cid}/{entry}", os.path.join(d, entry), "species"))
    return found


def check_conversation(label, data, st):
    """Validate one conversation. `st` carries cross-file state."""
    cid = data.get("id", "")
    if not check_str(label, "id", cid, ID_MAX) or not cid:
        err(label, "missing a non-empty id")
        return
    if cid in st["convs"]:
        err(label, f"duplicate id '{cid}' (also in {st['convs'][cid]})")
    st["convs"][cid] = label

    if "title" in data:
        check_str(label, "title", data["title"], TITLE_MAX)
    else:
        warn(label, "no title -- the journal will show the raw id")

    # --- gate ---
    w = data.get("when", {})
    if not isinstance(w, dict):
        err(label, "`when` must be an object")
        w = {}
    if w.get("nature") and w["nature"] not in st["natures"]:
        err(label, f"when.nature '{w['nature']}' is not a declared nature -- can never match")
    if w.get("personality") and w["personality"] not in st["traits"]:
        err(label, f"when.personality '{w['personality']}' is not a declared trait -- can never match")
    if w.get("mood") and w["mood"] not in MOOD_GATES:
        err(label, f"when.mood '{w['mood']}' must be one of {MOOD_GATES}")
    # Numeric gate ranges. The firmware clamps these now, but a clamped value still means
    # the file doesn't gate the way its author intended -- catch it at write time.
    for fld, lo, hi in (("minFriendship", 0, 65535), ("maxFriendship", 0, 65535),
                        ("minStage", 0, 7), ("minWins", 0, 10**9)):
        if fld in w and not (isinstance(w[fld], int) and lo <= w[fld] <= hi):
            err(label, f"when.{fld} must be an integer {lo}..{hi}")
    if isinstance(w.get("minFriendship"), int) and w["minFriendship"] > FRIENDSHIP_MAX:
        err(label, f"when.minFriendship {w['minFriendship']} is above FRIENDSHIP_MAX "
                   f"({FRIENDSHIP_MAX}) -- this gate can never pass")
    for h in ("hourMin", "hourMax"):
        if h in w and not (isinstance(w[h], int) and 0 <= w[h] <= 23):
            err(label, f"when.{h} must be an integer 0..23")
    if ("hourMin" in w) != ("hourMax" in w):
        err(label, "hourMin and hourMax must be given together (the device needs both)")
    if w.get("requireSeen"):
        st["require_seen"].append((label, w["requireSeen"]))
    fg = w.get("fact", {})
    if isinstance(fg, dict):
        for k in fg:
            st["used_facts"].setdefault(k, []).append(label)
    if w.get("notFact"):
        st["used_facts"].setdefault(w["notFact"], []).append(label)

    # --- nodes ---
    nodes = data.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        err(label, "needs a non-empty `nodes` array")
        return
    if len(nodes) > MAX_NODES:
        err(label, f"{len(nodes)} nodes, firmware keeps only {MAX_NODES}")

    ids, targets, dead_ends = [], [], []
    sets_ok, sets_upset = False, False

    for idx, nd in enumerate(nodes[:MAX_NODES]):
        at = f"{label} node[{idx}]"
        if not isinstance(nd, dict):
            err(at, "must be an object")
            continue
        nid = nd.get("id", "")
        if check_str(at, "id", nid, NODE_ID_MAX) and nid:
            if nid in ids:
                err(at, f"duplicate node id '{nid}'")
            ids.append(nid)
        else:
            err(at, "missing a non-empty id")

        text = nd.get("text", "")
        if check_str(at, "text", text, TEXT_MAX):
            if wrapped_lines(text, SPEECH_COLS) > SPEECH_LINES:
                warn(at, f"wraps to more than {SPEECH_LINES} lines -- the tail will be elided")

        chs = nd.get("choices", [])
        if not isinstance(chs, list):
            err(at, "`choices` must be an array")
            chs = []
        if len(chs) > MAX_CHOICES:
            err(at, f"{len(chs)} choices, firmware keeps only {MAX_CHOICES}")
        for ci, ch in enumerate(chs[:MAX_CHOICES]):
            cat = f"{at} choice[{ci}]"
            if not isinstance(ch, dict):
                err(cat, "must be an object")
                continue
            ctext = ch.get("text", "")
            if check_str(cat, "text", ctext, CHOICE_MAX):
                if wrapped_lines(ctext, CHOICE_COLS) > CHOICE_LINES:
                    warn(cat, f"wraps to more than {CHOICE_LINES} lines -- it will be elided "
                              f"on the button")
                # Convention: a reply the player SAYS is written plainly; anything they only do
                # (staying silent, a gesture) goes in parentheses.
                if ctext.strip() in ("...", "…", ".."):
                    warn(cat, "a bare '...' reads as dialogue. If the player is choosing to stay "
                              "silent, write it as an action: '(say nothing)'")
            to = ch.get("to", "end")
            if check_str(cat, "to", to, TO_MAX):
                targets.append((cat, to))
                # The creature should REACT to what the player said. A choice that jumps straight
                # to the end stops the exchange on the player's line.
                if to in ("", "end"):
                    dead_ends.append(cat)

            fx = ch.get("effects", {})
            if not isinstance(fx, dict):
                err(cat, "`effects` must be an object")
                continue
            for fld in ("friendship", "happiness"):
                if fld in fx:
                    if not isinstance(fx[fld], int):
                        err(cat, f"effects.{fld} must be an integer")
                    elif abs(fx[fld]) > 100:
                        warn(cat, f"effects.{fld} of {fx[fld]} is suspiciously large "
                                  f"(care actions grant 2-6, battle wins 25)")
            sf = fx.get("setFact")
            if sf is not None:
                if not isinstance(sf, dict) or len(sf) != 1:
                    err(cat, "setFact must be an object with exactly ONE key "
                             "(the firmware reads only the first)")
                else:
                    k, v = next(iter(sf.items()))
                    check_str(cat, "setFact key", k, KEY_MAX)
                    check_str(cat, "setFact value", v, KEY_MAX)
                    st["set_facts"].setdefault(k, []).append(label)
                    if not fx.get("factNote"):
                        warn(cat, f"sets fact '{k}' with no factNote -- the journal will show "
                                  f"the raw key/value")
            if fx.get("factNote"):
                check_str(cat, "factNote", fx["factNote"], NOTE_MAX)
            if "setMood" in fx:
                if fx["setMood"] not in MOODS:
                    err(cat, f"setMood '{fx['setMood']}' must be one of {MOODS}")
                elif fx["setMood"] == "ok":
                    sets_ok = True
                else:
                    sets_upset = True
            dr = fx.get("drift", {})
            if isinstance(dr, dict):
                for ax, val in dr.items():
                    if ax not in AXES:
                        err(cat, f"unknown drift axis '{ax}'")
                    elif not isinstance(val, (int, float)) or abs(val) > 1.0:
                        err(cat, f"drift.{ax} must be a number within -1..1")

    # node-level continuations (a choiceless node's next beat)
    for idx, nd in enumerate(nodes[:MAX_NODES]):
        if not isinstance(nd, dict) or "to" not in nd:
            continue
        at = f"{label} node[{idx}]"
        nto = nd["to"]
        if check_str(at, "to", nto, TO_MAX) and nto not in ("", "end"):
            if nd.get("choices"):
                err(at, "has both `choices` and a node-level `to`; the firmware only follows "
                        "`to` when there are NO choices")
            if nto not in ids:
                err(at, f"to '{nto}' is not a node id in this conversation")
            targets.append((at, nto))

    start = data.get("start")
    if start and start not in ids:
        err(label, f"start '{start}' is not one of this conversation's node ids")
    for cat, to in targets:
        if to not in ("", "end") and to not in ids:
            err(cat, f"to '{to}' is not a node id in this conversation (it will just end)")

    total_choices = sum(len(nd.get("choices", [])) for nd in nodes[:MAX_NODES]
                        if isinstance(nd, dict))
    if total_choices and len(dead_ends) == total_choices:
        warn(label, "EVERY choice ends the conversation -- the creature never reacts to what the "
                    "player said. Route choices to a reply node instead.")

    if sets_upset:
        st["upset_setters"].append(label)
    if sets_ok and w.get("mood") in ("upset", "hurt", "angry"):
        st["recovery"].append(label)

    reached = {start or (ids[0] if ids else None)}
    for _, to in targets:
        reached.add(to)
    for nid in ids:
        if nid not in reached:
            warn(label, f"node '{nid}' is unreachable")


def main():
    st = {
        "natures": load_ids("natures", "natures"),
        "traits": load_ids("personalities", "personalities"),
        "convs": {},
        "set_facts": {},
        "used_facts": {},
        "require_seen": [],
        "upset_setters": [],
        "recovery": [],
    }

    files = collect_files()
    packs = 0

    for label, path, _pool in files:
        size = os.path.getsize(path)
        if size > MAX_FILE_BYTES:
            err(label, f"is {size} bytes, over the firmware's {MAX_FILE_BYTES} read cap -- it "
                       f"would be silently skipped on device")
        elif size > MAX_FILE_BYTES * 8 // 10:
            warn(label, f"is {size} bytes, within 20% of the {MAX_FILE_BYTES} read cap")

        try:
            with open(path, encoding="utf-8") as f:
                raw = json.load(f)
        except Exception as e:                                    # noqa: BLE001
            err(label, f"unreadable / invalid JSON: {e}")
            continue

        # One conversation (object) or a PACK of them (array). Both load on device, in flash and
        # on SD alike.
        if isinstance(raw, list):
            packs += 1
            for i, d in enumerate(raw):
                lbl = f"{label}[{i}]"
                if isinstance(d, dict):
                    check_conversation(lbl, d, st)
                else:
                    err(lbl, "pack entries must be conversation objects")
        elif isinstance(raw, dict):
            check_conversation(label, raw, st)
        else:
            err(label, "top level must be a conversation object, or an array of them")

    for label, target in st["require_seen"]:
        if target not in st["convs"]:
            err(label, f"when.requireSeen '{target}' is not a known conversation id -- "
                       f"this can never unlock")

    for k, users in st["used_facts"].items():
        if k not in st["set_facts"]:
            warn(", ".join(users), f"gates on fact '{k}' which nothing ever sets")

    # An upset creature refuses petting, so a rift with no written way out isn't a sad story --
    # it's a permanently broken care loop.
    if st["upset_setters"] and not st["recovery"]:
        err(", ".join(st["upset_setters"]),
            "can leave the creature upset, but NO conversation gated on an upset mood sets it "
            "back to 'ok'. It would refuse petting forever. Write a resolution conversation.")
    elif st["recovery"]:
        print(f"  note: rift recovery available via {', '.join(st['recovery'])}")

    print(f"checked {len(files)} file(s) ({packs} pack(s)), {len(st['convs'])} conversation(s)")
    print(f"  natures declared: {len(st['natures'])}   personalities declared: {len(st['traits'])}")
    for w in warnings:
        print(f"  WARN  {w}")
    for e in errors:
        print(f"  ERROR {e}")
    if errors:
        print(f"\nFAIL: {len(errors)} error(s), {len(warnings)} warning(s)")
        return 1
    print(f"\nPASS ({len(warnings)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
