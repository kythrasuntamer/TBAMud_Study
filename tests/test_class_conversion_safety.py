"""Guard class conversion against save-format and endgame regressions.

Run: python3 -m unittest discover -s tests -p 'test_class_conversion_safety.py'
This is a source contract, not a substitute for gameplay tests.
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

class ClassConversionSafety(unittest.TestCase):
    def test_saved_original_class_ids_are_unchanged(self):
        s = read("src/structs.h")
        for name, id_ in (("MAGIC_USER", 0), ("CLERIC", 1), ("THIEF", 2), ("WARRIOR", 3)):
            with self.subTest(name=name):
                self.assertRegex(s, rf"#define\s+CLASS_{name}\s+{id_}\b")

    def test_monk_id_is_reserved_but_not_prematurely_playable(self):
        structs = read("src/structs.h")
        classes = read("src/class.c")
        self.assertRegex(structs, r"#define\s+CLASS_MONK\s+4\b")
        # Prevent raising NUM_CLASSES or permitting the selector until gameplay
        # systems, saving throws, level curves and trainers are implemented.
        self.assertRegex(structs, r"#define\s+NUM_CLASSES\s+4\b")
        parse_class_body = classes.split("int parse_class(char arg)", 1)[1].split("bitvector_t find_class_bitvector", 1)[0]
        self.assertNotIn("CLASS_MONK", parse_class_body)

    def test_mortal_and_immortal_levels_remain_50_and_60(self):
        s = read("src/structs.h")
        self.assertRegex(s, r"#define\s+LVL_IMMORT\s+51\b")
        self.assertRegex(s, r"#define\s+LVL_IMPL\s+60\b")

    def test_endgame_command_and_persistence_remain(self):
        for path, symbols in (
            ("src/act.other.c", ("ACMD(do_endgame)", "GET_ENDGAME_AP", "GET_ENDGAME_HP_RANK", "GET_ENDGAME_MP_RANK", "save_char(ch)")),
            ("src/limits.c", ("GET_ENDGAME_XP", "GET_ENDGAME_AP", "GET_LEVEL(ch) == LVL_IMMORT - 1")),
            ("src/players.c", ("EgAP", "EgXP", "EgHP", "EgMP")),
            ("src/act.informative.c", ("You have reached the maximum mortal level.",)),
        ):
            text = read(path)
            for symbol in symbols:
                with self.subTest(path=path, symbol=symbol):
                    self.assertIn(symbol, text)

    def test_manifest_reserves_existing_and_future_ids(self):
        data = json.loads(read("data/class_import_candidates.json"))
        original = [(c["id"], c["name"]) for c in data["existing"]]
        candidates = [(c["id"], c["name"]) for c in data["candidates"]]
        self.assertEqual(original, [(0, "Wizard"), (1, "Cleric"), (2, "Thief"), (3, "Warrior")])
        self.assertEqual(candidates, [
            (4, "Monk"), (5, "Druid"), (6, "Berserker"), (7, "Sorcerer"),
            (8, "Paladin"), (9, "Ranger"), (10, "Bard"),
        ])

    def test_manifest_never_claims_unimplemented_skills_are_live(self):
        data = json.loads(read("data/class_import_candidates.json"))
        for tree in data["skill_trees"]:
            for skill in tree["skills"]:
                with self.subTest(class_name=tree["class"], skill=skill["key"]):
                    self.assertIn("implementation", skill)
                    self.assertIn(skill["implementation"], ("NOT_IMPLEMENTED", "IMPLEMENTED"))
                    if skill["implementation"] == "IMPLEMENTED":
                        self.fail("Mark skills IMPLEMENTED only after adding gameplay-backed tests; update this guard in the same reviewed commit")

if __name__ == "__main__":
    unittest.main()
