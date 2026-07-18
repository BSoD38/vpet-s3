#!/usr/bin/env python3
"""png2c.py - convert a PNG to a C array for LovyanGFX pushImage().

Emits an RGB565 array. Fully-transparent pixels (alpha < 128) are written as a
transparent color key (default magenta 0xF81F) so they can be blitted with
fb.pushImage(x, y, w, h, data, TRANSP_KEY).

Usage:
    python tools/png2c.py creature.png --name spr_creature >> main/game/assets/sprites.hpp

Requires Pillow (pip install pillow). For quick placeholder art without art
files, use tools/gen_placeholder.py instead (pure Python, no dependencies).
"""
import argparse, sys

TRANSP = 0xF81F  # magenta color key

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--name", required=True, help="C identifier for the sprite")
    ap.add_argument("--transp", default=hex(TRANSP), help="transparent key (hex)")
    args = ap.parse_args()
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow required: pip install pillow")

    transp = int(args.transp, 16)
    im = Image.open(args.png).convert("RGBA")
    w, h = im.size
    px = im.load()
    vals = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            vals.append(transp if a < 128 else rgb565(r, g, b))

    print(f"// {args.png}  {w}x{h}")
    print(f"static const uint16_t {args.name}_data[{w*h}] = {{")
    for i in range(0, len(vals), 12):
        print("  " + ", ".join(f"0x{v:04X}" for v in vals[i:i+12]) + ",")
    print("};")
    print(f"#define {args.name.upper()}_W {w}")
    print(f"#define {args.name.upper()}_H {h}")

if __name__ == "__main__":
    main()
