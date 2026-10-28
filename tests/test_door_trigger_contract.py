#!/usr/bin/env python3
"""Guard do_doorcmd against acting on a world its door triggers changed.

door_mtrigger()/door_wtrigger() run scripts that can move or kill the
actor, remove the exit or purge the container; do_doorcmd then used
EXIT(ch, door) and obj as if nothing had happened.
"""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")
start = s.index("static int do_doorcmd(struct char_data *ch, struct obj_data *obj, int door, int scmd)\n{")
body = s[start:s.index("switch (scmd)", start)]
snap = body.index("room_rnum was_in = IN_ROOM(ch);")
assert snap < body.index("door_mtrigger("), "snapshot before any trigger"
assert "obj_script_id(obj)" in body[:body.index("door_mtrigger(")]
for trig in ("door_mtrigger(", "door_wtrigger("):
    after = body[body.index(trig):]
    after = after[:after.index("return FALSE;", after.index("door_target_still_valid("))]
    assert "door_target_still_valid(" in after, trig + " result is re-validated"

check = s[s.index("static int door_target_still_valid("):start]
for needle in ("DEAD(ch)", "IN_ROOM(ch) == was_in", "obj_uid_matches(obj_uid, obj)",
               "EXIT(ch, door)", "TB_WARN_MSG"):
    assert needle in check, needle
# AUTOKEY: the open runs only if the unlock was carried out.
assert "if (do_doorcmd(ch, obj, door, SCMD_UNLOCK))" in s
print("door trigger contract verified")
