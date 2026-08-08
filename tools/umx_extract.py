#!/usr/bin/env python3
"""Extract the tracker module (IT/S3M/XM/MOD) out of an Unreal Engine 1 .umx package.

WHY THIS EXISTS. A .umx is not a module: it is an Unreal package (same container as
.utx/.uax/.u) holding the module as a serialized "Music" object. The sound engine's
tracker player (and libxmp-lite behind it) expects a bare module starting with its own
magic, so UT/Unreal/Deus Ex music has to be unwrapped before it can go into
flash_gamedata/sounds/ or a mod pak.

The lazy way to unwrap is to scan for the inner magic ("IMPM"...) and cut to end of
file. That mostly works, but the package puts its name and export tables AFTER the
object data, so the cut file drags kilobytes of table garbage behind the module. This
tool instead parses the package properly -- name table, import table, export table,
then the Music object's own serialization -- so the output is exactly the byte range
the engine would have handed to its music player, no more, no less.

Format notes (Unreal package, versions ~35..69 for UE1; 100+ branches follow what
OpenMPT does for the rare later packages):

  header:   u32 signature 0x9E2A83C1, u16 version, u16 licensee, u32 flags,
            u32/u32 name count+offset, u32/u32 export count+offset,
            u32/u32 import count+offset  (heritage/GUID after this -- not needed)
  indices:  FCompactIndex, a variable-length signed int (see read_index)
  names:    version >= 64: index-prefixed length incl. NUL, then string; older:
            plain NUL-terminated. u32 flags after either.
  objrefs:  0 = none, positive = exports[ref-1], negative = imports[-ref-1]
  Music object data: property list (a single name index that should be "None"),
            a version-dependent preamble, then INDEX size + the raw module bytes.

Usage:
  python tools/umx_extract.py Foregone.umx [more.umx ...] [-o OUTDIR] [--list]

Output files are named after the export object inside the package (usually, but not
always, matching the .umx filename), with the extension chosen by sniffing the
extracted module's own magic.
"""
import argparse
import struct
import sys
from pathlib import Path

SIGNATURE = 0x9E2A83C1


