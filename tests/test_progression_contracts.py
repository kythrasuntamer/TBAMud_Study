"""Regression guards for protected mortal progression and endgame features.

These source-level checks complement, but do not replace, CTest and in-game
character persistence/combat testing. Run with:
python3 -m unittest discover -s tests -p 'test_progression_contracts.py'
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

def source(name):
    return (ROOT / "src" / name).read_text(encoding="utf-8")

class ProgressionContractTests(unittest.TestCase):
    def test_mortal_and_immortal_level_boundaries(self):
        text = source("structs.h")
        for key, value in (("LVL_IMMORT", 51), ("LVL_IMPL", 60)):
            self.assertRegex(text, rf"#define\s+{key}\s+{value}\b")

    def test_level_50_score_displays_cap_not_next_level(self):
        text = source("act.informative.c")
        self.assertRegex(text, r'if \(GET_LEVEL\(ch\) == LVL_IMMORT - 1\)\s+send_to_char\(ch, "You have reached the maximum mortal level')
        self.assertRegex(text, r'else if \(GET_LEVEL\(ch\) < LVL_IMMORT - 1\)\s+send_to_char\(ch, "You need %d exp to reach your next level')

    def test_level_50_xp_awards_ap_without_leveling(self):
        text = source("limits.c")
        start = text.index("void gain_exp(")
        stop = text.index("void gain_exp_regardless(", start)
        body = text[start:stop]
        self.assertIn("GET_LEVEL(ch) == LVL_IMMORT - 1", body)
        self.assertIn("GET_ENDGAME_AP(ch)", body)
        self.assertIn("GET_ENDGAME_XP(ch)", body)
        self.assertIn("/ 100000", body)
        self.assertIn("% 100000", body)
        self.assertIn("    return;\n  }\n  if (gain > 0)", body, "AP path must exit before normal level-up")
        self.assertIn("GET_LEVEL(ch) < LVL_IMMORT - 1", body)

    def test_endgame_upgrade_cost_and_cap(self):
        text = source("act.other.c")
        start = text.index("ACMD(do_endgame)")
        body = text[start:]
        self.assertIn("cost = 2;", body)
        self.assertIn("amount = 10;", body)
        self.assertIn("*rank >= 100", body)
        self.assertIn("save_char(ch);", body)

    def test_wizard_preserves_class_zero(self):
        structs = source("structs.h")
        classes = source("class.c")
        self.assertRegex(structs, r"#define\s+CLASS_MAGIC_USER\s+0\b")
        self.assertIn('"Wizard"', classes)
        self.assertIn("case 'm':", classes)
        self.assertIn("case 'z': return CLASS_MAGIC_USER;", classes)

    def test_endgame_save_fields_present(self):
        player = source("players.c")
        # Save files use stable four-character tags, not internal C field names.
        # Require both deserialization and serialization for every AP field.
        for tag, accessor in (
            ("EgAP", "GET_ENDGAME_AP"),
            ("EgXP", "GET_ENDGAME_XP"),
            ("EgHP", "GET_ENDGAME_HP_RANK"),
            ("EgMP", "GET_ENDGAME_MP_RANK"),
        ):
            with self.subTest(tag=tag):
                self.assertIn(f'!strcmp(tag, "{tag}")', player)
                self.assertIn(f'"{tag}: %d', player)
                self.assertGreaterEqual(player.count(accessor + "(ch)"), 2)

if __name__ == "__main__":
    unittest.main()
