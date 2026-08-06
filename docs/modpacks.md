# Mod packs (.pak)

One `.pak` file bundles an entire mod tree — creatures, foods, conversations, natures,
personalities — so a large mod costs the device **one** FAT file instead of hundreds.

## Why

Every loose mod file costs a FAT directory walk per `fopen` (~7–12 ms measured on this
hardware; see the `SCAN_BUDGET` note in `conversation.cpp`), and FAT has no directory
index, so the walk grows with the number of entries in the folder. A 700-creature roster
as loose folders pays that cost 700× at boot (one `creature.json` each) and again on
every lazy sprite load. It also wastes a ~4 KB FAT cluster per file and makes "install
the mod" a 1400-file copy.

A pack pays FAT once. Its sorted table of contents is loaded into PSRAM at boot, so:

* opening a packed file = binary search + one seek — no directory walk at all;
* listing a packed directory = a pure table walk — no I/O;
* installing a mod = copying one file.

## How it works on-device

`main/game/engine/pakfs.cpp` mounts packs at boot as read-only VFS roots (`/pak0`…, in
case-insensitive filename order). Because packs are ordinary filesystem roots, every loader
reads them through plain `fopen`/`opendir` — the registries just add the pak roots to the
scan lists they already had.

`app.cpp` mounts from **two** sources, in this order:

1. the `gamedata` partition, which holds the base game's own **`base.pak`** → `/pak0`
2. **`<SD>/mods/`**, the player's packs → `/pak1`…

The order is the override rule (see below), so the base game must be mounted first.
**The base game itself ships as a pack**: there is no longer a separate `creatures`
partition of loose folders, and the `gamedata` partition holds exactly one file. That is
the whole point of the format — the entire base data set costs one FAT open at boot rather
than a directory walk per file. `flash_creatures/` and `flash_gamedata/` remain the tracked,
diffable sources; `tools/make_paks.py` stages them into the layout below during the build.

A pack's internal layout mirrors the SD mod root exactly:

```
creatures/<id>/creature.json + <sprite>.png [+ conversations/*.json]
foods/<id>/food.json            (or foods/foods.json array pack)
conversations/<pool>/*.json     (pools: natures, personalities, player)
natures/*.json
personalities/*.json
```

So `tools/modpack.py pack sdcard_mod my_mod.pak` packs the existing staging tree as-is.

### Load / override order (later wins on an id clash)

1. `base.pak` (the base game) — `/pak0`, mounted first so it is the weakest layer
2. mod packs from the card, `/pak1` → … (alphabetically later `.pak` filename wins)
3. loose SD files (`/sdcard/creatures`, `/sdcard/foods`, …)

Loose files beating packs is deliberate: a player can always drop a single folder on the
card to patch over a pack without re-packing it. There is no separate "flash" layer any
more — the base game is just the first pack, which is why nothing a player installs needs
special handling to override it.

The cost of consolidating: base data is now one file, so a corrupt pack loses the whole
data set rather than one creature. That is what the update system's per-partition hashing
and boot repair (docs/firmware-updates.md) exist to catch, and `pakfs` rejects a pack whose
table of contents fails validation rather than serving garbage from it.

### Limits

* at most **4** packs mounted (`PAKFS_MAX_PAKS`); extras are logged and ignored
* entry paths: ASCII, ≤ 103 bytes, unique **case-insensitively** (pakfs compares like
  FAT does, with `strcasecmp`, so `Sheet.png` in JSON finds `sheet.png` in the pack)
* ≤ 16384 entries per pack (sanity cap; the TOC costs ~116 bytes of PSRAM per entry)
* each mounted pack permanently holds one of the SD mount's `max_files` slots (raised to
  10 in `SD_MMC.c` to cover this)
* packs are read at boot only, like the rest of the SD state — a mid-session card change
  already halts the game for a restart (`sdwatch`)
* packs do NOT raise the registry caps (`CreatureRegistry::MAX` = 200 etc.); they only
  make loading cheap

## File format

Little-endian, uncompressed (PNGs are already compressed; JSON is tiny). Any change must
move in lockstep between `tools/modpack.py` (writer) and `pakfs.cpp` (reader, which
validates all of this at mount and rejects the pack otherwise).

| section | contents |
|---|---|
| header (24 B) | `"VPETPAK1"`, u32 version=1, u32 count, u32 entrySize=116, u32 reserved |
| TOC (count × 116 B) | `char path[104]` (relative, `/`-separated, NUL-terminated), u32 offset (absolute), u32 size, u32 crc32 |
| data | blobs, each 4-byte aligned |

The TOC is sorted by the byte-lowercased path — that IS the device's `strcasecmp`
order (hence the ASCII requirement), it's what makes the binary search valid, and it
doubles as a duplicate check. CRCs are verified by the tool (`verify`, and `unpack`
refuses corrupt data); the device trusts them for speed.

## Tooling

```
python tools/modpack.py pack   <src_dir> <out.pak>    # build from a mod tree
python tools/modpack.py unpack <in.pak> <dest_dir>    # extract for editing / remixing
python tools/modpack.py list   <in.pak>               # table of contents
python tools/modpack.py verify <in.pak>               # deep check: order + bounds + CRCs
```

Authoring loop: keep the mod as a loose tree (diffable, testable directly on the card),
`pack` it for release; `unpack` someone else's pack to inspect or build on it. Inside a
pack there is no reason to merge conversations into JSON-array files — per-file opens
are nearly free there — so one-file-per-conversation authoring costs nothing.
