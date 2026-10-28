#!/usr/bin/env python3
"""OLC delete removes the vnum being edited, never the buffer's source.

After "copy", an editor's buffer carries the source prototype's rnum (or a
room's number); deleting by it removed the source instead of the vnum the
builder had open.
"""
from pathlib import Path
import sys

checks = {
    "oedit.c": ("case OEDIT_DELETE:", "delete_object(real_object(OLC_NUM(d)))",
                "delete_object(GET_OBJ_RNUM(OLC_OBJ(d)))"),
    "medit.c": ("case MEDIT_DELETE:", "delete_mobile(real_mobile(OLC_NUM(d)))",
                "delete_mobile(GET_MOB_RNUM(OLC_MOB(d)))"),
    "redit.c": ("case REDIT_DELETE:", "delete_room(real_room(OLC_NUM(d)))",
                "delete_room(real_room(OLC_ROOM(d)->number))"),
}
srcdir = Path(sys.argv[1])
for name, (case, wanted, banned) in checks.items():
    s = (srcdir / name).read_text(encoding="utf-8")
    body = s[s.index(case):]
    body = body[:body.index("cleanup_olc(d, CLEANUP_ALL);")]
    assert wanted in body, f"{name}: deletes the edited vnum"
    assert banned not in s, f"{name}: no delete by the buffer's identity"
print("OLC delete contract verified")
