#!/usr/bin/env python3
"""Preflight imported Circle/tba-style rooms, mobiles, objects and zone resets.

Fail closed on unresolved references except explicitly allowlisted external
rooms. Reads staged files and existing world, but never edits either.
"""
import argparse
import json
from pathlib import Path
import re
import sys

HEADER = re.compile(r"^#(\d+)\s*$")
FILE = re.compile(r"^\d+\.(wld|mob|obj|zon)$")
TYPES = ("wld", "mob", "obj", "zon")
RESET_FIELDS = {"M": ((1,"mob"), (3,"wld")),
                "O": ((1,"obj"), (3,"wld")),
                "G": ((1,"obj"),),
                "E": ((1,"obj"),),
                "P": ((1,"obj"), (3,"obj")),
                "D": ((1,"wld"),),
                "R": ((1,"wld"), (2,"obj"))}

def read_records(root, ext):
    records, issues = {}, []
    directory = root / ext
    if not directory.is_dir():
        return records, issues
    for file in directory.iterdir():
        if file.name in ("index", "index.mini"):
            continue
        if file.is_symlink() or not file.is_file() or not FILE.fullmatch(file.name) or file.suffix != "." + ext:
            issues.append(f"unrecognized file {file}")
            continue
        if file.stat().st_size > 8*1024*1024:
            issues.append(f"oversized {file}")
            continue
        current = None
        for lineno, line in enumerate(file.read_text(encoding="latin-1").splitlines(),1):
            match = HEADER.fullmatch(line)
            if match:
                number=int(match.group(1))
                if number in records:
                    issues.append(f"duplicate {ext} {number}: {file}:{lineno}")
                else:
                    records[number] = {"file":str(file),"line":lineno,"body":[]}
                current=records[number]
            elif current is not None:
                current["body"].append((lineno,line))
    return records,issues

def validate(existing, incoming, permitted_external_rooms=()):
    old, new, problems = {},{},[]
    for ext in TYPES:
        old[ext], errors=read_records(existing,ext);problems+=errors
        new[ext], errors=read_records(incoming,ext);problems+=errors
        for v in new[ext]:
            if v in old[ext]:
                problems.append(f"collision: {ext} {v}")
    allids={ext:set(old[ext])|set(new[ext]) for ext in TYPES}
    permitted=set(permitted_external_rooms)
    for v, rec in new["wld"].items():
        lines=rec["body"]
        for i, (lineno,line) in enumerate(lines):
            if re.fullmatch(r"D[0-5]",line.strip()):
                # Find the numeric door/key/destination triplet before the next record section.
                found=False
                for j in range(i+1,min(i+16,len(lines))):
                    raw=lines[j][1].strip()
                    if re.fullmatch(r"-?\d+\s+-?\d+\s+-?\d+",raw):
                        door,key,target=map(int,raw.split())
                        if target >= 0 and target not in allids["wld"] and target not in permitted:
                            problems.append(f"unresolved exit {v} -> {target} at {rec['file']}:{lineno}")
                        if key >= 0 and key not in allids["obj"]:
                            problems.append(f"unresolved door key {key} in room {v}")
                        found=True
                        break
                    if raw=="S" or re.fullmatch(r"D[0-5]",raw):
                        break
                if not found:
                    problems.append(f"unreadable exit room {v} at {rec['file']}:{lineno}")
    for zone,rec in new["zon"].items():
        seen_header=False
        for lineno,line in rec["body"]:
            raw=line.strip()
            if not raw or raw.startswith("*"):
                continue
            if not seen_header:
                # Zone title is tilde-delimited; numeric header follows it.
                if re.match(r"^\d+\s+\d+\s+\d+",raw):
                    seen_header=True
                continue
            parts=raw.split()
            cmd=parts[0]
            if cmd in ("S","$~","$"):
                break
            if cmd not in RESET_FIELDS:
                problems.append(f"unsupported reset command {cmd} in zone {zone}:{lineno}")
                continue
            for index,ext in RESET_FIELDS[cmd]:
                if len(parts)<=index:
                    problems.append(f"short reset in zone {zone}:{lineno}")
                    break
                try: target=int(parts[index])
                except ValueError:
                    problems.append(f"bad reset ref in zone {zone}:{lineno}")
                    continue
                if target>=0 and target not in allids[ext] and not (ext=="wld" and target in permitted):
                    problems.append(f"unresolved {ext} {target} in zone {zone}:{lineno}")
    return {"incoming_counts":{x:len(new[x]) for x in TYPES},
            "problems":problems,
            "warning":"Static preflight only; does not prove resets or object semantics are compatible. Boot-test required."}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("existing",type=Path)
    p.add_argument("incoming",type=Path)
    p.add_argument("--allow-external-room",type=int,action="append",default=[])
    args=p.parse_args()
    if not args.existing.is_dir() or not args.incoming.is_dir():
        p.error("both paths must be directories")
    report=validate(args.existing,args.incoming,args.allow_external_room)
    print(json.dumps(report,indent=2))
    return 1 if report["problems"] else 0

if __name__=="__main__":
    sys.exit(main())
