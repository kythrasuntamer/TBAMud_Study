#!/usr/bin/env python3
"""Convert one Crimson 2 area (.wld/.mob/.obj/.zon) into tbaMUD world files.

Field meanings follow Crimson 2's own loader (src/db.c and structs.h at
c2admins/Crimson-2-MUD 2b7207b): rooms db4300_boot_world, mobiles
db5000_read_mobile, objects db5100_read_object, zones db4700_boot_zones.
Every value is mapped by meaning, never copied by bit position; anything
tbaMUD cannot represent is dropped and reported as "file:line vnum: ...".
An unresolved reference or an unconvertible value is an error: nothing is
written and the exit status is 1.

    convert_crimson2.py imports/quarantine/crimson2/Moria34 --zone 47 \\
        --offset 600 --out /tmp/c2-staged [--drop-external]

writes /tmp/c2-staged/{wld,mob,obj,zon}/47.<ext>; stage, validate
(tools/validate_import_refs.py, tools/audit_world_vnums.py) and boot-test
before installing into lib/world.
"""
import argparse
import re
import sys
from pathlib import Path

MAX_VNUM = 65534                          # IDXTYPE; 65535 is NOWHERE

# --- Crimson -> tbaMUD tables ----------------------------------------------
# Room flags word 1 (RM1_*) -> ROOM_* bit. RM1_BFS_MARK is runtime state.
ROOM1 = {0: 0, 1: 1, 2: 2, 3: 3, 7: 7, 8: 8, 9: 9, 16: 4, 18: 5, 21: 6}
ROOM1_RUNTIME = {20}
ROOM1_NAMES = {4: "ANTI_GOOD", 5: "ANTI_NEUTRAL", 6: "ANTI_EVIL", 10: "DRAIN_MAGIC",
               11: "HARMFULL1", 12: "HARMFULL2", 14: "NOSTEAL", 15: "NOPKILL (PvP)",
               17: "WATER", 19: "DUMP", 22: "MAZE", 23: "ANTI_MAGIC", 24: "INN",
               25: "MYST", 26: "FALL_DMG", 28: "NO_TELEPORT_IN",
               29: "NO_TELEPORT_OUT", 30: "NO_SUMMON_OUT"}
ROOM2_NAMES = {20: "ARENA (PvP)"}
# Sectors 0-7 share numbering; Crimson SECT_UNDERWATER 8 is tbaMUD 9.
SECTOR = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 9}
# Mob act word 1 (PLR1_*) -> MOB_* bit.
ACT1 = {1: 1, 2: 2, 3: 3, 6: 6, 7: 7, 13: 8, 14: 10, 15: 9, 18: 13, 29: 18}
ACT1_NAMES = {0: "UNFILTER", 4: "CLASS_DEMON", 5: "CHAR_WAS_HELD", 8: "SIGNAL_HUNT_HELP",
              9: "GUARD1", 10: "GUARD2", 11: "NOSTEAL", 12: "INJURED_ATTACK",
              16: "POISONOUS", 17: "SHOW_DIAG", 19: "DRAIN_XP", 20: "CLASS_SHADOW",
              21: "CLASS_VAMPIRE", 22: "CLASS_DRAGON", 23: "CLASS_GIANT",
              24: "CLASS_ANIMAL", 25: "CLASS_UNDEAD", 26: "CANT_HUNT",
              27: "HUNTASSIST", 28: "NOFOLLOW", 30: "NOHOLD"}
# Affects (AFF_*) -> AFF_* bit.
AFF = {0: 1, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 7: 8, 9: 7, 10: 10, 12: 12, 13: 13,
       14: 17, 17: 15, 19: 19, 20: 20, 24: 18, 25: 11}
AFF_NAMES = {6: "HASTE", 8: "GROUP", 11: "SILENCE", 15: "RAGE", 16: "HOLD_PERSON",
             18: "BLOCK_SUMMON", 21: "DEMON_FLESH", 22: "CHARM", 23: "FOLLOW",
             26: "REGENERATION", 27: "TRANSFORMED", 28: "HEMMORAGE",
             29: "MAGIC_RESIST", 30: "MAGIC_IMMUNE"}
# Attack types: Crimson TYPE_HIT=100.. order -> tbaMUD attack index (TYPE_HIT=300..).
ATTACK = {0: 0, 1: 5, 2: 11, 3: 3, 4: 2, 5: 8, 6: 4, 7: 1, 8: 6}
ATTACK_NAMES = {9: "ARROW", 10: "DUALWIELD"}
# Object types sharing meaning and numbering in both engines.
OBJ_TYPES = {1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 22}
# Object flags word 1 (OBJ1_*) -> ITEM_* extra bit. Note thief/cleric swap.
OBJ1 = {0: 0, 1: 1, 5: 5, 6: 6, 7: 7, 8: 8, 9: 9, 10: 10, 11: 11, 12: 12,
        13: 14, 14: 13, 15: 15, 18: 2, 30: 17}
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
SPELL_TYPES = {2, 3, 4, 10}               # scroll, wand, staff, potion


