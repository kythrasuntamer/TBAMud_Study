#!/usr/bin/env python3
"""Fail-closed inventory of VNUM definitions in Circle/tbaMUD world trees.

Check candidate area definitions against *all* existing zone files. Does not
mutate either tree; game boot validation is still required.
"""
import argparse
import json
import pathlib
import re
import sys

FAMILIES = ("wld", "mob", "obj", "trg", "qst", "shp", "zon")
HEADER = re.compile(r"^#(\d+)\s*$")
FILE = re.compile(r"^\d{1,8}\.(?:wld|mob|obj|trg|qst|shp|zon)$")
LIMIT = 8 * 1024 * 1024

def inventory(root):
    records = {family: {} for family in FAMILIES}
    problems = []
    for family in FAMILIES:
        directory = root / family
        if not directory.exists():
            continue
        if not directory.is_dir() or directory.is_symlink():
            problems.append(f"Invalid directory: {directory}")
            continue
        for p in sorted(directory.iterdir()):
            if p.name in ("index", "index.mini"):
                continue
            if p.is_symlink() or not p.is_file() or not FILE.fullmatch(p.name) or p.suffix != "." + family:
                problems.append(f"Unexpected world file: {p}")
                continue
            if p.stat().st_size > LIMIT:
                problems.append(f"Oversized world file: {p}")
                continue
            content = p.read_text(encoding="latin-1").splitlines()
            for i, line in enumerate(content, 1):
                match = HEADER.fullmatch(line.strip())
                if match is None:
                    continue
                number = int(match.group(1))
                previous = records[family].get(number)
                if previous:
                    problems.append(f"Duplicate {family} VNUM {number}: {previous} and {p}:{i}")
                else:
                    records[family][number] = f"{p}:{i}"
    return records, problems

def audit(existing, incoming):
    old, errors = inventory(existing)
    new, more = inventory(incoming)
    errors.extend(more)
    for family in FAMILIES:
        for vnum, loc in new[family].items():
            if vnum in old[family]:
                errors.append(f"COLLISION {family} VNUM {vnum}: {old[family][vnum]} <> {loc}")
    return {
        "existing_counts": {x: len(old[x]) for x in FAMILIES},
        "incoming_counts": {x: len(new[x]) for x in FAMILIES},
        "problems": errors,
    }

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("existing", type=pathlib.Path)
    p.add_argument("incoming", type=pathlib.Path)
    p.add_argument("--report", type=pathlib.Path)
    a = p.parse_args()
    try:
        if not a.existing.is_dir() or not a.incoming.is_dir():
            raise ValueError("Both world roots must be directories")
        if a.existing.resolve() == a.incoming.resolve():
            raise ValueError("World roots must differ")
        report = audit(a.existing, a.incoming)
        output = json.dumps(report, indent=2)
        print(output)
        if a.report:
            if a.report.exists():
                raise ValueError("Refusing to overwrite report")
            a.report.write_text(output + "\n", encoding="utf-8")
        return 1 if report["problems"] else 0
    except (OSError, ValueError) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
