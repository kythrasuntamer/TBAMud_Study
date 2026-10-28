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
# Every reader of a rent-file header goes through the six-field parser.
readers = (("int Crash_delete_crashfile(", "int Crash_clean_file("),
           ("int Crash_clean_file(", "void update_obj_file("),
           ("void Crash_listrent(", "int Crash_load("),
           ("static int Crash_load_objs(struct char_data *ch) {", None))
for start_text, end_text in readers:
    body = source[source.index(start_text):]
    if end_text:
        body = body[:body.index(end_text)]
    assert body.count("objsave_parse_rent_header(line,") == 1, start_text
assert source.count("objsave_parse_rent_header(line,") == len(readers)
parser = source[source.index("static int objsave_parse_int_fields("):source.index("static int objsave_parse_rent_header(")]
assert "errno == ERANGE" in parser
assert "value < INT_MIN || value > INT_MAX" in parser
assert "return *p == '\\0';" in parser
print("Crash-save six-field reader contract verified")
