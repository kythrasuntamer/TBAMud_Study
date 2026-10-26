#!/usr/bin/env python3
"""Source-level regression guards for diagnostic-integrity instrumentation.

These checks do not replace compiled runtime/fault-injection testing.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "utils.c").read_text(encoding="utf-8")


class DiagnosticIntegrityTests(unittest.TestCase):
    def test_pool_exhaustion_is_reported(self):
        self.assertIn('if (create && free_slot == NULL)', SOURCE)
        self.assertIn('"parse-context-exhausted"', SOURCE)
        self.assertIn('tb_parse_context_overflows++', SOURCE)

    def test_pool_exhaustion_preserves_errno(self):
        section = SOURCE.split('if (create && free_slot == NULL)', 1)[1].split('return NULL;', 1)[0]
        self.assertIn('int saved_errno = errno;', section)
        self.assertIn('errno = saved_errno;', section)

    def test_parser_detail_truncation_is_visible(self):
        body = SOURCE.split('void tb_parse_context_error_at(', 1)[1].split('void basic_mud_vlog(', 1)[0]
        self.assertIn('(size_t)n >= sizeof(detail)', body)
        self.assertIn('...[DETAIL-TRUNCATED]', body)

    def test_breadcrumb_truncation_is_visible(self):
        body = SOURCE.split('size_t tb_breadcrumb_snapshot(', 1)[1].split('void tb_buf_init(', 1)[0]
        self.assertIn('if (out.truncated)', body)
        self.assertIn('...[BREADCRUMBS-TRUNCATED]', body)

    def test_logger_preserves_caller_errno(self):
        body = SOURCE.split('void tb_error_log_at(', 1)[1].split('void basic_mud_log(', 1)[0]
        self.assertIn('int incoming_errno = errno;', body)
        self.assertIn('errno = incoming_errno;', body)


if __name__ == "__main__":
    unittest.main()
