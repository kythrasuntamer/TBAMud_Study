#!/usr/bin/env python3
"""After cleanup_olc() an editor's parse function must not touch OLC state.

The "should never get here" branches freed the editor with cleanup_olc()
and then broke out of the switch into "OLC_VAL(d) = ...; *_disp_menu(d);",
which used the freed (now NULL) d->olc.
"""
from pathlib import Path
import re
import sys

srcdir = Path(sys.argv[1])
checked = 0
for path in sorted(srcdir.glob("*edit.c")):
    s = path.read_text(encoding="utf-8")
    for m in re.finditer(r"cleanup_olc\(d, CLEANUP_[A-Z]+\);", s):
        tail = s[m.end():m.end() + 400]
        if "Reached" not in tail.split("break;")[0].split("return;")[0]:
            continue
        nxt_break = tail.find("break;")
        nxt_return = tail.find("return;")
        checked += 1
        assert nxt_return != -1 and (nxt_break == -1 or nxt_return < nxt_break), \
            f"{path.name}: cleanup_olc() then break at offset {m.start()}"
assert checked >= 5, checked
print(f"OLC cleanup contract verified ({checked} branches)")
