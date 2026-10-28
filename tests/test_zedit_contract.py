#!/usr/bin/env python3
"""Guard zedit's use of OLC_VAL as an index into the zone's command list."""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")

# Aborting a change used OLC_CMD(d) with an OLC_VAL left from an earlier
# edit, which can be past the end once commands are deleted.
change = s[s.index("case ZEDIT_CHANGE_ENTRY:"):]
abort = change[change.index("if (toupper(*arg) == 'A') {"):change.index("zedit_disp_menu(d);")]
guard = "OLC_VAL(d) < count_commands(OLC_ZONE(d)->cmd)"
assert guard in abort and abort.index(guard) < abort.index("OLC_CMD(d).command")
# A door direction must be a real one: DIR_COUNT is one past the end.
door = s[s.index("/* Count directions. */"):]
assert "pos >= DIR_COUNT" in door[:200]
# The menu shows zone-file commands before any reset has checked them.
for guard in ("MYCMD.arg2 >= 0 && MYCMD.arg2 < NUM_OF_DIRS",
              "MYCMD.arg3 >= 0 && MYCMD.arg3 < NUM_WEARS"):
    assert guard in s, guard
print("zedit contract verified")
