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
assert "free_trigger(OLC_TRIG(d));" in copy, "the replaced buffer is released"
assert "free(OLC_STORAGE(d));" in copy
assert "GET_TRIG_RNUM(OLC_TRIG(d)) = real_trigger(OLC_NUM(d));" in copy
existing = s[s.index("/* make the prorotype look like what we have */"):]
assert existing.index("proto->nr = rnum;") < existing.index("go through the mud")
print("trigedit contract verified")
