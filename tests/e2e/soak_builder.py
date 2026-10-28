#!/usr/bin/env python3
"""End-to-end builder/immortal soak test for the live TBAMud server.

================================ README ================================

WHAT THIS DOES
  Boots the real `circle` server on a free port against a throwaway copy of
  lib/ with an empty player index, then drives it over telnet as a set of
  immortals and builders.  The FIRST character created on an empty index
  becomes the implementor (LVL_IMPL); this harness then creates two builders
  (advanced to LVL_BUILDER and granted per-zone OLC with `set <name> olc
  <zone>`) and two ordinary players.  With several builders connected at once
  it runs a deterministic COVERAGE PASS once (so every path below is hit in
  every seed), then a seeded-random interleaving of the same scenarios for the
  requested duration.  The code paths, all historically prone to lifetime bugs
  in CircleMUD/OasisOLC and the DG scripting engine:

    * redit / oedit / medit / zedit / sedit / qedit / hedit / trigedit:
      create, copy ('W'), change fields, add extra descriptions, delete with
      each editor's own delete command, save-and-quit, and abort-quit.
    * Two builders editing the SAME zone while the other holds a session
      open: one inserts/deletes rooms and zone-reset commands (shifting rnums)
      while the other has an editor buffer open, then `olc save` / zone resets.
    * DG triggers: attach/detach to mobs, objects and rooms; triggers authored
      through trigedit that use wait, mpurge, mforce, mteleport, %load%,
      mdamage and remote variables; fired as greet / speech / command / timer
      while the actor is extracted or the room/object is purged mid-script.
    * Immortal commands: set / stat / purge / load / force / switch / return /
      snoop / at / goto / transfer / restore / zreset / vnum / show / wizhelp /
      last.

  KNOWN crashing bugs are kept behind --known-bugs and are OFF by default, so
  that the rest of the soak is not masked by their guaranteed crash:
      g1-01: `set file <player> room <vnum>` then `goto <vnum>`   (UAF)
      g1-05: `force <linkdead player> return`              (NULL ch->desc)
             -- both fixed on audit-fix-G1, not on this base;
      shop-keeper: a keeperless shop (S_KEEPER == NOBODY) is renumbered by the
             next mob-prototype delete and save_shops() then reads
             mob_index[65534] (SEGV at genshp.c:410) -- found by this soak,
             fixed by 8cb7eba / 7b3d7e9, not on this base.  By default every
             shop the soak creates is given a keeper so this path is avoided;
             tests/e2e/repro_olc_bugs.py shop-keeper-nobody reproduces it.

HOW FAILURE IS DETECTED
  The run FAILS (non-zero exit) if:
    * the server process dies before we send `shutdown die`, or
    * any AddressSanitizer / UBSan / LeakSanitizer report appears (in the
      ASAN_OPTIONS/UBSAN_OPTIONS/LSAN_OPTIONS log files or inline in the
      server log), or
    * (Valgrind mode) Valgrind exits 99 (its --error-exitcode) or its log
      shows an invalid access, a "Process terminating", or a definite/indirect
      leak.  (Only genuine error records and NON-ZERO counts count; the
      server forks autowiz, so several PIDs print summaries.)
  A clean run ends with the server logging "Done." and exit 0.  LeakSanitizer
  reports are also broken down by allocation site and labelled against the
  leak families already root-caused on this base (see repro_olc_bugs.py and
  KNOWN_LEAK_SITES below); a site not in that table prints as "NEW".

KNOWN HARNESS GAPS (input is batched and replies are not parsed)
  * coverage_pass step 4: a zedit command at list position > 0 asks "Is this
    command dependent on the success of the previous one? (y/n)"; the batch
    does not answer it, so that zedit session desyncs and swallows step 5's
    `oedit X` deletes.  The deleted-object-prototype leak (F1-V02) is
    therefore covered by repro_olc_bugs.py oedit-new-delete, not by the soak.
    Fix: answer "y"/"n" after the G/O command letters in step 4.
  * sc_concurrent_rooms: the mover is a level-1 mortal, so its `goto` fails
    and the deleted room holds only the builder and an object.  Fix: have the
    implementor `transfer` the mover in.

USAGE
  soak_builder.py <circle-binary> <source-lib-dir> [options]
    --seed N        seed the scenario RNG (default 1); runs are reproducible.
    --minutes F     soak duration in minutes of wall-clock driving (default 1).
    --valgrind      the binary is being run under Valgrind: use long timeouts
                    and, after shutdown, scan the Valgrind log named by
                    --vg-log.  (This script does NOT launch Valgrind itself;
                    the caller wraps the binary.  When this flag is set the
                    FIRST positional argument is still the wrapper-launched
                    circle, i.e. the caller passes the whole `valgrind ...
                    circle` line via --vg-cmd, see below.)
    --vg-cmd CMD    full server command line as a shell string (overrides the
                    default `<binary> -q -d <lib> <port>`); used to run the
                    server under Valgrind.  {lib} and {port} are substituted.
    --vg-log PATH   Valgrind --log-file path to scan after shutdown.
    --known-bugs [g1-01,g1-05,shop-keeper]
                    enable known crashing paths (bare flag: g1-01,g1-05, run
                    after the soak; shop-keeper: soak-created shops stay
                    keeperless, crashing the first later shop save).  They are
                    expected to crash this base; the run then reports status
                    known_bugs_reproduced and exits 0.  G1-01 usually crashes
                    first, so pass `g1-05` alone to see G1-05.
    --work DIR      scratch directory root (default: a fresh mkdtemp).
    --keep          do not delete the scratch directory on exit.

EXIT CODES
  0  clean soak, server shut down normally, no sanitizer/Valgrind errors.
  1  a memory error / server death / sanitizer or Valgrind report was found.
  2  harness/setup error (could not boot, could not log in, etc.).

Stdlib only.  Designed to run several copies concurrently on one machine
(each picks its own free port and its own scratch lib).
========================================================================
"""

import argparse
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


# ----------------------------------------------------------------------------
# Low-level helpers
# ----------------------------------------------------------------------------

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


IAC_RE = re.compile(rb"\xff\xfa.*?\xff\xf0|\xff[\xfb-\xfe].|\xff.", re.S)


