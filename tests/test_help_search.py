#!/usr/bin/env python3
"""End-to-end: "help <topic>" never reads past the end of help_table.

Boots the real server on a free port against a scratch copy of lib/ with an
empty player index, creates a character (the first one is the implementor),
and asks for help on a topic that sorts after every help keyword and on one
that exists. Requires an answer to both, a server that is still running, and
no sanitizer report in the server log.

Regression: search_help() started its binary search at top = top_of_helpt,
but top_of_helpt counts entries, so "help zzzz" (any player can type it)
read help_table[top_of_helpt], one past the last entry; "help ~~~~" sorts
after every stock keyword and reaches it. Under
ASan that is a heap-buffer-overflow; without it, a crash or a bogus match.

Environment: TB_TEST_SHUTDOWN_GRACE is how many seconds to wait for the
server to exit after SIGTERM (default 30). A server that is still running
then is killed and the test fails, since its shutdown (and any leak report
from a sanitizer or Valgrind) was lost. Freeing the world under Valgrind
took 14-28 s on a loaded machine, so raise it (e.g. 120) for such runs.

Usage: test_help_search.py <circle binary> <source lib dir>
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BOOT_TIMEOUT = 300
WAIT = 30
SANITIZER = re.compile(rb"ERROR: AddressSanitizer|runtime error:|LeakSanitizer")


def shutdown_grace():
    """TB_TEST_SHUTDOWN_GRACE in seconds; a bad value fails before boot."""
    text = os.environ.get("TB_TEST_SHUTDOWN_GRACE", "30")
    try:
        grace = float(text)
    except ValueError:
        grace = float("nan")
    if not 0 < grace < float("inf"):
        sys.exit(f"FAIL: TB_TEST_SHUTDOWN_GRACE={text!r} is not a positive number of seconds")
    return grace


def stop_server(server, grace):
    """SIGTERM, wait up to grace seconds, then kill. None, or why it failed."""
    server.terminate()
    try:
        server.wait(timeout=grace)
        return None
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
        return (f"FAIL: server still running {grace:g}s after SIGTERM; killed, so its "
                f"shutdown and leak report were lost (raise TB_TEST_SHUTDOWN_GRACE)")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Client:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(0.5)
        self.buf = b""

    def expect(self, pattern):
        deadline = time.time() + WAIT
        rx = re.compile(pattern.encode(), re.I)
        while not rx.search(self.buf):
            if time.time() > deadline:
                sys.exit(f"FAIL: timed out waiting for {pattern!r}; tail {self.buf[-200:]!r}")
            try:
                data = self.s.recv(65536)
            except socket.timeout:
                continue
            if not data:
                sys.exit(f"FAIL: connection closed waiting for {pattern!r}")
            # Refuse telnet option offers; strip negotiation from the text.
            for m in re.finditer(rb"\xff([\xfb\xfd])(.)", data, re.S):
                self.s.sendall(b"\xff" + (b"\xfc" if m.group(1) == b"\xfd" else b"\xfe") + m.group(2))
            self.buf += re.sub(rb"\xff[\xfb-\xfe].|\xff.", b"", data)
        seen, self.buf = self.buf, b""
        return seen

    def send(self, line, expect):
        self.s.sendall(line.encode() + b"\r\n")
        return self.expect(expect)


def main():
    circle, src_lib = sys.argv[1], sys.argv[2]
    grace = shutdown_grace()
    work = tempfile.mkdtemp(prefix="tb-help-")
    lib = os.path.join(work, "lib")
    shutil.copytree(src_lib, lib)
    for sub in ("plrfiles", "plrobjs"):
        for root, _, files in os.walk(os.path.join(lib, sub)):
            for f in files:
                if f != "index":
                    os.remove(os.path.join(root, f))
    with open(os.path.join(lib, "plrfiles", "index"), "w") as f:
        f.write("~\n")

    port = free_port()
    log_path = os.path.join(work, "server.log")
    with open(log_path, "wb") as log:
        server = subprocess.Popen([circle, "-q", "-d", lib, str(port)],
                                  stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + BOOT_TIMEOUT
        while b"Entering game loop" not in open(log_path, "rb").read():
            if server.poll() is not None or time.time() > deadline:
                sys.exit(f"FAIL: server did not boot; see {log_path}")
            time.sleep(0.5)

        c = Client(port)
        c.expect(r"by what name")
        c.send("Helpo", r"did i get that right")
        c.send("y", r"password")
        c.send("secret12", r"retype")
        c.send("secret12", r"sex")
        c.send("m", r"class:")
        c.send("w", r"press return")
        c.send("", r"make your choice")
        c.send("1", r">")

        # Topics that sort after every help keyword must be refused outright.
        # The stock help file's last keywords are "||" and "~", so "~~~~" is
        # the one that drives the binary search to the end of the table.
        # Before the fix the search then read help_table[top_of_helpt], one
        # past the last entry: past the heap block, or a spare zeroed slot
        # whose NULL keywords strn_cmp() counts as a match.
        for topic in ("zzzzzzzz", "~~~~"):
            out = c.send("help " + topic, r">")
            if server.poll() is not None:
                sys.exit(f"FAIL: server died on 'help {topic}'")
            if b"no help on that word" not in out.lower():
                sys.exit(f"FAIL: 'help {topic}' was not refused; got {out[-200:]!r}")
        out = c.send("help score", r">")
        if b"no help on that word" in out.lower():
            sys.exit("FAIL: 'help score' found nothing; the search lost a real entry")
        if server.poll() is not None:
            sys.exit("FAIL: server died on 'help score'")
        print("PASS: help past the last keyword is refused, real topics found")
    finally:
        stopped = stop_server(server, grace)
        if stopped:
            print(stopped, file=sys.stderr)
        report = open(log_path, "rb").read()
        if SANITIZER.search(report):
            m = SANITIZER.search(report)
            print(report[max(0, m.start() - 200):m.start() + 1500].decode("latin-1"))
            shutil.rmtree(work, ignore_errors=True)
            sys.exit("FAIL: sanitizer report in the server log")
        shutil.rmtree(work, ignore_errors=True)
    if stopped:
        sys.exit(1)


if __name__ == "__main__":
    main()
