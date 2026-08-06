#!/usr/bin/env python3
"""Package the built firmware + game data for SD-card installation.

Usage:
    python tools/make_update.py                # inspect the built package only
    python tools/make_update.py E:\\           # write the package to an SD card root
    python tools/make_update.py some/folder    # write to a folder (copy contents to card root)

Produces at the destination:
    update.bin       app image (goes to the inactive ota_0/ota_1 slot)
    <name>.img       one image per FAT data partition (creatures, gamedata)
    update.json      manifest: size + sha256 of each piece

The device (Settings -> SYSTEM -> System Update) compares each manifest sha against a hash
of the installed partition and only writes what actually differs, so a code-only update
doesn't rewrite 3 MB of data. update.json is written LAST -- its presence marks a complete
package. A card holding only update.bin still works as an app-only update.
"""
import hashlib
import json
import re
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ESP_IMAGE_MAGIC = 0xE9
ESP_CHIP_ID_ESP32S3 = 9
ESP_APP_DESC_MAGIC = 0xABCD5432
# esp_app_desc_t sits after the 24-byte image header + 8-byte first segment header.
APP_DESC_OFFSET = 32


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def find_app_bin() -> Path:
    desc_path = ROOT / "build" / "project_description.json"
    if not desc_path.exists():
        die("build/project_description.json not found — run `idf.py build` first")
    desc = json.loads(desc_path.read_text(encoding="utf-8"))
    bin_path = ROOT / "build" / desc.get("app_bin", desc["project_name"] + ".bin")
    if not bin_path.exists():
        die(f"{bin_path} not found — run `idf.py build` first")
    return bin_path


def parse_image(data: bytes) -> dict:
    if len(data) < APP_DESC_OFFSET + 256:
        die("file too small to be an app image")
    if data[0] != ESP_IMAGE_MAGIC:
        die(f"bad image magic 0x{data[0]:02X} (expected 0xE9) — not an app image")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id != ESP_CHIP_ID_ESP32S3:
        die(f"image is for chip id {chip_id}, not ESP32-S3 ({ESP_CHIP_ID_ESP32S3})")

    # esp_app_desc_t: magic u32, secure_version u32, reserv1 u32[2],
    #                 version[32], project_name[32], time[16], date[16], idf_ver[32]
    magic, = struct.unpack_from("<I", data, APP_DESC_OFFSET)
    if magic != ESP_APP_DESC_MAGIC:
        die("no app descriptor found — not an ESP-IDF app image")

    def cstr(off: int, n: int) -> str:
        raw = data[APP_DESC_OFFSET + off : APP_DESC_OFFSET + off + n]
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace")

    return {
        "version": cstr(16, 32),
        "project": cstr(48, 32),
        "time": cstr(80, 16),
        "date": cstr(96, 16),
        "idf": cstr(112, 32),
    }


def parse_partitions() -> tuple[int, list[tuple[str, int]]]:
    """(smallest ota slot size, [(name, size) of FAT data partitions]) from partitions.csv."""
    slots, fats = [], []
    for line in (ROOT / "partitions.csv").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue
        m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)([KM]?)", cols[4])
        if not m:
            continue
        size = int(m.group(1), 0) * {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2)]
        if cols[1] == "app" and cols[2].startswith("ota_"):
            slots.append(size)
        elif cols[1] == "data" and cols[2] == "fat":
            fats.append((cols[0], size))
    if not slots:
        die("no ota_* app slots in partitions.csv — repartition before packaging updates")
    return min(slots), fats


def copy_verified(src: Path, out: Path, sha: str) -> None:
    shutil.copyfile(src, out)
    if hashlib.sha256(out.read_bytes()).hexdigest() != sha:
        die(f"copy verification FAILED — {out} is corrupt, delete it")


def main() -> None:
    bin_path = find_app_bin()
    app_data = bin_path.read_bytes()
    info = parse_image(app_data)
    slot, fats = parse_partitions()
    app_sha = hashlib.sha256(app_data).hexdigest()

    print(f"image    {bin_path.relative_to(ROOT)}")
    print(f"project  {info['project']}")
    print(f"version  {info['version']}")
    print(f"built    {info['date']} {info['time']}  (IDF {info['idf']})")
    print(f"size     {len(app_data):,} bytes ({len(app_data) / 1024:.0f} KB)"
          f" — slot {slot // (1024 * 1024)}M, {len(app_data) / slot:.0%} used")
    print(f"sha256   {app_sha}")
    if len(app_data) > slot:
        die("image does not fit the OTA slots — it can never install")

    # Data partition images produced by the build (fatfs_create_rawflash_image).
    data_entries = []
    for name, psize in fats:
        img = ROOT / "build" / f"{name}.bin"
        if not img.exists():
            print(f"data     {name}: no build/{name}.bin — skipped")
            continue
        blob = img.read_bytes()
        if len(blob) > psize:
            die(f"build/{name}.bin ({len(blob)} bytes) is larger than its partition ({psize})")
        if blob[38] == 0x29 and blob[39:43] != b"VPET":
            print(f"warning: {name}.bin volume id is not pinned (see tools/pin_fatfs_volid.py)"
                  f" — identical data will still hash as changed", file=sys.stderr)
        data_entries.append({
            "partition": name,
            "file": f"{name}.img",
            "size": len(blob),
            "sha256": hashlib.sha256(blob).hexdigest(),
            "_src": img,
        })
        print(f"data     {name}: {len(blob) // 1024} KB, sha {data_entries[-1]['sha256'][:16]}…")

    if len(sys.argv) < 2:
        print("\nno destination given — inspected only."
              "\nto package: python tools/make_update.py E:\\")
        return

    dest = Path(sys.argv[1])
    if dest.suffix:
        die("destination must be a folder (the package is several files), e.g. E:\\")
    if not dest.exists():
        die(f"destination {dest} does not exist (SD card not inserted?)")

    copy_verified(bin_path, dest / "update.bin", app_sha)
    print(f"\nwrote {dest / 'update.bin'}  (copy verified)")
    for e in data_entries:
        copy_verified(e.pop("_src"), dest / e["file"], e["sha256"])
        print(f"wrote {dest / e['file']}  (copy verified)")

    # Manifest last: its presence tells the device the package is complete.
    manifest = {
        "project": info["project"],
        "version": info["version"],
        "app": {"file": "update.bin", "size": len(app_data), "sha256": app_sha},
        "data": data_entries,
    }
    (dest / "update.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"wrote {dest / 'update.json'}")
    print("eject the card, insert it in the device: Settings -> SYSTEM -> System Update")


if __name__ == "__main__":
    main()
