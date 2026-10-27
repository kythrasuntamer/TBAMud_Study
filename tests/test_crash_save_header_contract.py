#!/usr/bin/env python3
"""Guard six-field parsing in crash-file deletion without weakening validation."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("int Crash_delete_crashfile(")
end = source.index("int Crash_clean_file(", start)
delete = source[start:end]
assert "objsave_parse_rent_header(line, &rentcode, &timed, &netcost," in delete
assert "&gold, &account, &nitems)" in delete
assert "objsave_parse_int_fields(line, &rentcode, 1)" not in delete
assert "if (rentcode == RENT_CRASH)" in delete
assert "Crash_delete_file(GET_NAME(ch));" in delete
parser = source[source.index("static int objsave_parse_int_fields("):source.index("static int objsave_parse_rent_header(")]
assert "errno == ERANGE" in parser
assert "value < INT_MIN || value > INT_MAX" in parser
assert "return *p == '\\0';" in parser
print("Crash-save six-field reader contract verified")
