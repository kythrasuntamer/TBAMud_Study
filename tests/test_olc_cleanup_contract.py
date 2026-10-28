#!/usr/bin/env python3
"""After cleanup_olc() an editor's parse function must not touch OLC state.

The "should never get here" branches freed the editor with cleanup_olc()
and then broke out of the switch into "OLC_VAL(d) = ...; *_disp_menu(d);",
which used the freed (now NULL) d->olc. Such a branch either ends with
OLC_ABORT() (olc_checks.h: report, cleanup_olc(), return) or, where the old
form remains, returns right after cleanup_olc() instead of breaking.
"""
from pathlib import Path
import re
import sys

srcdir = Path(sys.argv[1])

# OLC_ABORT() must free the session and then return, never fall through.
checks_h = (srcdir / "olc_checks.h").read_text(encoding="utf-8")
macro = checks_h[checks_h.index("#define OLC_ABORT(d, op)"):]
macro = macro[:macro.index("} while (0)")]
assert macro.index("cleanup_olc((d), CLEANUP_ALL);") < macro.index("return;"), \
    "OLC_ABORT must return after cleanup_olc()"

checked = 0
for path in sorted(srcdir.glob("*edit.c")):
    s = path.read_text(encoding="utf-8")
    checked += len(re.findall(r"\bOLC_ABORT\(d, \"[a-z-]+\"\);", s))
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
