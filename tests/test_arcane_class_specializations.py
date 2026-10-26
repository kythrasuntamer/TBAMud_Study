"""Regression contracts for Wizard efficiency and Sorcerer empowerment."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ArcaneSpecializationTests(unittest.TestCase):
    def test_mana_mechanics(self):
        source = (ROOT / "src/spell_parser.c").read_text()
        self.assertIn("GET_CLASS(ch) == CLASS_MAGIC_USER", source)
        self.assertIn("mana = MAX(1, (mana * 4 + 4) / 5);", source)
        self.assertIn("GET_CLASS(ch) == CLASS_SORCERER", source)
        self.assertIn("mana = (mana * 3 + 1) / 2;", source)
        self.assertIn("GET_LEVEL(ch) >= 20", source)

    def test_empowerment_restricted_to_direct_cast(self):
        source = (ROOT / "src/magic.c").read_text()
        self.assertIn("casttype == CAST_SPELL", source)
        self.assertIn("GET_CLASS(ch) == CLASS_SORCERER", source)
        self.assertIn("dam += dam / 4;", source)
        self.assertIn("CAST_UNDEFINED", source)
        header = (ROOT / "src/spells.h").read_text()
        self.assertIn("int spellnum, int savetype, int casttype);", header)

if __name__ == "__main__":
    unittest.main()
