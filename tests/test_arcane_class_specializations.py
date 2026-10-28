"""Check that native arcane calculations are used by the live spell paths."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ArcaneSpecializationTests(unittest.TestCase):
    def test_shared_mana_calculation(self):
        source = (ROOT / "src/spell_parser.c").read_text()
        helper = (ROOT / "src/arcane_specialization.h").read_text()
        manacost = source[source.index("static int mag_manacost(struct char_data *ch, int spellnum) {"):]
        manacost = manacost[:manacost.index("\n}\n")]
        self.assertIn("arcane_adjust_mana(cls,", manacost)
        self.assertIn("SINFO.mana_min);", manacost)
        # GET_CLASS() comes from the pfile; only the VALID_CLASS_NUM-guarded
        # SPELL_MIN_LEVEL() may index min_level[] with it.
        self.assertIn("SPELL_MIN_LEVEL(spellnum, cls)", manacost)
        self.assertNotIn("min_level[(int) GET_CLASS(ch)]", manacost)
        self.assertIn("class_id == CLASS_MAGIC_USER", helper)
        self.assertIn("class_id == CLASS_SORCERER", helper)
        self.assertIn("(base_mana * 4 + 4) / 5", helper)
        self.assertIn("(base_mana * 3 + 1) / 2", helper)

    def test_direct_cast_only_damage(self):
        source = (ROOT / "src/magic.c").read_text()
        helper = (ROOT / "src/arcane_specialization.h").read_text()
        self.assertIn("arcane_adjust_damage(GET_CLASS(ch)", source)
        self.assertIn("casttype == CAST_SPELL", source)
        self.assertIn("direct_cast && damage > 0", helper)
        self.assertIn("damage + damage / 4", helper)
        self.assertIn("CAST_UNDEFINED", source)

if __name__ == "__main__":
    unittest.main()
