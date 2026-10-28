#!/usr/bin/env python3
"""Offline, backed-up migration of Daeros from legacy Implementor to level 60.

Stop the MUD before running. This operates on local save data, not Git files.
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import shutil
import sys

PLAYER = Path("lib/plrfiles/A-E/daeros.plr")
INDEX = Path("lib/plrfiles/index")

def migrate_player(data):
    lines = data.splitlines(keepends=True)
    matches = [i for i, line in enumerate(lines) if re.match(rb"^Levl:\s*\d+\s*$", line.strip())]
    if len(matches) != 1:
        raise ValueError(f"Expected one Levl record for Daeros, found {len(matches)}")
    old = int(lines[matches[0]].split(b":", 1)[1].strip())
    if old != 34:
        raise ValueError(f"Expected legacy Implementor level 34, got {old}; refusing change")
    end = b"\r\n" if lines[matches[0]].endswith(b"\r\n") else b"\n"
    lines[matches[0]] = b"Levl: 60" + end
    return b"".join(lines)

def migrate_index(data):
    lines = data.splitlines(keepends=True)
    matches = []
    for i, line in enumerate(lines):
        fields = line.strip().split()
        if len(fields) == 5 and fields[1].lower() == b"daeros":
            matches.append((i, fields))
    if len(matches) != 1:
        raise ValueError(f"Expected one Daeros index record, found {len(matches)}")
    i, fields = matches[0]
    if fields[2] != b"34":
        raise ValueError(f"Expected Daeros index level 34, got {fields[2]!r}")
    fields[2] = b"60"
    newline = b"\r\n" if lines[i].endswith(b"\r\n") else b"\n"
    lines[i] = b" ".join(fields) + newline
    return b"".join(lines)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=Path("."))
    p.add_argument("--apply", action="store_true", help="Write changes; default is preview only")
    args = p.parse_args()
    root = args.root.resolve()
    files = (root / PLAYER, root / INDEX)
    try:
        original = []
        for file in files:
            if not file.is_file() or file.is_symlink():
                raise ValueError(f"Missing or unsafe save file: {file}")
            original.append(file.read_bytes())
        updated = (migrate_player(original[0]), migrate_index(original[1]))
        print("Validated Daeros: player level 34 -> 60, index level 34 -> 60.")
        if not args.apply:
            print("Preview only. Stop the game and re-run with --apply.")
            return 0
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        backups = []
        for file in files:
            backup = file.with_name(file.name + ".pre-level60-" + stamp + ".bak")
            with backup.open("xb") as out:
                out.write(original[len(backups)])
                out.flush()
                os.fsync(out.fileno())
            shutil.copymode(file, backup)
            backups.append(backup)
        for file, payload in zip(files, updated):
            temp = file.with_name(file.name + ".level60-tmp")
            if temp.exists():
                raise ValueError(f"Temporary file already exists: {temp}")
            with temp.open("xb") as out:
                out.write(payload)
                out.flush()
                os.fsync(out.fileno())
            shutil.copymode(file, temp)
            os.replace(temp, file)
        print("Updated both files. Backups:")
        for backup in backups:
            print(backup)
    except (OSError, ValueError) as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
