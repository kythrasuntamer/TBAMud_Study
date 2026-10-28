#!/usr/bin/env python3
"""OLC delete removes the vnum being edited, never the buffer's source.

After "copy", an editor's buffer carries the source prototype's rnum (or a
room's number); deleting by it removed the source instead of the vnum the
builder had open. Each editor resolves the target from OLC_NUM(d) and
deletes exactly that.
"""
from pathlib import Path
import sys

checks = {
    "oedit.c": ("case OEDIT_DELETE:", "obj_rnum doomed = real_object(OLC_NUM(d));",
                "delete_object(doomed)", "delete_object(GET_OBJ_RNUM(OLC_OBJ(d)))"),
    "medit.c": ("case MEDIT_DELETE:", "mob_rnum doomed = real_mobile(OLC_NUM(d));",
                "delete_mobile(doomed)", "delete_mobile(GET_MOB_RNUM(OLC_MOB(d)))"),
    "redit.c": ("case REDIT_DELETE:", "room_rnum doomed = real_room(OLC_NUM(d));",
                "delete_room(doomed)", "delete_room(real_room(OLC_ROOM(d)->number))"),
}
srcdir = Path(sys.argv[1])
for name, (case, target, call, banned) in checks.items():
    s = (srcdir / name).read_text(encoding="utf-8")
    body = s[s.index(case):]
    body = body[:body.index("Couldn't delete the")]
    assert target in body, f"{name}: target is the edited vnum"
    assert body.index(target) < body.index(call), f"{name}: deletes that target"
    assert banned not in s, f"{name}: no delete by the buffer's identity"
print("OLC delete contract verified")
