#!/usr/bin/env python3
"""Guard object script commands (dg_objcmd.c) that act after a callback."""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")


def body_of(name):
    start = s.index(f"static OCMD({name})\n{{")
    return s[start:s.index("\n}\n", start)]


# oat runs its command through a temporary copy that the command may purge
# or move: the copy is checked by UID and always removed, never read raw.
oat = body_of("do_oat")
assert "copy_uid = obj_script_id(object);" in oat
assert oat.index("copy_uid = obj_script_id(object);") < oat.index("obj_command_interpreter(")
after = oat[oat.index("obj_command_interpreter("):]
assert "obj_uid_matches(copy_uid, object)" in after
assert "object->in_room" not in after, "the copy may be freed by now"
# oteleport all reads the object's own room: never world[NOWHERE], and a
# teleport onto itself stops instead of being logged and carried out.
tele = body_of("do_oteleport")
# VALID_OBJ_ROOM() stores obj_room(obj) in rm and range-checks it.
rm = tele[tele.index("!VALID_OBJ_ROOM(obj, rm)"):tele.index("dg_teleport_all(rm, target);")]
assert "!VALID_OBJ_ROOM(obj, rm)" in rm and rm.count("return;") == 2
# Teleporting everyone goes through the UID snapshot, not next_in_room.
assert "next_in_room" not in tele
# omove stops at an invalid target instead of moving into world[NOWHERE].
move = body_of("do_omove")
bad = move[move.index("if (target == NOWHERE)"):move.index("obj_from_char(obj)")]
assert "return;" in bad
print("object command contract verified")
