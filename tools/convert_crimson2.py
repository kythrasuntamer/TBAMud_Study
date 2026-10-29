#!/usr/bin/env python3
"""Convert a Crimson 2 area (.wld/.mob/.obj/.zon) into tbaMUD world data.

Field meanings follow Crimson 2's own loader (src/db.c and structs.h at
c2admins/Crimson-2-MUD 2b7207b): rooms db4300_boot_world, mobiles
db5000_read_mobile, objects db5100_read_object, zones db4700_boot_zones.
Every value is mapped by meaning, never copied by bit position; anything
tbaMUD cannot represent is dropped and reported as "file:line vnum: ...".
An unresolved reference or an unconvertible value is an error: nothing is
written and the exit status is 1.

Two modes:
  zone   the area becomes its own zone: <zone>.{wld,mob,obj,zon}
  merge  only the mobiles/objects named in "map" (old vnum -> new vnum) and
         the resets that load them are converted, as <zone>.{mob,obj,zon}.add
         fragments for an existing stock zone whose rooms the area shares.

The installed Crimson 2 content is described by
imports/quarantine/crimson2/recipe.json; reproduce and install it with

    convert_crimson2.py --recipe imports/quarantine/crimson2/recipe.json \\
        --area Shatterstone --out /tmp/c2 [--install lib/world]

A recipe area may also carry "resets" (zone commands in source vnums, for a
zone whose upstream file has none; checked like the source's own), "exits"
(replacement text for a converted exit) and "lifespan"/"reset_mode".

--install copies a zone (and lists it in the indexes) or appends merge
fragments before the stock files' end markers; it refuses to overwrite or to
append twice. Validate (tools/validate_import_refs.py,
tools/audit_world_vnums.py) and boot-test afterwards.
"""
import argparse
import json
import re
import tempfile
import os
import sys
from pathlib import Path

MAX_VNUM = 65534                          # IDXTYPE; 65535 is NOWHERE
DIRS = {"north": 0, "east": 1, "south": 2, "west": 3, "up": 4, "down": 5}

# --- Crimson -> tbaMUD tables ----------------------------------------------
ROOM1 = {0: 0, 1: 1, 2: 2, 3: 3, 7: 7, 8: 8, 9: 9, 16: 4, 18: 5, 21: 6}
ROOM1_RUNTIME = {20}                      # RM1_BFS_MARK is runtime state
ROOM1_NAMES = {4: "ANTI_GOOD", 5: "ANTI_NEUTRAL", 6: "ANTI_EVIL", 10: "DRAIN_MAGIC",
               11: "HARMFULL1", 12: "HARMFULL2", 14: "NOSTEAL", 15: "NOPKILL (PvP)",
               17: "WATER", 19: "DUMP", 22: "MAZE", 23: "ANTI_MAGIC", 24: "INN",
               25: "MYST", 26: "FALL_DMG", 28: "NO_TELEPORT_IN",
               29: "NO_TELEPORT_OUT", 30: "NO_SUMMON_OUT"}
ROOM2_NAMES = {20: "ARENA (PvP)"}
SECTOR = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 9}   # underwater 8 -> 9
ACT1 = {1: 1, 2: 2, 3: 3, 6: 6, 7: 7, 13: 8, 14: 10, 15: 9, 18: 13, 29: 18}
ACT1_NAMES = {0: "UNFILTER", 4: "CLASS_DEMON", 5: "CHAR_WAS_HELD", 8: "SIGNAL_HUNT_HELP",
              9: "GUARD1", 10: "GUARD2", 11: "NOSTEAL", 12: "INJURED_ATTACK",
              16: "POISONOUS", 17: "SHOW_DIAG", 19: "DRAIN_XP", 20: "CLASS_SHADOW",
              21: "CLASS_VAMPIRE", 22: "CLASS_DRAGON", 23: "CLASS_GIANT",
              24: "CLASS_ANIMAL", 25: "CLASS_UNDEAD", 26: "CANT_HUNT",
              27: "HUNTASSIST", 28: "NOFOLLOW", 30: "NOHOLD"}
AFF = {0: 1, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 7: 8, 9: 7, 10: 10, 12: 12, 13: 13,
       14: 17, 17: 15, 19: 19, 20: 20, 24: 18, 25: 11}
AFF_NAMES = {6: "HASTE", 8: "GROUP", 11: "SILENCE", 15: "RAGE", 16: "HOLD_PERSON",
             18: "BLOCK_SUMMON", 21: "DEMON_FLESH", 22: "CHARM", 23: "FOLLOW",
             26: "REGENERATION", 27: "TRANSFORMED", 28: "HEMMORAGE",
             29: "MAGIC_RESIST", 30: "MAGIC_IMMUNE"}
