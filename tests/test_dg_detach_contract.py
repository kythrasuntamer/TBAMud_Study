#!/usr/bin/env python3
"""Guard do_detach against instantiating triggers and NULL trigger args.

do_detach used read_trigger() to build a whole trigger just to print its
name: the copy leaked on every call, and an unknown vnum gave a NULL
trigger whose name was then read. It also removed a NULL trigger name from
objects when no trigger argument was given.
"""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")
start = s.index("ACMD(do_detach)")
body = s[start:s.index("\n}\n", start)]
assert "read_trigger(" not in body, "do_detach must not instantiate triggers"
assert "GET_TRIG_NAME(trig)" not in body
assert body.count("detach_label(") == 3, "every confirmation uses the prototype label"
assert "trigger && remove_trigger(SCRIPT(victim), trigger)" in body
assert "trigger && remove_trigger(SCRIPT(object), trigger)" in body

label = s[s.index("static const char *detach_label("):start]
assert "rn != NOTHING" in label, "unknown vnums fall back to the argument"
assert "trig_index[rn]->proto" in label
print("DG detach contract verified")
