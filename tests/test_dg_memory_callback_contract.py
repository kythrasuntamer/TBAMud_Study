#!/usr/bin/env python3
"""Static contracts for callback-safe script-memory removal."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
for first, last in (("void greet_memory_mtrigger(", "int greet_mtrigger("),
                    ("void entry_memory_mtrigger(", "int entry_mtrigger(")):
    body = source[source.index(first):source.index(last, source.index(first))]
    assert "while (SCRIPT_MEM(ch))" in body
    assert "*link = mem->next;" in body
    assert body.index("*link = mem->next;") < body.index("script_driver(&ch,")
    assert "next_mem" not in body
    assert "while (prev->next != mem)" not in body
    assert "free(mem);" in body
greet = source[source.index("void greet_memory_mtrigger("):source.index("int greet_mtrigger(")]
assert "command_interpreter(ch, mem->cmd);" in greet
assert greet.index("if (mem->cmd)") < greet.index("*link = mem->next;")
entry = source[source.index("void entry_memory_mtrigger("):source.index("int entry_mtrigger(")]
assert "return;" not in entry[entry.index("while (SCRIPT_MEM(ch))"):]
assert "find_char(owner_uid) != ch" in greet
assert "find_char(owner_uid) != ch" in entry
print("DG script-memory source contracts verified")
