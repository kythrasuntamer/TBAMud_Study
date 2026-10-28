#!/usr/bin/env python3
"""OLC "copy" frees the buffer it replaces before building the copy.

oedit, medit and redit called *_setup_existing() over a live edit buffer,
leaking its strings and its private trigger-list copy (OLC_SCRIPT).
"""
from pathlib import Path
import sys

checks = {
    "oedit.c": ("case OEDIT_COPY:", "oedit_setup_existing(d, number);",
                ("OLC_OBJ(d)->proto_script = OLC_SCRIPT(d);",
                 "free_proto_script(OLC_OBJ(d), OBJ_TRIGGER);",
                 "free_object_strings(OLC_OBJ(d));", "free(OLC_OBJ(d));")),
    "medit.c": ("case MEDIT_COPY:", "medit_setup_existing(d, i);",
                ("OLC_MOB(d)->proto_script = OLC_SCRIPT(d);",
                 "free_mobile(OLC_MOB(d));")),
    "redit.c": ("case REDIT_COPY:", "redit_setup_existing(d, number);",
                ("OLC_ROOM(d)->proto_script = OLC_SCRIPT(d);",
                 "free_room(OLC_ROOM(d));")),
}
srcdir = Path(sys.argv[1])
for name, (case, setup, frees) in checks.items():
    s = (srcdir / name).read_text(encoding="utf-8")
    body = s[s.index(case):]
    body = body[:body.index(setup)]
    for f in frees:
        assert f in body, f"{name}: {f}"
    assert "OLC_SCRIPT(d) = NULL;" in body, name
print("OLC copy contract verified")
