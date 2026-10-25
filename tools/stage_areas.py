#!/usr/bin/env python3
"""Stage Circle/tba-format area trees without modifying the running world.

Convert non-Circle sources to a canonical Circle/tba tree with area_reader or
another reviewed converter first. Never assume format conversion is lossless.
"""
import argparse
import pathlib
import re
import shutil
import sys

FAMILIES = ("zon", "wld", "mob", "obj", "shp", "trg", "qst")
FILE_RE = re.compile(r"^([0-9]{1,8})\\.(zon|wld|mob|obj|shp|trg|qst)$")
HEADER_RE = re.compile(r"^#([0-9]+)(?:\\s|$)", re.MULTILINE)
MAX_FILE = 8 * 1024 * 1024

def indexed(root, family):
    folder = root / family
    if not folder.is_dir():
        return set()
    names = set()
    index = folder / "index"
    if index.exists():
        for line in index.read_text(encoding="latin-1").splitlines():
            item = line.strip()
            if not item or item == "$":
                continue
            if not FILE_RE.fullmatch(item) or not item.endswith("." + family):
                raise ValueError(f"Unexpected index entry: {index}: {item!r}")
            names.add(item)
    return names

def audit(source, dest):
    if source.resolve() == dest.resolve():
        raise ValueError("Source and destination must be different")
    collisions = []
    files = []
    for family in FAMILIES:
        folder = source / family
        if not folder.is_dir():
            continue
        existing = indexed(dest, family)
        source_index = indexed(source, family)
        for p in sorted(folder.iterdir()):
            if p.name in ("index", "index.mini"):
                continue
            if p.is_symlink() or not p.is_file() or not FILE_RE.fullmatch(p.name):
                raise ValueError(f"Unsupported path: {p}")
            if p.suffix != "." + family or p.stat().st_size > MAX_FILE:
                raise ValueError(f"Invalid type or excessive size: {p}")
            if source_index and p.name not in source_index:
                raise ValueError(f"Source file absent from index: {p}")
            try:
                data = p.read_bytes()
            except OSError as exc:
                raise ValueError(f"Cannot read {p}: {exc}") from exc
            if b"\\x00" in data:
                raise ValueError(f"NUL byte in {p}")
            # Zone numbers and record VNUMs are separate collision domains.
            # Refuse instead of attempting dangerous automatic remapping.
            if p.name in existing or (dest / family / p.name).exists():
                collisions.append(f"{family}/{p.name}")
            files.append((family, p, data))
    if not files:
        raise ValueError("No supported area files found")
    return files, collisions

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path, help="Canonical world tree")
    parser.add_argument("existing", type=pathlib.Path, help="Existing lib/world")
    parser.add_argument("staging", type=pathlib.Path, help="Empty staging directory")
    parser.add_argument("--write", action="store_true", help="Write only to staging")
    args = parser.parse_args()
    try:
        files, collisions = audit(args.source, args.existing)
        print(f"Audited {len(files)} area files")
        for item in collisions:
            print(f"CONFLICT {item}")
        if collisions:
            raise ValueError("Conflicts detected; do not overwrite or auto-remap")
        if args.write:
            if args.staging.exists() and any(args.staging.iterdir()):
                raise ValueError("Staging directory must be empty")
            if args.staging.resolve() == args.existing.resolve():
                raise ValueError("Staging must not be the live world")
            for family, src, data in files:
                folder = args.staging / family
                folder.mkdir(parents=True, exist_ok=True)
                (folder / src.name).write_bytes(data)
            for family in FAMILIES:
                selected = sorted(int(p.stem) for fam, p, _ in files if fam == family)
                if selected:
                    folder = args.staging / family
                    (folder / "index").write_text(
                        "".join(f"{n}.{family}\\n" for n in selected) + "$\\n")
            print(f"Staged files in {args.staging}")
        else:
            print("Dry run only; pass --write to stage")
        print("WARNING: record-level VNUMs, external references, resets and scripts "
              "still require manual or engine-level validation before install.")
    except (OSError, ValueError) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
