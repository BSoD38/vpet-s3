#!/usr/bin/env python3
"""gen_placeholder.py - generate crude placeholder creature sprites as a C header.

Pure Python (no Pillow). Draws simple procedural sprites (an egg and a baby
creature) into RGB565 arrays with a transparent color key, and writes
main/game/assets/sprites.hpp. Real art later: use tools/png2c.py to overwrite
these arrays from PNGs. Run from the project root:

    python tools/gen_placeholder.py
"""
import os

W = H = 48
TRANSP = 0xF81F

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def blank():
    return [TRANSP] * (W * H)

def put(buf, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        buf[y * W + x] = c

def fill_ellipse(buf, cx, cy, rx, ry, c):
    for y in range(H):
        for x in range(W):
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            if dx * dx + dy * dy <= 1.0:
                put(buf, x, y, c)

def fill_circle(buf, cx, cy, r, c):
    fill_ellipse(buf, cx, cy, r, r, c)

def gen_egg():
    b = blank()
    fill_ellipse(b, W // 2, H // 2 + 3, 16, 21, rgb565(250, 245, 220))   # cream shell
    # a few spots
    for (sx, sy) in [(19, 20), (30, 28), (24, 36), (33, 16)]:
        fill_circle(b, sx, sy, 3, rgb565(180, 140, 90))
    return b

def gen_baby():
    b = blank()
    fill_circle(b, W // 2, H // 2 + 2, 18, rgb565(250, 160, 40))          # body (orange)
    fill_circle(b, W // 2 - 7, H // 2 - 3, 6, rgb565(255, 255, 255))      # eyes
    fill_circle(b, W // 2 + 7, H // 2 - 3, 6, rgb565(255, 255, 255))
    fill_circle(b, W // 2 - 6, H // 2 - 2, 3, rgb565(20, 20, 20))         # pupils
    fill_circle(b, W // 2 + 8, H // 2 - 2, 3, rgb565(20, 20, 20))
    return b

def emit(f, name, buf):
    f.write(f"static const uint16_t {name}_data[{W*H}] = {{\n")
    for i in range(0, len(buf), 12):
        f.write("  " + ", ".join(f"0x{v:04X}" for v in buf[i:i+12]) + ",\n")
    f.write("};\n\n")

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "main", "game", "assets", "sprites.hpp")
    out = os.path.normpath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        f.write("#pragma once\n#include <cstdint>\n")
        f.write("// Auto-generated placeholder sprites (tools/gen_placeholder.py).\n")
        f.write("// Swap for real art with tools/png2c.py.\n\n")
        f.write(f"#define SPRITE_W {W}\n#define SPRITE_H {H}\n")
        f.write(f"#define SPRITE_TRANSP 0x{TRANSP:04X}\n\n")
        emit(f, "spr_egg", gen_egg())
        emit(f, "spr_baby", gen_baby())
    print("wrote", out)

if __name__ == "__main__":
    main()
