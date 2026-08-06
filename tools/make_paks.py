#!/usr/bin/env python3
"""Build this repo's reference mod packs into sd-paks/.

Two packs, for two different jobs:

  base.pak    The entire base-game data set. NOT meant to sit on a card next to the
              stock firmware: its ids ARE the flash ids, so every creature would be
              read twice (flash, then the pak overriding it with identical bytes) and
              boot would get SLOWER, not faster. It exists to (a) exercise the pak
              path with the real, full data set rather than a 9-creature sample,
              (b) give modders a canonical tree to `modpack.py unpack` and copy the
              data shapes from, (c) be a redistributable snapshot of the base data.

  dmc_v2.pak  The sdcard_mod/ staging tree: 9 DMC V2 creatures, the `treat` food
              override, the additive honey_drop, and the 2-entry conversation pack.
              This one IS meant for a card -- copy it to <SD>/mods/ and remove the
              loose creatures/foods/conversations dirs to see the pak path in use.

WHY THIS SCRIPT EXISTS. A pack's internal layout mirrors the SD MOD ROOT
(creatures/<id>/..., foods/..., natures/..., personalities/..., conversations/<pool>/...).
The base game's data is not stored in that shape: it is split across two FAT partitions
whose roots mean different things.

    flash_creatures/<id>/       -> /creatures/<id>/       -> pak  creatures/<id>/
    flash_gamedata/<system>/    -> /gamedata/<system>/    -> pak  <system>/

So neither directory can be packed as-is -- flash_creatures/ would need a `creatures/`
prefix it does not have, and flash_gamedata/ is already at the right depth. Hence a
staged merge. Packing itself is delegated to tools/modpack.py by import, so the on-disk
format stays defined in exactly one place and cannot drift from pakfs.cpp.

base.pak IS THE FLASHED BASE-GAME DATA. main/CMakeLists.txt calls this script during the
build to regenerate it into build/flash_data/, which becomes the `gamedata` partition image --
so the device reads the whole base data set out of one packed file. It is byte-deterministic
(modpack.py sorts by path and stores no timestamps), which is what lets the SD-update system
keep skipping data partitions whose content has not changed.

Usage:  python tools/make_paks.py [outdir] [pack...]     # default: sd-paks/, both packs
        python tools/make_paks.py build/flash_data base.pak     # what the build calls
"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modpack                      # noqa: E402  (path set up first, on purpose)

REPO = Path(__file__).resolve().parent.parent

# name -> [(source tree, prefix inside the pack)]. The prefix is what turns two partition
# layouts into the one mod-root shape every loader scans; see the module docstring.
PACKS = {
    "base.pak":   [("flash_gamedata", ""), ("flash_creatures", "creatures")],
    "dmc_v2.pak": [("sdcard_mod", "")],
}


def is_data(p: Path) -> bool:
    """READMEs are authoring docs, not game data.

    They would be harmless on-device -- the loaders only ever look for creature.json,
    food.json and *.json under a known directory -- but they are dead weight in a pack,
    and sdcard_mod/README.txt in particular is loose-file install instructions that make
    no sense once the tree has been packed into a single file.
    """
    return p.is_file() and p.stem.lower() != "readme"


def stage(pairs, dest: Path) -> None:
    """Copy (source dir, pak prefix) pairs into one staging tree rooted at `dest`."""
    for src, prefix in pairs:
        if not src.is_dir():
            modpack.die(f"missing source tree: {src}")
        for f in sorted(src.rglob("*")):
            if not is_data(f):
                continue
            out = dest / prefix / f.relative_to(src) if prefix else dest / f.relative_to(src)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)


def build(name: str, pairs, outdir: Path) -> None:
    tmp = Path(tempfile.mkdtemp(prefix="pak_stage_"))
    try:
        stage(pairs, tmp)
        print(f"--- {name}")
        modpack.cmd_pack(str(tmp), str(outdir / name))
        # Read the pack back before handing it over: cmd_pack's own output only proves
        # what it believed it wrote, and a pack that fails pakfs.cpp's mount-time
        # validation on the device is far more annoying to diagnose there than here.
        modpack.cmd_verify(str(outdir / name))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> None:
    argv = sys.argv[1:]
    outdir = Path(argv[0]) if argv else REPO / "sd-paks"
    names = argv[1:] or list(PACKS)
    unknown = [n for n in names if n not in PACKS]
    if unknown:
        modpack.die(f"unknown pack(s): {', '.join(unknown)} (have: {', '.join(PACKS)})")

    outdir.mkdir(parents=True, exist_ok=True)
    for name in names:
        build(name, [(REPO / src, prefix) for src, prefix in PACKS[name]], outdir)

    print(f"\nwritten to {outdir}")
    if "dmc_v2.pak" in names:
        print("install:  copy the .pak you want to <SD>/mods/  (see docs/modpacks.md)")


if __name__ == "__main__":
    main()
