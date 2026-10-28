#!/usr/bin/env python3
"""Generate a reviewed two-way exit bridge between existing and imported rooms.

This tool never modifies lib/world. It creates modified copies of the two
.wld files in an empty staging directory; review their diffs before install.
"""
import argparse
import pathlib
import re
import sys

ROOM_HEADER = re.compile(r"(?m)^#(\d+)[ \t]*\r?$")
DIRECTION = {"north": 0, "east": 1, "south": 2, "west": 3,
             "up": 4, "down": 5}
OPPOSITE = {0: 2, 1: 3, 2: 0, 3: 1, 4: 5, 5: 4}

def split_rooms(contents):
    matches = list(ROOM_HEADER.finditer(contents))
    if not matches:
        raise ValueError("No numbered rooms found")
    found = {}
    for i, match in enumerate(matches):
        number = int(match.group(1))
        end = matches[i + 1].start() if i + 1 < len(matches) else len(contents)
        if number in found:
            raise ValueError(f"Duplicate room VNUM {number}")
        found[number] = (match.start(), end, contents[match.start():end])
    return found

def add_exit(contents, vnum, direction, target):
    rooms = split_rooms(contents)
    if vnum not in rooms:
        raise ValueError(f"Room {vnum} absent from selected .wld")
    begin, end, body = rooms[vnum]
    # Only parse the room's D0-D5 exit records, not free-text descriptions.
    existing = {int(x) for x in re.findall(r"(?m)^D([0-5])[ \t]*\r?$", body)}
    if direction in existing:
        raise ValueError(f"Room {vnum} already has exit direction {direction}")
    # Room terminator is a bare S line. Other commands after S are retained.
    terminator = re.search(r"(?m)^S[ \t]*\r?$", body)
    if terminator is None:
        raise ValueError(f"Room {vnum} has no terminator S")
    # Fail if odd structure makes edit unsafe, including malformed exit labels.
    if re.search(r"(?m)^D\d+[ \t]*\r?$", body[:terminator.start()]) and len(existing) != len(re.findall(r"(?m)^D\d+[ \t]*\r?$", body[:terminator.start()])):
        raise ValueError(f"Room {vnum} has unsupported exit directions")
    # Standard Circle/tba exit syntax: Dn, description~, keywords~, door key to_room.
    injected = f"D{direction}\n~\n~\n0 -1 {target}\n"
    changed = body[:terminator.start()] + injected + body[terminator.start():]
    return contents[:begin] + changed + contents[end:]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--existing-wld", type=pathlib.Path, required=True)
    p.add_argument("--existing-room", type=int, required=True)
    p.add_argument("--imported-wld", type=pathlib.Path, required=True)
    p.add_argument("--imported-room", type=int, required=True)
    p.add_argument("--direction", choices=DIRECTION, required=True,
                   help="Direction FROM existing room TO imported room")
    p.add_argument("--staging", type=pathlib.Path, required=True)
    a = p.parse_args()
    try:
        sources = (a.existing_wld, a.imported_wld)
        if any(not f.is_file() or f.is_symlink() for f in sources):
            raise ValueError("Inputs must be regular, non-symlink .wld files")
        if any(f.suffix != ".wld" or f.stat().st_size > 8 * 1024 * 1024 for f in sources):
            raise ValueError("Expected .wld files at most 8 MiB each")
        if a.existing_wld.resolve() == a.imported_wld.resolve():
            raise ValueError("Choose distinct world files")
        if a.existing_wld.name == a.imported_wld.name:
            raise ValueError("Source filenames collide; remap and rename first")
        if a.existing_room == a.imported_room:
            raise ValueError("Room VNUM collision")
        contents = [f.read_text(encoding="latin-1") for f in sources]
        if a.imported_room in split_rooms(contents[0]):
            raise ValueError("Imported VNUM collides with existing file")
        if a.existing_room in split_rooms(contents[1]):
            raise ValueError("Existing VNUM collides with imported file")
        direction = DIRECTION[a.direction]
        changed = [
            add_exit(contents[0], a.existing_room, direction, a.imported_room),
            add_exit(contents[1], a.imported_room, OPPOSITE[direction], a.existing_room),
        ]
        if a.staging.exists() and any(a.staging.iterdir()):
            raise ValueError("Staging directory must be empty")
        if a.staging.resolve() in (f.parent.resolve() for f in sources):
            raise ValueError("Staging must not be a source directory")
        a.staging.mkdir(parents=True, exist_ok=True)
        for source, data in zip(sources, changed):
            (a.staging / source.name).write_text(data, encoding="latin-1")
        print(f"Staged reciprocal exits: {a.existing_room} -> {a.imported_room} "
              f"({a.direction}) and {a.imported_room} -> {a.existing_room} "
              f"({OPPOSITE[direction]}).")
        print("REVIEW REQUIRED: verify global VNUM uniqueness, zone ranges, "
              "indexes and scripts; isolated boot-test before installation.")
    except (OSError, ValueError, UnicodeError) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
