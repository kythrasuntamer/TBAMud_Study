#!/usr/bin/env python3
"""soak_gameplay.py -- end-to-end gameplay soak test for the TBAMud server.

README
======

What it does
------------
Boots the real server, ``circle -q -d <scratch lib> <free port>``, against a
scratch copy of lib/ whose player files and index are removed, so the first
character created becomes the implementor.  It then connects one implementor
and 4-6 ordinary players, each over its own telnet connection, and plays:

1. A scripted phase that takes every player through the ordinary gameplay
   surface at least once: look/score/inventory/equipment/who/where, walking
   through Midgaard, shopping (list/buy/sell/value) at six shops, get/drop/
   put/give/wear/remove/wield/hold/eat/drink/donate/junk, say/tell/reply/
   gossip/shout/emote/whisper/socials, group/follow/report/split, fighting
   mobs (kill/hit/kick/bash/backstab/rescue/assist/flee), casting common
   spells, practising at a guild, the questmaster (quest list/join/progress/
   history/leave, finishing a kill quest), rest/sit/sleep/wake, the bank, the
   post office (mail/check/receive), the bulletin board, the pet shop, rent,
   save, quit and log back in, a link-drop (socket closed mid-session, also
   mid-fight) and reconnect, logging in over a live session, and a death.
2. A seeded-random phase: every player runs in its own thread, all connected
   at once, and picks weighted random activities from the same repertoire
   (and wanders Midgaard and the zones next to it) for --minutes minutes.

The implementor only sets things up, with commands an implementor can type:
advance, set gold, skillset, restore, goto, teleport, at <player> load
mob/obj, who/where.  Players type only what players can type.  No source
changes are needed or made.

Finally the implementor types ``shutdown die`` and the test waits for the
server to exit.

Pass / fail
-----------
Exit status 0: PASS.  Exit status 1: FAIL, because
  * the server died before the shutdown or did not exit (status 0) after it;
  * the server log holds an AddressSanitizer, LeakSanitizer or
    UndefinedBehaviorSanitizer report; or
  * with --valgrind, Valgrind reports any error or any definitely/indirectly
    lost block (its exit status 99 also fails the run).
Exit status 2: the harness could not do its job (no boot, a character could
not be created, or a bot raised a Python exception).  This is never folded
into a PASS: a soak that silently stops exercising the game proves nothing.

Builds
------
ASan+UBSan (from the source tree)::

    cmake -S . -B ../build-asan -DCMAKE_BUILD_TYPE=Debug \\
          -DENABLE_SANITIZERS=ON -DSTATIC_ANALYSIS=OFF
    cmake --build ../build-asan -j2 --target circle
    tests/e2e/soak_gameplay.py --circle ../build-asan/bin/circle --seed 1

Valgrind (plain Debug build)::

    cmake -S . -B ../build-vg -DCMAKE_BUILD_TYPE=Debug -DSTATIC_ANALYSIS=OFF
    cmake --build ../build-vg -j2 --target circle
    tests/e2e/soak_gameplay.py --circle ../build-vg/bin/circle --valgrind --seed 1

``--valgrind`` runs ``valgrind --error-exitcode=99 --leak-check=full
--errors-for-leak-kinds=definite,indirect --track-origins=yes`` and scales
every client timeout by --timeout-scale (default 12 under Valgrind).

Options
-------
--circle PATH       server binary (required)
--lib DIR           source lib/ to copy (default: <repo>/lib)
--seed N            seed for every client-side choice (default: random; it is
                    printed so a run can be repeated)
--minutes M         length of the random phase in minutes (default 3).  The
                    game loop runs in real time and catches up missed pulses,
                    so this is also minutes of game time while the server
                    keeps up; under Valgrind fewer commands fit in a minute.
--players N         ordinary players, 4-6 (default 5: warrior, cleric,
                    wizard, thief, monk; the 6th is a second warrior)
--valgrind          run the server under Valgrind (see above)
--timeout-scale F   multiply every client timeout by F
--skip ACT[,ACT]    leave random-phase activities out (to hunt for a second
                    bug while a first one is still open); --list-activities
                    prints their names
--avoid CMD[,CMD]   never send commands whose first word is CMD (to mask a
                    known bug, e.g. --avoid whirlwind, while hunting others)
--no-scripted       skip the scripted phase (setup still runs)
--replay FILE       run a small command script instead of the soak; see below
--workdir DIR       where to put the scratch lib and logs (default: a new
                    temporary directory)
--keep              keep the work directory even when the run passes
--port N            port to use (default: a free one)

Reproducibility
---------------
Every client-side decision (which activity, which command, which target, how
long to think) comes from random.Random seeded with "<seed>:<bot name>", so a
seed replays the same command choices for each bot.  The server's own
randomness (combat rolls, mob movement) and thread timing are not seeded, so
two runs of one seed are similar, not identical.  commands.log in the work
directory records every line every bot sent, in global order, with times;
that is the starting point for reducing a failure.

Artifacts (kept on failure or with --keep)
------------------------------------------
server.log            server stdout+stderr (the syslog and sanitizer reports)
valgrind.log          Valgrind's report (--valgrind)
commands.log          every command sent, by whom, in order
transcripts/NAME.log  everything each connection received
summary.json          counters and the verdict

Replay scripts
--------------
``--replay FILE`` boots the server the same way, runs FILE, then shuts the
server down and applies the same pass/fail checks.  A reproducer is written
as such a file (see tests/e2e/repro_*.txt).  Lines::

    # comment
    create NAME CLASS      new connection, new character (c t w z o); the
                           first one created is the implementor
    login NAME             new connection, log in an existing character
    login NAME as ALIAS    a second, separate connection for NAME, addressed
                           as ALIAS in later lines (the first one stays open)
    NAME: COMMAND          send COMMAND as NAME and wait for the prompt
    NAME!: TEXT            send TEXT as NAME without waiting (menus, editor)
    expect NAME REGEX      wait until NAME has received text matching REGEX
    linkdrop NAME          close NAME's socket without quitting
    sleep SECONDS          pause (scaled by --timeout-scale)

Every player password is "soakpass".
"""

import argparse
import collections
import json
import os
import queue
import random
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback

PASSWORD = "soakpass"
IMP_NAME = "Soakimp"
PLAYER_ROSTER = [  # name, class letter, sex
    ("Alric", "w", "m"),
    ("Brena", "c", "f"),
    ("Corin", "z", "m"),
    ("Dagny", "t", "f"),
    ("Elsin", "o", "m"),
    ("Fargo", "w", "f"),
]
CLASS_NAMES = {"w": "warrior", "c": "cleric", "z": "wizard", "t": "thief", "o": "monk"}
PLAYER_LEVEL = 30          # advanced to this so ordinary fights do not kill them

# Midgaard landmarks (lib/world/wld/30.wld).
R_TEMPLE, R_TEMPLE_SQUARE, R_MARKET, R_READING_ROOM = 3001, 3005, 3014, 3000
R_BAKERY, R_GENERAL, R_WEAPONS, R_ARMORY, R_MAGIC = 3009, 3010, 3011, 3020, 3033
R_BAR, R_PETSHOP, R_POST, R_RECEPTION, R_DONATION = 3007, 3031, 3062, 3008, 3063
R_FIGHT, R_QUEST = 3044, 3043            # Poor Alley, Wall Road: not NOMOB/PEACEFUL
SHOP_ROOMS = [R_BAKERY, R_GENERAL, R_WEAPONS, R_ARMORY, R_MAGIC, R_BAR, 3058]
GUILD_ROOM = {"w": 3023, "c": 3002, "z": 3019, "t": 3029}   # guildmaster rooms
NEARBY_ZONES = {30, 31, 32, 35, 60, 70, 186}   # random walks stay in these

# Mobs the implementor loads as fodder: vnum -> keyword.  Low level, no
# special procedures that would wreck the run.
FODDER = {3062: "fido", 3065: "beggar", 3064: "drunk", 194: "mouse", 3060: "cityguard"}
QUESTMASTER, QUEST_TARGET = 179, 194     # the recruit; quest 100 kills 3 mice

DIRS = ["north", "east", "south", "west", "up", "down"]
AUTOEXIT = ["n", "e", "s", "w", "u", "d", "nw", "ne", "se", "sw"]
ROOM_DEATH, ROOM_NOMOB, ROOM_PEACEFUL = 1 << 1, 1 << 2, 1 << 4
ROOM_PRIVATE, ROOM_GODROOM, ROOM_HOUSE = 1 << 9, 1 << 10, 1 << 11
BAD_SECTORS = {7, 8, 9}                   # no-swim water, flying, underwater

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PROMPT_TAIL = re.compile(r"(?:^|\n)[^\n]{0,160}> $")
PAGER_TAIL = re.compile(r"\[ Return to continue, \(q\)uit, \(r\)efresh, \(b\)ack, "
                        r"or page number \((\d+)/(\d+)\) \]\s*$")
MENU_TAIL = re.compile(r"Make your choice: $")
EDITOR_TAIL = re.compile(r"(?:^|\n)\] $")
LOGIN_TAILS = re.compile(r"(By what name do you wish to be known\? |Name: |"
                         r"Okay, what IS it, then\? |Password: |PRESS RETURN: |"
                         r"\(Y/N\)\? |Please type Yes or No: )$")
EXITS_LINE = re.compile(r"^\[ Exits: (.*?)\s*\]$", re.M)
MOVE_FAIL = ("Alas, you cannot go that way", "seems to be closed", "is closed",
             "too exhausted", "fighting for your life", "get on your feet",
             "In your dreams", "need a boat", "blocks your way", "humiliates you",
             "isn't enough room", "aren't godly enough", "private conversation",
             "need to fly", "You can't", "You are too", "unable to", "No way!",
             "pretty bad shape", "can't do that", "pitch black")
SANITIZER_RE = re.compile(r"ERROR: AddressSanitizer|ERROR: LeakSanitizer|"
                          r"WARNING: AddressSanitizer|AddressSanitizer:DEADLYSIGNAL|"
                          r"SUMMARY: AddressSanitizer|SUMMARY: UndefinedBehaviorSanitizer|"
                          r"runtime error:|ERROR: MemorySanitizer|ERROR: ThreadSanitizer")


class Disconnected(Exception):
    """The server closed this bot's connection."""


class Stopped(Exception):
    """The run is being torn down (server gone, or time is up)."""


# --------------------------------------------------------------------------
# World data: rooms for navigation, objects and mobs for keywords.
# --------------------------------------------------------------------------

def _bits(token):
    """Parse a room/exit bitvector written either as a number or as letters."""
    if re.fullmatch(r"-?\d+", token):
        return int(token)
    value = 0
    for ch in token:
        if "a" <= ch <= "z":
            value |= 1 << (ord(ch) - ord("a"))
        elif "A" <= ch <= "Z":
            value |= 1 << (26 + ord(ch) - ord("A"))
    return value


