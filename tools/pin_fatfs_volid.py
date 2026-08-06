#!/usr/bin/env python3
"""Pin the FAT volume id of a fatfsgen image to a constant.

IDF's fatfsgen.py writes a RANDOM 4-byte volume id (uuid4) into the boot sector on every
build, while pinning everything else (--use_default_datetime). That one field makes
byte-identical content produce different images, which would defeat the SD-update system's
"skip unchanged data partitions" hash comparison — so main/CMakeLists.txt runs this on
each generated image (POST_BUILD) to make them fully deterministic.

Usage: pin_fatfs_volid.py <image.bin>
"""
import sys
from pathlib import Path

# FAT12/16 boot sector: BS_BootSig (0x29) at offset 38, BS_VolID at 39..43. Our images
# (1-2 MB) are always FAT12/16; FAT32 (different layout) starts far beyond this size.
BOOTSIG_OFF = 38
VOLID_OFF = 39
VOLID = b"VPET"

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
if len(data) < 64 or data[BOOTSIG_OFF] != 0x29:
    sys.exit(f"{path}: no FAT12/16 boot signature at offset {BOOTSIG_OFF} — layout changed?")
if data[VOLID_OFF:VOLID_OFF + 4] != VOLID:
    data[VOLID_OFF:VOLID_OFF + 4] = VOLID
    path.write_bytes(data)
