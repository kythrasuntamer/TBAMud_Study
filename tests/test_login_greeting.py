#!/usr/bin/env python3
"""End-to-end login smoke test: a new connection must reach the name prompt.

Boots the real server on a free port against a scratch copy of lib/, connects
like a client that refuses every telnet option, and requires the full
greeting, ending in "By what name do you wish to be known?", to arrive.

Regression: file_to_string() bounded its copy with sizeof() of a pointer
parameter, so every text file (greetings, motd, news, ...) was loaded as its
first 7 characters. The greeting lost its name prompt and every client sat
at a blank screen until the 30-second idle timeout.

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
        stopped = stop_server(server, grace)
        if stopped:
            print(stopped, file=sys.stderr)
        shutil.rmtree(work, ignore_errors=True)
    if stopped:
        sys.exit(1)


if __name__ == "__main__":
    main()