ATTACK = {0: 0, 1: 5, 2: 11, 3: 3, 4: 2, 5: 8, 6: 4, 7: 1, 8: 6}  # Crimson TYPE_HIT=100 order
ATTACK_NAMES = {9: "ARROW", 10: "DUALWIELD"}
OBJ_TYPES = {1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 22}
OBJ1 = {0: 0, 1: 1, 5: 5, 6: 6, 7: 7, 8: 8, 9: 9, 10: 10, 11: 11, 12: 12,
        13: 14, 14: 13, 15: 15, 18: 2, 30: 17}     # note the thief/cleric swap
OBJ1_LEVEL = {19: 10, 20: 20, 21: 30, 22: 41}
OBJ1_NAMES = {2: "DARK", 3: "OMAX", 4: "EVIL", 16: "ANTI_DRAGON", 17: "GOOD",
              23: "NO_SEE", 24: "SMALL_ONLY", 25: "LARGE_ONLY", 26: "NORMAL_ONLY",
              27: "NO_LOCATE", 28: "NO_SUMMON", 29: "PERSONAL"}
OBJ2 = {9: 7}
OBJ2_LEVEL = {3: 15, 4: 25, 5: 35, 6: 51}
OBJ2_NAMES = {0: "LOCKED", 1: "VEHICLE_REV", 2: "NO_IDENTIFY", 7: "ANTI_DRUID",
              8: "ANTI_RANGER", 10: "LORE", 11: "DUALWIELD", 12: "ANTI_PALADIN",
              13: "ANTI_PRIEST", 14: "ANTI_BARD"}
WEAR_BITS = 15                            # ITEM_TAKE..ITEM_HOLD share bits 0-14
APPLY_MAX = 24                            # APPLY_NONE..APPLY_SAVING_SPELL shared
SPELLS = {53: 52}                         # SPELL_IDENTIFY
SPELL_TYPES = {2, 3, 4, 10}
# Crimson colour codes "&X" -> tbaMUD's "@X" (protocol.c ProtocolOutput) for
# the colours both engines name the same way; "&n" ends a colour in both.
COLOUR = re.compile(r"&([nrgbymcwdRGBYMCWD])")
OBJ_OVERRIDES = {"keywords", "wear", "values", "weight", "cost", "rent", "level",
                 "applies", "reason"}
# Mob overrides set tbaMUD values directly: stats, money, the final action and
# affect bitvectors, and the four texts.
MOB_OVERRIDES = {"level", "thac0", "ac", "hp", "damage", "gold", "exp", "act",
                 "affects", "keywords", "short", "long", "description",
                 "str", "int", "wis", "dex", "con", "cha", "reason"}
# Exit text overrides replace a converted exit's description and/or keywords.
EXIT_OVERRIDES = {"room", "dir", "description", "keywords", "reason"}
# Arguments after the command letter and its if-flag, e.g. "M 0 mob max room".
RESET_FIELDS = {"M": 3, "O": 3, "G": 2, "E": 3, "P": 3, "D": 3, "R": 2}


