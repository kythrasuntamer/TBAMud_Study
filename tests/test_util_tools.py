#!/usr/bin/env python3
"""Regression tests for the standalone tools in src/util.

Each test runs the real tool binary (the one the build produced, so a
sanitizer build exercises the instrumented code) on crafted input in a
scratch directory, then checks the exit status, the output files and the
structured TBERR diagnostics on stderr.  A guard that stops a crash but
reports nothing fails these tests: they assert on the diagnostic operation.

Usage: test_util_tools.py tool=/path/to/binary [tool=/path ...]
Tests whose tool was not given are skipped.
"""

import os
import re
import subprocess
import sys
import tempfile

TOOLS = {}
TESTS = []


def test(tool):
    def wrap(fn):
        TESTS.append((tool, fn))
        return fn
    return wrap


class Failure(Exception):
    pass


def check(cond, message):
    if not cond:
        raise Failure(message)


def run(tool, args, cwd, stdin=None):
    proc = subprocess.run([TOOLS[tool]] + list(args), cwd=cwd, input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=60)
    err = proc.stderr.decode("utf-8", "replace")
    check("Sanitizer" not in err and "runtime error:" not in err,
          "sanitizer report from %s:\n%s" % (tool, err))
    return proc.returncode, proc.stdout.decode("utf-8", "replace"), err


def operations(stderr):
    return re.findall(r'^TBERR .*? operation="([^"]*)"', stderr, re.M)


def write(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


# ---------------------------------------------------------------- autowiz

def autowiz(tmp, index, *extra):
    write(os.path.join(tmp, "plrfiles", "index"), index)
    rc, out, err = run("autowiz", ["51", "wiz", "51", "imm"] + list(extra), tmp)
    return rc, read(os.path.join(tmp, "wiz")), read(os.path.join(tmp, "imm")), err


@test("autowiz")
def autowiz_lists_valid_records(tmp):
    """I2-01: get_line used sizeof(char *) and cut every record to 7 bytes."""
    rc, wiz, imm, err = autowiz(tmp, "1 alice 60 0 1700000000\n"
                                     "2 bob 52 0 1700000000\n~\n")
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check("Alice" in wiz and "Bob" in wiz, "names missing from wizlist:\n" + wiz)
    check(operations(err) == [], "unexpected diagnostics:\n" + err)


@test("autowiz")
def autowiz_reports_malformed_record(tmp):
    """I2-01: a rejected record is reported with its line, not skipped silently."""
    rc, wiz, imm, err = autowiz(tmp, "1 alice 60 0 1700000000\n"
                                     "2 bob notalevel 0 1700000000\n~\n")
    check(rc == 1, "rc=%d" % rc)
    check("Alice" in wiz, "valid record lost:\n" + wiz)
    check("index-record-parse" in operations(err), err)
    check("line=2 field=level id=2" in err, err)


# ------------------------------------------------------------- plrtoascii

def plr_record_size(tmp):
    """Learn sizeof(struct char_file_u_plrtoascii) from the size diagnostic."""
    write(os.path.join(tmp, "probe"), b"x")
    os.makedirs(os.path.join(tmp, "plrfiles"), exist_ok=True)
    rc, out, err = run("plrtoascii", ["probe"], tmp)
    m = re.search(r"record_size=(\d+)", err)
    check(rc == 1 and m, "no record size reported: rc=%d\n%s" % (rc, err))
    return int(m.group(1))


@test("plrtoascii")
def plrtoascii_rejects_partial_record(tmp):
    """I2-24: a trailing partial record ended in a silent, successful return."""
    size = plr_record_size(tmp)
    write(os.path.join(tmp, "players"), b"\0" * (size + 7))
    rc, out, err = run("plrtoascii", ["players"], tmp)
    check(rc == 1, "rc=%d" % rc)
    check("plrfile-size" in operations(err), err)
    check("writing:" not in out, "converted despite bad size:\n" + out)


def main(argv):
    for arg in argv[1:]:
        name, _, path = arg.partition("=")
        TOOLS[name] = path
    failures = 0
    ran = 0
    for tool, fn in TESTS:
        if tool not in TOOLS:
            print("SKIP %s (no %s binary)" % (fn.__name__, tool))
            continue
        ran += 1
        with tempfile.TemporaryDirectory() as tmp:
            try:
                fn(tmp)
                print("PASS %s" % fn.__name__)
            except Failure as exc:
                failures += 1
                print("FAIL %s: %s" % (fn.__name__, exc))
    print("%d tests, %d failures" % (ran, failures))
    return 1 if failures or ran == 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
