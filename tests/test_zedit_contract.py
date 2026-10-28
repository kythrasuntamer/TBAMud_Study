#!/usr/bin/env python3
"""Guard zedit's use of OLC_VAL as an index into the zone's command list,
its door directions, and the table names its menu prints."""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")

# Aborting a change used OLC_CMD(d) with an OLC_VAL left from an earlier
# edit, which can be past the end once commands are deleted. The index must
# be checked against the list (count_commands() entries plus the 'S'
# terminator) before OLC_CMD(d) is read.
change = s[s.index("case ZEDIT_CHANGE_ENTRY:"):]
abort = change[change.index("if (toupper(*arg) == 'A') {"):change.index("zedit_disp_menu(d);")]
guard = "OLC_VAL(d) < 0 || OLC_VAL(d) > count"
assert "int count = count_commands(OLC_ZONE(d)->cmd);" in abort
assert guard in abort and abort.index(guard) < abort.index("OLC_CMD(d).command")
# A door direction must be a real one: DIR_COUNT is one past the end.
door = s[s.index("pos = tb_parse_int_or_zero(arg);", s.index("    case 'D':\n      pos")):]
assert "pos < 0 || pos >= DIR_COUNT" in door[:400]
# The menu shows zone-file commands before any reset has checked them, so
# table names go through the bounded OLC_TABLE_NAME().
for guard in ("OLC_TABLE_NAME(d, dirs, NUM_OF_DIRS, MYCMD.arg2)",
              "OLC_TABLE_NAME(d, equipment_types, NUM_WEARS, MYCMD.arg3)"):
    assert guard in s, guard
assert "dirs[MYCMD.arg2]" not in s and "equipment_types[MYCMD.arg3]" not in s
print("zedit contract verified")