class Area:
    def __init__(self, base, zone, offset=0, drop_external=False, vnum_map=None,
                 overrides=None, links=(), resets=(), exits=()):
        self.base, self.zone, self.offset = Path(base), zone, offset
        self.drop_external = drop_external
        self.map = vnum_map                   # merge mode: {"mob": {old: new}, "obj": {...}}
        self.overrides = overrides or {}
        self.exclude = {}
        self.links = {}
        for link in links:
            self.links.setdefault(int(link["room"]), []).append(link)
        # Recipe resets: source-vnum zone commands appended after the source's
        # own, for an area whose upstream zone file has none.
        self.extra_resets = list(resets)
        self.exit_text = {}                   # (room, dir) -> override
        self.lifespan = self.reset_mode = None
        self.notes, self.errors, self.magic_users = [], [], []
        self.ids, self.short = {}, {}
        for k, ex in enumerate(exits):
            unknown = set(ex) - EXIT_OVERRIDES
            if unknown or ex.get("dir") not in DIRS or "room" not in ex:
                self.error(f"recipe exits[{k}]", f"needs room and dir (one of {list(DIRS)}); "
                                                 f"unknown field(s) {sorted(unknown)}")
                continue
            self.exit_text[(int(ex["room"]), DIRS[ex["dir"]])] = ex

    # -- reporting ----------------------------------------------------------
    def note(self, where, msg):
        self.notes.append(f"{where}: {msg}")

    def error(self, where, msg):
        self.errors.append(f"{where}: {msg}")

    def bits(self, value, table, names, where, label, runtime=()):
        out = 0
        for b in range(32):
            if not value & (1 << b):
                continue
            if b in table:
                out |= 1 << table[b]
            elif b not in runtime:
                self.note(where, f"dropped {label} {names.get(b, f'bit {b}')}")
        return out

    def selected(self, family, vnum):
        if vnum in self.exclude.get(family, ()):
            return False
        return self.map is None or vnum in self.map.get(family, {})

    def new(self, family, vnum, where):
        if self.map is not None:
            if family == "wld":
                return vnum                       # merge: the stock zone's own rooms
            if vnum not in self.map.get(family, {}):
                self.error(where, f"{family} vnum {vnum} is not selected by the merge map")
                return vnum
            return self.map[family][vnum]
        if vnum not in self.ids.get(family, ()):
            self.error(where, f"{family} vnum {vnum} is not defined in this area")
            return vnum
        nv = vnum + self.offset
        if not 1 <= nv <= MAX_VNUM:
            self.error(where, f"{family} vnum {vnum} -> {nv} outside 1..{MAX_VNUM}")
        return nv

    def keywords(self, namelist, prefix, where):
        """Crimson prefixes every namelist with its record type ("mob ...")."""
        words = namelist.split()
        if len(words) > 1 and words[0] == prefix:
            self.note(where, f"dropped Crimson keyword prefix '{prefix}'")
            return " ".join(words[1:])
        return namelist

    @staticmethod
    def ascending(records):
        """Rows of (vnum, lines) records in ascending vnum order: the boot loader
        expects each file's records to ascend, and a merge map can renumber
        records out of their source order."""
        return [line for _, rows in sorted(records, key=lambda r: r[0]) for line in rows]

    # -- reading --------------------------------------------------------------
    def records(self, ext):
        path = self.base.with_suffix("." + ext)
        lines = path.read_text(encoding="latin-1").splitlines()
        out, cur = [], None
        for no, line in enumerate(lines, 1):
            m = re.fullmatch(r"#(\d+)\s*", line)
            if m and int(m.group(1)) != 999999:
                cur = {"vnum": int(m.group(1)), "line": no, "body": []}
                out.append(cur)
                continue
            if m or line.startswith("$"):
                cur = None
                continue
            if cur is not None:
                cur["body"].append((no, line))
        return path.name, out

    @staticmethod
    def take_string(body, i, where="?"):
        """Lines up to and including the one ending in '~'; (text, next index).
        where (file:line and vnum) locates an unterminated string."""
        start = body[i][0] if i < len(body) else "end of record"
        parts = []
        while i < len(body):
            text = body[i][1]
            if text.endswith("~"):
                parts.append(text[:-1])
                return "\n".join(parts), i + 1
            parts.append(text)
            i += 1
        raise ValueError(f"{where}: unterminated string starting at line {start}")

    def load(self):
        self.src = {}
        for ext in ("wld", "mob", "obj"):
            name, recs = self.records(ext)
            self.src[ext] = (name, recs)
            self.ids[ext] = {r["vnum"] for r in recs}
        if self.map is not None:
            for family, entries in self.map.items():
                for old in entries:
                    if old not in self.ids.get(family, ()):
                        self.error("merge map", f"{family} {old} is not defined in this area")
        self.zon_name = self.base.with_suffix(".zon").name
        self.zon_lines = self.base.with_suffix(".zon").read_text(encoding="latin-1").splitlines()

    # -- rooms ----------------------------------------------------------------
    def rooms(self):
        fname, recs = self.src["wld"]
        out = []
        for r in recs:
            where = f"{fname}:{r['line']} room {r['vnum']}"
            b = r["body"]
            name, i = self.take_string(b, 0, where)
            desc, i = self.take_string(b, i, where)
            f = b[i][1].split()
            if len(f) != 6:
                self.error(where, f"room line has {len(f)} fields, expected 6")
                continue
            _, f1, f2, lo, hi, sect = map(int, f)
            flags = self.bits(f1, ROOM1, ROOM1_NAMES, where, "room flag", ROOM1_RUNTIME)
            self.bits(f2, {}, ROOM2_NAMES, where, "room flag2")
            if (lo, hi) not in ((0, 99), (1, 99)):
                self.note(where, f"dropped level limits {lo}-{hi} (no tbaMUD room levels)")
            if sect not in SECTOR:
                self.error(where, f"unknown sector {sect}")
                continue
            rows = [f"#{self.new('wld', r['vnum'], where)}", name + "~", desc + "~",
                    f"{self.zone} {flags} 0 0 0 {SECTOR[sect]}"]
            used = set()
            i += 1
            while i < len(b):
                tag = b[i][1].strip()
                if re.fullmatch(r"D[0-5]", tag):
                    edesc, i = self.take_string(b, i + 1, where)
                    kw, i = self.take_string(b, i, where)
                    info, key, to = map(int, b[i][1].split())
                    i += 1
                    door = (2 if info & 2 else 1) if info & 1 else 0
                    if info & ~3:
                        self.bits(info & ~3, {}, {2: "LOCKED (set by resets)", 3: "CLOSED (set by resets)",
                                                  4: "HIDDEN", 5: "NOMOB", 6: "NOCHAR",
                                                  7: "CLIMB_ONLY", 8: "NO_MOBS_IN_ROOM",
                                                  9: "PHASEPROOF", 10: "BASHPROOF",
                                                  11: "TRIPPROOF"}, where, f"exit {tag} flag")
                    nkey = -1 if key <= 0 else self.new("obj", key, where + f" exit {tag} key")
                    if to <= 0:
                        continue
                    if to not in self.ids["wld"]:
                        if self.drop_external:
                            self.note(where, f"dropped exit {tag} to room {to} outside this area")
                            continue
                        self.error(where, f"exit {tag} leads to room {to} outside this area")
                        continue
                    used.add(int(tag[1]))
                    ex = self.exit_text.pop((r["vnum"], int(tag[1])), None)
                    if ex is not None:              # recipe exit text, reported
                        edesc = ex.get("description", edesc)
                        kw = ex.get("keywords", kw)
                        self.note(where, f"exit {tag} text replaced: "
                                         f"{ex.get('reason', 'recipe override')}")
                    rows += [tag, edesc + "~", kw + "~",
                             f"{door} {nkey} {self.new('wld', to, where)}"]
                elif tag == "E":
                    kw, i = self.take_string(b, i + 1, where)
                    text, i = self.take_string(b, i, where)
                    rows += ["E", kw + "~", text + "~"]
                elif tag == "S":
                    break
                else:
                    self.error(where, f"unexpected line {tag!r}")
                    break
            for link in self.links.get(r["vnum"], []):
                d = DIRS[link["dir"]]
                if d in used:
                    self.error(where, f"link {link['dir']} would replace an existing exit")
                    continue
                rows += [f"D{d}", link.get("description", "") + "~", "~",
                         f"0 -1 {int(link['to'])}"]
                self.note(where, f"linked {link['dir']} to stock room {link['to']}")
            rows.append("S")
            out += rows
        for room, d in sorted(self.exit_text):
            self.error("recipe exits", f"room {room} has no converted exit {d} to replace")
        return out

    # -- mobiles --------------------------------------------------------------
    def mobs(self):
        fname, recs = self.src["mob"]
        out = []
        for r in recs:
            if not self.selected("mob", r["vnum"]):
                continue
            where = f"{fname}:{r['line']} mob {r['vnum']}"
            b = r["body"]
            strings = []
            i = 0
            for _ in range(4):
                s, i = self.take_string(b, i, where)
                strings.append(s)
            head = b[i][1].split()
            if len(head) != 6 or head[5] != "S":
                self.error(where, f"unsupported mob header {b[i][1]!r}")
                continue
            act1, act2, act3, aff, align = map(int, head[:5])
            act = self.bits(act1, ACT1, ACT1_NAMES, where, "mob flag") | (1 << 3)
            if act2:
                self.note(where, f"dropped act2 bits {act2:#x} (Crimson-only)")
            affects = self.bits(aff, AFF, AFF_NAMES, where, "affect")
            stats = b[i + 1][1].split()
            money = b[i + 2][1].split()
            pos = b[i + 3][1].split()
            attrs = list(map(int, b[i + 4][1].split()))
            if len(stats) != 5 or len(money) != 2 or len(pos) != 3 or len(attrs) != 8:
                self.error(where, "malformed numeric block")
                continue
            ov = self.overrides.get("mob", {}).get(str(r["vnum"]), {})
            unknown = set(ov) - MOB_OVERRIDES
            if unknown:
                self.error(where, f"unknown override field(s) {sorted(unknown)}")
            if ov:                              # recipe overrides, each reported
                for key, k in (("level", 0), ("thac0", 1), ("ac", 2), ("hp", 3), ("damage", 4)):
                    if key in ov:
                        stats[k] = str(ov[key])
                for key, k in (("gold", 0), ("exp", 1)):
                    if key in ov:
                        money[k] = str(ov[key])
                act = int(ov.get("act", act))
                affects = int(ov.get("affects", affects))
                for key, k in (("keywords", 0), ("short", 1), ("long", 2), ("description", 3)):
                    if key in ov:
                        strings[k] = ov[key]
                for key, k in (("str", 2), ("int", 3), ("wis", 4), ("dex", 5), ("con", 6), ("cha", 7)):
                    if key in ov:
                        attrs[k] = int(ov[key])
                self.note(where, f"override {sorted(set(ov) - {'reason'})}: "
                                 f"{ov.get('reason', 'recipe override')}")
            hp = re.fullmatch(r"(\d+)d(\d+)\+(-?\d+)", stats[3])
            dam = re.fullmatch(r"(\d+)d(\d+)\+(-?\d+)", stats[4])
            if not hp or not dam:
                self.error(where, "malformed hit or damage dice")
                continue
            hn, hs, hb = map(int, hp.groups())
            if hn * hs + hb > 32767:
                self.error(where, f"maximum hit points {hn * hs + hb} exceed tbaMUD's 32767")
            race, klass = attrs[0], attrs[1]
            if race or klass:
                self.note(where, f"dropped mob race {race} class {klass}")
            especs = []
            for label, value in zip(("Str", "Int", "Wis", "Dex", "Con", "Cha"), attrs[2:]):
                if not 3 <= value <= 25:
                    self.note(where, f"{label} {value} clamped to 3..25")
                    value = min(25, max(3, value))
                especs.append(f"{label}: {value}")
            attacks, spells, skills = [], [], 0
            for no, line in b[i + 5:]:
                line = line.strip()
                if line.startswith("ATTACK_TYPE="):
                    attacks.append(int(line.split("=", 1)[1]))
                elif line.startswith("ATTACK_SKILL="):
                    skills += 1
                elif line.startswith("SKILL="):
                    spells.append(line.split("=", 1)[1].split()[0])
                elif line and line != "$~":
                    self.error(f"{fname}:{no} mob {r['vnum']}", f"unknown mob line {line!r}")
            if attacks:
                kind = attacks[0] - 100
                if kind in ATTACK:
                    especs.insert(0, f"BareHandAttack: {ATTACK[kind]}")
                else:
                    self.note(where, f"dropped attack type {ATTACK_NAMES.get(kind, attacks[0])}")
                if len(attacks) > 1:
                    self.note(where, f"dropped {len(attacks) - 1} extra attack(s) {attacks[1:]} (one per round in tbaMUD)")
            if skills:
                self.note(where, f"dropped {skills} ATTACK_SKILL line(s)")
            level = int(stats[0])
            if not 1 <= level <= 50:
                self.error(where, f"level {level} outside mortal 1..50")
            nv = self.new("mob", r["vnum"], where)
            if spells:
                self.magic_users.append(nv)
                self.note(where, "casts " + ", ".join(spells) +
                          "; give it the magic_user spec proc to keep it a caster")
            strings[0] = self.keywords(strings[0], "mob", where)
            self.short[("mob", nv)] = strings[1]
            out.append((nv, [f"#{nv}", *[s + "~" for s in strings],
                             f"{act} 0 0 0 {affects} 0 0 0 {align} E",
                             " ".join(stats), " ".join(money), " ".join(pos), *especs, "E"]))
        return self.ascending(out)

    # -- objects --------------------------------------------------------------
    def objs(self):
        fname, recs = self.src["obj"]
        out = []
        for r in recs:
            if not self.selected("obj", r["vnum"]):
                continue
            where = f"{fname}:{r['line']} obj {r['vnum']}"
            ov = self.overrides.get("obj", {}).get(str(r["vnum"]), {})
            unknown = set(ov) - OBJ_OVERRIDES
            if unknown:
                self.error(where, f"unknown override field(s) {sorted(unknown)}")
            b = r["body"]
            name, i = self.take_string(b, 0, where)
            i += 1                              # db5100 skips this line (fgets)
            short, i = self.take_string(b, i, where)
            long_, i = self.take_string(b, i, where)
            action, i = self.take_string(b, i, where)
            t, f1, f2, wear = map(int, b[i][1].split())
            vals = list(map(int, b[i + 1][1].split()))
            weight, cost, rent = b[i + 2][1].split()
            i += 3
            if t not in OBJ_TYPES:
                self.error(where, f"object type {t} has no tbaMUD equivalent")
                continue
            extra = self.bits(f1, OBJ1, OBJ1_NAMES, where, "object flag",
                              runtime=set(OBJ1_LEVEL))
            extra |= self.bits(f2, OBJ2, OBJ2_NAMES, where, "object flag2",
                               runtime=set(OBJ2_LEVEL))
            levels = [lv for bit, lv in OBJ1_LEVEL.items() if f1 & (1 << bit)]
            levels += [lv for bit, lv in OBJ2_LEVEL.items() if f2 & (1 << bit)]
            level = max(levels, default=0)
            if wear >> WEAR_BITS:
                self.note(where, f"dropped wear bits {wear >> WEAR_BITS << WEAR_BITS:#x}")
            wear &= (1 << WEAR_BITS) - 1
            if t == 5:                          # weapon: v1 d v2, v3 attack
                if vals[3] not in ATTACK:
                    self.error(where, f"weapon attack type {vals[3]} has no tbaMUD equivalent")
                if vals[0]:
                    self.note(where, f"dropped weapon value0 {vals[0]} (unused by tbaMUD)")
                vals = [0, vals[1], vals[2], ATTACK.get(vals[3], 0)]
            elif t in SPELL_TYPES:
                first = 1 if t in (2, 10) else 3
                for k in range(first, 4):
                    if vals[k] > 0:
                        if vals[k] not in SPELLS:
                            self.error(where, f"spell {vals[k]} has no tbaMUD mapping")
                        vals[k] = SPELLS.get(vals[k], -1)
                    else:
                        vals[k] = -1
            elif t == 15 and vals[2] > 0:       # container key
                vals[2] = self.new("obj", vals[2], where + " container key")
            name = self.keywords(name, "obj", where)
            extras = []                         # ("E", keyword, text) / ("A", loc, mod)
            while i < len(b):
                tag = b[i][1].strip()
                if tag == "E":
                    kw, i = self.take_string(b, i + 1, where)
                    text, i = self.take_string(b, i, where)
                    extras.append(("E", kw, text))
                elif tag == "A":
                    loc, mod = map(int, b[i + 1][1].split())
                    i += 2
                    if not 0 < loc <= APPLY_MAX:
                        self.note(where, f"dropped Crimson-only apply {loc} {mod:+d}")
                        continue
                    extras.append(("A", loc, mod))
                elif tag in ("", "$~"):
                    i += 1
                else:
                    self.error(where, f"unexpected object line {tag!r}")
                    break
            if ov:                              # recipe overrides, each reported
                name = ov.get("keywords", name)
                wear = int(ov.get("wear", wear))
                vals = [int(v) for v in ov.get("values", vals)]
                weight = str(ov.get("weight", weight))
                cost = str(ov.get("cost", cost))
                rent = str(ov.get("rent", rent))
                level = int(ov.get("level", level))
                if "applies" in ov:
                    extras = [x for x in extras if x[0] == "E"] + \
                             [("A", int(l), int(m)) for l, m in ov["applies"]]
                self.note(where, f"override {sorted(set(ov) - {'reason'})}: "
                                 f"{ov.get('reason', 'recipe override')}")
            if sum(1 for x in extras if x[0] == "A") > 6:
                self.error(where, "more than 6 applies (MAX_OBJ_AFFECT)")
            nv = self.new("obj", r["vnum"], where)
            self.short[("obj", nv)] = short
            rows = [f"#{nv}", name + "~", short + "~", long_ + "~", action + "~",
                    f"{t} {extra} 0 0 0 {wear} 0 0 0 0 0 0 0",
                    " ".join(map(str, vals)), f"{weight} {cost} {rent} {level}"]
            for x in extras:
                rows += ["E", x[1] + "~", x[2] + "~"] if x[0] == "E" else ["A", f"{x[1]} {x[2]}"]
            out.append((nv, rows))
        return self.ascending(out)

    # -- zone -----------------------------------------------------------------
    def resets(self):
        rows, parent_loaded = [], False
        source = [(f"{self.zon_name}:{no}", line, False)
                  for no, line in enumerate(self.zon_lines, 1)]
        extra = [(f"recipe resets[{k}]", line, True) for k, line in enumerate(self.extra_resets)]
        if extra:
            self.note("recipe resets", f"{len(extra)} reset(s) added: the source zone has none "
                                       "for them (see the recipe notes)")
        for k, (where, line, from_recipe) in enumerate(source + extra):
            p = line.split()
            if from_recipe and (not p or p[0] not in RESET_FIELDS
                                or len(p) != 2 + RESET_FIELDS[p[0]]
                                or not all(re.fullmatch(r"-?\d+", x) for x in p[1:])):
                self.error(where, f"malformed reset {line!r}")
                continue
            if not p or len(p[0]) != 1 or p[0] not in "MOGEPDR":
                continue
            if from_recipe and k == len(source):
                parent_loaded = False           # recipe G/E never follow a source M
            c, ifl = p[0], p[1]
            a = list(map(int, p[2:]))
            if c == "M":
                parent_loaded = self.selected("mob", a[0])
                if not parent_loaded:
                    if from_recipe:
                        self.error(where, f"M {a[0]} is not a converted mob")
                    continue
                nv = self.new("mob", a[0], where)
                rows.append(f"M {ifl} {nv} {a[1]} {self.new('wld', a[2], where)} "
                            f"\t({self.short.get(('mob', nv), '')})")
            elif c in "GE":
                if not parent_loaded:
                    if from_recipe:
                        self.error(where, f"{c} {a[0]} does not follow a converted M")
                    continue                    # follows a mob that is not converted
                if not self.selected("obj", a[0]):
                    self.error(where, f"{c} {a[0]} follows a selected mob but is not selected")
                    continue
                nv = self.new("obj", a[0], where)
                if c == "G":
                    rows.append(f"G {ifl} {nv} {a[1]} -1 \t({self.short.get(('obj', nv), '')})")
                else:
                    mx = a[1]
                    if mx < 1:
                        self.note(where, "E max 0 (unused by Crimson) written as 1, or tbaMUD never loads it")
                        mx = 1
                    rows.append(f"E {ifl} {nv} {mx} {a[2]} \t({self.short.get(('obj', nv), '')})")
            elif c in "OP":
                if not self.selected("obj", a[0]):
                    if from_recipe:
                        self.error(where, f"{c} {a[0]} is not a converted object")
                    continue
                nv = self.new("obj", a[0], where)
                dest = self.new("wld" if c == "O" else "obj", a[2], where)
                rows.append(f"{c} {ifl} {nv} {a[1]} {dest} \t({self.short.get(('obj', nv), '')})")
            elif c == "D" and self.map is None:
                rows.append(f"D {ifl} {self.new('wld', a[0], where)} {a[1]} {a[2]}")
            elif c == "R" and self.map is None:
                rows.append(f"R {ifl} {self.new('wld', a[0], where)} {self.new('obj', a[1], where)}")
        return rows

    def zone_file(self, name, builders):
        header = self.zon_lines[2].split()
        lifespan, reset_mode = int(header[1]), int(header[2])
        if self.lifespan is not None or self.reset_mode is not None:
            new = (int(self.lifespan if self.lifespan is not None else lifespan),
                   int(self.reset_mode if self.reset_mode is not None else reset_mode))
            if new[0] < 1 or new[1] not in (0, 1, 2):
                self.error("recipe", f"lifespan {new[0]} must be >= 1 and reset_mode "
                                     f"{new[1]} one of 0, 1, 2")
            self.note("recipe", f"zone lifespan/reset mode {lifespan} {reset_mode} -> "
                                f"{new[0]} {new[1]}")
            lifespan, reset_mode = new
        bottom, top = self.zone * 100, self.zone * 100 + 99
        return ([f"#{self.zone}", builders + "~", name + "~",
                 f"{bottom} {top} {lifespan} {reset_mode}"] + self.resets() + ["S", "$"])