def _index_files(directory):
    index = os.path.join(directory, "index")
    names = []
    if os.path.exists(index):
        for line in open(index, encoding="latin-1"):
            line = line.strip()
            if line == "$":
                break
            if line:
                names.append(line)
    return [os.path.join(directory, n) for n in names]


def _skip_tilde_block(lines, i):
    """Return the index after the line that ends a ~-terminated text block."""
    while i < len(lines) and not lines[i].rstrip().endswith("~"):
        i += 1
    return i + 1


class World:
    def __init__(self, lib):
        self.rooms = {}       # vnum -> dict(name, flags, sector, exits{dir: (to, doorflag, keyword)})
        self.obj_kw = {}      # lowercased short description -> keyword
        self.mob_long = {}    # long description line -> (vnum, keyword)
        self.mob_short = []   # (Short description capitalised, vnum, keyword)
        self.names = collections.defaultdict(list)
        base = os.path.join(lib, "world")
        for path in _index_files(os.path.join(base, "wld")):
            self._parse_wld(path)
        for path in _index_files(os.path.join(base, "obj")):
            self._parse_obj(path)
        for path in _index_files(os.path.join(base, "mob")):
            self._parse_mob(path)
        for vnum, room in self.rooms.items():
            self.names[room["name"]].append(vnum)

    def _parse_wld(self, path):
        lines = open(path, encoding="latin-1").read().split("\n")
        i = 0
        try:
            while i < len(lines):
                line = lines[i]
                if not (line.startswith("#") and line[1:].strip().isdigit()):
                    i += 1
                    continue
                vnum = int(line[1:])
                name = lines[i + 1].rstrip().rstrip("~")
                i = _skip_tilde_block(lines, i + 2)
                fields = lines[i].split()
                i += 1
                flags = _bits(fields[1]) if len(fields) > 1 else 0
                sector = int(fields[-1]) if fields else 0
                exits = {}
                while i < len(lines) and lines[i].strip() != "S":
                    tag = lines[i].strip()
                    if tag.startswith("D") and tag[1:].isdigit():
                        door = int(tag[1:])
                        i = _skip_tilde_block(lines, i + 1)      # general description
                        keyword = lines[i].rstrip().rstrip("~").strip()
                        i = _skip_tilde_block(lines, i)          # keyword
                        nums = lines[i].split()
                        i += 1
                        exits[door] = (int(nums[2]), int(nums[0]), keyword.split()[0] if keyword else "door")
                    elif tag == "E":
                        i = _skip_tilde_block(lines, i + 1)
                        i = _skip_tilde_block(lines, i)
                    else:
                        i += 1
                self.rooms[vnum] = dict(name=name, flags=flags, sector=sector, exits=exits)
        except (IndexError, ValueError):
            pass   # a malformed file only costs us navigation data for it

    def _parse_obj(self, path):
        lines = open(path, encoding="latin-1").read().split("\n")
        for i, line in enumerate(lines[:-2]):
            if line.startswith("#") and line[1:].strip().isdigit():
                keywords = lines[i + 1].rstrip().rstrip("~").split()
                short = lines[i + 2].rstrip().rstrip("~").strip().lower()
                if keywords and short:
                    self.obj_kw.setdefault(short, keywords[0])

    def _parse_mob(self, path):
        lines = open(path, encoding="latin-1").read().split("\n")
        for i, line in enumerate(lines[:-3]):
            if line.startswith("#") and line[1:].strip().isdigit():
                vnum = int(line[1:])
                keywords = lines[i + 1].rstrip().rstrip("~").split()
                short = lines[i + 2].rstrip().rstrip("~").strip()
                long_first = lines[i + 3].strip()
                if not keywords:
                    continue
                if long_first and long_first != "~":
                    self.mob_long[long_first] = (vnum, keywords[0])
                if short:
                    self.mob_short.append((short[0].upper() + short[1:], vnum, keywords[0]))

    def walkable(self, vnum):
        room = self.rooms.get(vnum)
        return bool(room) and not (room["flags"] & (ROOM_DEATH | ROOM_GODROOM | ROOM_PRIVATE | ROOM_HOUSE)) \
            and room["sector"] not in BAD_SECTORS

    def neighbours(self, vnum, zones=None):
        out = []
        for door, (to, _flag, _kw) in self.rooms.get(vnum, {}).get("exits", {}).items():
            if door < len(DIRS) and self.walkable(to) and (zones is None or to // 100 in zones):
                out.append((door, to))
        return out

    def path(self, src, dst, zones=None):
        if src == dst:
            return []
        prev = {src: None}
        todo = collections.deque([src])
        while todo:
            here = todo.popleft()
            for door, to in self.neighbours(here, zones):
                if to not in prev:
                    prev[to] = (here, door)
                    if to == dst:
                        steps = []
                        while prev[to] is not None:
                            to, door = prev[to]
                            steps.append(door)
                        return list(reversed(steps))
                    todo.append(to)
        return None

    def visible_exits(self, vnum):
        exits = set()
        for door, (to, flag, _kw) in self.rooms[vnum]["exits"].items():
            if to in self.rooms and flag not in (3, 4):    # 3/4 = hidden doors
                exits.add(AUTOEXIT[door] if door < len(AUTOEXIT) else str(door))
        return exits

    def locate(self, text, expected=None, previous=None):
        """Find the room whose display is in text: the last '[ Exits: ]' line
        and the nearest line above it that names a room."""
        found = list(EXITS_LINE.finditer(text))
        if not found:
            return None
        last = found[-1]
        shown = set(tok.strip("()") for tok in last.group(1).split() if tok != "None!")
        above = text[:last.start()].split("\n")[::-1][:40]
        for line in above:
            line = line.strip()
            if line in self.names:
                cands = [v for v in self.names[line] if self.visible_exits(v) == shown] or self.names[line]
                if expected in cands:
                    return expected
                if previous is not None:
                    near = [v for v in cands if any(t == v for _d, t in self.neighbours(previous))]
                    if near:
                        return near[0]
                return cands[0]
        return None


# --------------------------------------------------------------------------
# Shared run state.
# --------------------------------------------------------------------------

class Timeouts:
    def __init__(self, scale):
        self.scale = scale
        self.boot = 300 * scale
        self.connect = 60 * scale
        self.login = 60 * scale       # argon2id hashing is slow under Valgrind
        self.cmd = 20 * scale
        self.grace = 2.0 * scale      # extra wait for an expected reply after a prompt
        self.shutdown = 120 * scale


class Ctx:
    def __init__(self, args, workdir, world):
        self.args = args
        self.workdir = workdir
        self.world = world
        self.T = Timeouts(args.timeout_scale)
        self.port = None
        self.server = None
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.stats = collections.Counter()
        self.harness_errors = []
        self.imp_requests = queue.Queue()
        self.avoid = set(w.lower() for w in (args.avoid or []))
        self.started = time.time()
        os.makedirs(os.path.join(workdir, "transcripts"), exist_ok=True)
        self.cmdlog = open(os.path.join(workdir, "commands.log"), "w", encoding="utf-8")

    def log(self, who, text):
        stamp = "%9.3f" % (time.time() - self.started)
        with self.lock:
            self.cmdlog.write("%s %-8s %s\n" % (stamp, who, text))
            self.cmdlog.flush()

    def count(self, key, n=1):
        with self.lock:
            self.stats[key] += n

    def harness_error(self, who, exc):
        with self.lock:
            self.harness_errors.append("%s: %s" % (who, "".join(
                traceback.format_exception(type(exc), exc, exc.__traceback__)).strip()))

    def say(self, msg):
        print("[%7.1fs] %s" % (time.time() - self.started, msg), flush=True)


# --------------------------------------------------------------------------
# The server process.
# --------------------------------------------------------------------------

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def prepare_lib(src_lib, dst_lib):
    """Copy lib/ and empty the player database so the first character created
    becomes the implementor."""
    shutil.copytree(src_lib, dst_lib)
    for sub in ("plrfiles", "plrobjs", "plrvars"):
        top = os.path.join(dst_lib, sub)
        if not os.path.isdir(top):
            continue
        for root, _dirs, files in os.walk(top):
            for name in files:
                if name != "00":         # the shipped "00" files are placeholders
                    os.unlink(os.path.join(root, name))
    for stale in ("etc/plrmail", "etc/players", "etc/last", "etc/hcontrol"):
        path = os.path.join(dst_lib, stale)
        if os.path.isfile(path):
            os.unlink(path)


class Server:
    def __init__(self, ctx):
        self.ctx = ctx
        self.proc = None
        self.log_path = os.path.join(ctx.workdir, "server.log")
        self.vg_log = os.path.join(ctx.workdir, "valgrind.log")
        self.shutdown_requested = False
        self.died_early = None

    def start(self):
        args = self.ctx.args
        lib = os.path.join(self.ctx.workdir, "lib")
        cmd = [os.path.abspath(args.circle), "-q", "-d", lib, str(self.ctx.port)]
        if args.valgrind:
            cmd = ["valgrind", "--error-exitcode=99", "--leak-check=full",
                   "--errors-for-leak-kinds=definite,indirect", "--track-origins=yes",
                   "--num-callers=40", "--log-file=" + self.vg_log] + cmd
        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1:print_summary=1")
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
        self.ctx.say("starting: " + " ".join(cmd))
        with open(self.log_path, "wb") as log:
            self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                         cwd=self.ctx.workdir, env=env)
        deadline = time.time() + self.ctx.T.boot
        while True:
            if b"Entering game loop" in open(self.log_path, "rb").read():
                return
            if self.proc.poll() is not None:
                raise SystemExit("HARNESS: server exited during boot (status %s); see %s"
                                 % (self.proc.returncode, self.log_path))
            if time.time() > deadline:
                raise SystemExit("HARNESS: server did not boot in %ds" % self.ctx.T.boot)
            time.sleep(0.5)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def watch(self):
        """Watchdog thread: notice the server dying before we asked it to."""
        while not self.ctx.stop.is_set():
            if not self.alive():
                if not self.shutdown_requested:
                    self.died_early = self.proc.returncode
                    self.ctx.say("SERVER DIED (status %s) before shutdown was requested"
                                 % self.proc.returncode)
                self.ctx.stop.set()
                return
            time.sleep(0.25)

    def wait_exit(self):
        try:
            return self.proc.wait(timeout=self.ctx.T.shutdown)
        except subprocess.TimeoutExpired:
            self.ctx.say("server did not exit %ds after shutdown; sending SIGTERM to pid %d"
                         % (self.ctx.T.shutdown, self.proc.pid))
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=60 * self.ctx.T.scale)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            return None

    def kill(self):
        if self.alive():
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=30 * self.ctx.T.scale)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# --------------------------------------------------------------------------
# One telnet connection / character.
# --------------------------------------------------------------------------

