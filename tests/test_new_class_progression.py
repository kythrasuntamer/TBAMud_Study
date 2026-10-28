"""Regression contracts for stable new class identities and core progression."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class NewClassProgressionTests(unittest.TestCase):
    def test_stable_class_ids(self):
        structs = (ROOT / "src/structs.h").read_text()
        names = ("MAGIC_USER", "CLERIC", "THIEF", "WARRIOR", "MONK",
                 "DRUID", "BERSERKER", "SORCERER", "PALADIN",
                 "RANGER", "BARD", "NECROMANCER")
        for number, name in enumerate(names):
            self.assertRegex(structs, rf"(?m)^#define CLASS_{name}\s+{number}\b")
        self.assertRegex(structs, r"(?m)^#define NUM_CLASSES\s+12\b")

    def test_all_new_classes_registered_for_creation(self):
        cls = (ROOT / "src/class.c").read_text()
        for letter, name in (("d", "DRUID"), ("b", "BERSERKER"),
                             ("s", "SORCERER"), ("p", "PALADIN"),
                             ("r", "RANGER"), ("a", "BARD"),
                             ("n", "NECROMANCER")):
            self.assertIn(f"case '{letter}': return CLASS_{name};", cls)
            self.assertIn(f"case CLASS_{name}:", cls)

    def test_new_class_spells_have_working_engine_ids(self):
        cls = (ROOT / "src/class.c").read_text()
        spells = (ROOT / "src/spells.h").read_text()
        for skill, name, level in re.findall(
            r"spell_level\((SPELL_\w+|SKILL_\w+), (CLASS_\w+), (\d+)\);", cls):
            self.assertIn(f"#define {skill}", spells,
                          f"{name} uses unknown spell/skill {skill}")
            self.assertGreaterEqual(int(level), 1)
            self.assertLessEqual(int(level), 50)

if __name__ == "__main__":
    unittest.main()