class Server:
    """A booted circle server against a scratch copy of lib/."""

    def __init__(self, binary, src_lib, work, port, valgrind=False,
                 vg_cmd=None, vg_log=None):
        self.binary = binary
        self.port = port
        self.valgrind = valgrind
        self.work = work
        self.lib = os.path.join(work, "lib")
        self.log_path = os.path.join(work, "server.log")
        self.asan_prefix = os.path.join(work, "asan")   # ASAN_OPTIONS log_path
        self.ubsan_prefix = os.path.join(work, "ubsan")
        self.lsan_prefix = os.path.join(work, "lsan")
        self.vg_log = vg_log or os.path.join(work, "valgrind.log")
        self.vg_cmd = vg_cmd
        self.proc = None

        shutil.copytree(src_lib, self.lib)
        # Empty player index so the first character created becomes the IMP.
        for sub in ("plrfiles", "plrobjs"):
            d = os.path.join(self.lib, sub)
            for root, _dirs, files in os.walk(d):
                for f in files:
                    if f != "index":
                        try:
                            os.unlink(os.path.join(root, f))
                        except OSError:
                            pass
        with open(os.path.join(self.lib, "plrfiles", "index"), "w") as f:
            f.write("~\n")

    def boot(self, boot_timeout):
        env = dict(os.environ)
        # Fail loudly on the first sanitizer error and write reports to files
        # we can scan.  detect_leaks is on; abort_on_error makes ASan crash the
        # process (so the server dies -> we detect it) in addition to logging.
        env["ASAN_OPTIONS"] = (
            "abort_on_error=1:halt_on_error=1:detect_leaks=1:"
            "detect_stack_use_after_return=1:strict_string_checks=1:"
            f"log_path={self.asan_prefix}")
        env["UBSAN_OPTIONS"] = (
            "print_stacktrace=1:halt_on_error=1:"
            f"log_path={self.ubsan_prefix}")
        env["LSAN_OPTIONS"] = f"log_path={self.lsan_prefix}"

        if self.vg_cmd:
            cmd = shlex.split(self.vg_cmd.format(lib=self.lib, port=self.port,
                                                 binary=self.binary,
                                                 vg_log=self.vg_log))
        else:
            cmd = [self.binary, "-q", "-d", self.lib, str(self.port)]

        log(f"booting: {' '.join(cmd)}")
        with open(self.log_path, "wb") as lf:
            self.proc = subprocess.Popen(cmd, stdout=lf,
                                         stderr=subprocess.STDOUT,
                                         start_new_session=True)
        deadline = time.time() + boot_timeout
        while True:
            try:
                data = open(self.log_path, "rb").read()
            except OSError:
                data = b""
            if b"Entering game loop" in data:
                log(f"server booted (pid {self.proc.pid})")
                return True
            if self.proc.poll() is not None:
                log(f"server EXITED during boot rc={self.proc.returncode}")
                return False
            if time.time() > deadline:
                log(f"server did not boot within {boot_timeout}s")
                return False
            time.sleep(0.5)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait_exit(self, timeout):
        try:
            self.proc.wait(timeout=timeout)
            return self.proc.returncode
        except subprocess.TimeoutExpired:
            return None

    def stop(self):
        """Stop by PID only (never pkill)."""
        if self.proc is None:
            return
        if self.proc.poll() is None:
            try:
                self.proc.terminate()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                try:
                    self.proc.kill()
                    self.proc.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    pass


class Conn:
    """A telnet client that refuses negotiation and buffers latin-1 text."""

    def __init__(self, who, port, wait):
        self.who = who
        self.wait = wait
        deadline = time.time() + wait
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port),
                                                  timeout=10)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"{who}: server never accepted a connection")
                time.sleep(1)
        self.s.settimeout(1.0)
        self.buf = ""
        self.closed = False

    def _read_once(self):
        try:
            data = self.s.recv(65536)
        except socket.timeout:
            return True
        except OSError:
            return False
        if not data:
            return False
        data = IAC_RE.sub(b"", data)
        self.buf += data.decode("latin-1", "replace")
        return True

    def expect(self, pattern):
        deadline = time.time() + self.wait
        while not re.search(pattern, self.buf, re.I | re.S):
            if not self._read_once():
                self.closed = True
                raise SystemExit(
                    f"{self.who}: connection closed waiting for {pattern!r}; "
                    f"tail={self.buf[-200:]!r}")
            if time.time() > deadline:
                raise SystemExit(
                    f"{self.who}: timeout waiting for {pattern!r}; "
                    f"tail={self.buf[-200:]!r}")
        self.buf = ""

    def send_many(self, lines):
        """Queue several input lines in one write.  The server keeps a FIFO
        per descriptor (bounded at 64 pending lines) and runs one line per
        0.1s pulse, so they are processed in order."""
        data = b"".join(l.encode("latin-1", "replace") + b"\r\n" for l in lines)
        try:
            self.s.sendall(data)
        except OSError as e:
            raise SystemExit(f"{self.who}: send failed ({e})")

    def send(self, line, expect=None):
        try:
            self.s.sendall(line.encode("latin-1", "replace") + b"\r\n")
        except OSError as e:
            raise SystemExit(f"{self.who}: send failed ({e})")
        if expect:
            self.expect(expect)

    def drain(self, quiet, cap):
        """Read until no data arrives for `quiet` seconds or `cap` elapses.

        Robust across the many editor menus that do not end in a normal '> '
        prompt.  Returns the text read.
        """
        got = ""
        start = time.time()
        last = time.time()
        self.buf = ""
        while True:
            if self._read_once() and self.buf:
                got += self.buf
                self.buf = ""
                last = time.time()
            now = time.time()
            if now - last >= quiet:
                break
            if now - start >= cap:
                break
        return got

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass
        self.closed = True


# ----------------------------------------------------------------------------
# Character creation / login (nanny flow)
# ----------------------------------------------------------------------------

PW = "secret12"


def create_char(port, name, wait, sex="m", cls="w"):
    c = Conn(name, port, wait)
    c.expect(r"by what name")
    c.send(name, r"did i get that right")
    c.send("y", r"password")
    c.send(PW, r"retype")
    c.send(PW, r"sex")
    c.send(sex, r"class")
    c.send(cls, r"press return")
    c.send("", r"make your choice")
    c.send("1", r">")
    return c


def login_char(port, name, wait):
    c = Conn(name, port, wait)
    c.expect(r"by what name")
    c.send(name, r"password")
    c.send(PW, r"(press return|make your choice|reconnect|>)")
    # Nudge through MOTD / menu to playing state.
    c.send("", r"(make your choice|>)")
    if "make your choice" in c.buf.lower() or True:
        c.send("1", r">")
    return c


# ----------------------------------------------------------------------------
# Soak context + scenario actions
# ----------------------------------------------------------------------------

# Building target zone 30 (Northern Midgaard, vnums 3000-3099): has existing
# rooms/mobs/objs/trigs/shops to copy and delete, plus free vnums to create in.
BUILD_ZONE_VNUM = 30
FREE_ROOM = [3070, 3071, 3072, 3073, 3074, 3075, 3076, 3077]
FREE_MOB = [3014, 3015, 3016, 3017, 3018, 3019, 3047, 3048]
FREE_OBJ = [3007, 3008, 3016, 3017, 3018, 3019, 3026, 3027]
FREE_TRIG = [3030, 3031, 3032, 3033, 3034, 3035, 3036, 3037]
FREE_SHOP = [3010, 3011, 3012, 3013]
FREE_QUEST = [3050, 3051, 3052, 3053]
SRC_ROOM = 3001       # exists
SRC_MOB = 3060        # a Midgaard citizen
SRC_OBJ = 3000
SRC_SHOP = 3000
BUILD_ROOM = 3001     # everyone co-locates here for DG trigger firing


