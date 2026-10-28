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


@test("autowiz")
def autowiz_high_flag_letter_is_defined(tmp):
    """I2-08: flag 'Z' was `1 << 51` on int (UBSan: shift exponent 51)."""
    rc, wiz, imm, err = autowiz(tmp, "1 alice 60 Z 1700000000\n~\n")
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check("Alice" in wiz, "record with flag Z lost:\n" + wiz)


@test("autowiz")
def autowiz_reports_level_out_of_range(tmp):
    """I2-07: level 150 became a negative byte; the bucket walk hit NULL."""
    rc, wiz, imm, err = autowiz(tmp, "1 alice 60 0 1700000000\n"
                                     "4 dave 150 0 1700000000\n"
                                     "5 abcdefghijklmnopqrst 55 0 1700000000\n"
                                     "~\n")
    check(rc == 1, "rc=%d err=%s" % (rc, err))
    check("index-level-range" in operations(err), err)
    check("Alice" in wiz and "Dave" not in wiz, wiz)
    check("Abcdefghijklmnopqrst" in wiz, "20-letter name rejected:\n" + wiz)


@test("autowiz")
def autowiz_reports_non_letter_name(tmp):
    """I2-07: add_name() dropped a non-letter name with a bare return."""
    rc, wiz, imm, err = autowiz(tmp, "1 al1ce 60 0 1700000000\n~\n")
    check(rc == 1, "rc=%d" % rc)
    check("field=name-chars" in err, err)


@test("autowiz")
def autowiz_line_ending_edge_cases(tmp):
    """I2-09: a NUL-first line wrote temp[-1]; a last line lost a byte."""
    rc, wiz, imm, err = autowiz(tmp, b"\0abc\n"
                                     b"2 bob 52 0 1700000000\n"
                                     b"3 " + b"x" * 300 + b" 52 0 1\n"
                                     b"1 alice 60 0 1700000005")
    check(rc == 1, "rc=%d err=%s" % (rc, err))
    check("index-line-too-long" in operations(err), err)
    check("Alice" in wiz and "Bob" in wiz, "records lost:\n" + wiz)


# ------------------------------------------------------- rebuildMailIndex

def mail_index(tmp, files):
    for name, text in files.items():
        write(os.path.join(tmp, "mail", name), text)
    rc, out, err = run("rebuildMailIndex", [os.path.join(tmp, "index")],
                       os.path.join(tmp, "mail"))
    return rc, read(os.path.join(tmp, "index")), out, err


def mail_file(flag_line):
    return ("MlID: 7\nSend: 1\nReci: 2\nSent: 1700000000\nSubj: hi\n"
            "Flag: %s\n" % flag_line)


@test("rebuildMailIndex")
def mail_index_closes_directories(tmp):
    """I2-25: walkdir never called closedir (LSan: DIR leaked per level)."""
    rc, index, out, err = mail_index(tmp, {"a/x.ml": mail_file("0 0 0 0")})
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check(re.search(r"^7 0 1 2 1700000000 +hi$", index, re.M),
          "record missing:\n" + index)


@test("rebuildMailIndex")
def mail_index_skips_symlink_loop(tmp):
    """I2-25: a directory symlink was followed through stat()."""
    os.makedirs(os.path.join(tmp, "mail", "a"))
    os.symlink("..", os.path.join(tmp, "mail", "a", "loop"))
    rc, index, out, err = mail_index(tmp, {"a/x.ml": mail_file("0 0 0 0")})
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check("mail-symlink-skipped" in operations(err), err)
    check(index.count("\n7 ") == 1, "record indexed more than once:\n" + index)


@test("rebuildMailIndex")
def mail_flag_bit31_is_defined(tmp):
    """I2-08: flag 'F' (bit 31) was `1 << 31` on int in the flag helpers."""
    rc, index, out, err = mail_index(tmp, {"x.ml": mail_file("F 0 0 0")})
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check(re.search(r"^7 0 1 2 1700000000 +hi$", index, re.M),
          "record missing:\n" + index)


@test("rebuildMailIndex")
def mail_flag_line_overflow_is_reported(tmp):
    """I2-04: sscanf("%s") wrote a 100-byte word into char[33]."""
    rc, index, out, err = mail_index(tmp, {
        "x.ml": mail_file("a" * 100 + " b c d"),
        "y.ml": mail_file("0 0 0 0").replace("MlID: 7", "MlID: 8"),
    })
    check(rc == 1, "rc=%d" % rc)
    check("mail-flag-parse" in operations(err), err)
    check("\n7 " not in index and "\n8 " in index, index)


# ----------------------------------------------------------- rebuildIndex

def rebuild_index(tmp, pfiles):
    root = os.path.join(tmp, "plrfiles")
    for name, text in pfiles.items():
        write(os.path.join(root, name), text)
    os.makedirs(root, exist_ok=True)
    rc, out, err = run("rebuildIndex", [os.path.join(tmp, "index")], root)
    lines = [l for l in read(os.path.join(tmp, "index")).splitlines()
             if l != "~"]
    return rc, lines, err


def pfile(*tags):
    return "Name: X\n" + "".join(t + "\n" for t in tags)


