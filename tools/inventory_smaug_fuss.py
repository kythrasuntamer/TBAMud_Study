#!/usr/bin/env python3
"""Room/mobile/object inventory of a SMAUG FUSS area for conversion planning.

Writes no tbaMUD world files. Preserves the raw text of #ROOM, #MOBILE and
#OBJECT records and their resets; other sections (#AREADATA, #SHOP,
#SPECIALS, #REPAIR, ...) are not inventoried, so it is not a lossless copy.
"""
import argparse
import json
import pathlib
import re
import sys

KINDS = {"#ROOM": "#ENDROOM", "#MOBILE": "#ENDMOBILE",
         "#OBJECT": "#ENDOBJECT"}
VNUM = re.compile(r"^\s*Vnum\s+(\d+)\s*$")
RESET = re.compile(r"^\s*Reset\s+([A-Za-z])\s+(.+?)\s*$")
REF = re.compile(r"^\s*ToRoom\s+(-?\d+)\s*$")
MAX_SIZE = 16 * 1024 * 1024

def scan(text):
    if not text.startswith("#FUSSAREA"):
        raise ValueError("requires SMAUG FUSS #FUSSAREA format")
    records = {kind[1:].lower(): [] for kind in KINDS}
    seen = {k: set() for k in records}
    current, block, start = None, [], 0
    for line_no, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if current is None:
            if stripped in KINDS:
                current, block, start = stripped, [line], line_no
            continue
        block.append(line)
        if stripped == KINDS[current]:
            values = [int(m.group(1)) for raw in block
                      if (m := VNUM.match(raw))]
            if len(values) != 1:
                raise ValueError(f"{current} at line {start}: expected exactly one Vnum")
            kind = current[1:].lower()
            vnum = values[0]
            if vnum in seen[kind]:
                raise ValueError(f"duplicate {kind} Vnum {vnum}")
            seen[kind].add(vnum)
            raw = "\n".join(block) + "\n"
            exits = [int(m.group(1)) for row in block
                     if (m := REF.match(row))]
            resets = [{"command": m.group(1), "arguments": m.group(2),
                       "line": start + i}
                      for i, row in enumerate(block)
                      if (m := RESET.match(row))]
            records[kind].append({
                "vnum": vnum, "source_line": start, "raw": raw,
                "exit_destinations": exits, "resets": resets,
            })
            current, block = None, []
    if current:
        raise ValueError(f"unterminated {current} at line {start}")
    if not any(records.values()):
        raise ValueError("no supported SMAUG FUSS records")
    return records

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=pathlib.Path)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    args = ap.parse_args()
    try:
        if args.source.is_symlink() or not args.source.is_file():
            raise ValueError("source must be a regular, non-symlink file")
        if args.source.stat().st_size > MAX_SIZE:
            raise ValueError("area exceeds 16 MiB limit")
        records = scan(args.source.read_text(encoding="latin-1"))
        if args.output.exists():
            raise ValueError("refusing to overwrite existing output")
        ids = {k: {r["vnum"] for r in v} for k, v in records.items()}
        missing_exits = sorted({target for r in records["room"]
                                for target in r["exit_destinations"]
                                if target not in ids["room"]})
        report = {
            "format": "smaug-fuss",
            "source": args.source.name,
            "counts": {k: len(v) for k, v in records.items()},
            "external_exit_destinations": missing_exits,
            "records": records,
            "warning": "Inventory only, not a game-compatible conversion. "
                       "Zone resets, special procedures, flags, shops, "
                       "mobiles and objects still need typed translation."
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n",
                               encoding="utf-8")
        print(json.dumps(report["counts"]))
        print(f"External exits: {len(missing_exits)}; saved {args.output}")
    except (OSError, ValueError) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
