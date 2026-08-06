#!/usr/bin/env python3
"""Pack, unpack or inspect v-pet mod packs (.pak).

A .pak bundles a whole mod tree (creatures/, foods/, conversations/, natures/,
personalities/) into ONE file, so the device pays FAT's per-file open cost (~7-12 ms
each, measured) once per boot instead of once per mod file. The game mounts every
<SD>/mods/*.pak at boot as a read-only filesystem (main/game/engine/pakfs.cpp); loose
files under /sdcard/<system>/ keep working and OVERRIDE packs on an id clash.

Usage:
    python tools/modpack.py pack   <src_dir> <out.pak>    # build a pack from a mod tree
    python tools/modpack.py unpack <in.pak> <dest_dir>    # extract, for editing a mod
    python tools/modpack.py list   <in.pak>               # table of contents
    python tools/modpack.py verify <in.pak>               # deep check: order + bounds + CRCs

Example (this repo's SD staging tree):
    python tools/modpack.py pack sdcard_mod dmc_v2.pak    # then copy to <SD>/mods/

Format (little-endian, uncompressed -- PNGs are already compressed, JSON is tiny):
    header : "VPETPAK1", u32 version=1, u32 count, u32 entrySize=116, u32 reserved
    TOC    : count x { char path[104]; u32 offset; u32 size; u32 crc32; },
             sorted by the byte-lowercased path (the device binary-searches with
             strcasecmp, so paths must be ASCII and unique case-insensitively --
             which is also exactly what FAT itself would require of the loose files)
    data   : file blobs, each 4-byte aligned
Any change here must move in lockstep with pakfs.cpp (it validates all of this at mount).
"""
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"VPETPAK1"
VERSION = 1
PATH_MAX = 104          # incl. NUL terminator; must match pakfs.cpp
HDR = struct.Struct("<8sIIII")
ENT = struct.Struct(f"<{PATH_MAX}sIII")
ENTRY_SIZE = ENT.size   # 116

SKIP_NAMES = {"thumbs.db", "desktop.ini"}   # OS litter that must never ship on-device


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def sort_key(rel: str) -> bytes:
    # Byte-wise lowercase == strcasecmp order on the device (guaranteed by the ASCII check).
    return rel.encode("ascii").lower()


def check_rel_path(rel: str) -> None:
    """One gate shared by pack (paths going in) and unpack (paths coming out)."""
    if not rel or rel.startswith("/") or "\\" in rel:
        die(f"bad entry path: {rel!r}")
    if any(part in ("", ".", "..") for part in rel.split("/")):
        die(f"bad entry path: {rel!r}")
    try:
        raw = rel.encode("ascii")
    except UnicodeEncodeError:
        die(f"non-ASCII path (FAT + the device's case-folding need ASCII): {rel!r}")
    if len(raw) > PATH_MAX - 1:
        die(f"path longer than {PATH_MAX - 1} bytes: {rel}")


def read_toc(data: bytes, src: str):
    if len(data) < HDR.size:
        die(f"{src}: too small to be a pack")
    magic, version, count, entry_size, _ = HDR.unpack_from(data, 0)
    if magic != MAGIC:
        die(f"{src}: bad magic (not a .pak)")
    if version != VERSION or entry_size != ENTRY_SIZE:
        die(f"{src}: unsupported pack (version {version}, entry {entry_size})")
    if len(data) < HDR.size + count * ENTRY_SIZE:
        die(f"{src}: truncated TOC")
    entries = []
    for i in range(count):
        raw_path, off, size, crc = ENT.unpack_from(data, HDR.size + i * ENTRY_SIZE)
        rel = raw_path.split(b"\0", 1)[0].decode("ascii", errors="replace")
        check_rel_path(rel)
        if off + size > len(data):
            die(f"{src}: '{rel}' points outside the file")
        if i and sort_key(entries[i - 1][0]) >= sort_key(rel):
            die(f"{src}: TOC not strictly sorted at '{rel}' (corrupt pack)")
        entries.append((rel, off, size, crc))
    return entries


def cmd_pack(src_dir: str, out_pak: str) -> None:
    src = Path(src_dir)
    if not src.is_dir():
        die(f"{src_dir} is not a directory")

    rels = []
    for p in src.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(src).as_posix()
        if any(part.startswith(".") for part in rel.split("/")):
            continue                                   # dotfiles/dirs: never mod data
        if p.name.lower() in SKIP_NAMES:
            continue
        check_rel_path(rel)
        rels.append(rel)
    if not rels:
        die(f"nothing to pack under {src_dir}")

    keys = [sort_key(r) for r in rels]
    if len(set(keys)) != len(keys):
        dupes = {k for k in keys if keys.count(k) > 1}
        die("paths colliding case-insensitively: " +
            ", ".join(sorted(r for r in rels if sort_key(r) in dupes)))
    rels.sort(key=sort_key)

    blobs = [(src / r).read_bytes() for r in rels]
    toc, data, off = [], bytearray(), HDR.size + len(rels) * ENTRY_SIZE
    for rel, blob in zip(rels, blobs):
        pad = -off % 4
        data += b"\0" * pad
        off += pad
        toc.append(ENT.pack(rel.encode("ascii"), off, len(blob), zlib.crc32(blob)))
        data += blob
        off += len(blob)

    out = Path(out_pak)
    out.write_bytes(HDR.pack(MAGIC, VERSION, len(rels), ENTRY_SIZE, 0) + b"".join(toc) + data)

    by_top = {}
    for rel in rels:
        top = rel.split("/", 1)[0] if "/" in rel else "(root)"
        by_top[top] = by_top.get(top, 0) + 1
    print(f"{out}: {len(rels)} files, {out.stat().st_size:,} bytes")
    for top, n in sorted(by_top.items()):
        print(f"    {top}: {n}")


def cmd_unpack(in_pak: str, dest_dir: str) -> None:
    data = Path(in_pak).read_bytes()
    entries = read_toc(data, in_pak)
    dest = Path(dest_dir)
    for rel, off, size, crc in entries:
        blob = data[off:off + size]
        if zlib.crc32(blob) != crc:
            die(f"{in_pak}: CRC mismatch on '{rel}' -- the pack is corrupt, not extracting")
        target = dest / rel                    # rel already gated by check_rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob)
    print(f"{in_pak}: extracted {len(entries)} files to {dest}")


def cmd_list(in_pak: str) -> None:
    data = Path(in_pak).read_bytes()
    entries = read_toc(data, in_pak)
    for rel, _off, size, _crc in entries:
        print(f"{size:>9,}  {rel}")
    print(f"{len(entries)} files, {len(data):,} bytes total")


def cmd_verify(in_pak: str) -> None:
    data = Path(in_pak).read_bytes()
    entries = read_toc(data, in_pak)           # already checks order, paths, bounds
    for rel, off, size, crc in entries:
        if off % 4:
            die(f"{in_pak}: '{rel}' is not 4-byte aligned")
        if zlib.crc32(data[off:off + size]) != crc:
            die(f"{in_pak}: CRC mismatch on '{rel}'")
    print(f"{in_pak}: OK ({len(entries)} files, {len(data):,} bytes)")


def main() -> None:
    args = sys.argv[1:]
    cmds = {"pack": (cmd_pack, 2), "unpack": (cmd_unpack, 2),
            "list": (cmd_list, 1), "verify": (cmd_verify, 1)}
    if not args or args[0] not in cmds or len(args) - 1 != cmds[args[0]][1]:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    cmds[args[0]][0](*args[1:])


if __name__ == "__main__":
    main()
