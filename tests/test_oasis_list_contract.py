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
# table by then), and a row that does not fit stops the listing instead of
# carrying len past the buffer.
aff = body_of("static void perform_obj_aff_list(struct char_data * ch, char *arg)")
for row in re.findall(r"real_object\(lst\[i\]\.vobj\)\) != NOTHING\) \{(.*?)\n    \}", aff, re.S):
    assert "obj_proto[num]" not in row, "row uses the scan index"
    assert "(size_t)n >= sizeof(buf) - len" in row
    assert "len += tmp_len" not in row
assert aff.count("real_object(lst[i].vobj)) != NOTHING) {") == 2
# olist type <n> indexes item_types[] with the builder's number.
typ = body_of("static void perform_obj_type_list(struct char_data * ch, char *arg)")
check = typ.index("itemtype < 0 || itemtype >= NUM_ITEM_TYPES")
assert check < typ.index("item_types[itemtype]")
print("oasis list contract verified")
