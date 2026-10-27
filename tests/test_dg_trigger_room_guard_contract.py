#!/usr/bin/env python3
"""Check world-room validity before DG greeting/memory entry dereferences."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
for name, actor in (("greet_mtrigger", "actor"),
                    ("entry_memory_mtrigger", "ch")):
    start = re.search(r"\b(?:int|void)\s+" + name + r"\s*\([^)]*\)\s*\{", source)
    if not start:
        raise SystemExit("Missing function: " + name)
    body = source[start.end():]
    deref = body.index("world[IN_ROOM(" + actor + ")].people")
    guards = ("!" + actor, "!world",
              "(long long)IN_ROOM(" + actor + ") < 0",
              "!VALID_ROOM_RNUM(IN_ROOM(" + actor + "))")
    for guard in guards:
        if guard not in body[:deref]:
            raise SystemExit(name + ": missing earlier guard " + guard)
print("DG world-room entry guard source contracts passed")