class Area:
    def __init__(self, base, zone, offset, drop_external):
        self.base, self.zone, self.offset = Path(base), zone, offset
        self.drop_external = drop_external
        self.notes, self.errors, self.magic_users = [], [], []
        self.ids = {}

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

    def new(self, family, vnum, where):
        if vnum not in self.ids.get(family, ()):
            self.error(where, f"{family} vnum {vnum} is not defined in this area")
            return vnum
        nv = vnum + self.offset
        if not 1 <= nv <= MAX_VNUM:
            self.error(where, f"{family} vnum {vnum} -> {nv} outside 1..{MAX_VNUM}")
        return nv

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

    def keywords(self, namelist, prefix, where):
        """Crimson prefixes every namelist with its record type ("mob ...")."""
        words = namelist.split()
        if len(words) > 1 and words[0] == prefix:
            self.note(where, f"dropped Crimson keyword prefix '{prefix}'")
            return " ".join(words[1:])
        return namelist

    @staticmethod
    def take_string(body, i):
        """Lines up to and including the one ending in '~'; (text, next index)."""
        parts = []
        while i < len(body):
            text = body[i][1]
            if text.endswith("~"):
                parts.append(text[:-1])
                return "\n".join(parts), i + 1
            parts.append(text)
            i += 1
        raise ValueError("unterminated string")

    def load(self):
        self.src = {}
        for ext in ("wld", "mob", "obj"):
            name, recs = self.records(ext)
            self.src[ext] = (name, recs)
            self.ids[ext] = {r["vnum"] for r in recs}
        self.zon_name = self.base.with_suffix(".zon").name
        self.zon_lines = self.base.with_suffix(".zon").read_text(encoding="latin-1").splitlines()

    # -- rooms ----------------------------------------------------------------
    def rooms(self):
        fname, recs = self.src["wld"]
        out = []
        for r in recs:
            where = f"{fname}:{r['line']} room {r['vnum']}"
            b = r["body"]
            name, i = self.take_string(b, 0)
            desc, i = self.take_string(b, i)
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
            i += 1
            while i < len(b):
                tag = b[i][1].strip()
                if re.fullmatch(r"D[0-5]", tag):
                    edesc, i = self.take_string(b, i + 1)
                    kw, i = self.take_string(b, i)
                    ex = b[i][1].split()
                    i += 1
                    info, key, to = map(int, ex)
                    door = 0
                    if info & 1:
                        door = 2 if info & 2 else 1
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
                    rows += [tag, edesc + "~", kw + "~",
                             f"{door} {nkey} {self.new('wld', to, where)}"]
                elif tag == "E":
                    kw, i = self.take_string(b, i + 1)
                    text, i = self.take_string(b, i)
                    rows += ["E", kw + "~", text + "~"]
                elif tag == "S":
                    break
                else:
                    self.error(where, f"unexpected line {tag!r}")
                    break
            rows.append("S")
            out += rows
        return out

    # -- mobiles --------------------------------------------------------------
    def mobs(self):
        fname, recs = self.src["mob"]
        out = []
        for r in recs:
            where = f"{fname}:{r['line']} mob {r['vnum']}"
            b = r["body"]
            strings = []
            i = 0
            for _ in range(4):
                s, i = self.take_string(b, i)
                strings.append(s)
            head = b[i][1].split()
            if len(head) != 6 or head[5] != "S":
                self.error(where, f"unsupported mob header {b[i][1]!r}")
                continue
            act1, act2, act3, aff, align = map(int, head[:5])
            act = self.bits(act1, ACT1, ACT1_NAMES, where, "mob flag") | (1 << 3)
            if act2:
                self.note(where, f"dropped act2 bits {act2:#x} (Crimson-only)")
            # act3 is Crimson's PLR3 word: the loader forces BRIEF/AUTOAGGR itself.
            affects = self.bits(aff, AFF, AFF_NAMES, where, "affect")
            stats = b[i + 1][1].split()
            money = b[i + 2][1].split()
            pos = b[i + 3][1].split()
            attrs = list(map(int, b[i + 4][1].split()))
            if len(stats) != 5 or len(money) != 2 or len(pos) != 3 or len(attrs) != 8:
                self.error(where, "malformed numeric block")
                continue
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
            if spells:
                self.magic_users.append(r["vnum"] + self.offset)
                self.note(where, "casts " + ", ".join(spells) +
                          "; give it the magic_user spec proc to keep it a caster")
            level = int(stats[0])
            if not 1 <= level <= 50:
                self.error(where, f"level {level} outside mortal 1..50")
            strings[0] = self.keywords(strings[0], "mob", where)
            out += [f"#{self.new('mob', r['vnum'], where)}", *[s + "~" for s in strings],
                    f"{act} 0 0 0 {affects} 0 0 0 {align} E",
                    " ".join(stats), " ".join(money), " ".join(pos), *especs, "E"]
        return out

    # -- objects --------------------------------------------------------------
    def objs(self):
        fname, recs = self.src["obj"]
        out = []
        for r in recs:
            where = f"{fname}:{r['line']} obj {r['vnum']}"
            b = r["body"]
            name, i = self.take_string(b, 0)
            i += 1                              # db5100 skips this line (fgets)
            short, i = self.take_string(b, i)
            long_, i = self.take_string(b, i)
            action, i = self.take_string(b, i)
            t, f1, f2, wear = map(int, b[i][1].split())
            vals = list(map(int, b[i + 1][1].split()))
            wcr = b[i + 2][1].split()
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
            rows = [f"#{self.new('obj', r['vnum'], where)}", name + "~", short + "~",
                    long_ + "~", action + "~",
                    f"{t} {extra} 0 0 0 {wear} 0 0 0 0 0 0 0",
                    " ".join(map(str, vals)),
                    f"{wcr[0]} {wcr[1]} {wcr[2]} {level}"]
            applies = 0
            while i < len(b):
                tag = b[i][1].strip()
                if tag == "E":
                    kw, i = self.take_string(b, i + 1)
                    text, i = self.take_string(b, i)
                    rows += ["E", kw + "~", text + "~"]
                elif tag == "A":
                    loc, mod = map(int, b[i + 1][1].split())
                    i += 2
                    if not 0 < loc <= APPLY_MAX:
                        self.note(where, f"dropped Crimson-only apply {loc} {mod:+d}")
                        continue
                    applies += 1
                    if applies > 6:
                        self.error(where, "more than 6 applies (MAX_OBJ_AFFECT)")
                    rows += ["A", f"{loc} {mod}"]
                elif tag in ("", "$~"):
                    i += 1
                else:
                    self.error(where, f"unexpected object line {tag!r}")
                    break
            out += rows
        return out

    # -- zone -----------------------------------------------------------------
    def zone_file(self, name, builders):
        lines = self.zon_lines
        header = lines[2].split()
        lifespan, reset_mode = int(header[1]), int(header[2])
        bottom, top = self.zone * 100, self.zone * 100 + 99
        rows = [f"#{self.zone}", builders + "~", name + "~",
                f"{bottom} {top} {lifespan} {reset_mode}"]
        for no, line in enumerate(lines, 1):
            p = line.split()
            if not p or p[0] not in "MOGEPDR" or len(p[0]) != 1:
                continue
            where = f"{self.zon_name}:{no}"
            c, ifl = p[0], p[1]
            a = list(map(int, p[2:]))
            if c == "M":
                rows.append(f"M {ifl} {self.new('mob', a[0], where)} {a[1]} {self.new('wld', a[2], where)}")
            elif c == "O":
                rows.append(f"O {ifl} {self.new('obj', a[0], where)} {a[1]} {self.new('wld', a[2], where)}")
            elif c == "G":
                rows.append(f"G {ifl} {self.new('obj', a[0], where)} {a[1]} -1")
            elif c == "E":
                mx = a[1]
                if mx < 1:
                    self.note(where, "E max 0 (unused by Crimson) written as 1, or tbaMUD never loads it")
                    mx = 1
                rows.append(f"E {ifl} {self.new('obj', a[0], where)} {mx} {a[2]}")
            elif c == "P":
                rows.append(f"P {ifl} {self.new('obj', a[0], where)} {a[1]} {self.new('obj', a[2], where)}")
            elif c == "D":
                rows.append(f"D {ifl} {self.new('wld', a[0], where)} {a[1]} {a[2]}")
            elif c == "R":
                rows.append(f"R {ifl} {self.new('wld', a[0], where)} {self.new('obj', a[1], where)}")
        return rows + ["S", "$"]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", help="area path without extension, e.g. .../Moria34")
    p.add_argument("--zone", type=int, required=True)
    p.add_argument("--offset", type=int, default=0, help="added to every vnum")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--name")
    p.add_argument("--builders", default="Crimson 2 import")
    p.add_argument("--drop-external", action="store_true",
                   help="drop (and report) exits that leave the area instead of failing")
    a = p.parse_args(argv)
    area = Area(a.source, a.zone, a.offset, a.drop_external)
    try:
        area.load()
        files = {"wld": area.rooms(), "mob": area.mobs(), "obj": area.objs(),
                 "zon": area.zone_file(a.name or area.zon_lines[1].rstrip("~"), a.builders)}
    except (OSError, ValueError, IndexError) as exc:
        print(f"ERROR: {a.source}: {exc}", file=sys.stderr)
        return 1
    for n in area.notes:
        print(f"NOTE: {n}")
    if area.magic_users:
        print("MAGIC_USER: " + " ".join(map(str, area.magic_users)))
    if area.errors:
        for e in area.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    for ext, rows in files.items():
        out = a.out / ext / f"{a.zone}.{ext}"
        if out.exists():
            print(f"ERROR: refusing to overwrite {out}", file=sys.stderr)
            return 1
    for ext, rows in files.items():
        out = a.out / ext / f"{a.zone}.{ext}"
        out.parent.mkdir(parents=True, exist_ok=True)
        tail = [] if ext == "zon" else ["$~"]
        out.write_text("\n".join(rows + tail) + "\n", encoding="latin-1")
    print(f"converted {a.source} -> zone {a.zone} in {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