@test("rebuildIndex")
def rebuild_index_server_format(tmp):
    """I2-05/I2-V02: 6 fields with level clamped to 30; server wants 5."""
    rc, lines, err = rebuild_index(tmp, {
        "A-E/alice.plr": pfile("Levl: 60", "Id  : 1", "Last: 1700000000"),
        "U-Z/zed.plr": pfile("Levl: 3", "Id  : 2", "Last: 1700000001",
                             "Act : k 0 0 0"),
    })
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check(sorted(lines) == ["1 alice 60 0 1700000000",
                            "2 zed 3 a 1700000001"],
          "index is not 'id name level flags last':\n%r" % lines)


@test("rebuildIndex")
def rebuild_index_reports_missing_tag(tmp):
    """I2-05: a pfile without Id was indexed as idnum 0, silently."""
    rc, lines, err = rebuild_index(tmp, {
        "A-E/alice.plr": pfile("Levl: 60", "Last: 1700000000"),
        "A-E/bob.plr": pfile("Id  : 2", "Last: 1700000000"),
    })
    check(rc == 1, "rc=%d" % rc)
    check("pfile-tag-missing" in operations(err), err)
    check(lines == ["2 bob 0 0 1700000000"], "%r" % lines)


@test("rebuildIndex")
def rebuild_index_reports_level_range(tmp):
    """I2-V02: an out-of-range level was clamped to 30 without a report."""
    rc, lines, err = rebuild_index(tmp, {
        "A-E/alice.plr": pfile("Levl: 99", "Id  : 1", "Last: 1700000000"),
    })
    check(rc == 1, "rc=%d" % rc)
    check("pfile-level-range" in operations(err), err)
    check(lines == [], "%r" % lines)


@test("rebuildIndex")
def rebuild_index_ignores_tags_in_description(tmp):
    """I2-06: a Desc line 'Levl: 60' / 'Id  : 1' forged level and idnum."""
    rc, lines, err = rebuild_index(tmp, {
        "K-O/mal.plr": "Name: Mal\nDesc:\nLevl: 60\nId  : 1\nnice~\n"
                       "Levl: 3\nId  : 77\nLast: 1700000000\n"
                       "Alis: 1\n x\nLast: 5\n0\n",
    })
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    check(lines == ["77 mal 3 0 1700000000"], "%r" % lines)


@test("rebuildIndex")
def rebuild_index_reports_duplicate_tag(tmp):
    """I2-06: a second record-level Id was silently ignored."""
    rc, lines, err = rebuild_index(tmp, {
        "K-O/mal.plr": pfile("Id  : 77", "Id  : 1", "Last: 1700000000"),
    })
    check(rc == 1, "rc=%d" % rc)
    check("pfile-duplicate-tag" in operations(err), err)
    check(lines == [], "%r" % lines)


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


def plr_record(size, name):
    """A zero-filled binary record whose first bytes are the name field."""
    name = name.encode() if isinstance(name, str) else name
    return name + b"\0" * (size - len(name))


def plrtoascii(tmp, *records):
    for d in ("A-E", "F-J", "K-O", "P-T", "U-Z", "ZZZ"):
        os.makedirs(os.path.join(tmp, "plrfiles", d), exist_ok=True)
    write(os.path.join(tmp, "players"), b"".join(records))
    return run("plrtoascii", ["players"], tmp)


@test("plrtoascii")
def plrtoascii_rejects_unterminated_name(tmp):
    """I2-02: an unterminated name overflowed outname[40] on the stack."""
    size = plr_record_size(tmp)
    rc, out, err = plrtoascii(tmp, plr_record(size, "a" * 60),
                              plr_record(size, "alice"))
    check(rc == 1, "rc=%d err=%s" % (rc, err))
    check("plr-name" in operations(err), err)
    check("record=1" in err, err)
    check(os.path.exists(os.path.join(tmp, "plrfiles", "A-E", "alice.plr")),
          "valid record after the bad one was not converted:\n" + out)


@test("plrtoascii")
def plrtoascii_rejects_path_in_name(tmp):
    """I2-02: '../x' was used as a path component (plrfiles/ZZZ/../x.plr)."""
    size = plr_record_size(tmp)
    rc, out, err = plrtoascii(tmp, plr_record(size, "../evil"))
    check(rc == 1, "rc=%d" % rc)
    check("plr-name" in operations(err), err)
    check(not os.path.exists(os.path.join(tmp, "plrfiles", "evil.plr")),
          "path from the name was written")


@test("plrtoascii")
def plrtoascii_index_names_the_player(tmp):
    """I2-03: the index line printed an uninitialised buffer as the name."""
    size = plr_record_size(tmp)
    rc, out, err = plrtoascii(tmp, plr_record(size, "Alice"),
                              plr_record(size, "Bob"))
    check(rc == 0, "rc=%d err=%s" % (rc, err))
    index = read(os.path.join(tmp, "plrfiles", "index")).splitlines()
    check(index == ["0 alice 0 0 0", "0 bob 0 0 0"],
          "index is not 'id name level flags last':\n%r" % index)


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