class Bot:
    def __init__(self, ctx, name, cls="w", sex="m", imp=False, login_name=None):
        self.ctx = ctx
        self.name = name                 # label in logs and transcripts
        self.login_name = login_name or name
        self.cls = cls
        self.sex = sex
        self.imp = imp
        self.rng = random.Random("%s:%s" % (ctx.args.seed, name))
        self.sock = None
        self.state = "offline"          # offline, login, menu, game
        self.room = None
        self.last_room = None
        self.lost_steps = 0
        self.pending = ""
        self.backlog = ""               # async text received between commands
        self.transcript = open(os.path.join(ctx.workdir, "transcripts", name + ".log"),
                               "a", encoding="utf-8", errors="replace")
        self.blocked = set()            # (room, door) pairs that failed recently
        self.corpse_room = None

    # ---- low-level I/O ---------------------------------------------------
    def connect(self):
        deadline = time.time() + self.ctx.T.connect
        while True:
            if self.ctx.stop.is_set():
                raise Stopped()
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.ctx.port), timeout=10)
                break
            except OSError:
                if time.time() > deadline:
                    raise Disconnected("could not connect")
                time.sleep(1)
        self.sock.setblocking(False)
        self.pending = ""
        self.state = "login"
        self.ctx.log(self.name, "<connect>")
        self.ctx.count("connections")

    def close(self, abrupt=False):
        if self.sock is not None:
            try:
                if abrupt:
                    self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
        self.sock = None
        self.state = "offline"

    def _negotiate(self, data):
        """Refuse every telnet option the server offers (no MCCP, no MXP...)
        and drop the negotiation bytes."""
        replies = b""
        for m in re.finditer(rb"\xff([\xfb\xfd])(.)", data, re.S):
            replies += b"\xff" + (b"\xfe" if m.group(1) == b"\xfb" else b"\xfc") + m.group(2)
        if replies:
            try:
                self.sock.sendall(replies)
            except OSError:
                pass
        data = re.sub(rb"\xff\xfa.*?\xff\xf0", b"", data, flags=re.S)
        data = re.sub(rb"\xff[\xfb-\xfe].", b"", data, flags=re.S)
        return re.sub(rb"\xff[\xf0-\xfa\xff]", b"", data)

    def _fill(self, timeout):
        """Wait up to timeout for data; append it to self.pending."""
        if self.sock is None:
            raise Disconnected("not connected")
        ready, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        if not ready:
            return False
        try:
            data = self.sock.recv(65536)
        except BlockingIOError:
            return False
        except OSError as exc:
            self.close()
            raise Disconnected(str(exc))
        if not data:
            self.close()
            raise Disconnected("EOF")
        text = ANSI_RE.sub("", self._negotiate(data).decode("latin-1")).replace("\r", "")
        self.transcript.write(text)
        self.transcript.flush()
        self.pending += text
        return True

    def avoided(self, line):
        words = line.split()
        return bool(words) and words[0].lower() in self.ctx.avoid

    def send(self, line):
        if self.sock is None:
            raise Disconnected("not connected")
        self.ctx.log(self.name, line)
        try:
            self.sock.sendall(line.encode("latin-1", "replace") + b"\r\n")
        except OSError as exc:
            self.close()
            raise Disconnected(str(exc))
        self.transcript.write("\n>>> %s\n" % line)
        self.ctx.count("commands")

    def drain(self):
        while self._fill(0):
            pass
        text, self.pending = self.pending, ""
        if text:
            self.events(text)
        return text

    def wait_for(self, regex, timeout=None):
        """Read until regex matches the received text; returns the text."""
        pat = re.compile(regex, re.S | re.I)
        deadline = time.time() + (timeout if timeout is not None else self.ctx.T.login)
        while not pat.search(self.pending):
            if self.ctx.stop.is_set() and not self.ctx.server.shutdown_requested:
                raise Stopped()
            if time.time() > deadline:
                raise Disconnected("timed out waiting for %r; tail %r" % (regex, self.pending[-300:]))
            self._fill(min(1.0, max(0.0, deadline - time.time())))
        text, self.pending = self.pending, ""
        self.events(text)
        return text

    def read_reply(self, expect=None, timeout=None):
        """Read the reply to one command: up to the next prompt (game, menu,
        editor or login), answering the pager on the way.  With expect, keep
        reading (for a grace period after the prompt) until it matches."""
        timeout = self.ctx.T.cmd if timeout is None else timeout
        deadline = time.time() + timeout
        grace_end = None
        pages = 0
        pat = re.compile(expect, re.S) if expect else None
        while True:
            if self.ctx.stop.is_set() and not self.ctx.server.shutdown_requested:
                raise Stopped()
            text = self.pending
            m = PAGER_TAIL.search(text)
            if m:
                pages += 1
                choice = "q" if pages > 12 else self.rng.choice(["", "", "", "r", "b", "q", m.group(2)])
                self.send(choice)
                self.ctx.count("pager_replies")
                self.pending = text[:m.start()] + "[pager answered]\n"
                continue
            done = PROMPT_TAIL.search(text) or MENU_TAIL.search(text) or \
                EDITOR_TAIL.search(text) or LOGIN_TAILS.search(text)
            if done and (pat is None or pat.search(text)):
                break
            if done and pat is not None and grace_end is None:
                grace_end = time.time() + self.ctx.T.grace
            now = time.time()
            if now > deadline or (grace_end is not None and now > grace_end):
                if not done:
                    self.ctx.count("reply_timeouts")
                break
            self._fill(min(0.5, max(0.0, min(deadline, grace_end or deadline) - now)))
        text, self.pending = self.pending, ""
        self.events(text)
        return text

    def events(self, text):
        """React to things that happen to the character, whoever caused them."""
        self.backlog = (self.backlog + text)[-20000:]
        if "You are dead!" in text:
            self.ctx.count("deaths")
            self.corpse_room = self.room
            self.room = None
        if "is dead!  R.I.P." in text:
            self.ctx.count("kills_seen")
        if "completed your quest" in text:
            self.ctx.count("quests_completed")
        if "You join the quest" in text:
            self.ctx.count("quests_joined")
        if re.search(r"has (teleported|transferred|summoned) you|You flee head over heels|"
                     r"You feel yourself being pulled|disappears\.\s*$|You are hurled|"
                     r"panic, and attempt to flee", text, re.M):
            self.room = None
        located = self.ctx.world.locate(text, previous=self.last_room)
        if located is not None:
            self.room = located
            self.last_room = located
        tail = text.rstrip(" ")
        if MENU_TAIL.search(text) or tail.endswith("Make your choice:"):
            self.state = "menu"
        elif PROMPT_TAIL.search(text) and self.state != "offline":
            self.state = "game"
        if "Multiple login detected" in text:
            self.ctx.count("usurped")

    # ---- login -------------------------------------------------------------
    def create(self):
        self.connect()
        self.wait_for(r"By what name do you wish to be known\?")
        self.send(self.login_name)
        self.wait_for(r"Did I get that right")
        self.send("y")
        self.wait_for(r"Give me a password")
        self.send(PASSWORD)
        self.wait_for(r"Please retype password")
        self.send(PASSWORD)
        self.wait_for(r"What is your sex")
        self.send(self.sex)
        self.wait_for(r"Class:")
        self.send(self.cls)
        self.wait_for(r"PRESS RETURN")
        self.send("")
        self.wait_for(r"Make your choice")
        self.state = "menu"
        self.enter_game()
        self.ctx.count("characters_created")

    def login(self):
        """Log in over a new connection.  Handles both a fresh login (motd,
        menu) and taking over a character that is still in the game."""
        self.connect()
        self.wait_for(r"By what name do you wish to be known\?")
        self.send(self.login_name)
        self.wait_for(r"Password:")
        self.send(PASSWORD)
        text = self.wait_for(r"PRESS RETURN|Reconnecting|You take over your own body|"
                             r"Wrong password|Too many login|restricted")
        if "PRESS RETURN" in text:
            self.send("")
            self.wait_for(r"Make your choice")
            self.state = "menu"
            self.enter_game()
        elif "Reconnecting" in text or "take over" in text:
            self.ctx.count("reconnects")
            self.state = "game"
            self.room = None
            self.cmd("look")
        else:
            raise Disconnected("login refused: %r" % text[-200:])
        self.ctx.count("logins")

    def enter_game(self):
        self.send("1")
        self.wait_for(r"> $|\n> ")
        self.state = "game"
        self.room = None
        self.cmd("look")

    def ensure_game(self):
        if self.state == "offline":
            self.login()
        elif self.state == "menu":
            self.enter_game()
            self.ctx.count("menu_reentries")

    # ---- commands ------------------------------------------------------------
    def cmd(self, line, expect=None, timeout=None, editor=False):
        """Send one command and return what arrived since the previous one
        (anything asynchronous first, then the reply up to the prompt)."""
        self.ensure_game()
        before = self.drain()
        if self.state != "game":         # died (or was sent to the menu) meanwhile:
            self.ensure_game()           # never let a command land on the menu
            before += self.drain()
        if self.avoided(line):
            self.ctx.log(self.name, "<avoided: %s>" % line)
            self.ctx.count("avoided_commands")
            return before
        self.send(line)
        text = self.read_reply(expect, timeout)
        if EDITOR_TAIL.search(text) and not editor:   # in the text editor unexpectedly
            self.send("/a")
            self.read_reply()
        return before + text

    def hp(self):
        """Hit points from the last prompt seen, or None."""
        found = re.findall(r"(?:^|\n)(-?\d+)H [^\n]*> ", self.backlog[-4000:])
        return int(found[-1]) if found else None

    def quit_to_menu(self):
        """Type quit; True once the main menu is up.  The server sends a game
        prompt after "Goodbye" and the menu after that, so wait for it."""
        out = self.cmd("quit")
        if "Goodbye" in out and self.state != "menu":
            try:
                self.wait_for(r"Make your choice", self.ctx.T.cmd)
            except Disconnected:
                return False
            self.state = "menu"
        return self.state == "menu"

    def think(self, lo=0.05, hi=0.35):
        time.sleep(self.rng.uniform(lo, hi))

    # ---- movement ------------------------------------------------------------
    def move(self, door):
        here = self.room
        expected = None
        if here is not None:
            ex = self.ctx.world.rooms.get(here, {}).get("exits", {}).get(door)
            expected = ex[0] if ex else None
        text = self.cmd(DIRS[door], expect=r"\[ Exits:|pitch black|infinite darkness|" +
                        "|".join(re.escape(f) for f in MOVE_FAIL))
        located = self.ctx.world.locate(text, expected=expected, previous=here)
        if located is not None:
            self.room = located
            self.ctx.count("moves")
            return located != here
        if expected is not None and ("pitch black" in text or "infinite darkness" in text):
            self.room = expected
            self.ctx.count("moves")
            return True
        if any(f in text for f in MOVE_FAIL):
            self.room = here
            if "is closed" in text or "seems to be closed" in text:
                return self.open_and_retry(door, text)
            if "exhausted" in text:
                self.recover_moves()
            elif "on your feet" in text or "In your dreams" in text:
                self.cmd("wake")
                self.cmd("stand")
            if here is not None:
                self.blocked.add((here, door))
            return False
        self.room = None
        return False

    def open_and_retry(self, door, text):
        here = self.room
        ex = self.ctx.world.rooms.get(here, {}).get("exits", {}).get(door) if here else None
        keyword = ex[2] if ex else "door"
        out = self.cmd("open %s %s" % (keyword, DIRS[door]))
        self.ctx.count("doors_opened")
        if "locked" in out:
            if self.cls == "t":
                self.cmd("pick %s %s" % (keyword, DIRS[door]))
                self.cmd("open %s %s" % (keyword, DIRS[door]))
            else:
                if here is not None:
                    self.blocked.add((here, door))
                return False
        text = self.cmd(DIRS[door], expect=r"\[ Exits:|pitch black|" +
                        "|".join(re.escape(f) for f in MOVE_FAIL))
        located = self.ctx.world.locate(text, expected=ex[0] if ex else None, previous=here)
        if located is not None and located != here:
            self.room = located
            if self.rng.random() < 0.5:
                self.cmd("close %s %s" % (keyword, DIRS[(door + 2) % 4 if door < 4 else 9 - door]))
            return True
        if here is not None:
            self.blocked.add((here, door))
        return False

    def recover_moves(self):
        self.cmd("rest")
        time.sleep(self.rng.uniform(2, 5) * self.ctx.T.scale ** 0.5)
        self.cmd("stand")

    def identify(self):
        text = self.cmd("look", expect=r"\[ Exits:|pitch black|infinite darkness")
        self.room = self.ctx.world.locate(text, previous=self.last_room)
        return self.room

    def walk_to(self, target, rescue=None, zones=None):
        """Walk to target along the world graph.  rescue is a callable that
        puts the bot there some other way when walking fails."""
        for _attempt in range(3):
            if self.room is None and self.identify() is None:
                break
            if self.room == target:
                return True
            path = self.ctx.world.path(self.room, target, zones)
            if not path:
                break
            for door in path:
                if self.ctx.stop.is_set():
                    raise Stopped()
                if not self.move(door):
                    break
                self.think(0.0, 0.1)
            if self.room == target:
                return True
        if rescue is not None:
            rescue(self, target)
            self.identify()
        return self.room == target

    def wander(self, steps, zones=NEARBY_ZONES):
        for _ in range(steps):
            if self.ctx.stop.is_set():
                raise Stopped()
            if self.room is None:
                self.identify()
            if self.room is not None:
                options = [(d, t) for d, t in self.ctx.world.neighbours(self.room, zones)
                           if (self.room, d) not in self.blocked]
                if not options:
                    options = self.ctx.world.neighbours(self.room)
                if not options:
                    return
                door, _ = self.rng.choice(options)
                self.lost_steps = 0
            else:
                exits = EXITS_LINE.findall(self.backlog[-3000:])
                shown = [tok.strip("()") for tok in (exits[-1].split() if exits else [])]
                shown = [AUTOEXIT.index(t) for t in shown if t in AUTOEXIT[:6]]
                if not shown:
                    shown = list(range(6))
                door = self.rng.choice(shown)
                self.lost_steps += 1
                if self.lost_steps > 12:
                    self.ctx.imp_requests.put(("teleport", self, R_TEMPLE_SQUARE))
                    self.lost_steps = 0
                    time.sleep(2 * self.ctx.T.scale ** 0.5)
            self.move(door)
            self.think(0.0, 0.2)

    # ---- helpers for parsing replies -------------------------------------------
    def inventory(self):
        text = self.cmd("inventory", expect=r"You are carrying:")
        items = []
        if "You are carrying:" not in text:
            return items
        body = text.split("You are carrying:", 1)[1]
        for line in body.split("\n")[1:]:
            line = line.strip()
            if not line or line.endswith(">") or line == "Nothing.":
                if line.endswith(">"):
                    break
                continue
            line = re.sub(r"^\(\s*\d+\)\s*", "", line)
            line = re.sub(r"\s*(\([^)]*\)|\.\.[^.].*)$", "", line).strip()
            if line:
                items.append(self.keyword_for(line))
        return items

    def keyword_for(self, short):
        kw = self.ctx.world.obj_kw.get(short.lower())
        if kw:
            return kw
        words = re.findall(r"[A-Za-z]+", short)
        return words[-1].lower() if words else "all"

    def mobs_here(self, text=None):
        """Keywords of the mobs listed in a room display."""
        text = text if text is not None else self.cmd("look", expect=r"\[ Exits:|pitch black")
        found = []
        shown = list(EXITS_LINE.finditer(text))
        body = text[shown[-1].end():] if shown else text
        for line in body.split("\n"):
            line = line.strip()
            hit = self.ctx.world.mob_long.get(line)
            if hit:
                found.append(hit)
                continue
            for short, vnum, kw in self.ctx.world.mob_short:
                if line.startswith(short + " ") and len(short) > 3:
                    found.append((vnum, kw))
                    break
        return found

    def others_here(self, everyone):
        return [b for b in everyone if b is not self and b.room is not None and b.room == self.room]


