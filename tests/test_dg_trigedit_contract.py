#!/usr/bin/env python3
"""Guard trigedit's prototype bookkeeping.

Renumbering when a new trigger prototype is inserted:

Inserting at rnum shifts every prototype from rnum up by one. Live triggers
at exactly rnum were left behind (> instead of >=), so they named the new
prototype and were later rewritten or freed as the wrong trigger; the
editor's own buffer and buffers for unsaved (NOTHING) triggers were bumped.
"""
from pathlib import Path
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")
start = s.index("/* this is a new trigger */")
body = s[start:s.index("now write the trigger out to disk", start)]
live = body[body.index("for (live_trig = trigger_list;"):]
live = live[:live.index("for (dsc = descriptor_list;")]
assert "GET_TRIG_RNUM(live_trig) >= rnum" in live
assert "GET_TRIG_RNUM(live_trig) != NOTHING" in live
edit = body[body.index("for (dsc = descriptor_list;"):]
assert "dsc != d" in edit, "the new trigger's own buffer already names rnum"
assert "GET_TRIG_RNUM(OLC_TRIG(dsc)) != NOTHING" in edit
assert "GET_TRIG_RNUM(OLC_TRIG(dsc)) >= rnum" in edit

# Copying another trigger into the buffer must keep the edited trigger's
# identity; the copied rnum used to end up in the saved prototype.
copy = s[s.index("case TRIGEDIT_COPY:"):]
copy = copy[:copy.index("break;")]
assert "!= NOTHING" in copy
assert "tb_parse_int_checked(arg, &i)" in copy, "a non-numeric answer is not trigger #0"
# trigedit_setup_existing() releases the old scratch trigger and buffer
# itself; freeing them here as well was a double free.
assert "free_trigger(OLC_TRIG(d));" not in copy, "setup_existing releases the buffer"
assert "free(OLC_STORAGE(d));" not in copy
assert "GET_TRIG_RNUM(OLC_TRIG(d)) = real_trigger(OLC_NUM(d));" in copy
setup = s[s.index("void trigedit_setup_existing("):]
setup = setup[:setup.index("\n}\n")]
assert "free_trigger(OLC_TRIG(d));" in setup and "OLC_TRIG(d) = NULL;" in setup
assert "free(OLC_STORAGE(d));" in setup and "OLC_STORAGE(d) = NULL;" in setup
existing = s[s.index("make the prorotype look like what we have"):]
assert existing.index("proto->nr = rnum;") < existing.index("go through the mud")
# Setting the argument list replaces it: the old one is freed first.
arg = s[s.index("case TRIGEDIT_ARGUMENT:"):]
arg = arg[:arg.index("break;")]
assert arg.index("free(OLC_TRIG(d)->arglist);") < arg.index("OLC_TRIG(d)->arglist = ")
print("trigedit contract verified")
