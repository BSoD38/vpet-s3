# libxmp-lite (vendored)

Tracker module playback for the sound engine: **MOD, XM, S3M and IT**. Used by
[`main/game/engine/audio/tracker.cpp`](../../main/game/engine/audio/tracker.cpp), which is a
thin `Decoder` adapter over `xmp_play_buffer()`.

- **Upstream:** <https://github.com/libxmp/libxmp> — version **4.7.2**
- **Licence:** MIT (see `LICENSE`, and the header of every source file). Note that *full*
  libxmp is LGPL-2.1+ because of third-party depackers; the **lite** subset deliberately
  contains only MIT-licensed sources, which is why this is the subset we vendor.
- **Modified from upstream:** no. Only `CMakeLists.txt` and this README are ours.

## Why vendored rather than hand-written

The sound engine originally shipped a hand-written MOD/XM player (~1,670 lines). It worked,
but tracker playback is defined as much by the *bugs* of the tracker that created each format
as by any specification, and matching those is a job measured in years of exposure to real
modules — not something a from-scratch player validated against synthetic fixtures can claim.
libxmp has that exposure. Swapping to it also made S3M and IT free, which the hand-written
player had scoped out on implementation cost.

## How this tree was produced

The `lite/` directory upstream is a *build recipe*, not a source tree: its `src/` holds only
Makefiles that select a subset of the main `src/`. So the file list comes from
`lite/src/Makefile` and `lite/src/loaders/Makefile`, and `include/xmp.h` from the main
`include/`. To update:

```sh
git clone --depth 1 https://github.com/libxmp/libxmp
# then copy, per lite/src/Makefile SRC_OBJS + SRC_DFILES
#              and lite/src/loaders/Makefile LOADERS_OBJS + LOADERS_DFILES:
#   src/{23 .c}  src/{17 .h}  src/loaders/{7 .c}  src/loaders/{5 .h}  include/xmp.h  COPYING
```

`CMakeLists.txt` lists those files in upstream's own order so a version bump reads as a diff.

## Notes for this board

- **No allocator hooks.** libxmp uses plain `malloc`, and a module's sample data can run to a
  megabyte, which would not fit the ~190 KB of free internal RAM. This works only because the
  project sets `CONFIG_SPIRAM_USE_MALLOC=y` with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`,
  so allocations above 16 KB are served from PSRAM automatically. **Do not turn those off**
  without giving libxmp another route to PSRAM.
- The adapter renders at the mixer's rate (44100 stereo) via `xmp_start_player`, so nothing
  resamples a module.