# --------------------------------------------------------------------------
# Implementor-side helpers.
# --------------------------------------------------------------------------

def imp_teleport(imp, bot, room):
    imp.cmd("teleport %s %d" % (bot.name.lower(), room))
    imp.ctx.count("imp_teleports")


def make_rescuer(imp):
    def rescue(bot, room):
        imp_teleport(imp, bot, room)
        time.sleep(0.3 * imp.ctx.T.scale ** 0.5)
    return rescue


def imp_spawn(imp, bot, vnum, count=1):
    """Load count mobs into bot's room (the implementor types it)."""
    where = str(bot.room) if bot.room is not None else bot.name.lower()
    out = imp.cmd("at %s load mob %d %d" % (where, vnum, count))
    if "has created" not in out and "You create" not in out and bot.room is not None:
        imp.cmd("at %s load mob %d %d" % (bot.name.lower(), vnum, count))
    imp.ctx.count("imp_spawns")


SKILLS = {
    "w": ["kick", "bash", "rescue", "bandage", "track", "whirlwind"],
    "o": ["kick", "rescue", "bandage", "track"],
    "t": ["sneak", "hide", "backstab", "steal", "pick lock", "track"],
    "c": ["cure light", "armor", "bless", "create food", "create water", "detect invisibility",
          "cure critic", "summon", "word of recall", "sanctuary", "heal", "dispel evil",
          "earthquake", "remove poison", "group armor", "group heal", "identify",
          "protection from evil", "sense life", "call lightning", "harm", "cure blind",
          "detect alignment", "detect poison", "control weather", "remove curse", "infravision"],
    "z": ["magic missile", "detect invisibility", "detect magic", "chill touch", "invisibility",
          "armor", "burning hands", "locate object", "strength", "shocking grasp",
          "lightning bolt", "color spray", "fireball", "identify", "fly", "infravision",
          "sleep", "blindness", "curse", "energy drain", "charm person", "enchant weapon",
          "poison"],
}


# --------------------------------------------------------------------------
# Activities: each is one burst of ordinary play by one player.
# --------------------------------------------------------------------------

