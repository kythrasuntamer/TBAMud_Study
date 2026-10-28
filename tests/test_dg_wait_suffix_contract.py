#!/usr/bin/env python3
"""Guard accepted DG wait suffix spellings and strict rejection."""
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text(encoding="utf-8")
start = s.index("static int process_wait(void *go", s.index("static int script_wait_number"))
body = s[start:s.index("GET_TRIG_WAIT(trig) = event_create", start)]
for token in ("s", "sec", "secs", "second", "seconds", "t"):
    assert f'strcmp(suffix, "{token}") == 0' in body, token
assert "invalid wait suffix" in body
assert "suffix_len >= sizeof(suffix)" in body
assert "while (isspace((unsigned char)*suffix_end))" in body
assert "memcpy(suffix, rest, suffix_len);" in body
# "t" counts mud hours, every other accepted suffix counts seconds.
assert "long unit = (c == 't') ? PULSES_PER_MUD_HOUR : PASSES_PER_SEC;" in body
assert "when *= unit;" in body
# Oversized waits saturate and are capped (with a log), never wrapped.
assert "when > DG_MAX_WAIT_PULSES" in body
print("DG wait suffix contract verified")
