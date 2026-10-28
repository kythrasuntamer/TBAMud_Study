#!/usr/bin/env python3
"""Inventory Crimson 2 area bundles and quarantine PvP/unknown mechanics.

Audit only. No conversion or live-world writes. Input is a local checkout of
Crimson-2-MUD/dev/lib/areas; all records are retained for subsequent mapping.
"""
import argparse
import json
from pathlib import Path
import re
import sys

EXTS = ("wld", "mob", "obj", "zon", "shp")
HEADER = re.compile(r"(?m)^#([0-9]+)[ \t]*$")
PVP = re.compile(r"(?i)\b(pvp|pkill|player.?kill|player.?loot|arena|duel|bounty|clan.?war)\b")
MAX_BYTES = 8 * 1024 * 1024

def audit(folder):
    bundles = {}
    errors = []
    identifiers = {ext: {} for ext in EXTS}
    for path in sorted(folder.iterdir()):
        if path.name.startswith(".") or path.suffix[1:] not in EXTS:
            continue
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BYTES:
            errors.append(f"unsafe or oversized file: {path.name}")
            continue
        ext = path.suffix[1:]
        text = path.read_text(encoding="latin-1")
        if "\x00" in text:
            errors.append(f"NUL byte: {path.name}")
            continue
        ids = [int(m.group(1)) for m in HEADER.finditer(text)
               if int(m.group(1)) != 999999]
        key = path.stem
        entry = bundles.setdefault(key, {})
        entry[ext] = {"filename": path.name, "bytes": path.stat().st_size,
                      "record_headers": len(ids),
                      "pvp_keyword_lines": [
                          i for i, line in enumerate(text.splitlines(), 1)
                          if PVP.search(line)][:100]}
        for ident in ids:
            prev = identifiers[ext].get(ident)
            if prev:
                errors.append(f"duplicate {ext} identifier {ident}: {prev}, {path.name}")
            else:
                identifiers[ext][ident] = path.name
    return {"format": "crimson2", "mode": "AUDIT_ONLY",
            "bundles": bundles, "errors": errors,
            "limitations": [
                "PvP keyword hits are review flags, not proof all PvP mechanics were found",
                "No semantic conversion of mobs, objects, shops, exits or resets yet",
                "No target-world VNUM relocation or gameplay compatibility testing",
                "No permission determination for individual source-area authors"]}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("area_directory", type=Path)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    try:
        if not a.area_directory.is_dir() or a.area_directory.is_symlink():
            raise ValueError("area_directory must be a regular directory")
        if a.output.exists():
            raise ValueError("refusing to overwrite existing report")
        report = audit(a.area_directory)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Inventoried {len(report['bundles'])} bundles; {len(report['errors'])} problems")
        print("AUDIT ONLY: no converted files or installations generated")
        return 1 if report["errors"] else 0
    except (OSError, ValueError) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