class Player(Bot):
    def __init__(self, ctx, name, cls, sex, everyone):
        Bot.__init__(self, ctx, name, cls, sex)
        self.everyone = everyone

    def other(self):
        others = [b for b in self.everyone if b is not self]
        return self.rng.choice(others) if others else self

    def peer_here(self):
        here = self.others_here(self.everyone)
        return self.rng.choice(here) if here else None

    def request(self, what, *args):
        self.ctx.imp_requests.put((what, self) + args)

    # -- information ---------------------------------------------------------
    def act_info(self):
        pool = ["look", "score", "inventory", "equipment", "who", "where", "time", "weather",
                "exits", "levels", "practice", "toggle", "gold", "whoami", "news", "motd",
                "credits", "info", "version", "policy", "commands", "socials", "areas",
                "wizlist", "immlist", "history", "alive", "alive server", "pulse",
                "help", "help kill", "help group", "help quest", "hindex a", "scan",
                "diagnose", "look up", "look north", "examine fountain", "who -l 1-50",
                "whois %s" % self.other().name.lower(), "where %s" % self.other().name.lower(),
                "consider fido", "consider beggar", "gold",
                "ping %s" % self.other().name.lower(), "look %s" % self.other().name.lower(),
                "quest", "quest history", "quest progress"]
        for line in self.rng.sample(pool, self.rng.randint(3, 7)):
            self.cmd(line)
            self.think()

    # -- chat and socials --------------------------------------------------------
    def act_chat(self):
        target = self.other().name.lower()
        pool = ["say Hello from %s, seed %s." % (self.name, self.ctx.args.seed),
                "tell %s Are you there?" % target, "reply Yes, still here.",
                "gossip Anyone seen a beastly fido?", "shout Market square in five!",
                "emote stretches and yawns.", "whisper %s psst" % target,
                "ask %s Where is the bakery?" % target, "grats %s" % target,
                "gsay Group check.", "gtell Ready?", "holler Midgaard rocks!",
                "auction A slightly used bag!", "qsay Mice everywhere.", "gemote waves.",
                "' Short for say.", "tell %s %s" % (target, "x" * self.rng.randint(1, 200))]
        for line in self.rng.sample(pool, self.rng.randint(2, 5)):
            self.cmd(line)
            self.think()

    SOCIALS = ["smile", "bow", "hug", "wave", "nod", "laugh", "grin", "dance", "clap",
               "comfort", "poke", "applaud", "giggle", "shrug", "wink", "sigh", "cheer",
               "bounce", "cackle", "thank", "pat", "tickle", "highfive", "salute"]

    def act_social(self):
        for _ in range(self.rng.randint(1, 4)):
            social = self.rng.choice(self.SOCIALS)
            arg = self.rng.choice(["", self.other().name.lower(), "self", "fido", "nobody"])
            self.cmd(("%s %s" % (social, arg)).strip())
            self.think()

    # -- walking ------------------------------------------------------------------
    def act_wander(self):
        self.ensure_standing()
        self.wander(self.rng.randint(3, 14))

    def ensure_standing(self):
        self.cmd("stand")

    # -- shops --------------------------------------------------------------------
    def act_shop(self):
        self.ensure_standing()
        shop = self.rng.choice(SHOP_ROOMS)
        if not self.walk_to(shop, zones={30}):
            return
        self.shop_here()

    def shop_here(self, buys=None):
        text = self.cmd("list", expect=r"Available|nothing for sale|cannot do that here|"
                        r"doesn't trade|not open")
        rows = re.findall(r"^\s*(\d+)\)\s+(\S+)\s+(.*?)\s+(\d+)\s*$", text, re.M)
        for _ in range(buys if buys is not None else self.rng.randint(1, 3)):
            if not rows:
                break
            num, _avail, short, _cost = self.rng.choice(rows)
            how = self.rng.random()
            if how < 0.4:
                line = "buy %s" % num
            elif how < 0.6:
                line = "buy #%s" % num
            elif how < 0.8:
                line = "buy %d %s" % (self.rng.randint(1, 3), self.keyword_for(short))
            else:
                line = "buy %s" % self.keyword_for(short)
            out = self.cmd(line)
            if "That'll be" in out or "That will be" in out or "You now have" in out or "You buy" in out:
                self.ctx.count("purchases")
            self.think()
        items = self.inventory()
        if items:
            item = self.rng.choice(items)
            self.cmd("value %s" % item)
            if self.rng.random() < 0.5:
                out = self.cmd("sell %s" % item)
                if "You'll get" in out or "coins for it" in out:
                    self.ctx.count("sales")
        if self.rng.random() < 0.2:
            self.cmd("buy nonexistentthing")
            self.cmd("sell nothinghere")

    # -- items --------------------------------------------------------------------
    def act_items(self):
        items = self.inventory()
        peer = self.peer_here()
        pool = ["get all", "get all.coins", "wear all", "remove all", "equipment",
                "get all corpse", "look in bag", "get all bag", "put all bag", "drink fountain",
                "sip fountain", "eat bread", "taste bread", "hold torch", "hold lantern",
                "grab staff", "quaff potion", "recite scroll", "use staff", "use wand",
                "sacrifice corpse", "get all.bread", "drop all.bread", "examine bag"]
        for _ in range(self.rng.randint(2, 6)):
            r = self.rng.random()
            if items and r < 0.45:
                item = self.rng.choice(items)
                verb = self.rng.choice(["drop", "wear", "wield", "remove", "hold", "put",
                                        "give", "junk", "donate", "examine", "look", "eat",
                                        "drink", "value", "recite", "quaff", "use", "pour"])
                if verb == "put":
                    line = "put %s bag" % item
                elif verb == "give":
                    line = "give %s %s" % (item, peer.name.lower() if peer else "fido")
                elif verb == "pour":
                    line = "pour %s out" % item
                else:
                    line = "%s %s" % (verb, item)
                if verb == "drop":
                    self.cmd(line)
                    line = "get %s" % item
            else:
                line = self.rng.choice(pool)
            self.cmd(line)
            if line.startswith("give") or line.startswith("drop"):
                items = self.inventory()
            self.think()
        if peer is not None and self.rng.random() < 0.3:
            self.cmd("give %d coins %s" % (self.rng.randint(1, 50), peer.name.lower()))

    def act_eatdrink(self):
        self.ensure_standing()
        if self.rng.random() < 0.5 and self.walk_to(R_TEMPLE_SQUARE, zones={30}):
            for line in ("drink fountain", "sip fountain", "look fountain", "fill bottle fountain",
                         "drink bottle"):
                self.cmd(line)
        else:
            for line in ("eat bread", "eat waybread", "taste danish", "drink bottle", "pour bottle out"):
                self.cmd(line)

    # -- groups ---------------------------------------------------------------------
    def act_group(self):
        peer = self.peer_here()
        if peer is None:
            peer = self.other()
        name = peer.name.lower()
        pool = ["group new", "group option open", "group join %s" % name, "group",
                "group list", "follow %s" % name, "report", "split %d" % self.rng.randint(1, 30),
                "gsay hi team", "group kick %s" % name, "group regroup", "group leave",
                "follow self", "group option anonymous", "assist %s" % name, "autoassist",
                "autosplit"]
        for line in self.rng.sample(pool, self.rng.randint(2, 6)):
            self.cmd(line)
            self.think()

    # -- fighting -------------------------------------------------------------------
    def act_fight(self):
        self.ensure_standing()
        mobs = [m for m in self.mobs_here() if m[0] in FODDER or m[0] == QUEST_TARGET]
        if not mobs:
            room = self.ctx.world.rooms.get(self.room or -1)
            if room is None or room["flags"] & (ROOM_NOMOB | ROOM_PEACEFUL):
                if not self.walk_to(self.rng.choice([R_FIGHT, R_QUEST, 3024, 3025, 3042]),
                                    zones={30}):
                    return
            vnum = self.rng.choice(list(FODDER))
            self.request("spawn", vnum, self.rng.randint(1, 2))
            mobs = [(vnum, FODDER[vnum])]
            self.wait_until(r"has created|appears|arrives", 3)
        vnum, kw = self.rng.choice(mobs)
        self.fight(kw)

    def wait_until(self, regex, seconds):
        deadline = time.time() + seconds * self.ctx.T.scale ** 0.5
        while time.time() < deadline:
            self._fill(0.2)
            if re.search(regex, self.pending):
                break
        self.drain()

    def fight(self, kw, rounds=12):
        if self.cls == "t" and self.rng.random() < 0.6:
            opener = "backstab %s" % kw
        else:
            opener = self.rng.choice(["kill %s", "hit %s", "kill %s"]) % kw
        out = self.cmd(opener)
        self.ctx.count("fights_started")
        if re.search(r"aren't here|Kill who|Hit who|Backstab who|no-one by that name|"
                     r"peaceful|don't see", out, re.I):
            return
        for _ in range(rounds):
            if self.ctx.stop.is_set():
                raise Stopped()
            if self.state != "game":
                return
            action = self.combat_action(kw)
            out = self.cmd(action)
            if re.search(r"is dead!  R\.I\.P\.|You are dead|aren't fighting|"
                         r"No-one by that name|They don't seem to be here|You flee", out):
                break
            time.sleep(self.rng.uniform(0.5, 1.5))
        if self.state == "game":
            self.cmd(self.rng.choice(["get all corpse", "get all.coins corpse", "look in corpse",
                                      "sacrifice corpse", "get all"]))
            self.ctx.count("fights_finished")

    def combat_action(self, kw):
        r = self.rng.random()
        if r < 0.08:
            return "flee"
        if r < 0.15:
            return self.rng.choice(["diagnose", "consider %s" % kw, "score", "look"])
        if self.cls == "w":
            return self.rng.choice(["kick %s" % kw, "bash %s" % kw, "whirlwind", "hit %s" % kw,
                                    "rescue %s" % self.other().name.lower()])
        if self.cls == "o":
            return self.rng.choice(["kick %s" % kw, "hit %s" % kw,
                                    "rescue %s" % self.other().name.lower(),
                                    "bandage %s" % self.other().name.lower()])
        if self.cls == "t":
            return self.rng.choice(["hit %s" % kw, "kill %s" % kw, "steal coins %s" % kw])
        if self.cls == "z":
            spell = self.rng.choice(["magic missile", "chill touch", "burning hands",
                                     "shocking grasp", "lightning bolt", "color spray",
                                     "fireball", "blindness", "curse", "energy drain"])
            return "cast '%s' %s" % (spell, kw)
        if self.cls == "c":
            spell = self.rng.choice(["dispel evil", "harm", "call lightning", "earthquake",
                                     "cure light", "cure critic", "heal"])
            target = kw if spell in ("dispel evil", "harm", "call lightning") else ""
            return ("cast '%s' %s" % (spell, target)).strip()
        return "hit %s" % kw

    # -- spells -------------------------------------------------------------------------
    def act_cast(self):
        peer = self.peer_here()
        ally = peer.name.lower() if peer else self.name.lower()
        if self.cls == "z":
            pool = ["cast 'detect invisibility'", "cast 'detect magic'", "cast 'armor' %s" % ally,
                    "cast 'strength' %s" % ally, "cast 'invisibility'", "visible",
                    "cast 'locate object' bread", "cast 'identify' bag", "cast 'fly'",
                    "cast 'infravision'", "cast 'enchant weapon' dagger", "cast 'sleep' fido",
                    "cast 'charm person' fido", "order followers sit", "order followers stand",
                    "cast 'invisibility' %s" % ally, "cast 'magic missile'", "cast 'clone'",
                    "order followers follow self"]
        elif self.cls == "c":
            pool = ["cast 'cure light' %s" % ally, "cast 'armor' %s" % ally,
                    "cast 'bless' %s" % ally, "cast 'create food'", "cast 'detect invisibility'",
                    "cast 'sanctuary' %s" % ally, "cast 'heal' %s" % ally, "cast 'group armor'",
                    "cast 'group heal'", "cast 'remove poison' %s" % ally,
                    "cast 'summon' %s" % self.other().name.lower(), "cast 'sense life'",
                    "cast 'protection from evil'", "cast 'control weather' better",
                    "cast 'cure blind' %s" % ally, "cast 'detect alignment'",
                    "cast 'identify' bread", "cast 'remove curse' %s" % ally,
                    "cast 'word of recall'", "cast 'create water' bottle"]
        elif self.cls == "t":
            pool = ["sneak", "hide", "visible", "track %s" % self.other().name.lower(),
                    "steal coins fido", "pick door", "sneak"]
        else:
            pool = ["track %s" % self.other().name.lower(), "bandage %s" % ally,
                    "rescue %s" % ally, "kick", "whirlwind"]
        for line in self.rng.sample(pool, min(len(pool), self.rng.randint(1, 4))):
            out = self.cmd(line)
            if line.startswith("cast"):
                self.ctx.count("casts")
            if "summon" in line or "recall" in line:
                self.room = None
            self.think(0.2, 0.8)

    # -- resting --------------------------------------------------------------------------
    def act_rest(self):
        seq = self.rng.choice([["rest", "stand"], ["sit", "stand"], ["sleep", "wake", "stand"],
                               ["rest", "sleep", "wake", "sit", "stand"], ["sleep", "stand", "wake"]])
        for line in seq:
            self.cmd(line)
            time.sleep(self.rng.uniform(0.2, 2.0))

    # -- quests ---------------------------------------------------------------------------
    def act_quest(self):
        self.ensure_standing()
        if not self.walk_to(R_QUEST, zones={30}):
            return
        if not any(v == QUESTMASTER for v, _ in self.mobs_here()):
            self.request("spawn", QUESTMASTER, 1)
            self.wait_until(r"has created", 3)
        for line in ("quest list", "quest join 1", "quest progress"):
            self.cmd(line)
            self.think()
        if self.rng.random() < 0.7:
            self.request("spawn", QUEST_TARGET, 1)
            self.wait_until(r"has created", 3)
            self.fight("mouse")
            self.cmd("quest progress")
        if self.rng.random() < 0.3:
            self.cmd("quest leave")
        self.cmd("quest history")

    # -- misc ------------------------------------------------------------------------------
    def act_save(self):
        self.cmd("save")
        self.ctx.count("saves")

    def act_bank(self):
        self.ensure_standing()
        if self.walk_to(R_TEMPLE, zones={30}):
            for line in ("balance", "deposit %d" % self.rng.randint(1, 500),
                         "withdraw %d" % self.rng.randint(1, 300), "balance", "deposit all",
                         "withdraw 999999999"):
                self.cmd(line)

    def act_toggles(self):
        pool = ["brief", "brief", "compact", "compact", "autoexits", "autoexits", "autoloot",
                "autogold", "autosplit", "autosac", "autoassist", "autodoor", "autokey",
                "noshout", "noshout", "notell", "notell", "nogossip", "nogossip", "norepeat",
                "afk", "afk", "toggle wimpy 20", "toggle wimpy 0", "title the soaker",
                "prompt all", "prompt auto", "prompt none", "prompt all", "display H",
                "toggle color off", "toggle color on", "alias xx look", "xx",
                "alias yy look;score", "yy", "alias xx", "alias", "toggle",
                "toggle screenwidth 80", "toggle pagelength 30", "toggle pagelength 5",
                "nosummon", "nosummon", "toggle autoloot on", "toggle brief off"]
        for line in self.rng.sample(pool, self.rng.randint(2, 6)):
            self.cmd(line)
        self.cmd("prompt all")
        self.cmd("toggle color on")

    def act_mail(self):
        self.ensure_standing()
        if not self.walk_to(R_POST, zones={30}):
            return
        out = self.cmd("mail %s" % self.other().name.lower(), editor=True)
        if EDITOR_TAIL.search(out) or "Write your message" in out:
            self.editor_session()
            self.ctx.count("mails")
        self.cmd("check")
        out = self.cmd("receive")
        if "gives you" in out or "piece of mail" in out:
            self.cmd("read mail")
            self.cmd(self.rng.choice(["junk mail", "drop mail", "put mail bag"]))

    def editor_session(self, torture=False):
        """At the editor's ']' prompt: type a few lines and save.  Default
        order is lifecycle-correct (write, then format/list/replace, then
        save) so mail/board/ibt sessions do not trip known editor bugs.  With
        torture=True (the editor activity) the formatting/edit/delete/insert
        commands are permuted, which is how the soak found the delete+insert
        overflow; mask it with --skip editor while that bug is open."""
        self.send("Soak test line one from %s." % self.name)
        self.read_reply()
        self.send("Second %sline." % ("long " * self.rng.randint(0, 20)))
        self.read_reply()
        edits = ["/l", "/f", "/c", "/r 'Second' 'Changed'", "/n"]
        if torture:
            edits += ["/d1", "/i1 Inserted first line", "/e2 Edited line two",
                      "/d2", "/i2 another", "/l 1 2", "/d1"]
            self.rng.shuffle(edits)
        for line in self.rng.sample(edits, self.rng.randint(1, len(edits))):
            self.send(line)
            self.read_reply()
            if self.state != "game" and not EDITOR_TAIL.search(self.backlog[-200:]):
                return                    # editor already saved/aborted us out
        self.send(self.rng.choice(["/s", "/s", "/s", "/a"]))
        self.read_reply()

    def act_editor(self):
        """Exercise the string editor through a scratch idea, permuting the
        line-editing commands (/d /i /e /l /f /c /r) to stress their buffer
        handling."""
        out = self.cmd("idea submit soak editor torture", editor=True)
        if EDITOR_TAIL.search(out) or "Terminate" in out or "Write" in out:
            self.editor_session(torture=True)
            self.ctx.count("editor_sessions")

    def act_board(self):
        self.ensure_standing()
        if not self.walk_to(R_READING_ROOM, zones={30}):
            return
        self.cmd("look board")
        out = self.cmd("write Soak from %s" % self.name, editor=True)
        if EDITOR_TAIL.search(out):
            self.editor_session()
            self.ctx.count("board_posts")
        self.cmd("read %d" % self.rng.randint(1, 3))
        if self.rng.random() < 0.3:
            self.cmd("remove %d" % self.rng.randint(1, 3))

    def act_pet(self):
        self.ensure_standing()
        if not self.walk_to(R_PETSHOP, zones={30}):
            return
        self.cmd("list")
        out = self.cmd("buy %s" % self.rng.choice(["kitten", "puppy", "beagle", "1"]))
        if "May you enjoy" in out or "pet" in out.lower():
            self.ctx.count("pets")
        for line in ("order followers sit", "order followers stand", "order followers smile"):
            self.cmd(line)

    def act_practice(self):
        room = GUILD_ROOM.get(self.cls)
        if room is None:
            self.cmd("practice")
            return
        self.ensure_standing()
        if self.walk_to(room, zones={30}):
            self.cmd("practice")
            skill = self.rng.choice(SKILLS[self.cls])
            self.cmd("practice %s" % skill)
            self.cmd("practice nosuchskill")

    def act_rent(self):
        self.ensure_standing()
        if not self.walk_to(R_RECEPTION, zones={30}):
            return
        self.cmd("offer")
        self.cmd("rent")                 # free rent: "Just quit, and your objects will be saved"
        if self.state == "menu" or self.quit_to_menu():
            self.ctx.count("rents")
            time.sleep(self.rng.uniform(0.5, 2.0))
            self.menu_extras()
            self.ensure_game()
            self.cmd("inventory")

    def act_quit_relogin(self):
        self.ensure_standing()
        if not self.quit_to_menu():
            return
        self.ctx.count("quits")
        self.menu_extras()
        if self.rng.random() < 0.5:
            self.send("0")
            try:
                self.wait_for(r"Goodbye", 10 * self.ctx.T.scale)
            except Disconnected:
                pass
            self.close()
            time.sleep(self.rng.uniform(0.2, 2.0))
            self.login()
        else:
            self.ensure_game()

    def menu_extras(self):
        """At the main menu: sometimes edit the description, read the
        background story, or change the password (to the same one)."""
        r = self.rng.random()
        if r < 0.25:
            self.send("2")
            out = self.read_reply()
            if EDITOR_TAIL.search(out) or "Enter the new text" in out:
                self.editor_session()
                self.ctx.count("descriptions")
        elif r < 0.45:
            self.send("3")              # background story: paged, then back to the menu
            for _ in range(30):
                deadline = time.time() + 2 * self.ctx.T.scale
                while time.time() < deadline and not (PAGER_TAIL.search(self.pending) or
                                                      MENU_TAIL.search(self.pending)):
                    self._fill(0.3)
                text, self.pending = self.pending, ""
                self.events(text)
                if MENU_TAIL.search(text):
                    break
                self.send("")
        elif r < 0.6:
            self.send("4")
            self.wait_for(r"old password")
            self.send(PASSWORD)
            self.wait_for(r"new password|Password:")
            self.send(PASSWORD)
            self.wait_for(r"retype password")
            self.send(PASSWORD)
            self.wait_for(r"Make your choice")
            self.ctx.count("password_changes")
        self.state = "menu"

    def act_quirks(self):
        """Input edge cases any player can type: history repeat and
        substitution, abbreviations, numbered and all. targets, over-long
        lines, tab colour codes, and $ % ~ in free text."""
        other = self.other().name.lower()
        pool = ["!", "^look^score", "^nothing^here", "l", "i", "eq", "sc", "inv", "LOOK",
                "  look   ", "look 2.fido", "look in 2.bag", "get 2.bread", "get all.bread",
                "drop all.bread", "get all.bread", "put bag bag", "give bread fido",
                "say \tRred\tn and \tGgreen\tn", "say \t[F500]rgb\t[B020]bg\tn",
                "say \t[u2665]heart\tn", "say \t[f123]odd\t[", "say \t\t\t",
                "emote " + "y" * 250, "say " + "x" * 600, "tell %s %s" % (other, "z" * 400),
                "say $n $N $e $s %s %d %n ~tilde~", "gossip 50% off at $5 and $$6",
                "title \tYshiny\tn %s title", "say \\t\\n", "alias zz say $1 and $*",
                "zz one two three", "alias zz", "whisper %s %s" % (other, "w" * 300),
                "''", "'", "say", "tell", "tell %s" % other, "emote", "give", "put", "get",
                "kill", "cast", "cast 'unknown spell'", "cast 'magic missile",
                "buy", "sell", "practice ''"]
        for line in self.rng.sample(pool, self.rng.randint(3, 8)):
            self.cmd(line)
            self.think(0.0, 0.2)
        self.ctx.count("quirk_bursts")

    def act_ibt(self):
        """The bug/idea/typo commands the MOTD asks players to use."""
        kind = self.rng.choice(["bug", "idea", "typo"])
        out = self.cmd("%s submit Soak %s from %s" % (kind, kind, self.name), editor=True)
        if EDITOR_TAIL.search(out):
            self.editor_session()
            self.ctx.count("ibt_submissions")
        for line in self.rng.sample(["%s list" % kind, "%s show 1" % kind, "%s show 99" % kind,
                                     kind, "%s remove 1" % kind, "%s submit" % kind], 3):
            out = self.cmd(line, editor=True)
            if EDITOR_TAIL.search(out):
                self.editor_session()

    def act_linkdrop(self):
        self.ctx.count("linkdrops")
        self.close(abrupt=True)
        self.ctx.log(self.name, "<link dropped>")
        time.sleep(self.rng.uniform(1.0, 6.0))
        self.login()

    def act_usurp(self):
        """Log in again over a live connection (the old one gets kicked)."""
        old = self.sock
        self.sock = None
        self.state = "offline"
        try:
            self.login()
        finally:
            if old is not None:
                old.close()

    def act_corpse_run(self):
        if self.corpse_room is None:
            return
        target, self.corpse_room = self.corpse_room, None
        self.ensure_standing()
        if self.walk_to(target, zones=NEARBY_ZONES):
            self.cmd("get all corpse")
            self.cmd("wear all")


