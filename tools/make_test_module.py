#!/usr/bin/env python3
"""Generate a ProTracker MOD and a FastTracker II XM, for testing the tracker player.

WHY THIS EXISTS. game/engine/audio/tracker.cpp reads two binary formats whose fields are
mostly offsets and counts, and the failure mode when one of those is wrong is not a crash --
it is music that plays at the wrong pitch, or an instrument that is silent, or a sample that
sounds like a ramp instead of a square. Debugging that against somebody's 200 KB demoscene
module is miserable. These files are deliberately tiny and their content is KNOWN, so the
device's own log is enough to tell whether the loader read them correctly:

  * one sample only, a full-scale square wave -> the decoded peak must be near 32512.
    XM stores sample data DELTA encoded, so getting that backwards turns the square into a
    staircase and drops the peak. This is the check that catches it.
  * a C major arpeggio at a known tempo -> the pitch and speed are audibly verifiable.
  * a volume envelope in the XM -> exercises the instrument layer, which MOD does not have.

Neither file is meant to ship: they are test fixtures. Nothing in the game references them.

Usage:  python tools/make_test_module.py [outdir]        # default: build/test_modules
"""
import struct
import sys
from pathlib import Path

RATE_HINT = 8363          # the rate a sample plays at for note C-4, by convention
SAMPLE_LEN = 64           # one cycle of a square wave, looped
PEAK = 127                # full scale for 8-bit signed

# --- shared musical content ---------------------------------------------------------------
# A C major arpeggio, as semitone offsets from the root, then a walk back down.
ARPEGGIO = [0, 4, 7, 12, 7, 4, 0, -5]


def square_sample() -> bytes:
    """One cycle of a square wave as 8-bit signed PCM."""
    half = SAMPLE_LEN // 2
    return bytes((PEAK & 0xFF,)) * half + bytes(((-PEAK) & 0xFF,)) * half


def delta_encode_8(data: bytes) -> bytes:
    """XM stores sample data as successive differences, not absolute values."""
    out = bytearray()
    prev = 0
    for b in data:
        cur = b - 256 if b > 127 else b
        out.append((cur - prev) & 0xFF)
        prev = cur
    return bytes(out)


# =========================================================================================
# MOD
# =========================================================================================

# ProTracker periods, C-1..B-3. Index 12 is the middle C the arpeggio is built on.
MOD_PERIODS = [
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
]
MOD_ROOT = 12          # period 428


def mod_cell(period: int, sample: int, effect: int, param: int) -> bytes:
    """Pack one MOD cell: 4 bytes holding a period, a sample number and an effect."""
    b0 = ((sample & 0xF0)) | ((period >> 8) & 0x0F)
    b1 = period & 0xFF
    b2 = ((sample & 0x0F) << 4) | (effect & 0x0F)
    return bytes((b0, b1, b2, param))


