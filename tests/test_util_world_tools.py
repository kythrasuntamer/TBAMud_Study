#!/usr/bin/env python3
"""Regression tests for the standalone tools in src/util (wld2html, split).

Usage: test_util_tools.py <path-to-wld2html> <path-to-split>

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

    def test_door_index_in_range_accepted(self):
        rc, err = self.wld("#100\nRoom~\nDesc~\n0 0 0\nD9\n~\n~\n"
                           "0 0 100\nS\n$~\n")
        self.assertEqual(rc, 0, err)
        self.assertIn(b"South West to", self.read("100.html"))


def main():
    global WLD2HTML, SPLIT
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    WLD2HTML = os.path.abspath(sys.argv[1])
    SPLIT = os.path.abspath(sys.argv[2])
    unittest.main(argv=[sys.argv[0]] + sys.argv[3:], verbosity=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
