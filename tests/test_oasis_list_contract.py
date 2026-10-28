#!/usr/bin/env python3
"""Guard the OLC object listings (oasis_list.c)."""
from pathlib import Path
import re
import sys

s = Path(sys.argv[1]).read_text(encoding="utf-8")


def body_of(signature):
    start = s.index(signature + "\n{")
    return s[start:s.index("\n}\n", start)]


# perform_obj_aff_list prints rows after scanning with num up to
# top_of_objt: a row must describe obj_proto[r_num] (num is one past the
# table by then), and rows go through olc_list_rowf(), which refuses a row
# that does not fit, instead of an unbounded len += snprintf() that carried
# len past the buffer.
aff = body_of("static void perform_obj_aff_list(struct char_data * ch, char *arg)")
head = "real_object(lst[i].vobj)) != NOTHING) {"
rows = []
for m in re.finditer(re.escape(head), aff):
    rows.append(aff[m.end():aff.index("found++;", m.end())])
assert len(rows) == 2, len(rows)
for row in rows:
    assert "obj_proto[num]" not in row, "row uses the scan index"
    assert "olc_list_rowf(&tb," in row
    assert "len +=" not in row and "snprintf(buf+len" not in row
# olist type <n> indexes item_types[] with the builder's number.
typ = body_of("static void perform_obj_type_list(struct char_data * ch, char *arg)")
check = typ.index("itemtype < 0 || itemtype >= NUM_ITEM_TYPES")
assert check < typ.index("item_types[itemtype]")
print("oasis list contract verified")
