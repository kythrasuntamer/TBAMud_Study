"""Source-level regression guards for warning-hardening fixes.

These complement compiler diagnostics and runtime regression testing.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
UTILS = (ROOT / "src" / "utils.c").read_text(encoding="utf-8")


class FormattingAndBoundsContracts(unittest.TestCase):
    def test_information_level_ranges_checked(self):
        source = (ROOT / "src" / "act.informative.c").read_text(encoding="utf-8")
        self.assertIn("static int parse_info_level_range(", source)
        self.assertIn("errno == ERANGE", source)
        self.assertIn("first > INT_MAX", source)
        self.assertIn("second > INT_MAX", source)
        self.assertNotIn('sscanf(arg, "%d-%d"', source)
        self.assertEqual(source.count("if (!parse_info_level_range(arg, &low, &high))"), 2)
        self.assertIn("ret = parse_info_level_range(arg, &min_lev, &max_lev);", source)

    def test_last_login_cleanup_never_deletes_original_before_replace(self):
        wizard = (ROOT / "src" / "act.wizard.c").read_text(encoding="utf-8")
        begin = wizard.index("void clean_llog_entries(void) {")
        end = wizard.index("/* debugging stuff", begin)
        routine = wizard[begin:end]
        self.assertIn("copied != MAX_LAST_ENTRIES", routine)
        self.assertIn("fflush(nfp) != 0", routine)
        self.assertIn("fclose(nfp) != 0", routine)
        self.assertIn("rename(temp_name, LAST_FILE) != 0", routine)
        self.assertNotIn("remove(LAST_FILE)", routine)
        self.assertIn("if (failed)", routine)

    def test_last_login_record_seeks_checked(self):
        wizard = (ROOT / "src" / "act.wizard.c").read_text(encoding="utf-8")
        start = wizard.index("struct last_entry *find_llog_entry(")
        end = wizard.index("void clean_llog_entries(", start)
        section = wizard[start:end]
        self.assertEqual(section.count('TB_REPORT_ERRNO("seek last-login file"'), 2)
        self.assertEqual(section.count('TB_REPORT_ERRNO("seek last-login record"'), 4)
        self.assertIn("(size = ftell(fp)) < 0", section)

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
