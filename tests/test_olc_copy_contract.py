#!/usr/bin/env python3
"""OLC "copy" frees the buffer it replaces before building the copy.

oedit, medit, redit and sedit called *_setup_existing() over a live edit
buffer, leaking its strings and its private trigger-list copy (OLC_SCRIPT).
Each handler must release the scratch copy the way cleanup_olc() does
(olc_free_script_copy() plus the type's free routine) before the setup.
"""
from pathlib import Path
import sys

checks = {
    "oedit.c": ("case OEDIT_COPY:", "oedit_setup_existing(d, number);",
                ("olc_free_script_copy(d);", "free_object_strings(OLC_OBJ(d));",
                 "free(OLC_OBJ(d));", "OLC_OBJ(d) = NULL;")),
    "medit.c": ("case MEDIT_COPY:", "medit_setup_existing(d, i);",
                ("olc_free_script_copy(d);", "free_mobile(OLC_MOB(d));",
                 "OLC_MOB(d) = NULL;")),
    "redit.c": ("case REDIT_COPY:", "redit_setup_existing(d, number);",
                ("olc_free_script_copy(d);", "free_room(OLC_ROOM(d));",
                 "OLC_ROOM(d) = NULL;")),
    "sedit.c": ("case SEDIT_COPY:", "sedit_setup_existing(d, i);",
                ("free_shop(OLC_SHOP(d));", "OLC_SHOP(d) = NULL;")),
}
srcdir = Path(sys.argv[1])
for name, (case, setup, frees) in checks.items():
    s = (srcdir / name).read_text(encoding="utf-8")
    body = s[s.index(case):]
    body = body[:body.index(setup)]
    for f in frees:
        assert f in body, f"{name}: {f}"

# olc_free_script_copy() must free OLC_SCRIPT(d) and clear it, or the setup
# that follows would leak (or a later cleanup double-free) the old list.
zedit = (srcdir / "zedit.c").read_text(encoding="utf-8")
helper = zedit[zedit.index("void olc_free_script_copy(struct descriptor_data *d)\n{"):]
helper = helper[:helper.index("\n}\n")]
assert "free(tp);" in helper and "OLC_SCRIPT(d) = NULL;" in helper
print("OLC copy contract verified")