def settings_from(args):
    """Merge --recipe/--area with explicit command-line options."""
    s = {"zone": args.zone, "offset": args.offset, "name": args.name,
         "builders": args.builders, "drop_external": args.drop_external,
         "mode": "zone", "map": None, "overrides": {}, "links": [], "exclude": {}, "source": args.source,
         "resets": [], "exits": [], "lifespan": None, "reset_mode": None}
    if args.recipe:
        recipe = json.loads(args.recipe.read_text())
        area = recipe["areas"][args.area]
        for key in ("zone", "offset", "name", "builders", "drop_external", "mode",
                    "overrides", "links", "exclude", "resets", "exits", "lifespan",
                    "reset_mode"):
            if key in area:
                s[key] = area[key]
        s["source"] = str(args.recipe.parent / area["source"])
        if area.get("map"):
            s["map"] = {fam: {int(o): int(n) for o, n in m.items()}
                        for fam, m in area["map"].items()}
    if s["zone"] is None or not s["source"]:
        raise ValueError("need a source and --zone, or --recipe and --area")
    return s


def outputs(area, s):
    area.load()
    if s["mode"] == "merge":
        mobs, objs = area.mobs(), area.objs()
        return {"mob": mobs, "obj": objs, "zon": area.resets()}
    rooms, mobs, objs = area.rooms(), area.mobs(), area.objs()
    return {"wld": rooms, "mob": mobs, "obj": objs,
            "zon": area.zone_file(s["name"] or area.zon_lines[1].rstrip("~"),
                                  s["builders"] or "Crimson 2 import")}