def build_mod() -> bytes:
    channels, rows = 4, 64
    out = bytearray()

    out += b"tracker test MOD".ljust(20, b"\0")

    # 31 sample headers. Only the first is real; the rest are empty, which is normal.
    for i in range(31):
        if i == 0:
            out += b"square".ljust(22, b"\0")
            out += struct.pack(">H", SAMPLE_LEN // 2)   # length in WORDS, big-endian
            out += bytes((0,))                          # finetune
            out += bytes((64,))                         # volume
            out += struct.pack(">H", 0)                 # loop start, in words
            out += struct.pack(">H", SAMPLE_LEN // 2)   # loop length, in words
        else:
            out += b"\0" * 22 + struct.pack(">H", 0) + bytes((0, 0)) + struct.pack(">HH", 0, 0)

    out += bytes((1,))            # song length: one pattern
    out += bytes((0,))            # restart position
    out += bytes(128)             # order table, all zeros -> pattern 0
    out += b"M.K."

    # Pattern 0: the arpeggio on channel 0 every 8th row, a root note on channel 1, and a
    # tempo set on row 0 so the timing under test is explicit rather than the default.
    pat = bytearray()
    for r in range(rows):
        for c in range(channels):
            if c == 0 and r % 8 == 0:
                semi = ARPEGGIO[(r // 8) % len(ARPEGGIO)]
                pat += mod_cell(MOD_PERIODS[MOD_ROOT + semi], 1, 0, 0)
            elif c == 1 and r % 16 == 0:
                # An octave below, with a volume slide down so effect processing is exercised.
                pat += mod_cell(MOD_PERIODS[MOD_ROOT - 12], 1, 0x0C, 48)
            elif c == 1 and r % 16 == 8:
                pat += mod_cell(0, 0, 0x0A, 0x02)       # volume slide down
            elif r == 0 and c == 3:
                pat += mod_cell(0, 0, 0x0F, 6)          # speed = 6 ticks/row
            else:
                pat += mod_cell(0, 0, 0, 0)
    out += pat
    out += square_sample()
    return bytes(out)


# =========================================================================================
# XM
# =========================================================================================

XM_C4 = 49          # XM note numbering: 1 = C-0, so 49 = C-4


def xm_cell(note: int, instr: int, vol: int, fx: int, fxp: int) -> bytes:
    """Pack one XM cell in the explicit form: a flag byte then all five fields."""
    return bytes((0x80 | 0x1F, note, instr, vol, fx, fxp))


def build_xm() -> bytes:
    channels, rows, patterns, instruments = 4, 64, 1, 1
    order_bytes = 256
    header_size = 20 + order_bytes

    out = bytearray()
    out += b"Extended Module: "                          # 17 bytes
    out += b"tracker test XM".ljust(20, b"\0")           # 20
    out += bytes((0x1A,))
    out += b"make_test_module.py".ljust(20, b"\0")       # 20
    out += struct.pack("<H", 0x0104)                     # version
    out += struct.pack("<I", header_size)
    out += struct.pack("<H", 1)                          # song length
    out += struct.pack("<H", 0)                          # restart position
    out += struct.pack("<H", channels)
    out += struct.pack("<H", patterns)
    out += struct.pack("<H", instruments)
    out += struct.pack("<H", 1)                          # flags: bit0 = linear frequency table
    out += struct.pack("<H", 6)                          # default speed (ticks per row)
    out += struct.pack("<H", 125)                        # default BPM
    out += bytes(order_bytes)                            # order table -> pattern 0

    # --- pattern 0 ---
    pat = bytearray()
    for r in range(rows):
        for c in range(channels):
            if c == 0 and r % 8 == 0:
                semi = ARPEGGIO[(r // 8) % len(ARPEGGIO)]
                pat += xm_cell(XM_C4 + semi, 1, 0, 0, 0)
            elif c == 1 and r % 16 == 0:
                pat += xm_cell(XM_C4 - 12, 1, 0x10 + 48, 0, 0)   # volume column: set volume 48
            elif c == 2 and r == 32:
                # Tone portamento with a target, so the pitch-slide path is covered.
                pat += xm_cell(XM_C4 + 7, 1, 0, 0x03, 4)
            elif c == 1 and r % 16 == 12:
                pat += xm_cell(97, 0, 0, 0, 0)                   # note off -> envelope fadeout
            else:
                pat += xm_cell(0, 0, 0, 0, 0)
    out += struct.pack("<I", 9)                # pattern header length
    out += bytes((0,))                         # packing type
    out += struct.pack("<H", rows)
    out += struct.pack("<H", len(pat))
    out += pat

    # --- instrument 1 ---
    ins = bytearray()
    ins += struct.pack("<I", 29 + 214)         # instrument size
    ins += b"square inst".ljust(22, b"\0")
    ins += bytes((0,))                         # type
    ins += struct.pack("<H", 1)                # one sample

    ext = bytearray(214)
    ext[0:4] = struct.pack("<I", 40)           # sample header size
    # keymap at 4..99 stays all zeros: every note uses sample 0.

    # Volume envelope: instant attack, slow decay to silence. Points are (tick, level 0..64).
    vol_points = [(0, 64), (16, 48), (64, 24), (160, 0)]
    for i, (x, y) in enumerate(vol_points):
        ext[100 + i * 4:104 + i * 4] = struct.pack("<HH", x, y)
    ext[196] = len(vol_points)                 # volume point count
    ext[197] = 0                               # panning point count
    ext[198] = 2                               # volume sustain point
    ext[199] = 0                               # volume loop start
    ext[200] = 0                               # volume loop end
    ext[204] = 0x01 | 0x02                     # volume type: envelope on + sustain on
    ext[205] = 0                               # panning type: off
    ext[206] = 0                               # vibrato type
    ext[207] = 0                               # vibrato sweep
    ext[208] = 0                               # vibrato depth
    ext[209] = 0                               # vibrato rate
    ext[210:212] = struct.pack("<H", 1024)     # fadeout
    ins += ext

    raw = square_sample()
    ins += struct.pack("<I", len(raw))         # sample length in BYTES
    ins += struct.pack("<I", 0)                # loop start
    ins += struct.pack("<I", len(raw))         # loop length
    ins += bytes((64,))                        # volume
    ins += bytes((0,))                         # finetune
    ins += bytes((0x01,))                      # type: forward loop, 8-bit
    ins += bytes((128,))                       # panning (centre)
    ins += bytes((0,))                         # relative note
    ins += bytes((0,))                         # reserved
    ins += b"square".ljust(22, b"\0")
    ins += delta_encode_8(raw)

    out += ins
    return bytes(out)


# =========================================================================================
# Pitch reference: ONE voice, a chromatic scale, nothing else
# =========================================================================================
#
# test.mod/test.xm above deliberately stack channels and effects, which is what you want for
# exercising the player but useless for MEASURING it: overlapping square waves share
# harmonics, so "which note is sounding" stops having a single answer. (A square wave at
# 65 Hz has a third harmonic at 196 Hz, which is exactly the G the arpeggio plays -- so a
# bass note masquerades as a melody note.)
#
# These two play a single channel, one chromatic step every 8 rows, no bass and no effects.
# The dominant frequency is then unambiguous and can be tracked over time, which tests the
# note table and the row clock together.

SCALE_LEN = 12          # one octave, chromatic
SCALE_ROWS_PER_NOTE = 8


def build_scale_mod() -> bytes:
    channels, rows = 4, 64
    out = bytearray()
    out += b"scale MOD".ljust(20, b"\0")
    for i in range(31):
        if i == 0:
            out += b"square".ljust(22, b"\0")
            out += struct.pack(">H", SAMPLE_LEN // 2) + bytes((0, 64))
            out += struct.pack(">HH", 0, SAMPLE_LEN // 2)
        else:
            out += b"\0" * 22 + struct.pack(">H", 0) + bytes((0, 0)) + struct.pack(">HH", 0, 0)
    out += bytes((1, 0)) + bytes(128) + b"M.K."

    pat = bytearray()
    for r in range(rows):
        for c in range(channels):
            if c == 0 and r % SCALE_ROWS_PER_NOTE == 0:
                step = (r // SCALE_ROWS_PER_NOTE) % SCALE_LEN
                pat += mod_cell(MOD_PERIODS[MOD_ROOT + step], 1, 0, 0)
            else:
                pat += mod_cell(0, 0, 0, 0)
    out += pat + square_sample()
    return bytes(out)


def build_scale_xm() -> bytes:
    channels, rows, order_bytes = 4, 64, 256
    header_size = 20 + order_bytes

    out = bytearray()
    out += b"Extended Module: " + b"scale XM".ljust(20, b"\0") + bytes((0x1A,))
    out += b"make_test_module.py".ljust(20, b"\0") + struct.pack("<H", 0x0104)
    out += struct.pack("<I", header_size)
    out += struct.pack("<HHHHHHHH", 1, 0, channels, 1, 1, 1, 6, 125)
    out += bytes(order_bytes)

    pat = bytearray()
    for r in range(rows):
        for c in range(channels):
            if c == 0 and r % SCALE_ROWS_PER_NOTE == 0:
                step = (r // SCALE_ROWS_PER_NOTE) % SCALE_LEN
                pat += xm_cell(XM_C4 + step, 1, 0, 0, 0)
            else:
                pat += xm_cell(0, 0, 0, 0, 0)
    out += struct.pack("<I", 9) + bytes((0,)) + struct.pack("<HH", rows, len(pat)) + pat

    # A single sample, no envelope at all: the note must hold at full volume for its whole
    # 8 rows so a windowed measurement has something steady to lock onto.
    ins = bytearray()
    ins += struct.pack("<I", 29 + 214) + b"square inst".ljust(22, b"\0") + bytes((0,))
    ins += struct.pack("<H", 1)
    ext = bytearray(214)
    ext[0:4] = struct.pack("<I", 40)
    ins += ext

    raw = square_sample()
    ins += struct.pack("<III", len(raw), 0, len(raw))
    ins += bytes((64, 0, 0x01, 128, 0, 0)) + b"square".ljust(22, b"\0")
    ins += delta_encode_8(raw)
    out += ins
    return bytes(out)


def main() -> None:
    outdir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/test_modules")
    outdir.mkdir(parents=True, exist_ok=True)

    files = (
        ("test.mod",  build_mod()),        # stacked channels + effects: exercises the player
        ("test.xm",   build_xm()),
        ("scale.mod", build_scale_mod()),  # one voice, chromatic: measures the player
        ("scale.xm",  build_scale_xm()),
    )
    for name, data in files:
        p = outdir / name
        p.write_bytes(data)
        print(f"{p}: {len(data)} bytes")

    print("\nexpected: 4 ch, 1 pattern, 1 order; first-sample peak 32512")
    print("(a much lower XM peak means the delta decoding is wrong)")
    print(f"scale.*: one note every {SCALE_ROWS_PER_NOTE} rows = 960 ms at 125 BPM / 6 ticks,")
    print(f"         a chromatic scale of {SCALE_LEN} notes starting at {RATE_HINT/SAMPLE_LEN:.1f} Hz")


if __name__ == "__main__":
    main()