class Ctx:
    def __init__(self, server, rng, quiet, cap):
        self.server = server
        self.port = server.port
        self.rng = rng
        self.quiet = quiet
        self.cap = cap
        self.slow = max(3.0, quiet * 8)   # log any single step slower than this
        self.conns = {}          # name -> Conn
        self.actions = 0

    def act(self, name, line):
        """Send one line on a connection and drain the reply.

        Tolerant of a peer that closed the socket (e.g. a player extracted or
        disconnected by a trigger): mark the connection closed and continue,
        so one dead connection does not abort the whole soak.  A dead SERVER
        is detected separately by the caller via server.alive().
        """
        c = self.conns[name]
        if c.closed:
            return ""
        try:
            c.send(line)
        except SystemExit:
            c.closed = True
            return ""
        self.actions += 1
        t0 = time.time()
        try:
            out = c.drain(self.quiet, self.cap)
        except SystemExit:
            c.closed = True
            return ""
        took = time.time() - t0
        if took > self.slow:
            # Name the slow step so stalls are diagnosable from the run log.
            log(f"slow act: {name} {line!r} took {took:.1f}s "
                f"({len(out)} bytes); tail={out[-120:]!r}")
        return out

    def menu(self, name, lines):
        """Walk ONE connection through a sequence of input lines at once
        (an editor session), then drain.  Much faster than one act() per line
        and still ordered: the server runs a descriptor's queued lines in
        FIFO order, and callers drain this connection before another one
        acts, so cross-connection ordering is preserved."""
        lines = list(lines)
        if not lines:
            return ""
        if len(lines) > 60:                 # stay under the 64-line queue cap
            return (self.menu(name, lines[:60]) + self.menu(name, lines[60:]))
        c = self.conns[name]
        if c.closed:
            return ""
        try:
            c.send_many(lines)
        except SystemExit:
            c.closed = True
            return ""
        self.actions += len(lines)
        t0 = time.time()
        floor = 0.12 * len(lines)           # >= one 0.1s pulse per line
        out = ""
        try:
            out = c.drain(self.quiet, self.cap)
            while time.time() - t0 < floor:
                out += c.drain(self.quiet, self.cap)
        except SystemExit:
            c.closed = True
            return out
        took = time.time() - t0
        if took > self.slow + floor:
            log(f"slow batch: {name} {lines[:3]!r}... ({len(lines)} lines) "
                f"took {took:.1f}s; tail={out[-120:]!r}")
        return out


# --- individual editor scenarios --------------------------------------------

# Existing entities that carry PROTOTYPE triggers ("T <vnum>" lines in the
# world files).  Opening one copies its trigger list into the editor
# (dg_olc_script_copy), which is the ownership path behind the OLC_SCRIPT
# leak; plain sources like SRC_OBJ/SRC_MOB have no triggers and miss it.
TRIGGERED = {"redit": 3030,   # room 3030: T 3004 (never deleted here)
             "oedit": 3006,   # obj 3006: T 3014, T 3015
             "medit": 3012}   # mob 3012: T 3016

# Original rooms (no triggers) that sc_concurrent_rooms deletes one at a time,
# each after a player and an object have been moved in, so delete_room has
# people and contents to evacuate.  Room 3001 (start room) is never deleted.
DELETABLE_ROOMS = [3040, 3041, 3042, 3043, 3044, 3045, 3046, 3047, 3048, 3049]

# Commands such as purge / switch / stat resolve NAMES, not vnums.
SRC_MOB_KW = "cityguard"      # keyword of mob 3060


def sc_peek_quit(ctx, who):
    """Open an existing triggered room/object/mob and quit WITHOUT changes --
    the commonest builder action ("look at it in the editor")."""
    editor = ctx.rng.choice(sorted(TRIGGERED))
    ctx.act(who, f"{editor} {TRIGGERED[editor]}")
    ctx.act(who, "q")


# Each editor session below is one connection's input sequence, so it is sent
# as a batch with ctx.menu() (ordered per descriptor, much faster than one
# round trip per line).  Random choices are made up front by the harness;
# nothing branches on the server's replies.

def sc_redit(ctx, who):
    rng = ctx.rng
    vnum = rng.choice(FREE_ROOM)
    lines = [f"redit {vnum}",
             "1", f"Soak room {vnum} {ctx.actions}",   # name
             "3", "1", "0",                            # toggle a room flag
             "4", "1",                                 # sector
             "f", "1", f"plaque{vnum}",                # extra desc keyword
             "2", "An extra description for the soak.", "/s",
             "0"]                                      # leave extradesc menu
    if rng.random() < 0.5:
        lines += ["w", str(SRC_ROOM)]       # copy another room over the buffer
    if rng.random() < 0.3:
        lines += ["x", "y"]                 # delete via the editor
    else:
        lines += ["q", "y"]                 # save
    ctx.menu(who, lines)


def sc_redit_abort(ctx, who):
    vnum = ctx.rng.choice(FREE_ROOM)
    ctx.menu(who, [f"redit {vnum}", "1", f"Doomed draft {ctx.actions}",
                   "q", "n"])               # abort: discard changes


def sc_oedit(ctx, who):
    rng = ctx.rng
    vnum = rng.choice(FREE_OBJ)
    lines = [f"oedit {vnum}",
             "1", f"soak obj{vnum}",                   # keywords
             "2", f"a soak object {vnum}",             # short desc
             "8", str(rng.randint(1, 50)),             # weight
             "e", "1", f"blade{vnum}",                 # extra desc
             "2", "Soak extra desc text.", "/s", "0"]
    if rng.random() < 0.5:                  # copy (sometimes a triggered obj)
        lines += ["w", str(rng.choice([SRC_OBJ, TRIGGERED["oedit"]]))]
    lines += ["x", "y"] if rng.random() < 0.3 else ["q", "y"]
    ctx.menu(who, lines)


def sc_medit(ctx, who):
    rng = ctx.rng
    vnum = rng.choice(FREE_MOB)
    lines = [f"medit {vnum}",
             "2", f"soakmob mob{vnum}",                # keywords
             "3", f"a soak mob {vnum}",                # short desc
             "5", f"A soak mob {vnum} stands here.", "/s",
             "1", str(rng.randint(0, 2))]              # sex
    if rng.random() < 0.5:                  # copy (sometimes a triggered mob)
        lines += ["w", str(rng.choice([SRC_MOB, TRIGGERED["medit"]]))]
    lines += ["x", "y"] if rng.random() < 0.3 else ["q", "y"]
    ctx.menu(who, lines)


def shop_keeper_lines(ctx):
    """Menu input that gives a shop a keeper -- unless the shop-keeper known
    bug is being reproduced.  A keeperless shop (S_KEEPER == NOBODY) is
    corrupted by the next delete_mobile() and crashes save_shops() (SEGV at
    genshp.c:410); left in, it would kill every soak and hide all later
    errors, so it is kept behind --known-bugs shop-keeper."""
    return [] if ctx.keeperless_shops else ["0", str(SRC_MOB)]


def sc_sedit(ctx, who):
    rng = ctx.rng
    vnum = rng.choice(FREE_SHOP)
    lines = [f"sedit {vnum}"]
    if rng.random() < 0.5:
        lines += ["w", str(SRC_SHOP)]       # copy an existing shop
    lines += shop_keeper_lines(ctx)
    lines += ["5", "1.2",                   # buy profit
              "p", "a", str(SRC_OBJ), "q",  # add a product
              "q", "y"]
    ctx.menu(who, lines)


def sc_qedit(ctx, who):
    rng = ctx.rng
    vnum = rng.choice(FREE_QUEST)
    lines = [f"qedit {vnum}",
             "1", f"Soak Quest {vnum}",
             "2", "A soak quest description.",
             "8", str(SRC_MOB)]             # questmaster
    lines += ["x", "y"] if rng.random() < 0.3 else ["q", "y"]
    ctx.menu(who, lines)


