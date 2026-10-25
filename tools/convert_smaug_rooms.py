#!/usr/bin/env python3
"""Convert SMAUG FUSS room geometry into isolated tbaMUD room files.

This is a *rooms-only* converter: NPCs, objects, scripts and resets are NOT
imported. Reject unknown or unsafe room/exit syntax rather than guessing.
"""
import argparse
import pathlib
import re
import sys

DIRS = {"north": 0, "east": 1, "south": 2, "west": 3, "up": 4, "down": 5}
SECTORS = {
    "inside": 0, "city": 1, "field": 2, "forest": 3, "hills": 4,
    "mountain": 5, "water_swim": 6, "water_noswim": 7,
    "underwater": 8, "air": 9, "desert": 10,
}
# Known SMAUG flags mapped conservatively. Others intentionally rejected.
ROOM_FLAGS = {"dark": 1, "nomob": 4, "indoors": 8}
MAX_TEXT = 16000

def reject(msg):
    raise ValueError(msg)

def clean(s, field):
    s = s.strip()
    if not s or "\x00" in s or len(s) > MAX_TEXT or "~" in s:
        reject(f"invalid {field}")
    return s

def read_tilde(lines, i, initial, field):
    parts = [initial]
    if "~" in initial:
        if not initial.rstrip().endswith("~"):
            reject(f"unexpected terminator in {field}")
        return clean(initial.rstrip()[:-1], field), i
    while i < len(lines):
        current = lines[i]
        i += 1
        parts.append(current)
        if current.rstrip().endswith("~"):
            parts[-1] = current.rstrip()[:-1]
            return clean("\n".join(parts), field), i
    reject(f"unterminated {field}")

def parse_area(text):
    lines = text.splitlines()
    rooms = {}
    i = 0
    while i < len(lines):
        if lines[i].strip() != "#ROOM":
            i += 1
            continue
        i += 1
        room = {"exits": [], "flags": 0, "sector": 0}
        while i < len(lines):
            line = lines[i].strip()
            i += 1
            if line == "#ENDROOM":
                if "vnum" not in room or "name" not in room:
                    reject("room missing Vnum/Name")
                if room["vnum"] in rooms:
                    reject(f"duplicate room {room['vnum']}")
                rooms[room["vnum"]] = room
                break
            if not line or line.startswith("*"):
                continue
            if line == "#EXIT":
                ex = {}
                while i < len(lines):
                    entry = lines[i].strip()
                    i += 1
                    if entry == "#ENDEXIT":
                        break
                    if not entry:
                        continue
                    key, _, value = entry.partition(" ")
                    if key == "Direction":
                        name, i = read_tilde(lines, i, value, "direction")
                        if name not in DIRS:
                            reject(f"unsupported exit direction {name}")
                        ex["dir"] = DIRS[name]
                    elif key == "ToRoom":
                        ex["to"] = int(value)
                    elif key in ("Desc", "Keywords"):
                        ex[key.lower()], i = read_tilde(lines, i, value, key)
                    elif key == "Key":
                        ex["key"] = int(value)
                    elif key == "Flags":
                        flags, i = read_tilde(lines, i, value, "exit flags")
                        if any(v not in ("isdoor", "closed", "locked") for v in flags.split()):
                            reject(f"unsupported exit flags {flags}")
                        ex["door"] = 1 if "isdoor" in flags.split() else 0
                    else:
                        reject(f"unsupported exit field {key}")
                else:
                    reject("unterminated #EXIT")
                if "dir" not in ex or "to" not in ex:
                    reject("exit missing direction or target")
                room["exits"].append(ex)
                continue
            if line.startswith("#"):
                reject(f"unsupported room section {line}")
            key, _, value = line.partition(" ")
            if key == "Vnum":
                room["vnum"] = int(value)
            elif key == "Name":
                room["name"], i = read_tilde(lines, i, value, "room name")
            elif key == "Desc":
                room["desc"], i = read_tilde(lines, i, value, "room description")
            elif key == "Sector":
                val, i = read_tilde(lines, i, value, "sector")
                if val not in SECTORS:
                    reject(f"unsupported sector {val}")
                room["sector"] = SECTORS[val]
            elif key == "Flags":
                val, i = read_tilde(lines, i, value, "room flags")
                for flag in val.split():
                    if flag not in ROOM_FLAGS:
                        reject(f"unsupported room flag {flag}")
                    room["flags"] |= ROOM_FLAGS[flag]
            elif key == "Reset":
                reject("room contains resets; full conversion required to preserve gameplay")
            else:
                reject(f"unsupported room field {key}")
        else:
            reject("unterminated #ROOM")
    if not rooms:
        reject("no rooms found")
    return rooms

def convert(rooms, offset):
    remap = {n: n + offset for n in rooms}
    if any(n < 0 or n > 99999999 for n in remap.values()):
        reject("destination VNUM outside permitted range")
    result = []
    unresolved = []
    for original, room in sorted(rooms.items()):
        new = remap[original]
        result += [f"#{new}", room["name"] + "~",
                   room.get("desc", "An empty room.") + "~",
                   f"0 {room['flags']} {room['sector']} 0 0 0"]
        used = set()
        for ex in room["exits"]:
            if ex["dir"] in used:
                reject(f"duplicate direction in room {original}")
            used.add(ex["dir"])
            if ex["to"] not in remap:
                unresolved.append((original, ex["to"]))
                continue
            if "key" in ex and ex["key"] >= 0:
                reject("keyed doors require object VNUM remapping")
            result += [f"D{ex['dir']}", ex.get("desc", "") + "~",
                       ex.get("keywords", "") + "~",
                       f"{ex.get('door', 0)} -1 {remap[ex['to']]}"]
        result.append("S")
    if unresolved:
        reject("unresolved external exits: " +
               ", ".join(f"{a}->{b}" for a, b in unresolved[:20]))
    return "\n".join(result + ["$"]) + "\n"

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("area", type=pathlib.Path)
    ap.add_argument("--offset", type=int, required=True,
                    help="Add this amount to source room VNUMs")
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--allow-partial", action="store_true",
                    help="Acknowledge mobs, objects, scripts and resets are omitted")
    args = ap.parse_args()
    if not args.allow_partial:
        ap.error("room-only conversion requires explicit --allow-partial")
    try:
        if args.area.stat().st_size > 16 * 1024 * 1024:
            reject("input exceeds 16 MiB")
        text = args.area.read_text(encoding="latin-1")
        if "#FUSSAREA" not in text[:200]:
            reject("input does not look like a SMAUG FUSS area")
        rooms = parse_area(text)
        output = convert(rooms, args.offset)
        if args.output.exists():
            reject(f"refusing to overwrite {args.output}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
        print(f"Converted {len(rooms)} rooms to {args.output}; NOT an installable zone.")
        print("Next: validate VNUMs, create zone definition and index, stage, boot-test.")
    except (OSError, ValueError) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
