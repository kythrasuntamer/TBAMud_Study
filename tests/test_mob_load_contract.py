#!/usr/bin/env python3
"""Mob file values that index name tables are range-checked at load (db.c)."""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")
mob = s[s.index("static void parse_simple_mob(FILE *mob_f, int i, int nr)\n{"):]
mob = mob[:mob.index("\n}\n")]
for field, count in (('"position", t[0]', "NUM_POSITIONS"),
                     ('"default position", t[1]', "NUM_POSITIONS"),
                     ('"sex", t[2]', "NUM_GENDERS")):
    assert f"mob_table_value(nr, {field}, {count}," in mob, field
for raw in ("= t[0];\n  GET_DEFAULT_POS", "GET_SEX(mob_proto + i) = t[2];"):
    assert raw not in mob
helper = s[s.index("static int mob_table_value("):]
helper = helper[:helper.index("\n}\n")]
assert "value >= 0 && value < count" in helper and "TB_ERROR_MSG" in helper
print("mob load contract verified")
