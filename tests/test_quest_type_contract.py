#!/usr/bin/env python3
"""qedit names quest types through a bounded lookup.

Every new quest has type AQ_UNDEFINED (-1), and a loaded quest's type comes
from the quest file, so indexing quest_types[] with a quest's type directly
read before (or past) the table. qedit_type_name() is the one place that
indexes it: only after checking 0..NUM_AQ_TYPES-1, otherwise "Undefined"
(with a warning for anything but AQ_UNDEFINED).
"""
from pathlib import Path
import re
import sys

src = Path(sys.argv[1]).read_text(encoding="utf-8")
start = src.index("static const char *qedit_type_name(const struct aq_data *quest)\n{")
helper = src[start:src.index("\n}\n", start)]
check = helper.index("quest->type >= 0 && quest->type < NUM_AQ_TYPES")
assert check < helper.index("return quest_types[quest->type];")
assert 'return "Undefined";' in helper and "TB_WARN_MSG" in helper

rest = src[:start] + src[start + len(helper):]
raw = re.findall(r"quest_types\[[^\]]*->type\]", rest)
assert not raw, f"raw quest type indexing outside qedit_type_name(): {raw}"
menu = src[src.index("static void qedit_disp_menu(struct descriptor_data *d)\n{"):]
assert "qedit_type_name(quest)," in menu[:menu.index("\n}\n")]
print("quest type contract verified")