ACTIVITIES = [  # name, weight
    ("wander", 16), ("info", 10), ("chat", 8), ("social", 5), ("shop", 8), ("items", 8),
    ("eatdrink", 3), ("group", 5), ("fight", 12), ("cast", 8), ("rest", 4), ("quest", 4),
    ("save", 2), ("bank", 2), ("toggles", 3), ("mail", 1.5), ("board", 1.5), ("pet", 1),
    ("practice", 1.5), ("rent", 1), ("quit_relogin", 1.5), ("linkdrop", 1.5), ("usurp", 0.7),
    ("corpse_run", 1), ("quirks", 3), ("ibt", 1), ("editor", 2),
]


# --------------------------------------------------------------------------
# The implementor.
# --------------------------------------------------------------------------

def imp_serve(ctx, imp, deadline):
    """Random-phase implementor: answer the players' setup requests and keep
    them alive, until the deadline."""
    next_restore = time.time() + 20
    while time.time() < deadline and not ctx.stop.is_set():
        try:
            what, bot, *rest = ctx.imp_requests.get(timeout=1.0)
        except queue.Empty:
            what = None
        try:
            if what == "spawn":
                imp_spawn(imp, bot, *rest)
            elif what == "teleport":
                imp_teleport(imp, bot, *rest)
            if time.time() > next_restore:
                imp.cmd("restore all")
                imp.cmd(imp.rng.choice(["who", "where", "users", "where fido", "time"]))
                if imp.rng.random() < 0.3:
                    imp.cmd("zreset 30")      # what a zone reset does with players about
                next_restore = time.time() + imp.rng.uniform(20, 45)
        except Disconnected:
            if not ctx.server.alive():
                raise Stopped()
            imp.login()


