#!/usr/bin/env python3
"""End-to-end login smoke test: a new connection must reach the name prompt.

Boots the real server on a free port against a scratch copy of lib/, connects
like a client that refuses every telnet option, and requires the full
greeting, ending in "By what name do you wish to be known?", to arrive.

Regression: file_to_string() bounded its copy with sizeof() of a pointer
parameter, so every text file (greetings, motd, news, ...) was loaded as its
first 7 characters. The greeting lost its name prompt and every client sat
at a blank screen until the 30-second idle timeout.

The server must also stop cleanly: the test fails if it exits before the
test stops it, exits non-zero after SIGTERM, or leaves an AddressSanitizer,
LeakSanitizer or UndefinedBehaviorSanitizer report in its log, so a defect
after the last check (or in shutdown) cannot pass unnoticed.

Environment: TB_TEST_SHUTDOWN_GRACE is how many seconds to wait for the
server to exit after SIGTERM (default 30). A server that is still running
then is killed and the test fails, since its shutdown (and any leak report
from a sanitizer or Valgrind) was lost. Freeing the world under Valgrind
took 14-28 s on a loaded machine, so raise it (e.g. 120) for such runs.

Usage: test_login_greeting.py <circle binary> <source lib dir>
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BOOT_TIMEOUT = 300   # generous: the binary may be sanitizer-instrumented
PROMPT_TIMEOUT = 20
# A report from any sanitizer the server may be built with (ASan, LSan, UBSan).
SANITIZER = re.compile(rb"ERROR: AddressSanitizer|ERROR: LeakSanitizer|"
                       rb"UndefinedBehaviorSanitizer|runtime error:")


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
    """Stop the server; return why its run was abnormal ([] if it was not).

    Abnormal: it exited before the test stopped it, it was still running
    grace seconds after SIGTERM (it is then killed), or it exited non-zero.
    """
    if server.poll() is not None:
        return [f"server exited by itself (rc={server.returncode}) before the test stopped it"]
    server.terminate()
    try:
        rc = server.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
        return [f"server still running {grace:g}s after SIGTERM; killed, so its "
                f"shutdown and leak report were lost (raise TB_TEST_SHUTDOWN_GRACE)"]
    return [] if rc == 0 else [f"server exited with rc={rc} after SIGTERM (expected 0)"]


def server_problems(server, grace, log_path):
    """stop_server()'s findings plus a sanitizer report in the server log.
    Each is printed to stderr (a report with its context) and returned."""
    problems = stop_server(server, grace)
    log = open(log_path, "rb").read()
    m = SANITIZER.search(log)
    if m:
        print(log[max(0, m.start() - 200):m.start() + 1500].decode("latin-1"), file=sys.stderr)
        problems.append("sanitizer report in the server log")
    for problem in problems:
        print("FAIL: " + problem, file=sys.stderr)
    return problems


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    circle, src_lib = sys.argv[1], sys.argv[2]
    grace = shutdown_grace()
    greeting = open(os.path.join(src_lib, "text", "greetings"), "rb").read()
    expected = greeting.strip().splitlines()[-1].strip()   # the name prompt

    work = tempfile.mkdtemp(prefix="tb-login-")
    lib = os.path.join(work, "lib")
    shutil.copytree(src_lib, lib)
    port = free_port()
    log_path = os.path.join(work, "server.log")
    with open(log_path, "wb") as log:
        server = subprocess.Popen([circle, "-q", "-d", lib, str(port)],
                                  stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + BOOT_TIMEOUT
        while b"Entering game loop" not in open(log_path, "rb").read():
            if server.poll() is not None:
                sys.exit(f"FAIL: server exited during boot (rc={server.returncode}); see {log_path}")
            if time.time() > deadline:
                sys.exit(f"FAIL: server did not finish booting in {BOOT_TIMEOUT}s")
            time.sleep(0.5)

        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.settimeout(0.5)
        received = b""
        answered = set()
        deadline = time.time() + PROMPT_TIMEOUT
        while expected not in received and time.time() < deadline:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            received += data
            # Refuse every telnet option offer, as a dumb client would.
            for m in re.finditer(rb"\xff([\xfb\xfd])(.)", data, re.S):
                if m.group(0) not in answered:
                    answered.add(m.group(0))
                    sock.sendall(b"\xff" + (b"\xfc" if m.group(1) == b"\xfd" else b"\xfe") + m.group(2))
        sock.close()

        if expected not in received:
            sys.exit(f"FAIL: no name prompt {expected!r} after {len(received)} bytes; "
                     f"tail {received[-120:]!r}")
        print(f"PASS: name prompt received ({len(received)} bytes)")
    finally:
        problems = server_problems(server, grace, log_path)
        shutil.rmtree(work, ignore_errors=True)
    if problems:
        sys.exit(1)


if __name__ == "__main__":
    main()