def sc_hedit(ctx, who):
    topic = f"soaktopic{ctx.rng.randint(0, 3)}"
    ctx.menu(who, [f"hedit {topic}", "y",   # confirm add/edit
                   "1", f"Soak help for {topic}.", "/s",
                   "2", "0",                # min level
                   "q", "y"])


def sc_zedit(ctx, who):
    """Edit zone reset commands for a room in the build zone."""
    rng = ctx.rng
    room = rng.choice([BUILD_ROOM] + FREE_ROOM[:2])
    lines = [f"zedit {room}",
             "n", "0",                      # new command at position 0
             "M", str(SRC_MOB), "1"]        # load mob, max 1
    if rng.random() < 0.5:
        lines += ["d", "0"]                 # delete a command
    lines += ["q", "y"]
    ctx.menu(who, lines)


def sc_zedit_new(ctx, who):
    """IMP creates a fresh zone (stresses zone-table growth + rnum shifts)."""
    rng = ctx.rng
    base = rng.choice([650, 655, 660, 665])
    ctx.act(who, f"zedit new {base} {base*100} {base*100+9}")


# --- DG trigger scenarios ----------------------------------------------------

# Mob trigger scripts that drive the historically fragile lifetime paths.
# meta = (type_menu, arglist, fires_how):
#   type_menu is the 1-based index in trigedit's type list -- 7 = MTRIG_GREET
#   (1<<6), 4 = MTRIG_SPEECH (1<<3), 3 = MTRIG_COMMAND (1<<2).  arglist is the
#   word the speech/command trigger matches ("*" = any command).  Each script
#   is the body typed into trigedit's command editor.
TRIG_SCRIPTS = {
    # GREET: purge the mob itself mid-script (owner freed while its own frame
    # is still running) -- script_driver must notice and stop.
    "purge_self": (7, None, "greet (re-entry)", [
        "* soak: purge self mid script",
        "wait 1",
        "mecho The air shimmers.",
        "mpurge self",
        "mecho This echo must not run on a freed mob.",
    ]),
    # GREET: damage the actor to death, then keep referencing %actor%.
    "extract_actor": (7, None, "greet (re-entry)", [
        "* soak: damage then touch the actor",
        "mdamage %actor% 100000",
        "mforce %actor% look",
        "mecho done with actor",
    ]),
    # GREET: teleport the actor away, %load% an object, set a remote variable
    # on the (possibly gone) actor.
    "teleport_load_remote": (7, None, "greet (re-entry)", [
        "* soak: teleport, load, remote",
        "%load% obj 3000",
        "mteleport %actor% 3002",
        "set counter 1",
        "remote counter %actor.id%",
        "mecho teleport/load/remote complete",
    ]),
    # SPEECH on the word "soak": wait, then purge the room's contents.
    "speech_purge": (4, "soak", "speech (say soak)", [
        "* soak: speech-triggered purge",
        "wait 1",
        "mechoaround %actor% The mob reacts to %actor.name%.",
        "mpurge",
        "mecho purged after speech",
    ]),
    # COMMAND on the word "soakcmd": wait, then echo (the harness purges the
    # owner while the wait may still be pending).
    "command_echo": (3, "soakcmd", "command (soakcmd)", [
        "* soak: command-triggered wait and echo",
        "wait 1",
        "mecho command trigger ran",
    ]),
}

IMP_NAME = "Implo"   # the implementor; transfer needs LVL_GOD


def ensure_trigger(ctx, who, vnum, script_key):
    """Author a mob trigger via trigedit: name, intended=mob, type, narg,
    arglist, commands -- then save."""
    type_menu, arglist, _fires, body = TRIG_SCRIPTS[script_key]
    lines = [f"trigedit {vnum}",
             "1", f"soak {script_key} {vnum}",         # name
             "2", "0",                                 # intended for: mobiles
             "3", str(type_menu), "0",                 # toggle the type bit
             "4", "100"]                               # 100% fire chance
    if arglist is not None:
        lines += ["5", arglist]             # speech word / command word
    lines += ["6"] + list(body) + ["/s",    # commands text editor
              "q", "y"]                     # save
    ctx.menu(who, lines)


def sc_trig_attach_fire(ctx, who, actor, key=None):
    """Author a trigger, load a mob, attach it, and fire it the way its type
    demands (re-enter for greet, say the word for speech, type for command),
    while the script purges/extracts/teleports things mid-run."""
    rng = ctx.rng
    tvnum = rng.choice(FREE_TRIG)
    mvnum = SRC_MOB
    key = key or rng.choice(sorted(TRIG_SCRIPTS))
    type_menu = TRIG_SCRIPTS[key][0]
    ensure_trigger(ctx, who, tvnum, key)

    # Load a mob in the build room and attach the trigger.
    # Syntax: attach { mob | obj | room } <trigger vnum> <target>.
    ctx.act(who, f"goto {BUILD_ROOM}")
    ctx.act(who, f"load mob {mvnum}")
    ctx.act(who, f"attach mob {tvnum} {mvnum}")

    # Fire it according to its type.  The actor is a level-1 mortal (no goto),
    # so the implementor transfers it into the build room first, and greet is
    # fired by WALKING out and back in (3001 -south-> 3005 -north-> 3001).
    if actor in ctx.conns and not ctx.conns[actor].closed:
        ctx.act(IMP_NAME, f"goto {BUILD_ROOM}")
        ctx.act(IMP_NAME, f"transfer {actor}")
        if type_menu == 7:                  # GREET: walk out and back in
            ctx.act(actor, "south")
            ctx.act(actor, "north")
        elif type_menu == 4:                # SPEECH: say the matched word
            ctx.act(actor, "say soak please")
        elif type_menu == 3:                # COMMAND: type the trigger word
            ctx.act(actor, "soakcmd")
        ctx.act(actor, "look")
    # Detach (syntax: detach mob <target> <trigger|all>), then purge by name
    # (purge resolves a keyword, not a vnum).
    ctx.act(who, f"detach mob {mvnum} {tvnum}")
    ctx.act(who, f"detach mob {mvnum} all")
    ctx.act(who, f"purge {SRC_MOB_KW}")


def sc_trig_room(ctx, who):
    """Author a room trigger, attach it to the current room, then detach.

    Authored as a room (WLD) trigger so the periodic random check
    (script_trigger_check, every PULSE_DG_SCRIPT ~= 13s) scans and may fire
    it while it is attached -- the 'random while attached' path."""
    rng = ctx.rng
    tvnum = rng.choice(FREE_TRIG)
    # Author directly as a room random trigger (intended=2, type menu 2 =
    # WTRIG_RANDOM) with a short wander/purge body.
    ctx.menu(who, [f"trigedit {tvnum}",
                   "1", f"soak wld-random {tvnum}",
                   "2", "2",                # intended for = rooms
                   "3", "2", "0",           # type menu 2 = WTRIG_RANDOM
                   "4", "100",
                   "6", "* soak: room random", "wait 1",
                   "wecho The walls hum.", "/s",
                   "q", "y"])
    ctx.act(who, f"goto {BUILD_ROOM}")
    ctx.act(who, f"attach room {tvnum} .")
    ctx.act(who, f"detach room . {tvnum}")
    ctx.act(who, "detach room . all")


# --- same-zone concurrent editing -------------------------------------------

