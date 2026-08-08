"""Check that rendered tracker output has the right NOTES at the right TIMES.

The harness proves the modules load and are not silent. This checks the two things "not
silent" cannot: that the tick/row/BPM clock runs at the right speed, and that the notes come
out at the pitches the pattern names.

Run against scale.mod/scale.xm, which play ONE channel: a chromatic scale, one note every
8 rows, no bass and no effects. That matters -- overlapping square waves share harmonics (a
65 Hz square's third harmonic lands on 196 Hz, exactly the G a C-major arpeggio plays), so on
a multi-channel module "which note is sounding" has no single answer to measure.

Method: slide a window across the audio, Goertzel (a one-bin DFT) at each of the 12 candidate
fundamentals, take the strongest per window, then check the sequence and the timing of the
changes. Immune to the phase noise that defeats an RMS-based onset detector at these
frequencies.
"""
import math
import struct
import sys
import wave

RATE = 44100
ROW_SAMPLES = RATE * 2.5 / 125 * 6      # 125 BPM, 6 ticks per row -> 5292 samples = 120 ms
ROWS_PER_NOTE = 8
SAMPLE_CYCLE = 64                        # the test sample is one 64-frame square cycle

# Playing "note C-4" plays the sample at 8363 Hz; the sample is one 64-frame cycle, so the
# TONE it produces is 8363/64 = 130.7 Hz. Index i is i semitones above that.
def fund(i):
    return 8363.0 * (2.0 ** (i / 12.0)) / SAMPLE_CYCLE

CANDIDATES = [fund(i) for i in range(12)]
NAMES = ["+0", "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9", "+10", "+11"]

WIN = 8192
HOP = 2048


def read_mono(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == RATE, w.getframerate()
        raw = w.readframes(w.getnframes())
    v = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return [(v[i] + v[i + 1]) * 0.5 for i in range(0, len(v) - 1, 2)]


def goertzel(x, freq):
    k = 2.0 * math.cos(2.0 * math.pi * freq / RATE)
    s1 = s2 = 0.0
    for s in x:
        s0 = s + k * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - k * s1 * s2, 0.0)) / len(x)


def track(x):
    """(window centre in ms, strongest candidate index, its magnitude) per window."""
    out = []
    for i in range(0, len(x) - WIN, HOP):
        seg = x[i:i + WIN]
        mags = [goertzel(seg, f) for f in CANDIDATES]
        best = max(range(12), key=lambda k: mags[k])
        out.append(((i + WIN / 2) * 1000.0 / RATE, best, mags[best]))
    return out


def check(path, label):
    print(f"\n=== {label} ===")
    x = read_mono(path)
    print(f"    {len(x)} frames ({len(x)/RATE:.2f} s)")

    tr = track(x)

    # Collapse consecutive windows that agree, giving one run per note.
    runs = []
    for t, idx, mag in tr:
        if runs and runs[-1][0] == idx:
            runs[-1][2] = t
        else:
            runs.append([idx, t, t])
    print(f"    detected {len(runs)} note runs:")
    for idx, t0, t1 in runs[:14]:
        print(f"      {NAMES[idx]:>3} ({CANDIDATES[idx]:6.1f} Hz)  {t0:7.0f} -> {t1:7.0f} ms")

    expect_ms = ROW_SAMPLES * ROWS_PER_NOTE * 1000.0 / RATE     # 960 ms
    ok = True

    # 1. The sequence must ascend by one semitone per run, wrapping after 12.
    seq = [r[0] for r in runs]
    ideal = [i % 12 for i in range(len(seq))]
    if seq != ideal:
        # Tolerate the very last run being clipped by the end of the render.
        if seq[:-1] == ideal[:len(seq) - 1]:
            print("    sequence ascends chromatically (last run clipped by render end)  OK")
        else:
            print(f"    SEQUENCE MISMATCH: got {seq}, want {ideal}")
            ok = False
    else:
        print("    sequence ascends chromatically  OK")

    # 2. Each note must last one note-slot.
    durs = [round(r[2] - r[1]) for r in runs[:-1]]
    good = [d for d in durs if abs(d - expect_ms) < expect_ms * 0.20]
    print(f"    note durations (ms): {durs}")
    print(f"    expected ~{expect_ms:.0f} ms -> {len(good)}/{len(durs)} within 20%", end="")
    if durs and len(good) >= len(durs) - 1:
        print("  OK")
    else:
        print("  TIMING MISMATCH")
        ok = False

    return ok


if __name__ == "__main__":
    base = sys.argv[1]
    a = check(base + "/scale.mod.wav", "MOD chromatic scale")
    b = check(base + "/scale.xm.wav", "XM chromatic scale")
    print(f"\nMOD {'OK' if a else 'FAIL'}   XM {'OK' if b else 'FAIL'}")
    sys.exit(0 if (a and b) else 1)
