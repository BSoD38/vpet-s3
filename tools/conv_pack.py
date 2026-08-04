#!/usr/bin/env python3
"""Combine single-conversation files in a pool into one pack (a JSON array).

Why bother: the device's scan cost is dominated by file OPENS, not parsing, and FAT allocates a
~4 KB cluster per file however small it is. So thirty 250-byte conversations cost thirty opens and
120 KB of partition; the same thirty in one pack cost one open and one cluster.

Both layouts work everywhere, in flash and on SD -- so base content can ship packed while mods
stay drop-in single files. This just converts one direction.

Re-running with an existing pack name MERGES into it: the pack's current entries are the base,
loose files override an entry with the same id or append. (An earlier version silently rebuilt
the pack from only the loose files -- destroying everything packed on a previous run, whose
sources had already been deleted.)

The result is refused if it would exceed the firmware's 32 KB read cap: an oversized pack is
skipped wholesale on device, taking every conversation in it along.

Usage:
    python tools/conv_pack.py <pool-dir> <pack-name.json> [--keep name.json ...] [--apply]

Without --apply it only reports what it would do.
"""

import json
import os
import sys

# read_file()'s cap in main/game/sim/conversation.cpp -- over it, the device skips the file.
MAX_FILE_BYTES = 32768


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    pool = argv[1]
    out_name = argv[2]
    keep = set()
    apply_changes = False
    i = 3
    while i < len(argv):
        if argv[i] == "--apply":
            apply_changes = True
        elif argv[i] == "--keep" and i + 1 < len(argv):
            i += 1
            keep.add(argv[i])
        i += 1

    if not os.path.isdir(pool):
        print(f"not a directory: {pool}")
        return 1

    out_path = os.path.join(pool, out_name)

    # Existing pack entries are the merge base -- their source files are long gone.
    entries, index = [], {}
    if os.path.isfile(out_path):
        with open(out_path, encoding="utf-8") as f:
            base = json.load(f)
        if not isinstance(base, list):
            print(f"{out_path} exists but is not a pack (JSON array) -- refusing to overwrite")
            return 1
        for e in base:
            if isinstance(e, dict) and e.get("id"):
                index[e["id"]] = len(entries)
                entries.append(e)
        print(f"merging into existing pack ({len(entries)} entrie(s) kept)")

    sources, added, replaced = [], 0, 0
    for fn in sorted(os.listdir(pool)):
        if not fn.endswith(".json") or fn == out_name or fn in keep:
            continue
        path = os.path.join(pool, fn)
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list):
            print(f"  skip {fn} (already a pack)")
            continue
        if not isinstance(data, dict):
            print(f"  skip {fn} (not a conversation object)")
            continue
        cid = data.get("id")
        if cid in index:                        # a loose re-edit of a packed conversation wins
            entries[index[cid]] = data
            replaced += 1
        else:
            if cid:
                index[cid] = len(entries)
            entries.append(data)
            added += 1
        sources.append(path)

    if not sources:
        print("nothing to pack")
        return 0

    print(f"pack {added} new + {replaced} replaced conversation(s) -> {out_path} "
          f"({len(entries)} total)")
    for p in sources:
        print(f"  + {os.path.basename(p)}")
    for k in sorted(keep):
        print(f"  (kept standalone: {k})")

    # Serialize FIRST so the firmware cap is checked before anything is written or deleted.
    blob = json.dumps(entries, indent=2, ensure_ascii=False) + "\n"
    size = len(blob.encode("utf-8"))
    if size > MAX_FILE_BYTES:
        print(f"\nREFUSED: the pack would be {size} bytes, over the firmware's "
              f"{MAX_FILE_BYTES} read cap -- the device would silently skip the whole file. "
              f"Split the pool into two packs (use --keep to hold some files back).")
        return 1
    print(f"pack size: {size} bytes (cap {MAX_FILE_BYTES})")

    if not apply_changes:
        print("\ndry run -- pass --apply to write it")
        return 0

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(blob)
    for p in sources:
        os.remove(p)
    print(f"\nwrote {out_path} and removed {len(sources)} source file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