def sc_concurrent_rooms(ctx, b1, b2, mover=None):
    """Two builders editing the same zone at once.

    b1 holds a NEW room buffer open while b2 inserts a room (shifts every
    rnum, including b1's open buffer -- the renumber-other-editors path), then
    b2 deletes a POPULATED original room (a player and an object are moved in
    first, so delete_room evacuates occupants to the void).  b1 then saves its
    still-open buffer; both persist and the zone resets."""
    rng = ctx.rng
    r1, r2 = FREE_ROOM[0], FREE_ROOM[1]
    victim = rng.choice(DELETABLE_ROOMS)

    ctx.act(b1, f"redit {r1}")              # b1 holds a NEW room buffer open
    ctx.act(b1, "1")
    ctx.act(b1, f"Concurrent {ctx.actions}")

    # b2 creates and saves another new room (add_room shifts rnums for all).
    ctx.act(b2, f"redit {r2}")
    ctx.act(b2, "1")
    ctx.act(b2, "Inserter room")
    ctx.act(b2, "q")
    ctx.act(b2, "y")

    # Populate the victim room, then b2 deletes it (evacuation path).
    if mover and mover in ctx.conns and not ctx.conns[mover].closed:
        ctx.act(mover, f"goto {victim}")
    ctx.act(b2, f"goto {victim}")
    ctx.act(b2, f"load obj {SRC_OBJ}")      # an object now sits in the room
    ctx.act(b2, f"redit {victim}")
    ctx.act(b2, "x")
    ctx.act(b2, "y")                        # delete_room moves people+obj to void
    ctx.act(b2, f"goto {BUILD_ROOM}")
    if mover and mover in ctx.conns and not ctx.conns[mover].closed:
        ctx.act(mover, f"goto {BUILD_ROOM}")

    # b1 finally saves its still-open (renumbered) buffer.
    ctx.act(b1, "q")
    ctx.act(b1, "y")
    ctx.act(b1, f"redit save {BUILD_ZONE_VNUM}")
    ctx.act(b2, f"zreset {BUILD_ZONE_VNUM}")


# --- immortal commands -------------------------------------------------------

def sc_olc_save(ctx, who):
    """`olc` lists the pending-save set; each editor's `save <zone>` form and
    `saveall` write it to disk."""
    z = BUILD_ZONE_VNUM
    ctx.act(who, "olc")
    for ed in ("redit", "oedit", "medit", "zedit", "sedit", "qedit"):
        ctx.act(who, f"{ed} save {z}")
    ctx.act(who, "saveall")
    ctx.act(who, "olc")


def sc_imm_commands(ctx, who, targets, every=False):
    rng = ctx.rng
    t = rng.choice(targets)
    choices = [
        f"stat room {BUILD_ROOM}",         # stat by room vnum
        f"vstat m {SRC_MOB}",              # vstat/vnum take vnums
        f"vstat o {SRC_OBJ}",
        f"vstat r {BUILD_ROOM}",
        "vnum obj light",
        "vnum mob man",
        "show zones",
        "show stats",
        "show player Implo",
        "wizhelp",
        "last Implo",
        f"goto {BUILD_ROOM}",
        f"at {BUILD_ROOM} look",
        f"load obj {SRC_OBJ}",             # load takes a vnum
        f"load mob {SRC_MOB}",
        f"purge {SRC_MOB_KW}",             # purge resolves a keyword
        f"stat mob {SRC_MOB_KW}",          # stat <name> resolves a keyword
        f"restore {t}",                    # these resolve a player name
        f"set {t} hitpoints 100",
        f"transfer {t}",
        f"zreset {BUILD_ZONE_VNUM}",
        f"snoop {t}",
        "snoop self",
    ]
    if every:
        for c in choices:
            # self-target commands that read a player descriptor are skipped
            # for the file/other targets but fine for a live target here.
            ctx.act(who, c.replace("{t}", t) if "{t}" in c else c)
    else:
        for _ in range(ctx.rng.randint(2, 5)):
            ctx.act(who, rng.choice(choices))


def sc_switch_return(ctx, who):
    """switch into a mob, act, then return (classic descriptor-lifetime path)."""
    ctx.act(who, f"goto {BUILD_ROOM}")
    ctx.act(who, f"load mob {SRC_MOB}")     # load by vnum
    ctx.act(who, f"switch {SRC_MOB_KW}")    # switch resolves a keyword
    ctx.act(who, "look")
    ctx.act(who, "return")
    ctx.act(who, f"purge {SRC_MOB_KW}")


# --- deterministic coverage pass --------------------------------------------
# Runs once at the start of every soak so that every path the README claims is
# exercised in EVERY seed, not just when the dice fall right in a short run;
# the seeded-random loop then repeats and interleaves these for the remainder.

def _save_new(ctx, who, editor, vnum, fields):
    """Open `editor vnum`, enter menu choices/values, quit and SAVE."""
    ctx.menu(who, [f"{editor} {vnum}"] + list(fields) + ["q", "y"])


def _delete(ctx, who, editor, vnum):
    """Reopen a saved entity and delete it with the editor's own X command."""
    ctx.menu(who, [f"{editor} {vnum}", "x", "y"])


def coverage_pass(ctx, imp, b1, b2, p1, p2):
    o_new, m_new, r_new = FREE_OBJ[0], FREE_MOB[0], FREE_ROOM[0]
    o_cp, m_cp, r_cp = FREE_OBJ[1], FREE_MOB[1], FREE_ROOM[1]

    # 1. create + save, with extra descriptions, in every editor
    _save_new(ctx, b1, "redit", r_new, ["1", "Coverage room", "f", "1", "cvkey",
                                         "2", "Coverage extra.", "/s", "0"])
    _save_new(ctx, b1, "oedit", o_new, ["1", "cvobj coverage", "2", "a coverage obj",
                                         "e", "1", "cvobj", "2", "Obj extra.", "/s", "0"])
    _save_new(ctx, b2, "medit", m_new, ["2", "cvmob coverage", "3", "a coverage mob",
                                         "5", "A coverage mob is here.", "/s"])
    _save_new(ctx, b2, "sedit", FREE_SHOP[0],
              shop_keeper_lines(ctx) + ["5", "1.1"])
    _save_new(ctx, b1, "qedit", FREE_QUEST[0], ["1", "Coverage Quest"])
    sc_hedit(ctx, imp)

    # 2. copy ('W') from TRIGGERED sources into new vnums, then save (so the
    #    copied trigger list is handed to a prototype, not just dropped)
    _save_new(ctx, b1, "redit", r_cp, ["w", str(TRIGGERED["redit"])])
    _save_new(ctx, b1, "oedit", o_cp, ["w", str(TRIGGERED["oedit"])])
    _save_new(ctx, b2, "medit", m_cp, ["w", str(TRIGGERED["medit"])])

    # 3. open-and-quit unchanged (peek), and change-then-abort (q, n)
    for ed in sorted(TRIGGERED):
        ctx.menu(b1, [f"{ed} {TRIGGERED[ed]}", "q"])
    for ed, v, fld in (("redit", r_new, "1"), ("oedit", o_new, "1"),
                       ("medit", m_new, "2"), ("qedit", FREE_QUEST[0], "1")):
        ctx.menu(b2, [f"{ed} {v}", fld, "doomed draft", "q", "n"])

    # 4. zedit reset commands that reference the new obj and mob
    ctx.act(b1, f"goto {r_new}")
    ctx.menu(b1, [f"zedit {r_new}",
                  "n", "0", "M", str(m_new), "1",      # load the new mob
                  "n", "1", "G", str(o_new), "1",      # give it the new obj
                  "n", "2", "O", str(o_new), "1",      # drop one on the floor
                  "q", "y"])
    ctx.act(b1, f"zreset {BUILD_ZONE_VNUM}")

    # 5. b2 holds a zedit buffer on the same room while b1 deletes the
    #    referenced obj and mob; then b2 saves its stale buffer; reset.
    ctx.act(b2, f"goto {r_new}")
    ctx.act(b2, f"zedit {r_new}")
    _delete(ctx, b1, "oedit", o_new)
    _delete(ctx, b1, "medit", m_new)
    ctx.act(b2, "z")
    ctx.act(b2, "Zone renamed while stale")
    ctx.act(b2, "q")
    ctx.act(b2, "y")
    ctx.act(imp, f"zreset {BUILD_ZONE_VNUM}")

    # 6. two builders on one zone, deleting a populated original room
    sc_concurrent_rooms(ctx, b1, b2, mover=p1)

    # 7. delete what survived, olc save, zone reset
    _delete(ctx, b1, "redit", r_cp)
    _delete(ctx, b1, "oedit", o_cp)
    _delete(ctx, b2, "medit", m_cp)
    _delete(ctx, b2, "qedit", FREE_QUEST[0])
    sc_olc_save(ctx, b1)
    ctx.act(imp, f"zreset {BUILD_ZONE_VNUM}")

    # 8. every trigger script: author, attach, fire (greet/speech/command),
    #    plus a room random trigger
    for key in sorted(TRIG_SCRIPTS):
        sc_trig_attach_fire(ctx, b1, p1, key=key)
    sc_trig_room(ctx, b2)

    # 9. immortal commands (all of them), switch/return, a fresh zone
    sc_imm_commands(ctx, imp, [p1, p2, "self"], every=True)
    sc_switch_return(ctx, imp)
    sc_zedit_new(ctx, imp)


