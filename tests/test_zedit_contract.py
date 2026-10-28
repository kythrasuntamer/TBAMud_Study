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
print("zedit contract verified")
