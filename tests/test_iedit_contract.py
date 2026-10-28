#!/usr/bin/env python3
"""iedit identifies the live object by UID, never by address alone.

The live object may be extracted while it is being edited, and a new object
can be allocated at the same address; an address search of object_list then
accepts the impostor and the edit is applied to it.
"""
from pathlib import Path
import re
import sys

oasis = Path(sys.argv[1]).read_text(encoding="utf-8")
oedit = Path(sys.argv[2]).read_text(encoding="utf-8")
assert re.search(r"\blong iedit_uid;", oasis) and "#define OLC_IEDIT_UID(d)" in oasis
start = oedit[oedit.index("ACMD(do_oasis_iedit)"):]
assert "OLC_IEDIT_UID(d) = obj_script_id(obj);" in start[:start.index("\n}\n")]
m = re.search(r"static \w+ iedit_save_internally\(struct descriptor_data \*d\)\n\{", oedit)
assert m, "iedit_save_internally definition"
save = oedit[m.start():]
save = save[:save.index("iedit_replace_string(")]
assert "obj_uid_matches(OLC_IEDIT_UID(d), target)" in save
assert "iedit_object_still_exists" not in oedit
print("iedit contract verified")