# --- known pre-existing bugs (off by default) --------------------------------

def sc_known_bug_g1_01(ctx, imp, victim_name):
    """G1-01: set file <player> room <vnum> then goto -> freed char in room."""
    log("KNOWN BUG G1-01: set file room + goto (expected to crash on base)")
    ctx.act(imp, f"set file {victim_name} room {BUILD_ROOM}")
    ctx.act(imp, f"goto {BUILD_ROOM}")
    ctx.act(imp, "look")


def sc_known_bug_g1_05(ctx, imp, victim_name):
    """G1-05: force <linkdead player> return -> do_return derefs NULL desc."""
    log("KNOWN BUG G1-05: force <linkdead> return (expected to crash on base)")
    # victim must be linkdead: caller closes its socket before this.
    ctx.act(imp, f"force {victim_name} return")


# ----------------------------------------------------------------------------
# Error scanning
# ----------------------------------------------------------------------------

SAN_MARKERS = [
    "ERROR: AddressSanitizer", "runtime error:", "ERROR: LeakSanitizer",
    "SUMMARY: AddressSanitizer", "SUMMARY: UndefinedBehaviorSanitizer",
    "detected memory leaks", "heap-use-after-free", "heap-buffer-overflow",
    "stack-buffer-overflow", "global-buffer-overflow", "AddressSanitizer:DEADLYSIGNAL",
]

# Valgrind error HEADERS (the first line of a real error record).  Matched
# against the text after the "==pid== " prefix with startswith(), so summary
# lines such as "definitely lost: 0 bytes in 0 blocks" are never mistaken for
# an error.  Leak and error COUNTS are checked separately and only when > 0.
# (A naive substring scan for "definitely lost" flagged every clean run.)
VG_ERROR_HEADERS = (
    "Invalid read of size", "Invalid write of size", "Invalid free",
    "Mismatched free", "Process terminating with default action",
    "Conditional jump or move depends on uninitialised",
    "Use of uninitialised value", "Source and destination overlap",
    "Argument 'size' of function", "Syscall param",
)
VG_LINE_RE = re.compile(r"^==(\d+)== ?(.*)$")

# Leak families already root-caused on this base (see repro_olc_bugs.py),
# keyed by the first project frame that is not an allocation wrapper.
KNOWN_LEAK_SITES = {
    "qedit.c:221": "qedit-save (F2-13, fixed by ef4d615)",
    "qedit.c:222": "qedit-save (F2-13, fixed by ef4d615)",
    "qedit.c:223": "qedit-save (F2-13, fixed by ef4d615)",
    "qedit.c:224": "qedit-save (F2-13, fixed by ef4d615)",
    "qedit.c:588": "qedit-save (F2-13, fixed by ef4d615)",
    "qedit.c:596": "qedit-save (F2-13, fixed by ef4d615)",
    "dg_olc.c:1087": "OLC_SCRIPT copy (F3-08, fixed by 703f088)",
    "dg_olc.c:1094": "OLC_SCRIPT copy (F3-08, fixed by 703f088)",
    # copy_mobile_strings / copy_object_strings: strings of a prototype that
    # was later deleted -- delete_mobile/delete_object shift the table over
    # the deleted slot without freeing it.
    "genmob.c:201": "deleted mob prototype (C1-23, fixed by 3b08462/64d803c)",
    "genmob.c:203": "deleted mob prototype (C1-23, fixed by 3b08462/64d803c)",
    "genmob.c:205": "deleted mob prototype (C1-23, fixed by 3b08462/64d803c)",
    "genmob.c:207": "deleted mob prototype (C1-23, fixed by 3b08462/64d803c)",
    "genmob.c:209": "deleted mob prototype (C1-23, fixed by 3b08462/64d803c)",
    "genobj.c:376": "deleted obj prototype (F1-V02, fixed by 50f326f)",
    "genobj.c:377": "deleted obj prototype (F1-V02, fixed by 50f326f)",
    "genobj.c:378": "deleted obj prototype (F1-V02, fixed by 50f326f)",
    "genobj.c:379": "deleted obj prototype (F1-V02, fixed by 50f326f)",
}
# Fallback by file, for the OLC 'W'-copy / setup_existing leak family: opening
# "copy <vnum>" rebuilt the editor buffer over the live one, leaking its
# struct, strings and trigger-list copy (and any edited text buffer in
# modify.c).  All of these are fixed on audit-fix-F1 (5845db0 "'W' copy
# releases the scratch copy", 75bc7f0 "trigger-list copies have one owner").
KNOWN_LEAK_FILES = {
    "redit.c": "OLC 'W' copy over live buffer (F1-09/F1-10, fixed on audit-fix-F1)",
    "oedit.c": "OLC 'W' copy over live buffer (F1-09/F1-10, fixed on audit-fix-F1)",
    "medit.c": "OLC 'W' copy over live buffer (F1-09/F1-10, fixed on audit-fix-F1)",
    "sedit.c": "OLC 'W' copy over live buffer (F1-10, fixed on audit-fix-F1)",
    "modify.c": "OLC editor text buffer (freed with the copy family on audit-fix-F1)",
}
_ALLOC_WRAPPERS = ("utils.c:", "genolc.c:76")   # tb_*alloc*_at, str_udup


def is_runtime_finding(snip):
    """True for an invalid-access / UB report, False for a leak report.

    UBSan "runtime error:", an AddressSanitizer report that is not the
    at-exit LeakSanitizer summary, or a Valgrind error header."""
    return ("runtime error:" in snip or
            ("AddressSanitizer" in snip and "LeakSanitizer" not in snip) or
            snip.lstrip("=0123456789 ").startswith(VG_ERROR_HEADERS))


