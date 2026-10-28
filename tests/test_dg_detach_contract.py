#!/usr/bin/env python3
"""Guard do_detach against instantiating triggers and NULL trigger args.

do_detach used read_trigger() to build a whole trigger just to print its
name: the copy leaked on every call, and an unknown vnum gave a NULL
trigger whose name was then read. It also removed a NULL trigger name from
objects when no trigger argument was given. remove_trigger() now hands back
the vnum and name of the trigger it removed, captured before extraction.
"""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")
start = s.index("ACMD(do_detach)")
body = s[start:s.index("\n}\n", start)]
assert "read_trigger(" not in body, "do_detach must not instantiate triggers"
assert "GET_TRIG_NAME(trig)" not in body
# Every confirmation names the trigger remove_trigger() actually removed.
assert body.count("&removed_vnum") == 3, \
    "every confirmation reports the removed trigger"
assert "trigger && remove_trigger(SCRIPT(victim), trigger," in body
assert "trigger && remove_trigger(SCRIPT(object), trigger," in body

rt = s[s.rindex("static int remove_trigger(struct script_data *sc, char *name,\n                          trig_vnum *vnum_out"):]
rt = rt[:rt.index("\n}\n")]
assert "if (!name)" in rt, "a NULL trigger name is refused"
assert "*vnum_out = GET_TRIG_VNUM(i);" in rt, "the removed trigger is captured before it is freed"
print("DG detach contract verified")
