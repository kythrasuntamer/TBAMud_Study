"""Security regression contracts for player-stat parsing and compatibility."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PlayerStatParsingContracts(unittest.TestCase):
    def test_player_stat_parser_checks_both_numbers(self):
        text = (ROOT / "src/players.c").read_text(encoding="utf-8")
        parser = text.split("static void load_HMVS(struct char_data *ch, const char *line, int mode)\n{", 1)[1].split("\nstatic void write_aliases_ascii", 1)[0]
        self.assertIn("strtol(line, &end, 10)", parser)
        self.assertIn("errno == ERANGE", parser)
        self.assertIn("first < INT_MIN", parser)
        self.assertIn("second < INT_MIN", parser)
        self.assertIn("*end != '/'", parser)
        self.assertIn("*end != '\\0'", parser)
        self.assertNotIn("sscanf(", parser)
        for name in ("GET_HIT", "GET_MANA", "GET_MOVE", "LOAD_STRENGTH"):
            self.assertIn(name, parser)

    def test_saved_ap_tags_unchanged(self):
        text = (ROOT / "src/players.c").read_text(encoding="utf-8")
        for tag in ("EgAP", "EgXP", "EgHP", "EgMP"):
            with self.subTest(tag=tag):
                self.assertIn(f'!strcmp(tag, "{tag}")', text)
                self.assertIn(f'"{tag}: %d', text)

if __name__ == "__main__":
    unittest.main()
