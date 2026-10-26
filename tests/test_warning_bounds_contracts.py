"""Source-level regression guards for warning-hardening fixes.

These complement compiler diagnostics and runtime regression testing.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
UTILS = (ROOT / "src" / "utils.c").read_text(encoding="utf-8")


class FormattingAndBoundsContracts(unittest.TestCase):
    def test_ibt_lookahead_checks_eof_and_ungetc(self):
        ibt = (ROOT / "src" / "ibt.c").read_text(encoding="utf-8")
        start = ibt.index("static IBT_DATA *read_ibt(")
        body = ibt[start:ibt.index("ibtData = new_ibt();", start)]
        self.assertIn("int letter;", body)
        self.assertIn("letter == EOF", body)
        self.assertIn("isspace((unsigned char)letter)", body)
        self.assertIn("ungetc(letter, fp) == EOF", body)

    def test_mudlog_checks_format_failure(self):
        start = UTILS.index("void mudlog(")
        end = UTILS.index("\n}", start)
        body = UTILS[start:end]
        self.assertRegex(body, r"formatted_length\s*=\s*vsnprintf\(")
        self.assertRegex(body, r"if\s*\(formatted_length\s*<\s*0\)")

    def test_strpaste_checks_remaining_capacity_without_underflow(self):
        start = UTILS.index("char *strpaste(")
        end = UTILS.index("\n}", start)
        body = UTILS[start:end]
        self.assertIn("if (used > MAX_STRING_LENGTH - 2)", body)
        self.assertIn("ret[used++] = '\\r';", body)
        self.assertIn("ret[used++] = '\\n';", body)


if __name__ == "__main__":
    unittest.main()