def render(ext, rows, mode):
    if mode == "merge":
        return "\n".join(rows) + "\n" if rows else ""
    tail = [] if ext == "zon" else ["$~"]
    return "\n".join(rows + tail) + "\n"


def install(world, zone, texts, mode):
    """Install the converted texts. Every target is checked first; each file is
    then written to a temporary sibling and moved into place with os.replace(),
    so a refused install changes nothing."""
    plan = []                                   # (path, full new text)
    if mode == "merge":
        for ext, text in texts.items():
            path = world / ext / f"{zone}.{ext}"
            cur = path.read_text(encoding="latin-1")
            if text and text in cur:
                raise ValueError(f"{path} already holds this fragment")
            end = "S\n$\n" if ext == "zon" else ("$~\n" if cur.endswith("$~\n") else "$\n")
            if not cur.endswith(end):
                raise ValueError(f"{path} does not end with {end!r}")
            plan.append((path, cur[:-len(end)] + text + end))
    else:
        for ext, text in texts.items():
            path = world / ext / f"{zone}.{ext}"
            if path.exists():
                raise ValueError(f"refusing to overwrite {path}")
            idx = world / ext / "index"
            names = idx.read_text(encoding="latin-1").splitlines()
            if not names or names[-1] != "$":
                raise ValueError(f"{idx} does not end with $")
            files = sorted(set(names[:-1]) | {f"{zone}.{ext}"}, key=lambda n: int(n.split(".")[0]))
            plan += [(path, text), (idx, "\n".join(files + ["$"]) + "\n")]
    staged = []
    try:
        for path, text in plan:
            fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
            with os.fdopen(fd, "w", encoding="latin-1", newline="") as fh:
                fh.write(text)
            staged.append((tmp, path))
    except BaseException:
        for tmp, _ in staged:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        raise
    done = []
    for k, (tmp, path) in enumerate(staged):
        try:
            os.replace(tmp, path)
        except OSError as exc:
            for left, _ in staged[k:]:
                try:
                    os.unlink(left)
                except OSError:
                    pass
            raise OSError(f"install stopped at {path}: {exc}; already replaced: "
                          f"{', '.join(str(p) for p in done) or 'none'}") from exc
        done.append(path)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", nargs="?", help="area path without extension, e.g. .../Moria34")
    p.add_argument("--recipe", type=Path)
    p.add_argument("--area", help="area name inside --recipe")
    p.add_argument("--zone", type=int)
    p.add_argument("--offset", type=int, default=0, help="added to every vnum")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--install", type=Path, help="world root to install into (lib/world)")
    p.add_argument("--name")
    p.add_argument("--builders")
    p.add_argument("--drop-external", action="store_true",
                   help="drop (and report) exits that leave the area instead of failing")
    a = p.parse_args(argv)
    try:
        s = settings_from(a)
        area = Area(s["source"], s["zone"], s["offset"], s["drop_external"],
                    s["map"], s["overrides"], s["links"], s["resets"], s["exits"])
        area.exclude = {f: set(v) for f, v in s.get("exclude", {}).items()}
        area.lifespan, area.reset_mode = s["lifespan"], s["reset_mode"]
        rows = outputs(area, s)
    except (OSError, ValueError, IndexError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    for n in area.notes:
        print(f"NOTE: {n}")
    if area.magic_users:
        print("MAGIC_USER: " + " ".join(map(str, area.magic_users)))
    if area.errors:
        for e in area.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    suffix = ".add" if s["mode"] == "merge" else ""
    texts = {}
    for ext, r in rows.items():
        texts[ext], n = COLOUR.subn(r"@\1", render(ext, r, s["mode"]))
        if n:
            print(f"NOTE: {s['zone']}.{ext}: {n} Crimson colour code(s) &X written as @X")
    for ext in texts:
        if (a.out / ext / f"{s['zone']}.{ext}{suffix}").exists():
            print(f"ERROR: refusing to overwrite {a.out / ext}", file=sys.stderr)
            return 1
    for ext, text in texts.items():
        out = a.out / ext / f"{s['zone']}.{ext}{suffix}"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="latin-1")
    if a.install:
        try:
            install(a.install, s["zone"], texts, s["mode"])
        except (OSError, ValueError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
    print(f"converted {s['source']} ({s['mode']}) -> zone {s['zone']} in {a.out}"
          + (f", installed into {a.install}" if a.install else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