def _leak_label(site):
    if site in KNOWN_LEAK_SITES:
        return KNOWN_LEAK_SITES[site]
    return KNOWN_LEAK_FILES.get(site.split(":", 1)[0], "NEW")


def summarize_lsan(text):
    """Group a LeakSanitizer report by allocation site.

    Returns [(site, count, bytes, label)] where site is the first project
    frame past the allocation wrappers (tb_*alloc*_at in utils.c and
    str_udup in genolc.c, whose callers are the real owners).  label names
    the known family, or "NEW" for a site not yet root-caused.
    """
    out = {}
    if "ERROR: LeakSanitizer" not in text:
        return []
    report = text[text.index("ERROR: LeakSanitizer"):]
    for m in re.finditer(r"(?:Direct|Indirect) leak of (\d+) byte.*?\n((?:\s+#\d+ .*\n)+)",
                         report):
        frames = re.findall(r"/src/([\w.]+:\d+)", m.group(2))
        site = next((f for f in frames if not f.startswith(_ALLOC_WRAPPERS)),
                    frames[0] if frames else "?")
        n, b = out.get(site, (0, 0))
        out[site] = (n + 1, b + int(m.group(1)))
    return [(s, n, b, _leak_label(s)) for s, (n, b) in sorted(out.items())]


def scan_sanitizer(server):
    """Return a list of (file, snippet) sanitizer findings."""
    findings = []
    # Dedicated sanitizer log files (log_path writes <prefix>.<pid>).
    for prefix in (server.asan_prefix, server.ubsan_prefix, server.lsan_prefix):
        d = os.path.dirname(prefix)
        base = os.path.basename(prefix)
        try:
            names = os.listdir(d)
        except OSError:
            names = []
        for n in names:
            if n.startswith(base):
                p = os.path.join(d, n)
                try:
                    txt = open(p, "r", errors="replace").read()
                except OSError:
                    continue
                if txt.strip():
                    findings.append((p, txt[:4000]))
    # Inline in the server log too (abort_on_error prints to stderr->log).
    try:
        slog = open(server.log_path, "r", errors="replace").read()
    except OSError:
        slog = ""
    for m in SAN_MARKERS:
        if m in slog:
            idx = slog.find(m)
            findings.append((server.log_path, slog[idx:idx + 2000]))
            break
    return findings


def scan_valgrind(server, vg_rc):
    """Return (source, snippet) for each REAL Valgrind error or leak.

    The log can hold several PIDs: the server forks (system()) to run
    autowiz, and each forked copy prints its own summary.  Every PID is
    checked, but only for genuine error records and NON-ZERO counts.
    """
    findings = []
    try:
        lines = open(server.vg_log, "r", errors="replace").read().splitlines()
    except OSError:
        lines = []

    # 1. Real error records: an error header plus its stack, which runs until
    #    the next blank "==pid==" line.
    i = 0
    while i < len(lines):
        m = VG_LINE_RE.match(lines[i])
        if m and m.group(2).startswith(VG_ERROR_HEADERS):
            pid = m.group(1)
            block = [lines[i]]
            j = i + 1
            while j < len(lines):
                mj = VG_LINE_RE.match(lines[j])
                if not mj or mj.group(1) != pid or not mj.group(2).strip():
                    break
                block.append(lines[j])
                j += 1
            findings.append((server.vg_log, "\n".join(block[:40])))
            i = j
            continue
        i += 1

    # 2. Non-zero error and leak counts (definite/indirect only; "still
    #    reachable" and "possibly lost" are not failures here).
    for ln in lines:
        m = VG_LINE_RE.match(ln)
        if not m:
            continue
        body = m.group(2)
        es = re.search(r"ERROR SUMMARY: ([\d,]+) errors", body)
        if es and int(es.group(1).replace(",", "")) > 0:
            findings.append((server.vg_log, ln))
        lk = re.search(r"(definitely|indirectly) lost: ([\d,]+) bytes", body)
        if lk and int(lk.group(2).replace(",", "")) > 0:
            findings.append((server.vg_log, ln))

    if vg_rc == 99:
        findings.append(("valgrind", "valgrind exited 99 (--error-exitcode)"))
    return findings


# ----------------------------------------------------------------------------
# Main soak
# ----------------------------------------------------------------------------

