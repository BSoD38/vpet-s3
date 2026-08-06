#pragma once

// Read-only "mod pack" filesystem: one .pak archive bundling a whole mod tree behind a
// single FAT file.
//
// WHY: every loose mod file costs a FAT directory walk per open (~7-12 ms measured on this
// hardware -- see conversation.cpp's SCAN_BUDGET note), and a large modded roster pays that
// hundreds of times at boot and again on every lazy sprite load. A pack pays FAT once: its
// sorted table of contents is held in PSRAM, so opening a packed file is a binary search
// plus one seek, and listing a packed directory is a pure table walk with no I/O at all.
//
// Each pack registers as an ordinary VFS root ("/pak0".."/pak3"), so every existing loader
// (creatures, foods, natures/personalities, conversations) reads it through plain
// fopen/opendir unchanged -- registries only add the pak roots to the scan lists they
// already have. A pack's internal layout mirrors the SD mod root (creatures/<id>/...,
// foods/..., conversations/<pool>/...). Loose SD files keep working and keep OVERRIDING
// packs; load order and the on-disk format live in docs/modpacks.md, and packs are built,
// listed and extracted by tools/modpack.py.

// 5 = one slot for the base game's own base.pak (mounted from the gamedata partition) plus the
// 4 user mod packs a card may carry. Raising it costs a VFS registration each (see
// CONFIG_VFS_MAX_COUNT) and one held-open file descriptor on the mount the pack came from.
static const int PAKFS_MAX_PAKS = 5;

// Mount every *.pak found in `dir` as /pak0../pakN, in case-insensitive filename order so
// that a later pack overrides an earlier one wherever both define the same id -- deterministic,
// and it matches what a file listing shows. Returns how many mounted; a missing dir just means
// "no packs here" and returns 0 quietly.
//
// CALLED ONCE PER PACK SOURCE, and mount points are handed out across calls in call order, so
// the ORDER OF CALLS IS THE OVERRIDE ORDER: app.cpp mounts the base game's data partition
// first (weakest) and the card's /sdcard/mods second, so a player's pack can override the base
// game. The PAKFS_MAX_PAKS bound therefore applies to the running total, not to one call.
// Each mounted pack permanently holds one max_files slot on the filesystem it was read from.
int pakfs_mount_all(const char* dir);

int         pakfs_count();        // mounted pack count
const char* pakfs_root(int i);    // mount point ("/pak0"), or nullptr out of range
const char* pakfs_source(int i);  // the underlying .pak path (for logs/UI), or nullptr
