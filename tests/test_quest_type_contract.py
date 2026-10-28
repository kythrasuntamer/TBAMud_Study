#!/usr/bin/env python3
"""Quest types are names looked up through QUEST_TYPE_NAME().

Every new quest has type AQ_UNDEFINED (-1), so indexing quest_types[] with
a quest's type directly read before the table.
"""
from pathlib import Path
import re
import sys

header = Path(sys.argv[1]).read_text(encoding="utf-8")
assert "#define QUEST_TYPE_NAME(type)" in header
macro = header[header.index("#define QUEST_TYPE_NAME(type)"):]
macro = macro[:macro.index('"Undefined")') + len('"Undefined")')]
assert "(type) >= 0 && (type) < NUM_AQ_TYPES" in macro

for path in sys.argv[2:]:
    src = Path(path).read_text(encoding="utf-8")
    raw = re.findall(r"quest_types\[[^\]]*->type\]", src)
    assert not raw, f"{path}: {raw}"
print("quest type contract verified")
