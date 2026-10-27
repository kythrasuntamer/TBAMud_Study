#!/usr/bin/env python3
"""Regression contract: movement validates room and zone before zone accesses."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("int do_simple_move(")
end = source.index("int perform_move(", start)
body = source[start:end]
guards = [
    "!VALID_ROOM_RNUM(was_in)",
    "!VALID_ROOM_RNUM(going_to)",
    "!world", "!zone_table",
    "destination_zone = world[going_to].zone;",
    "destination_zone == NOWHERE",
    "destination_zone > top_of_zone_table",
]
for guard in guards:
    assert guard in body, f"Missing movement guard: {guard}"
assert body.index("!VALID_ROOM_RNUM(going_to)") < body.index("destination_zone = world[going_to].zone;")
assert body.index("destination_zone > top_of_zone_table") < body.index("ZONE_MINLVL(destination_zone)")
assert "ZONE_MINLVL(GET_ROOM_ZONE(going_to))" not in body
assert "ZONE_FLAGGED(GET_ROOM_ZONE(going_to)" not in body
# Callbacks can change world and actor state after the first validation.
assert body.count("!VALID_ROOM_RNUM(going_to)") >= 3
assert "world[going_to].zone != destination_zone" in body
assert body.count("destination_zone > top_of_zone_table") >= 3
assert "!VALID_ROOM_RNUM(IN_ROOM(ch))" in body
assert body.index("world[going_to].zone != destination_zone") < body.index("leave_mtrigger(ch, dir)")
print("Movement room and zone indexing guards verified")