# --------------------------------------------------------------------------
# Phases.
# --------------------------------------------------------------------------

def setup(ctx, imp, players):
    """Create everyone and make the players sturdy enough to play."""
    imp.create()
    out = imp.cmd("holylight")
    if "off" in out.lower():
        imp.cmd("holylight")
    imp.cmd("goto %d" % R_TEMPLE)
    ctx.say("implementor %s created" % imp.name)
    errors = []

    def make(p):
        try:
            p.create()
        except Exception as exc:          # noqa: BLE001 - reported below
            errors.append((p.name, exc))

    threads = [threading.Thread(target=make, args=(p,)) for p in players]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise SystemExit("HARNESS: could not create characters: %s" % errors)
    ctx.say("players created in parallel: %s" % ", ".join(p.name for p in players))
    for p in players:
        n = p.name.lower()
        out = imp.cmd("advance %s %d" % (n, PLAYER_LEVEL))
        if "Okay" not in out:
            raise SystemExit("HARNESS: advance %s failed: %r" % (n, out[-200:]))
        imp.cmd("set %s gold 50000" % n)
        for skill in SKILLS.get(p.cls, []):
            imp.cmd("skillset %s '%s' 95" % (n, skill))
        imp.cmd("restore %s" % n)


def scripted(ctx, imp, players):
    """Walk every player through the whole gameplay surface once."""
    rescue = make_rescuer(imp)
    by = {p.cls: p for p in players}
    lead = players[0]
    names = [p.name.lower() for p in players]

    def all_cmd(line):
        for p in players:
            p.cmd(line.replace("$me", p.name.lower()))

    def gather(room):
        for p in players:
            p.cmd("stand")
            p.walk_to(room, rescue=rescue, zones={30})

    ctx.say("scripted: information commands")
    for line in ("look", "score", "inventory", "equipment", "who", "where", "time", "weather",
                 "exits", "levels", "practice", "toggle", "help", "commands", "socials", "news"):
        all_cmd(line)

    ctx.say("scripted: shopping")
    plan = [(R_GENERAL, ["buy bag", "buy torch", "buy 2 #1"]),
            (R_BAKERY, ["buy 3 bread", "buy waybread", "buy danish", "value bread", "sell bread"]),
            (R_WEAPONS, None), (R_ARMORY, None),
            (R_MAGIC, ["buy recall", "buy yellow", "buy identify", "value recall"]),
            (R_BAR, ["buy bottle", "buy 2 #1"])]
    weapon = {"w": "sword", "t": "dagger", "z": "dagger", "c": "club", "o": None}
    for room, lines in plan:
        gather(room)
        for p in players:
            p.cmd("list")
            if lines is not None:
                for line in lines:
                    out = p.cmd(line)
                    if "That'll be" in out or "That will be" in out:
                        ctx.count("purchases")
            elif room == R_WEAPONS and weapon.get(p.cls):
                p.cmd("buy %s" % weapon[p.cls])
                p.cmd("buy dagger")
                p.cmd("value dagger")
                out = p.cmd("sell dagger")
                if "coins for it" in out:
                    ctx.count("sales")
            elif room == R_ARMORY:
                p.shop_here(buys=3)

    ctx.say("scripted: items")
    gather(R_TEMPLE_SQUARE)
    for p in players:
        for line in ("wear all", "wield %s" % (weapon.get(p.cls) or "dagger"), "hold torch",
                     "equipment", "remove torch", "hold torch", "put bread bag",
                     "look in bag", "get bread bag", "put all.bread bag", "get all bag",
                     "drink fountain", "eat bread", "fill bottle fountain", "drink bottle",
                     "pour bottle out", "quaff yellow", "recite identify bag", "examine bag",
                     "drop torch", "get torch", "junk danish", "donate waybread"):
            p.cmd(line)
    for i, p in enumerate(players):
        nxt = players[(i + 1) % len(players)]
        p.cmd("give bread %s" % nxt.name.lower())
        p.cmd("give 25 coins %s" % nxt.name.lower())
    ctx.say("scripted: talking")
    for i, p in enumerate(players):
        nxt = names[(i + 1) % len(names)]
        for line in ("say Hello everyone", "tell %s hi" % nxt, "gossip soak test", "emote waves.",
                     "whisper %s secret" % nxt, "shout hey", "smile", "bow %s" % nxt,
                     "hug %s" % nxt, "ask %s ready?" % nxt):
            p.cmd(line)
    for p in players:
        p.cmd("reply thanks")

    ctx.say("scripted: groups and following")
    lead.cmd("group new")
    lead.cmd("group option open")
    for p in players[1:]:
        p.cmd("group join %s" % lead.name.lower())
        p.cmd("follow %s" % lead.name.lower())
    lead.cmd("group")
    lead.cmd("group list")
    lead.cmd("report")
    lead.cmd("gsay to the alley")
    lead.cmd("split 100")
    for p in players[1:]:
        p.room = None
    lead.walk_to(R_FIGHT, rescue=rescue, zones={30})
    time.sleep(1.0 * ctx.T.scale ** 0.5)
    for p in players[1:]:
        p.identify()
        if p.room != R_FIGHT:
            p.walk_to(R_FIGHT, rescue=rescue, zones={30})

    ctx.say("scripted: fighting")
    for p in players:
        p.cmd("autoassist")
    imp_spawn(imp, lead, 3062, 3)
    imp_spawn(imp, lead, 3060, 1)
    lead.drain()
    if "t" in by:
        by["t"].cmd("wield dagger")
        by["t"].cmd("backstab fido")
    lead.cmd("kill fido")
    for p in players[1:]:
        p.cmd("assist %s" % lead.name.lower())
    for _round in range(10):
        for p in players:
            p.cmd(p.combat_action(p.rng.choice(["fido", "cityguard"])))
        time.sleep(0.5)
    for p in players:
        p.cmd("get all corpse")
        p.cmd("get all.coins")
    lead.cmd("sacrifice corpse")
    lead.cmd("rescue %s" % players[1].name.lower())
    lead.cmd("flee")
    lead.room = None
    lead.walk_to(R_FIGHT, rescue=rescue, zones={30})

    ctx.say("scripted: spells")
    for p in players:
        p.act_cast()
    if "c" in by:
        c = by["c"]
        for line in ("cast 'armor' %s" % lead.name.lower(), "cast 'bless' %s" % lead.name.lower(),
                     "cast 'sanctuary' %s" % lead.name.lower(), "cast 'create food'",
                     "cast 'group armor'", "cast 'heal' %s" % lead.name.lower()):
            c.cmd(line)
    if "z" in by:
        z = by["z"]
        for line in ("cast 'detect invisibility'", "cast 'invisibility'", "visible",
                     "cast 'strength' %s" % lead.name.lower(), "cast 'locate object' bread",
                     "cast 'identify' bag"):
            z.cmd(line)

    ctx.say("scripted: quests")
    gather(R_QUEST)
    imp_spawn(imp, lead, QUESTMASTER, 1)
    for p in players:
        p.cmd("quest list")
        p.cmd("quest join 1")
        p.cmd("quest progress")
    imp_spawn(imp, lead, QUEST_TARGET, 3)
    lead.drain()
    lead.fight("mouse", rounds=20)
    lead.fight("mouse", rounds=20)
    lead.fight("mouse", rounds=20)
    for p in players:
        p.cmd("quest progress")
        p.cmd("quest history")
    for p in players[1:]:
        p.cmd("quest leave")
    for p in players:
        p.cmd("group leave")
        p.cmd("follow self")

    ctx.say("scripted: guild practice, resting, bank")
    for p in players:
        p.act_practice()
    for p in players:
        for line in ("rest", "sit", "stand", "sleep", "wake", "stand"):
            p.cmd(line)
    gather(R_TEMPLE)
    for p in players:
        for line in ("balance", "deposit 1000", "withdraw 500", "balance"):
            p.cmd(line)

    ctx.say("scripted: post office, board, pet shop")
    a, b = players[0], players[1]
    if a.walk_to(R_POST, rescue=rescue, zones={30}):
        out = a.cmd("mail %s" % b.name.lower(), editor=True)
        if EDITOR_TAIL.search(out) or "Write your message" in out:
            a.editor_session()
            ctx.count("mails")
    if b.walk_to(R_POST, rescue=rescue, zones={30}):
        b.cmd("check")
        b.cmd("receive")
        b.cmd("read mail")
        b.cmd("junk mail")
    c = players[2]
    if c.walk_to(R_READING_ROOM, rescue=rescue, zones={30}):
        c.cmd("look board")
        out = c.cmd("write A soak test post", editor=True)
        if EDITOR_TAIL.search(out):
            c.editor_session()
            ctx.count("board_posts")
        c.cmd("read 1")
    d = players[3]
    if d.walk_to(R_PETSHOP, rescue=rescue, zones={30}):
        d.cmd("list")
        d.cmd("buy kitten")
        d.cmd("order followers sit")
        d.cmd("order followers stand")

    ctx.say("scripted: rent, quit and re-login")
    e = players[min(4, len(players) - 1)]
    if e.walk_to(R_RECEPTION, rescue=rescue, zones={30}):
        e.cmd("offer")
        e.cmd("rent")
        if e.quit_to_menu():
            ctx.count("rents")
            e.send("2")
            out = e.read_reply()
            if EDITOR_TAIL.search(out):
                e.editor_session()
                ctx.count("descriptions")
        e.ensure_game()
        e.cmd("inventory")
    c.act_quit_relogin()
    if c.quit_to_menu():
        c.send("0")
        c.close()
        c.login()
        ctx.count("quits")

    ctx.say("scripted: link-drops and reconnects")
    d.act_linkdrop()
    a.walk_to(R_FIGHT, rescue=rescue, zones={30})
    imp_spawn(imp, a, 3060, 1)
    a.drain()
    a.cmd("kill cityguard")
    a.ctx.count("linkdrops")
    a.close(abrupt=True)
    a.ctx.log(a.name, "<link dropped mid-fight>")
    imp.cmd("where %s" % a.name.lower())
    imp.cmd("users")
    time.sleep(4 * ctx.T.scale ** 0.5)
    a.login()
    a.cmd("score")
    b.act_usurp()
    b.cmd("look")

    ctx.say("scripted: a death")
    victim = players[-1]
    victim.cmd("stand")
    victim.walk_to(R_FIGHT, rescue=rescue, zones={30})
    victim.cmd("wear all")
    imp.cmd("set %s hit 1" % victim.name.lower())
    imp_spawn(imp, victim, 3060, 2)
    victim.drain()
    victim.cmd("kill cityguard")
    for _round in range(60):
        if victim.state != "game":
            break
        hp = victim.hp()
        if hp is not None and hp <= -3:
            victim.cmd("quit")        # below stunned: "You die before your time..."
        else:
            victim.cmd(victim.rng.choice(["hit cityguard", "look", "kill cityguard"]))
        time.sleep(1.0)
    victim.ensure_game()
    victim.act_corpse_run()
    imp.cmd("restore all")

    ctx.say("scripted: save")
    all_cmd("save")