def run_soak(args):
    work = args.work or tempfile.mkdtemp(prefix="tb-soak-")
    os.makedirs(work, exist_ok=True)
    port = free_port()
    boot_timeout = 900 if args.valgrind else 300
    wait = 600 if args.valgrind else 30          # per-prompt timeout
    quiet = 2.5 if args.valgrind else 0.35       # drain idle threshold
    cap = 120 if args.valgrind else 15           # drain hard cap

    server = Server(args.binary, args.src_lib, work, port,
                    valgrind=args.valgrind, vg_cmd=args.vg_cmd,
                    vg_log=args.vg_log)

    result = {"status": "unknown", "findings": [], "actions": 0,
              "work": work, "port": port, "seed": args.seed}

    if not server.boot(boot_timeout):
        result["status"] = "boot_failed"
        result["findings"] = scan_sanitizer(server)
        return result, server

    import random
    rng = random.Random(args.seed)
    ctx = Ctx(server, rng, quiet, cap)
    ctx.keeperless_shops = "shop-keeper" in args.known_bugs

    try:
        # --- population: imp first (LVL_IMPL), then builders + players -------
        log("creating implementor")
        ctx.conns["Implo"] = create_char(port, "Implo", wait)
        for b in ("Builda", "Buildo"):
            log(f"creating builder {b}")
            ctx.conns[b] = create_char(port, b, wait)
        for p in ("Playa", "Playo"):
            log(f"creating player {p}")
            ctx.conns[p] = create_char(port, p, wait)

        # advance builders and grant per-zone OLC on the build zone.
        for b in ("Builda", "Buildo"):
            ctx.act("Implo", f"advance {b} 51")        # LVL_BUILDER
            ctx.act("Implo", f"set {b} olc {BUILD_ZONE_VNUM}")
        # Long listings (show zones, wizhelp, vnum, olc) must not leave a
        # client parked in the pager, which would swallow later commands.
        for name in ctx.conns:
            ctx.act(name, "toggle pagelength 255")
        # Co-locate everyone in the build zone.
        for name in ctx.conns:
            ctx.act(name, f"goto {BUILD_ROOM}")

        builders = ["Builda", "Buildo"]
        players = ["Playa", "Playo"]
        everyone_targets = players + ["self"]

        single_editor = [sc_redit, sc_redit_abort, sc_oedit, sc_medit,
                         sc_sedit, sc_qedit, sc_zedit, sc_peek_quit]

        # --- deterministic coverage pass (runs once, every seed) -------------
        log("coverage pass: exercising every editor/trigger/immortal path once")
        coverage_pass(ctx, "Implo", "Builda", "Buildo", "Playa", "Playo")
        log(f"coverage pass done after {ctx.actions} actions; "
            f"server alive={server.alive()}")

        # --- main soak loop (seeded-random interleaving) ---------------------
        deadline = time.time() + args.minutes * 60
        log(f"soaking for {args.minutes} min (seed={args.seed}, "
            f"{'valgrind' if args.valgrind else 'asan'})")
        while time.time() < deadline and server.alive():
            roll = rng.random()
            if roll < 0.33:
                who = rng.choice(builders)
                rng.choice(single_editor)(ctx, who)
            elif roll < 0.45:
                sc_hedit(ctx, "Implo")
            elif roll < 0.58:
                sc_concurrent_rooms(ctx, "Builda", "Buildo",
                                    mover=rng.choice(players))
            elif roll < 0.74:
                sc_trig_attach_fire(ctx, rng.choice(builders),
                                    rng.choice(players))
            elif roll < 0.82:
                sc_trig_room(ctx, rng.choice(builders))
            elif roll < 0.88:
                sc_switch_return(ctx, "Implo")
            elif roll < 0.94:
                sc_zedit_new(ctx, "Implo")
            else:
                sc_imm_commands(ctx, "Implo", everyone_targets)

            if not server.alive():
                break

        result["actions"] = ctx.actions
        log(f"soak loop done after {ctx.actions} actions; server alive={server.alive()}")

        # --- optional known bugs (expected crash on base) --------------------
        if args.known_bugs and server.alive():
            wanted = args.known_bugs
            if "g1-01" in wanted:
                ctx.act("Implo", "save")
                sc_known_bug_g1_01(ctx, "Implo", "Playa")
            # G1-05: make Playo linkdead, then force return.  (When both are
            # selected G1-01 usually crashes first; select g1-05 alone to see
            # its own report.)
            if ("g1-05" in wanted and "Playo" in ctx.conns
                    and not ctx.conns["Playo"].closed and server.alive()):
                ctx.conns["Playo"].close()
                time.sleep(3 if not args.valgrind else 20)
                sc_known_bug_g1_05(ctx, "Implo", "Playo")

        # --- shutdown --------------------------------------------------------
        if server.alive():
            log("shutting down cleanly (shutdown die)")
            try:
                ctx.conns["Implo"].send("shutdown die")
            except SystemExit:
                pass
            # Give the server time to free the world and log "Done.".
            rc = server.wait_exit(180 if args.valgrind else 60)
            if rc is None:
                log("server did not exit after shutdown die; stopping by PID")
                result.setdefault("notes", []).append("shutdown-die did not exit cleanly")
                server.stop()
        else:
            log("server already dead before shutdown")
            result["died_before_shutdown"] = True

    except SystemExit as e:
        log(f"driver aborted: {e}")
        result.setdefault("notes", []).append(f"driver abort: {e}")
    finally:
        for c in list(ctx.conns.values()):
            c.close()

    # --- verdict -------------------------------------------------------------
    rc = server.proc.returncode if server.proc else None
    result["returncode"] = rc
    san = scan_sanitizer(server)
    result["findings"].extend(san)

    vg_findings = []
    if args.valgrind:
        vg_findings = scan_valgrind(server, rc)

    try:
        slog_tail = open(server.log_path, "r", errors="replace").read()
    except OSError:
        slog_tail = ""
    clean_done = (rc == 0) and ("Done." in slog_tail)
    server_died = (rc is not None) and (rc != 0)
    result["leak_sites"] = summarize_lsan(slog_tail)

    if args.known_bugs:
        # A crash here is the EXPECTED reproduction of G1-01/G1-05, not a
        # harness failure.  Record evidence; exit 0 so the suite continues.
        # Only a crash or a RUNTIME sanitizer report reproduces a known bug.
        # The at-exit leak report (LSan / Valgrind leak counts) comes from the
        # soak itself and must not be mistaken for G1-01/G1-05 firing.
        evidence = [f for f in list(san) + list(vg_findings)
                    if is_runtime_finding(f[1])]
        result["findings"] = []          # expected crash is evidence, not failure
        if result.get("died_before_shutdown") or evidence:
            result["status"] = "known_bugs_reproduced"
            result["evidence"] = evidence or [
                ("server", f"server exited rc={rc} during known-bug phase; "
                           f"tail:\n{slog_tail[-1500:]}")]
        else:
            result["status"] = "known_bugs_not_reproduced"
            result.setdefault("notes", []).append(
                "known-bug scenarios ran without a crash on this base")
        return result, server

    result["findings"].extend(vg_findings)
    if result["findings"]:
        result["status"] = "errors_found"
    elif server_died:
        result["status"] = "server_died"
        result["findings"].append(
            ("server", f"server exited rc={rc} before clean shutdown; "
                       f"log tail:\n{slog_tail[-1500:]}"))
    elif not clean_done:
        result["status"] = "no_clean_shutdown"
        result["findings"].append(
            ("server", f"did not log clean 'Done.' (rc={rc}); tail:\n{slog_tail[-1500:]}"))
    else:
        result["status"] = "clean"
    return result, server


def main():
    ap = argparse.ArgumentParser(description="TBAMud builder/immortal soak test")
    ap.add_argument("binary", help="path to the circle server binary")
    ap.add_argument("src_lib", help="source lib/ directory to copy")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--minutes", type=float, default=1.0)
    ap.add_argument("--valgrind", action="store_true")
    ap.add_argument("--vg-cmd", default=None,
                    help="server command template; {lib} {port} {binary} {vg_log}")
    ap.add_argument("--vg-log", default=None)
    ap.add_argument("--known-bugs", nargs="?", const="g1-01,g1-05", default="",
                    type=lambda s: {x.strip().lower() for x in s.split(",") if x.strip()},
                    help="enable known crashing paths: g1-01, g1-05 (run "
                         "after the soak; bare flag = both) and/or shop-keeper "
                         "(soak-created shops stay keeperless)")
    ap.add_argument("--work", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    result, server = run_soak(args)

    print("\n================ SOAK RESULT ================")
    print(f"status   : {result['status']}")
    print(f"seed     : {result['seed']}")
    print(f"actions  : {result['actions']}")
    print(f"port     : {result['port']}")
    print(f"returncd : {result.get('returncode')}")
    print(f"work     : {result['work']}")
    if result.get("notes"):
        for n in result["notes"]:
            print(f"note     : {n}")
    if result.get("leak_sites"):
        print("LEAKS    : by allocation site (count, bytes, family)")
        for site, n, b, label in result["leak_sites"]:
            print(f"           {site:<16} x{n:<3} {b:>6} B  {label}")
    if result.get("evidence"):
        print(f"EVIDENCE : {len(result['evidence'])} (expected known-bug crash)")
        for i, (src, snip) in enumerate(result["evidence"], 1):
            print(f"\n--- evidence {i} ({src}) ---\n{snip}")
    if result["findings"]:
        print(f"FINDINGS : {len(result['findings'])}")
        for i, (src, snip) in enumerate(result["findings"], 1):
            print(f"\n--- finding {i} ({src}) ---\n{snip}")
    print("============================================")

    passed = result["status"] in ("clean", "known_bugs_reproduced",
                                   "known_bugs_not_reproduced")
    if not args.keep and passed:
        shutil.rmtree(result["work"], ignore_errors=True)
    else:
        print(f"(scratch kept at {result['work']})")

    # Exit code: 0 clean / known-bug run; 1 error/leak/death; 2 setup failure.
    if passed:
        sys.exit(0)
    elif result["status"] == "boot_failed":
        sys.exit(2)
    else:
        sys.exit(1)


if __name__ == "__main__":
    main()
