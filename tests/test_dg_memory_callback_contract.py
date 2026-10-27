#!/usr/bin/env python3
"""Static contracts for callback-safe script-memory removal."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
for first, last in (("void greet_memory_mtrigger(", "int greet_mtrigger("),
                    ("void entry_memory_mtrigger(", "int entry_mtrigger(")):
    body = source[source.index(first):source.index(last, source.index(first))]
    assert "SCRIPT_MEM(ch)) {" in body
    assert "find_char(target->uid)" in body
    assert "*link = mem->next;" in body
    assert body.index("*link = mem->next;") < body.index("script_driver(&ch,")
    assert "next_mem" not in body
    assert "while (prev->next != mem)" not in body
    assert "free(mem);" in body
greet = source[source.index("void greet_memory_mtrigger("):source.index("int greet_mtrigger(")]
assert "char *command_copy = strdup(mem->cmd);" in greet
assert "command_interpreter(ch, command_copy);" in greet
assert "free(command_copy);" in greet
assert greet.index("if (mem->cmd)") < greet.index("*link = mem->next;")
entry = source[source.index("void entry_memory_mtrigger("):source.index("int entry_mtrigger(")]
assert "static int dg_memory_character_live(" in source
assert "MOB_NOTDEADYET" in source and "PLR_NOTDEADYET" in source
assert "if (!dg_memory_character_live(ch, owner_uid, origin_room))" in entry
assert "dg_memory_character_live(actor, target->uid, origin_room)" in entry
assert "dg_memory_character_live(actor, actor_uid, origin_room)" in greet
assert "dg_memory_character_live(ch, target->uid, origin_room)" in greet
assert "free(mem);\n        /* The callback" in entry
assert greet.index("command_interpreter(ch, command_copy);") < greet.index("free(command_copy);")
print("DG script-memory source contracts verified")
