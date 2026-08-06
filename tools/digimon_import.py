#!/usr/bin/env python3
"""digimon_import.py - build flash_creatures/ entries from a DMC sprite pack.

Takes a roster JSON (see tools/rosters/dmc_v1.json) that pairs each creature's
gameplay data with a source folder of 16 individual sprite PNGs (Name_0.png ..
Name_15.png, the standard DMC frame order). For every entry it:

  1. reads the 16 frames (must all share one WxH; width varies per digimon,
     48..68 px in the packs, height is 48),
  2. packs them into ONE 4x4 atlas PNG (left-to-right, top-to-bottom, same
     order as the indices) -> <out>/<id>/sheet.png,
  3. writes <out>/<id>/creature.json with "frames": 16.

One file per digimon keeps FAT overhead down (16 loose PNGs would each burn a
cluster) and lets the loader derive the cell size as sheetW/4 x sheetH/4.

Pure Python (zlib + struct) like gen_placeholder.py -- no Pillow needed; the
packs are plain 8-bit RGBA non-interlaced PNGs.

Usage:
    python tools/digimon_import.py tools/rosters/dmc_v1.json
    python tools/digimon_import.py roster.json --sprite-root <pack dir> --out flash_creatures
"""
import argparse, json, os, struct, sys, zlib

FRAMES = 16
GRID = 4  # 4x4 atlas


# --- minimal PNG I/O ---------------------------------------------------------

def read_png_rgba(path):
    """Decode an 8-bit RGBA non-interlaced PNG -> (w, h, bytearray RGBA)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"not a PNG: {path}")
    pos, w, h, idat = 8, 0, 0, b""
    while pos < len(data):
        (ln,), typ = struct.unpack(">I", data[pos:pos+4]), data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+ln]
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            if (depth, ctype, inter) != (8, 6, 0):
                sys.exit(f"{path}: unsupported PNG (need 8-bit RGBA non-interlaced, "
                         f"got depth={depth} colortype={ctype} interlace={inter})")
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray(stride * h)
    prev = bytearray(stride)
    for y in range(h):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1: (y + 1) * (stride + 1)])
        if ftype == 1:    # Sub
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif ftype == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            sys.exit(f"{path}: unknown PNG filter {ftype}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def write_png_rgba(path, w, h, rgba):
    def chunk(typ, payload):
        c = struct.pack(">I", len(payload)) + typ + payload
        return c + struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF)
    stride = w * 4
    raw = b"".join(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


# --- atlas packing -----------------------------------------------------------

def find_frames(src_dir):
    """Locate the 16 frame files in a folder, tolerating pack quirks: the filename
    prefix may differ from the folder name (MetalGreymon/MetalGreymon Virus_0.png)
    and some sets are numbered 1..16 instead of 0..15 (Devimon). Returns a list of
    16 paths in frame order."""
    import re
    groups = {}
    for f in os.listdir(src_dir):
        m = re.fullmatch(r"(.+)_(\d+)\.png", f)
        if m:
            groups.setdefault(m.group(1), {})[int(m.group(2))] = f
    if not groups:
        sys.exit(f"no <name>_<index>.png frames in {src_dir}")
    base = max(groups, key=lambda k: len(groups[k]))
    idx = groups[base]
    lo = min(idx)
    if lo not in (0, 1) or len(idx) < FRAMES:
        sys.exit(f"{src_dir}: expected 16 frames indexed from 0 (or 1), "
                 f"got {sorted(idx)} for '{base}'")
    if lo == 1:
        print(f"  note: {base}: frames numbered 1..16, shifting to 0..15")
    missing = [i for i in range(lo, lo + FRAMES) if i not in idx]
    if missing:
        sys.exit(f"{src_dir}: missing frame indices {missing} for '{base}'")
    return [os.path.join(src_dir, idx[i]) for i in range(lo, lo + FRAMES)]


def build_sheet(src_dir):
    """Pack a folder's 16 frames into a 4x4 RGBA atlas. Returns (w, h, rgba)."""
    frames, fw, fh = [], 0, 0
    base = os.path.basename(src_dir)
    for i, p in enumerate(find_frames(src_dir)):
        w, h, px = read_png_rgba(p)
        if i == 0:
            fw, fh = w, h
        elif (w, h) != (fw, fh):
            sys.exit(f"{p}: frame size {w}x{h} != frame 0's {fw}x{fh}")
        frames.append(px)
    sw, sh = fw * GRID, fh * GRID
    sheet = bytearray(sw * sh * 4)          # transparent black
    magenta = 0
    for i, px in enumerate(frames):
        ox, oy = (i % GRID) * fw, (i // GRID) * fh
        for y in range(fh):
            dst = ((oy + y) * sw + ox) * 4
            src = y * fw * 4
            sheet[dst:dst + fw * 4] = px[src:src + fw * 4]
        # opaque pixels that land on the RGB565 color key would blit as holes
        for o in range(0, len(px), 4):
            r, g, b, a = px[o], px[o+1], px[o+2], px[o+3]
            if a >= 128 and (r >> 3, g >> 2, b >> 3) == (31, 0, 31):
                magenta += 1
    if magenta:
        print(f"  WARNING: {base}: {magenta} opaque magenta pixel(s) collide "
              f"with the transparency key and will blit as holes")
    return sw, sh, sheet


# --- creature.json -----------------------------------------------------------

def creature_json(entry):
    """Roster entry -> creature.json dict (field order matches existing files)."""
    return {
        "name":         entry["name"],
        "tier":         entry["tier"],
        "attribute":    entry.get("attribute", "free"),
        "base":         entry["base"],
        "needs":        entry["needs"],
        "minStageSecs": entry["minStageSecs"],
        "sprite":       "sheet.png",
        "frames":       FRAMES,
        "evolutions":   entry.get("evolutions", []),
        "id":           entry["id"],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roster", help="roster JSON (see tools/rosters/)")
    ap.add_argument("--sprite-root", help="override the roster's spriteRoot")
    ap.add_argument("--out", default="flash_creatures", help="output dir (default flash_creatures)")
    args = ap.parse_args()

    with open(args.roster, encoding="utf-8") as f:
        roster = json.load(f)
    root = args.sprite_root or roster["spriteRoot"]
    creatures = roster["creatures"]

    # every evolution target should exist (in this roster or as an existing entry on disk)
    ids = {c["id"] for c in creatures}
    for c in creatures:
        for ev in c.get("evolutions", []):
            if ev["to"] not in ids and not os.path.isdir(os.path.join(args.out, ev["to"])):
                print(f"  WARNING: {c['id']}: evolution target '{ev['to']}' not in roster or {args.out}/")

    for c in creatures:
        src = os.path.join(root, c["source"])
        sw, sh, sheet = build_sheet(src)
        out_dir = os.path.join(args.out, c["id"])
        os.makedirs(out_dir, exist_ok=True)
        write_png_rgba(os.path.join(out_dir, "sheet.png"), sw, sh, sheet)
        with open(os.path.join(out_dir, "creature.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(creature_json(c), f, indent=2)
            f.write("\n")
        print(f"  {c['id']}: {sw}x{sh} sheet ({sw//GRID}x{sh//GRID} frames) -> {out_dir}/")

    print(f"done: {len(creatures)} creatures")


if __name__ == "__main__":
    main()
