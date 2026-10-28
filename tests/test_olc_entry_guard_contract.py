#!/usr/bin/env python3
"""OLC entry commands refuse callers without their own playing descriptor.

The editor state lives on ch->desc; a forced mob or switched body has none,
and these commands dereferenced it.
"""
from pathlib import Path
import sys

GUARD = "if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)"
srcdir = Path(sys.argv[1])
for name, entry in (("hedit.c", "ACMD(do_oasis_hedit)"),
                    ("qedit.c", "ACMD(do_oasis_qedit)"),
                    ("msgedit.c", "ACMD(do_msgedit)"),
                    ("prefedit.c", "ACMD(do_oasis_prefedit)")):
    s = (srcdir / name).read_text(encoding="utf-8")
    body = s[s.index(entry + "\n{"):]
    body = body[:body.index("\n}\n")]
    assert GUARD in body, name
print("OLC entry guard contract verified")