def random_phase(ctx, imp, players):
    deadline = time.time() + ctx.args.minutes * 60
    skip = set(ctx.args.skip or [])
    acts = [(n, w) for n, w in ACTIVITIES if n not in skip]
    names = [n for n, _ in acts]
    weights = [w for _, w in acts]

    def run(p):
        while time.time() < deadline and not ctx.stop.is_set():
            act = p.rng.choices(names, weights)[0]
            ctx.log(p.name, "<activity %s>" % act)
            ctx.count("activity_" + act)
            try:
                getattr(p, "act_" + act)()
                if p.corpse_room is not None and "corpse_run" not in skip:
                    p.act_corpse_run()
            except Stopped:
                return
            except Disconnected as exc:
                if not ctx.server.alive() or ctx.stop.is_set():
                    return
                ctx.count("unexpected_disconnects")
                ctx.log(p.name, "<disconnected: %s>" % exc)
                try:
                    p.close()
                    p.login()
                except (Disconnected, Stopped):
                    if not ctx.server.alive():
                        return
            except Exception as exc:      # noqa: BLE001 - a harness bug: report it
                ctx.harness_error(p.name, exc)
                ctx.count("harness_errors")
                if not ctx.server.alive():
                    return
            p.think(0.05, 0.5)

    def run_imp():
        try:
            imp_serve(ctx, imp, deadline)
        except Stopped:
            pass
        except Exception as exc:          # noqa: BLE001
            ctx.harness_error(imp.name, exc)

    threads = [threading.Thread(target=run, args=(p,), name=p.name) for p in players]
    threads.append(threading.Thread(target=run_imp, name=imp.name))
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        for t in threads:
            t.join(timeout=1.0)
        if ctx.stop.is_set():
            break
    for t in threads:
        t.join(timeout=60 * ctx.T.scale)


# --------------------------------------------------------------------------
# Replay scripts (reproducers).
# --------------------------------------------------------------------------

def replay(ctx, path):
    bots = {}
    imp = None
    for lineno, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        ctx.say("replay %d: %s" % (lineno, line))
        if not ctx.server.alive():
            ctx.say("server is gone; stopping the replay")
            break
        try:
            words = line.split()
            if words[0] == "create":
                bot = Bot(ctx, words[1], words[2] if len(words) > 2 else "w", "m", imp=imp is None)
                bots[words[1].lower()] = bot
                imp = imp or bot
                bot.create()
            elif words[0] == "login":
                if len(words) == 4 and words[2] == "as":
                    key = words[3].lower()
                    bots[key] = Bot(ctx, words[3], login_name=words[1])
                else:
                    key = words[1].lower()
                    bots.setdefault(key, Bot(ctx, words[1]))
                bots[key].login()
            elif words[0] == "linkdrop":
                bots[words[1].lower()].close(abrupt=True)
            elif words[0] == "sleep":
                time.sleep(float(words[1]) * ctx.T.scale ** 0.5)
            elif words[0] == "expect":
                bots[words[1].lower()].wait_for(line.split(None, 2)[2], ctx.T.cmd)
            else:
                m = re.match(r"^(\w+)(!?):\s?(.*)$", line)
                if not m:
                    raise SystemExit("HARNESS: %s:%d: cannot parse %r" % (path, lineno, line))
                bot = bots[m.group(1).lower()]
                if m.group(2):
                    bot.send(m.group(3))
                    time.sleep(0.3)
                else:
                    bot.cmd(m.group(3))
        except (Disconnected, Stopped) as exc:
            ctx.say("replay %d: %s" % (lineno, exc))
            if not ctx.server.alive():
                break
    return imp


# --------------------------------------------------------------------------
# Verdict.
# --------------------------------------------------------------------------

def scan_logs(ctx, server):
    problems = []
    log = open(server.log_path, encoding="latin-1").read()
    reports = [m.start() for m in SANITIZER_RE.finditer(log)]
    if reports:
        first = reports[0]
        start = log.rfind("\n", 0, max(0, first - 200)) + 1
        problems.append("sanitizer report in server.log:\n" + log[start:start + 6000])
    syserrs = [l for l in log.split("\n") if "SYSERR" in l or "TB_ERROR" in l or "[ERROR]" in l]
    if ctx.args.valgrind and os.path.exists(server.vg_log):
        vg = open(server.vg_log, encoding="latin-1").read()
        errs = re.findall(r"ERROR SUMMARY: ([\d,]+) errors", vg)
        definite = re.findall(r"definitely lost: ([\d,]+) bytes", vg)
        indirect = re.findall(r"indirectly lost: ([\d,]+) bytes", vg)
        n_err = int(errs[-1].replace(",", "")) if errs else -1
        n_def = int(definite[-1].replace(",", "")) if definite else 0
        n_ind = int(indirect[-1].replace(",", "")) if indirect else 0
        ctx.stats["valgrind_errors"] = n_err
        ctx.stats["valgrind_definitely_lost"] = n_def
        ctx.stats["valgrind_indirectly_lost"] = n_ind
        if n_err != 0 or n_def or n_ind:
            first = vg.find("\n==", vg.find("Invalid") if "Invalid" in vg else 0)
            problems.append("valgrind: %s errors, %d bytes definitely lost, %d indirectly lost; "
                            "see %s" % (errs[-1] if errs else "no ERROR SUMMARY", n_def, n_ind,
                                        server.vg_log))
    return problems, syserrs


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--circle", required=False, help="server binary")
    ap.add_argument("--lib", default=os.path.join(repo, "lib"))
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--minutes", type=float, default=3.0)
    ap.add_argument("--players", type=int, default=5)
    ap.add_argument("--valgrind", action="store_true")
    ap.add_argument("--timeout-scale", type=float, default=None)
    ap.add_argument("--skip", type=lambda s: [x for x in s.split(",") if x], default=[])
    ap.add_argument("--avoid", type=lambda s: [x for x in s.split(",") if x], default=[])
    ap.add_argument("--list-activities", action="store_true")
    ap.add_argument("--no-scripted", action="store_true")
    ap.add_argument("--replay")
    ap.add_argument("--workdir")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--port", type=int)
    args = ap.parse_args()
    if args.list_activities:
        print("\n".join(n for n, _ in ACTIVITIES))
        return 0
    if not args.circle:
        ap.error("--circle is required")
    if not 4 <= args.players <= 6:
        ap.error("--players must be 4-6")
    bad = set(args.skip) - {n for n, _ in ACTIVITIES}
    if bad:
        ap.error("unknown activities in --skip: %s" % ", ".join(sorted(bad)))
    if args.seed is None:
        args.seed = random.SystemRandom().randrange(1, 10 ** 6)
    if args.timeout_scale is None:
        args.timeout_scale = 12.0 if args.valgrind else 1.0

    workdir = args.workdir or tempfile.mkdtemp(prefix="tb-soak-")
    os.makedirs(workdir, exist_ok=True)
    prepare_lib(args.lib, os.path.join(workdir, "lib"))
    world = World(os.path.join(workdir, "lib"))
    ctx = Ctx(args, workdir, world)
    ctx.port = args.port or free_port()
    ctx.say("seed=%d minutes=%g players=%d mode=%s workdir=%s port=%d rooms=%d"
            % (args.seed, args.minutes, args.players, "valgrind" if args.valgrind else "native",
               workdir, ctx.port, len(world.rooms)))
    server = Server(ctx)
    ctx.server = server
    harness_failure = None
    imp = None
    try:
        server.start()
        watchdog = threading.Thread(target=server.watch, daemon=True)
        watchdog.start()
        if args.replay:
            imp = replay(ctx, args.replay)
        else:
            players = []
            imp = Bot(ctx, IMP_NAME, "w", "m", imp=True)
            for name, cls, sex in PLAYER_ROSTER[:args.players]:
                players.append(Player(ctx, name, cls, sex, players))
            setup(ctx, imp, players)
            if not args.no_scripted:
                ctx.say("scripted phase")
                scripted(ctx, imp, players)
            ctx.say("random phase: %g minutes" % args.minutes)
            random_phase(ctx, imp, players)
            ctx.say("random phase over")
    except SystemExit as exc:
        harness_failure = str(exc)
    except (Disconnected, Stopped) as exc:
        if server.alive():
            harness_failure = "HARNESS: %s: %s" % (type(exc).__name__, exc)
            ctx.harness_error("main", exc)
    except Exception as exc:              # noqa: BLE001
        harness_failure = "HARNESS: %s" % exc
        ctx.harness_error("main", exc)

    # Shut the server down the way an implementor does.
    exit_status = None
    if server.alive():
        try:
            if imp is None or imp.sock is None:
                imp = imp or Bot(ctx, IMP_NAME, imp=True)
                imp.login()
            server.shutdown_requested = True
            imp.send("shutdown die")
        except (Disconnected, Stopped) as exc:
            ctx.say("could not send shutdown: %s" % exc)
        exit_status = server.wait_exit()
    else:
        exit_status = server.proc.returncode if server.proc else None
    ctx.stop.set()

    problems, syserrs = scan_logs(ctx, server)
    if server.died_early is not None:
        problems.insert(0, "server died during play (status %s)" % server.died_early)
    elif exit_status != 0:
        problems.insert(0, "server exit status %s after shutdown die" % exit_status)
    if ctx.harness_errors and not harness_failure:
        harness_failure = "HARNESS: %d bot exception(s)" % len(ctx.harness_errors)

    verdict = "FAIL" if problems else ("HARNESS-ERROR" if harness_failure else "PASS")
    summary = dict(verdict=verdict, seed=args.seed, minutes=args.minutes, players=args.players,
                   valgrind=args.valgrind, exit_status=exit_status, problems=problems,
                   harness_failure=harness_failure, harness_errors=ctx.harness_errors,
                   stats=dict(sorted(ctx.stats.items())), syserr_lines=len(syserrs),
                   syserr_sample=syserrs[:20], workdir=workdir,
                   elapsed=round(time.time() - ctx.started, 1))
    with open(os.path.join(workdir, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2)

    print("=" * 72)
    print("%s  seed=%d mode=%s elapsed=%.0fs exit_status=%s"
          % (verdict, args.seed, "valgrind" if args.valgrind else "native",
             summary["elapsed"], exit_status))
    print("counters: " + ", ".join("%s=%s" % kv for kv in sorted(ctx.stats.items())))
    print("SYSERR/error lines in server log: %d" % len(syserrs))
    for p in problems:
        print("PROBLEM: " + p)
    if harness_failure:
        print(harness_failure)
    for e in ctx.harness_errors[:5]:
        print("harness exception: " + e)
    keep = args.keep or verdict != "PASS"
    print("work directory: %s%s" % (workdir, "" if keep else " (removed)"))
    if not keep:
        shutil.rmtree(workdir, ignore_errors=True)
    if problems:
        return 1
    if harness_failure:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
