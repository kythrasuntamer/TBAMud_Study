#!/usr/bin/env python3
"""Regression tests for the standalone tools in src/util (wld2html, split).

Usage: test_util_tools.py <wld2html> <split> [<fortified-wld2html> | -]

The optional third argument is a -O2 -D_FORTIFY_SOURCE=3 build of wld2html
("-" when the build tree has none, e.g. under sanitizers).

Each case runs the real tool binary in a fresh temporary directory on a
crafted input and asserts on the exit status and on the stable
``operation="..."`` field of the TBERR diagnostic it must emit.  When the
binaries are built with ASan/UBSan, any sanitizer report also fails the test.
"""

import os
import subprocess
import sys
import tempfile
import unittest

WLD2HTML = None
SPLIT = None
WLD2HTML_FORTIFIED = None

SANITIZER_MARKERS = ("AddressSanitizer", "LeakSanitizer", "runtime error:")

VALID_ROOM = "#100\nRoom~\nDesc\n~\n0 0 0\nS\n"


def run_tool(argv, workdir, stdin_bytes=b""):
    proc = subprocess.run(argv, cwd=workdir, input=stdin_bytes,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=60)
    return proc.returncode, proc.stderr.decode("utf-8", "replace")


class ToolCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name, data):
        mode = "wb" if isinstance(data, bytes) else "w"
        with open(os.path.join(self.dir, name), mode) as fh:
            fh.write(data)

    def read(self, name):
        with open(os.path.join(self.dir, name), "rb") as fh:
            return fh.read()

    def exists(self, name):
        return os.path.exists(os.path.join(self.dir, name))

    def assertNoSanitizer(self, err):
        for marker in SANITIZER_MARKERS:
            self.assertNotIn(marker, err)

    def assertDiag(self, err, op):
        self.assertIn('operation="%s"' % op, err)

    def wld(self, data, *extra_files):
        self.write("in.wld", data)
        rc, err = run_tool([WLD2HTML, "in.wld"] + list(extra_files), self.dir)
        self.assertNoSanitizer(err)
        return rc, err

    def split(self, data):
        rc, err = run_tool([SPLIT], self.dir, data)
        self.assertNoSanitizer(err)
        return rc, err


class Wld2HtmlTests(ToolCase):
    def test_valid_room_converts(self):
        rc, err = self.wld(VALID_ROOM + "$~\n")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("TBERR", err)
        self.assertIn(b"Room", self.read("100.html"))

    def test_parse_error_names_file_line_and_vnum(self):
        # I3-12: a malformed flags line reports input file, line and vnum.
        rc, err = self.wld("#100\nRoom~\nDesc~\nnot-a-number\nS\n$~\n")
        self.assertEqual(rc, 1)
        self.assertDiag(err, "wld-room-flags-line")
        self.assertIn("file in.wld line 4 room #100", err)

    def test_door_index_out_of_range_rejected(self):
        # I3-01: D12 / D-1 / Dx used to index dir_option[] unchecked.
        for door in ("D12", "D-1", "Dx", "D10"):
            with self.subTest(door=door):
                rc, err = self.wld("#100\nRoom~\nDesc~\n0 0 0\n%s\n~\n~\n"
                                   "0 0 100\nS\n$~\n" % door)
                self.assertEqual(rc, 1, err)
                self.assertDiag(err, "wld-door-index")
                self.assertIn("room #100", err)
                self.assertFalse(self.exists("100.html"))

    def test_overlong_string_line_rejected(self):
        # I3-02 / I3-V01: a 700-byte description line wrote past tmp[512]
        # and corrupted the heap through the strlcpy in fread_string.
        rc, err = self.wld("#100\nRoom~\n" + "x" * 700 + "\n~\n0 0 0\nS\n$~\n")
        self.assertEqual(rc, 1, err)
        self.assertDiag(err, "wld-line-too-long")
        self.assertIn("file in.wld line 3 room #100", err)

    def test_string_line_at_limit_accepted(self):
        rc, err = self.wld("#100\nRoom~\n" + "y" * 509 + "\n~\n0 0 0\nS\n$~\n")
        self.assertEqual(rc, 0, err)
        self.assertIn(b"y" * 509 + b"\r\n", self.read("100.html"))

    def test_nul_in_string_line_rejected(self):
        # I3-02: a leading NUL made fread_string write tmp[-1].
        rc, err = self.wld(b"#100\nRoom~\n\0abc\n~\n0 0 0\nS\n$~\n")
        self.assertEqual(rc, 1, err)
        self.assertDiag(err, "wld-line-nul")

    def test_record_count_mismatch_stops_before_overflow(self):
        # I3-03: a 259-byte comment line was not a record for the counter
        # but the loader split it, so '#101' at offset 255 started a second
        # room in a world[] sized for one.  Since I3-07 the loader no
        # longer splits lines, so the overlong line itself is reported.
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 0 0\nS\n*" + "a" * 254 +
                           "#101\nRoom2~\nD~\n0 0 0\nS\n$~\n")
        self.assertEqual(rc, 1, err)
        self.assertDiag(err, "wld-line-too-long")
        self.assertIn("file in.wld line 6 room #100", err)

    def test_large_vnum_converted_not_dropped(self):
        # I3-06: '#100000' silently ended the file with exit status 0.
        rc, err = self.wld(VALID_ROOM + VALID_ROOM.replace("#100", "#100000")
                           + "$~\n")
        self.assertEqual(rc, 0, err)
        self.assertTrue(self.exists("100.html"))
        self.assertTrue(self.exists("100000.html"))

    def test_sentinel_vnum_reported(self):
        rc, err = self.wld(VALID_ROOM + "#99999\n" +
                           VALID_ROOM.replace("#100", "#101") + "$~\n")
        self.assertEqual(rc, 0, err)
        self.assertDiag(err, "wld-sentinel")
        self.assertIn("severity=WARN", err)
        self.assertFalse(self.exists("101.html"))

    def test_flag_letter_past_bit_31_rejected(self):
        # I3-08: 'Z' meant 1 << 51 on a signed int (UBSan: left shift of 1
        # by 31 places for 'F' already overflowed int).
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 aZ 0\nS\n$~\n")
        self.assertEqual(rc, 1, err)
        self.assertDiag(err, "wld-flag-bit")
        self.assertIn("room #100", err)

    def test_flag_letters_up_to_bit_31_accepted(self):
        # 'F' is bit 31: must not trip UBSan's signed-shift check.
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 aFz 0\nS\n$~\n")
        self.assertEqual(rc, 0, err)

    def test_numeric_flags_out_of_range_rejected(self):
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 99999999999 0\nS\n$~\n")
        self.assertEqual(rc, 1, err)
        self.assertDiag(err, "wld-flag-value")

    def test_empty_room_name_and_description_reported(self):
        # I3-10: '~' fields were NULL; tb_fputs(NULL) failed without a vnum
        # and a nameless room was skipped as if never loaded.
        rc, err = self.wld("#100\n~\n~\n0 0 0\nS\n$~\n")
        self.assertEqual(rc, 0, err)
        self.assertDiag(err, "wld-empty-field")
        self.assertIn("room #100", err)
        self.assertNotIn('operation="fputs"', err)
        self.assertTrue(self.exists("100.html"))

    def test_exit_to_unused_slot_is_missing_room(self):
        # I3-10: findRoom(0) matched the zeroed slot left by '#99999' and
        # printed its NULL name ("North to (null)").
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 0 0\nD0\n~\n~\n0 0 0\nS\n"
                           "#99999\n$~\n")
        self.assertEqual(rc, 0, err)
        page = self.read("100.html")
        self.assertNotIn(b"(null)", page)
        self.assertIn(b"North to missing room 0", page)

    def test_final_line_without_newline_accepted(self):
        # I3-07: a valid file whose '$~' had no newline was rejected as a
        # format error blaming room #100.
        rc, err = self.wld(VALID_ROOM + "$~")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("TBERR", err)
        self.assertTrue(self.exists("100.html"))

    def test_crlf_record_lines_accepted(self):
        rc, err = self.wld(VALID_ROOM.replace("\n", "\r\n") + "$~\r\n")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("TBERR", err)

    def test_multiple_files_share_record_capacity(self):
        self.write("b.wld", "#200\nOther~\nD~\n0 0 0\nD0\n~\n~\n0 0 100\nS\n$~\n")
        rc, err = self.wld(VALID_ROOM + "$~\n", "b.wld")
        self.assertEqual(rc, 0, err)
        self.assertIn(b'href = "100.html"', self.read("200.html"))

    def test_door_index_in_range_accepted(self):
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 0 0\nD9\n~\n~\n"
                           "0 0 100\nS\n$~\n")
        self.assertEqual(rc, 0, err)
        self.assertIn(b"South West to", self.read("100.html"))


