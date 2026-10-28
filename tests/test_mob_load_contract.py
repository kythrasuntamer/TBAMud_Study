#!/usr/bin/env python3
"""Mob file values that index name tables are range-checked at load (db.c).

Position, default position and sex index position_types[], positions[] and
the gender tables wherever the mob is shown; a hand-edited .mob file used to
store any value. parse_simple_mob() must check all three before storing
them and refuse the world with a structured FATAL naming the mob.
"""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")
mob = s[s.index("static void parse_simple_mob(FILE *mob_f, int i, int nr)\n{"):]
mob = mob[:mob.index("\n}\n")]
check = mob.index("if (!VALID_POS(t[0]) || !VALID_POS(t[1]) || !VALID_SEX(t[2])) {")
fatal = mob.index('BOOT_FATAL("mob-parse", "position",', check)
for store in ("GET_POS(mob_proto + i) = t[0];",
              "GET_DEFAULT_POS(mob_proto + i) = t[1];",
              "GET_SEX(mob_proto + i) = t[2];"):
    assert fatal < mob.index(store), store
for macro, table in (("VALID_POS(p)", "NUM_POSITIONS"), ("VALID_SEX(x)", "NUM_GENDERS")):
    line = s[s.index("#define " + macro):]
    assert table in line[:line.index("\n")], macro
print("mob load contract verified")
