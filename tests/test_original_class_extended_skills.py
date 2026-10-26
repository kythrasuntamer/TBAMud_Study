"""Keep original class IDs and late-game skill access stable."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class OriginalClassSkillTests(unittest.TestCase):
    def test_existing_class_ids_and_extended_skills(self):
        structs = (ROOT / "src/structs.h").read_text()
        cls = (ROOT / "src/class.c").read_text()
        for name, number in (("MAGIC_USER", 0), ("CLERIC", 1),
                             ("THIEF", 2), ("WARRIOR", 3), ("MONK", 4)):
            self.assertRegex(structs, rf"(?m)^#define CLASS_{name}\s+{number}\b")
        for skill, klass, level in (
            ("TRACK", "MAGIC_USER", 34), ("BANDAGE", "MAGIC_USER", 40),
            ("RESCUE", "MAGIC_USER", 46), ("TRACK", "CLERIC", 30),
            ("BANDAGE", "CLERIC", 34), ("RESCUE", "CLERIC", 40),
            ("KICK", "THIEF", 18), ("BANDAGE", "THIEF", 26),
            ("RESCUE", "THIEF", 34), ("BASH", "THIEF", 42),
            ("SNEAK", "WARRIOR", 30), ("HIDE", "WARRIOR", 38)):
            self.assertIn(f"spell_level(SKILL_{skill}, CLASS_{klass}, {level});", cls)

if __name__ == "__main__":
    unittest.main()