class Reader:
    """A byte cursor with the primitives Unreal packages are made of."""

    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos

    def bytes(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise ValueError(f"truncated package: wanted {n} bytes at {self.pos}")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def skip(self, n: int) -> None:
        self.bytes(n)

    def u8(self) -> int:
        return self.bytes(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.bytes(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.bytes(4))[0]

    def index(self) -> int:
        """FCompactIndex: byte 0 carries sign (0x80), continuation (0x40) and 6 value
        bits; each later byte carries continuation (0x80) and 7 value bits."""
        b = self.u8()
        neg = bool(b & 0x80)
        value = b & 0x3F
        if b & 0x40:
            shift = 6
            for _ in range(4):
                b = self.u8()
                value |= (b & 0x7F) << shift
                if not (b & 0x80):
                    break
                shift += 7
        return -value if neg else value

    def cstring(self) -> bytes:
        end = self.data.index(b"\0", self.pos)
        out = self.data[self.pos:end]
        self.pos = end + 1
        return out


class Package:
    def __init__(self, data: bytes, source: str):
        self.source = source
        r = Reader(data)
        if r.u32() != SIGNATURE:
            raise ValueError(f"{source}: not an Unreal package (bad signature)")
        self.version = r.u16()
        r.u16()  # licensee version
        r.u32()  # package flags
        name_count, name_offset = r.u32(), r.u32()
        export_count, export_offset = r.u32(), r.u32()
        import_count, import_offset = r.u32(), r.u32()

        self.names = self._read_names(Reader(data, name_offset), name_count)
        self.imports = self._read_imports(Reader(data, import_offset), import_count)
        self.exports = self._read_exports(Reader(data, export_offset), export_count)
        self.data = data

    def _read_names(self, r: Reader, count: int) -> list:
        names = []
        for _ in range(count):
            if self.version >= 64:
                length = r.index()          # includes the trailing NUL
                raw = r.bytes(max(length, 0))
                names.append(raw.split(b"\0", 1)[0].decode("latin-1"))
            else:
                names.append(r.cstring().decode("latin-1"))
            r.u32()                         # name flags
        return names

    def _read_imports(self, r: Reader, count: int) -> list:
        imports = []
        for _ in range(count):
            r.index()                       # class package (name index)
            class_name = r.index()
            r.u32()                         # outer package (object ref)
            object_name = r.index()
            imports.append({"class_name": class_name, "object_name": object_name})
        return imports

    def _read_exports(self, r: Reader, count: int) -> list:
        exports = []
        for _ in range(count):
            class_ref = r.index()
            r.index()                       # super
            if self.version >= 60:
                r.u32()                     # group (object ref)
            name = r.index()
            r.u32()                         # object flags
            size = r.index()
            offset = r.index() if size > 0 else 0
            exports.append({"class_ref": class_ref, "name": name,
                            "size": size, "offset": offset})
        return exports

    def name(self, idx: int) -> str:
        return self.names[idx] if 0 <= idx < len(self.names) else f"<bad name {idx}>"

    def class_of(self, export: dict) -> str:
        """Resolve an export's class object ref to its plain name."""
        ref = export["class_ref"]
        if ref < 0:
            return self.name(self.imports[-ref - 1]["object_name"])
        if ref > 0:
            return self.name(self.exports[ref - 1]["name"])
        return "Class"                      # a null class means the export IS a class

    def music_payload(self, export: dict) -> bytes:
        """The serialized Music object, minus Unreal's wrapping = the bare module."""
        r = Reader(self.data, export["offset"])
        end = export["offset"] + export["size"]

        prop = r.index()                    # first (only) property name: "None"
        if self.name(prop).lower() != "none":
            # No UE1 music object seen in the wild has real properties, but if one
            # does, refusing is better than emitting garbage that half-plays.
            raise ValueError(f"{self.source}: unexpected property "
                             f"'{self.name(prop)}' on music object")

        # Version-dependent preamble between the properties and the data. These
        # branches mirror OpenMPT's loader, which has survived every known .umx.
        if self.version >= 120:             # UT2003-era
            r.index()
            r.skip(8)
        elif self.version >= 100:           # America's Army
            r.skip(4)
            r.index()
            r.skip(4)
        elif self.version >= 62:            # Unreal Tournament, Deus Ex
            r.index()                       # format name (e.g. "it" -- name index)
            r.skip(4)                       # absolute offset past this object
        else:                               # Unreal and betas
            r.index()                       # format name

        size = r.index()
        if size <= 0 or r.pos + size > end:
            raise ValueError(f"{self.source}: music chunk size {size} does not fit "
                             f"in its export ({export['size']} bytes)")
        return r.bytes(size)


def detect_extension(module: bytes) -> str:
    if module[:4] == b"IMPM":
        return "it"
    if len(module) >= 48 and module[44:48] == b"SCRM":
        return "s3m"
    if module[:17] == b"Extended Module: ":
        return "xm"
    if len(module) >= 1084 and module[1080:1084] in (
            b"M.K.", b"M!K!", b"4CHN", b"6CHN", b"8CHN", b"FLT4", b"FLT8"):
        return "mod"
    return "bin"


def extract_file(path: Path, outdir: Path | None, list_only: bool) -> int:
    pkg = Package(path.read_bytes(), path.name)
    music = [e for e in pkg.exports if pkg.class_of(e).lower() == "music"]

    if list_only:
        print(f"{path.name}: package version {pkg.version}, "
              f"{len(pkg.exports)} export(s)")
        for e in pkg.exports:
            tag = ""
            if e in music and e["size"] > 0:
                tag = f" -> .{detect_extension(pkg.music_payload(e))}"
            print(f"  {pkg.class_of(e):>10}  {pkg.name(e['name']):<24} "
                  f"{e['size']:>8} bytes{tag}")
        return len(music)

    if not music:
        print(f"{path.name}: no Music object in this package "
              f"(classes: {sorted({pkg.class_of(e) for e in pkg.exports})})",
              file=sys.stderr)
        return 0

    target_dir = outdir if outdir is not None else path.parent
    target_dir.mkdir(parents=True, exist_ok=True)
    for e in music:
        module = pkg.music_payload(e)
        stem = pkg.name(e["name"]) or path.stem
        out = target_dir / f"{stem}.{detect_extension(module)}"
        out.write_bytes(module)
        print(f"{path.name} -> {out}  ({len(module)} bytes)")
    return len(music)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Unwrap tracker modules from Unreal Engine 1 .umx packages.")
    ap.add_argument("files", nargs="+", type=Path, help=".umx package(s)")
    ap.add_argument("-o", "--outdir", type=Path, default=None,
                    help="output directory (default: next to each input)")
    ap.add_argument("--list", action="store_true",
                    help="show package contents instead of extracting")
    args = ap.parse_args()

    # PowerShell/cmd hand wildcards through unexpanded; do it ourselves so
    # `umx_extract.py Music\*.umx` works the same in every shell.
    files = []
    for p in args.files:
        if not p.exists() and any(ch in p.name for ch in "*?["):
            files.extend(sorted((p.parent if p.parent != Path("") else Path(".")).glob(p.name)))
        else:
            files.append(p)
    if not files:
        print("error: no input files matched", file=sys.stderr)
        sys.exit(1)

    extracted, failed = 0, 0
    for path in files:
        try:
            extracted += extract_file(path, args.outdir, args.list)
        except (ValueError, OSError) as err:
            print(f"error: {err}", file=sys.stderr)
            failed += 1
    if failed:
        sys.exit(1)
    if not args.list and extracted == 0:
        sys.exit(2)


if __name__ == "__main__":
    main()