class SplitTests(ToolCase):
    def test_split_writes_files_and_index(self):
        rc, err = self.split(b"=a.wld\nfirst\n=b.wld\nsecond\n")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("a.wld"), b"first\n")
        self.assertEqual(self.read("b.wld"), b"second\n")
        self.assertEqual(self.read("index"), b"a.wld\nb.wld\n$\r\n")

    def test_overlong_name_line_rejected(self):
        # I3-05: *(strchr(line, '\n')) stored through NULL (SIGSEGV).
        rc, err = self.split(b"=" + b"n" * 300 + b"\nabc\n")
        self.assertEqual(rc, 1, err)
        self.assertDiag(err, "split-name-line")
        self.assertIn("stdin line 1", err)

    def test_final_name_line_without_newline(self):
        # I3-05: the same NULL store for a last '=' line with no newline.
        rc, err = self.split(b"=out\nabc\n=last")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("out"), b"abc\n")
        self.assertTrue(self.exists("last"))

    def test_equals_at_chunk_boundary_is_data(self):
        data = b"x" * 255 + b"=notafile\n"
        rc, err = self.split(b"=out\n" + data)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("out"), data)
        self.assertFalse(self.exists("notafile"))


class FortifiedWld2HtmlTests(ToolCase):
    """Valid input through a -O2 -D_FORTIFY_SOURCE=3 build of wld2html."""

    def setUp(self):
        if not WLD2HTML_FORTIFIED:
            self.skipTest("no fortified wld2html in this build tree")
        super().setUp()

    def test_fread_string_copy_bound_matches_allocation(self):
        # I3-04: strlcpy(rslt, buf, 8192) into a length+1 block aborted
        # with "*** buffer overflow detected ***" on every valid file.
        self.write("in.wld", VALID_ROOM + "$~\n")
        rc, err = run_tool([WLD2HTML_FORTIFIED, "in.wld"], self.dir)
        self.assertNotIn("buffer overflow detected", err)
        self.assertEqual(rc, 0, err)
        self.assertIn(b"Desc\r\n", self.read("100.html"))


def main():
    global WLD2HTML, SPLIT, WLD2HTML_FORTIFIED
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    WLD2HTML = os.path.abspath(sys.argv[1])
    SPLIT = os.path.abspath(sys.argv[2])
    rest = sys.argv[3:]
    if rest:
        if rest[0] != "-":
            WLD2HTML_FORTIFIED = os.path.abspath(rest[0])
        rest = rest[1:]
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
