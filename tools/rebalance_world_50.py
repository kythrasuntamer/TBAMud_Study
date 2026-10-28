#!/usr/bin/env python3
"""Stage a level-50 combat rebalance of tbaMUD mobile files, with a report.

Reads <world>/mob: the files listed in mob/index when it exists (a listed
file that is missing is an error), otherwise every *.mob.  Writes rebalanced
copies plus level50-report.json into --staging; never edits the live world.

Levels: 1-10 are kept; 11..old_top are stretched monotonically onto
11..max_mortal, where old_top is the highest level found (at least 34, the
stock tbaMUD maximum) and max_mortal is LVL_IMMORT - 1 from src/structs.h
(50).  A mobile at an immortal level is rejected.  Creature statistics,
experience, combat output and special-procedure balance still need a test
server review before installation.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import sys
import tempfile

HEADER = re.compile(r"(?m)^#(\d+)[ \t]*$")
DICE = re.compile(r"^(\d+)d(\d+)\+(-?\d+)$")
STATS = re.compile(r"^(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+d\d+\+-?\d+)\s+(\d+d\d+\+-?\d+)$")
NUMS = re.compile(r"^(\d+)\s+(\d+)$")
MAX_SIZE = 8 * 1024 * 1024
STOCK_TOP = 34            # highest mobile level in stock tbaMUD
DEFAULT_MAX_MORTAL = 50   # LVL_IMMORT - 1 when src/structs.h is not found
# A .mob file stores AC in tenths of the in-game value (db.c: GET_AC = 10 * t[2]).
AC_MAX_DROP = 5           # file units, i.e. at most 50 points of in-game AC
AC_FLOOR = -30            # file units, i.e. in-game AC -300


class RecordError(ValueError):
    """A problem in one input file, already located as path:line."""


def max_mortal_level(source_root):
    """LVL_IMMORT - 1 from src/structs.h, or the stock 50."""
    structs = source_root / "src" / "structs.h"
    try:
        match = re.search(r"#define\s+LVL_IMMORT\s+(\d+)", structs.read_text(encoding="latin-1"))
    except OSError:
        match = None
    return int(match.group(1)) - 1 if match else DEFAULT_MAX_MORTAL


def target_level(level, old_top=STOCK_TOP, new_top=DEFAULT_MAX_MORTAL):
    """Monotonic: 1-10 kept; 11..old_top stretched onto 11..new_top."""
    if level <= 10:
        return level
    if old_top <= 10:
        return min(level, new_top)
    return min(new_top, 10 + round((min(level, old_top) - 10) * (new_top - 10) / (old_top - 10)))


def scale_dice(value, factor, maximum):
    match = DICE.fullmatch(value)
    if match is None:
        raise ValueError(f"unsupported dice expression {value!r}")
    count, sides, bonus = map(int, match.groups())
    if count > 10000 or sides > 10000 or abs(bonus) > 10000000:
        raise ValueError(f"unreasonably large dice expression {value!r}")
    new_bonus = max(-maximum, min(maximum, round(bonus * factor)))
    return f"{count}d{sides}+{new_bonus}"


def records(text, path):
    """Yield (vnum, header line number, lines, stats row index) per mobile."""
    matches = list(HEADER.finditer(text))
    if not matches:
        if text.strip() in ("$", "$~"):
            return
        raise RecordError(f"{path}:1: no numbered mobile records")
    for k, match in enumerate(matches):
        end = matches[k + 1].start() if k + 1 < len(matches) else len(text)
        base = text.count("\n", 0, match.start()) + 1
        lines = text[match.start():end].splitlines(keepends=True)
        where = f"{path}:{base}: mobile #{match.group(1)}"
        # Skip the vnum header and the four tilde-terminated text fields.
        line_no, count = 1, 0
        while line_no < len(lines) and count < 4:
            if lines[line_no].rstrip("\r\n").endswith("~"):
                count += 1
            line_no += 1
        if count != 4 or line_no + 2 >= len(lines):
            raise RecordError(f"{where} has an invalid text layout")
        if not re.search(r"\b[ES]\s*$", lines[line_no].strip()):
            raise RecordError(f"{path}:{base + line_no}: mobile #{match.group(1)} uses an unsupported format")
        yield match, base, lines, line_no + 1


def highest_level(text, path):
    top = 0
    for match, base, lines, index in records(text, path):
        stats = STATS.fullmatch(lines[index].strip())
        if stats:
            top = max(top, int(stats.group(1)))
    return top


def process(text, path="<input>", old_top=STOCK_TOP, new_top=DEFAULT_MAX_MORTAL):
    outputs, report = [], []
    first = None
    for match, base, lines, index in records(text, path):
        if first is None:
            first = match.start()
        vnum = int(match.group(1))
        stats = STATS.fullmatch(lines[index].strip())
        economy = NUMS.fullmatch(lines[index + 1].strip())
        if stats is None or economy is None:
            bad = index if stats is None else index + 1
            raise RecordError(f"{path}:{base + bad}: mobile #{vnum} lacks standard combat and economy rows")
        level, thaco, ac = map(int, stats.group(1, 2, 3))
        if level > new_top:
            raise RecordError(f"{path}:{base + index}: mobile #{vnum} is level {level}, "
                              f"an immortal level (mortals end at {new_top})")
        newlevel = target_level(level, old_top, new_top)
        if newlevel != level:
            factor = newlevel / level
            try:
                hp = scale_dice(stats.group(4), factor, 10000000)
                damage = scale_dice(stats.group(5), min(1.35, 1 + (factor - 1) * 0.4), 10000000)
            except ValueError as error:
                raise RecordError(f"{path}:{base + index}: mobile #{vnum}: {error}") from None
            new_thaco = max(-20, thaco - min(8, round((newlevel - level) * 0.25)))
            new_ac = max(AC_FLOOR, ac - min(AC_MAX_DROP, round((newlevel - level) * 0.25)))
            if ac < AC_FLOOR:          # never raise an already-better AC
                new_ac = ac
            newline = "\r\n" if lines[index].endswith("\r\n") else "\n"
            lines[index] = f"{newlevel} {new_thaco} {new_ac} {hp} {damage}" + newline
            gold, exp = map(int, economy.groups())
            adjusted_xp = min(2000000000, round(exp * factor))   # currency unchanged
            lines[index + 1] = f"{gold} {adjusted_xp}" + newline
            report.append({"vnum": vnum, "line": base + index,
                           "old": {"level": level, "thac0": thaco, "ac_file": ac,
                                   "hp": stats.group(4), "damage": stats.group(5), "exp": exp},
                           "new": {"level": newlevel, "thac0": new_thaco, "ac_file": new_ac,
                                   "hp": hp, "damage": damage, "exp": adjusted_xp}})
        outputs.append("".join(lines))
    if first is None:
        return text, []
    return text[:first] + "".join(outputs), report


def mobile_files(original):
    """The files mob/index lists (missing ones are errors), else every *.mob."""
    index = original / "index"
    if index.is_file():
        names = []
        for lineno, raw in enumerate(index.read_text(encoding="latin-1").splitlines(), 1):
            name = raw.strip()
            if name == "$":
                break
            if not name:
                continue
            if "/" in name or name.startswith("."):
                raise RecordError(f"{index}:{lineno}: unsafe file name {name!r}")
            if not (original / name).is_file():
                raise RecordError(f"{index}:{lineno}: listed file {name} is missing")
            names.append(original / name)
        return names, "index"
    return sorted(original.glob("*.mob")), "glob"


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--world", type=pathlib.Path, default=pathlib.Path("lib/world"))
    p.add_argument("--staging", type=pathlib.Path, required=True)
    p.add_argument("--source-root", type=pathlib.Path,
                   default=pathlib.Path(__file__).resolve().parents[1],
                   help="tree holding src/structs.h (for LVL_IMMORT)")
    args = p.parse_args()
    original = args.world / "mob"
    staging = args.staging
    temp = None
    try:
        if not original.is_dir() or original.is_symlink():
            raise ValueError(f"{original}: missing mobile world directory")
        if staging.resolve() == args.world.resolve() or args.world.resolve() in staging.resolve().parents:
            raise ValueError(f"{staging}: staging cannot be inside the live world")
        if staging.exists() and (not staging.is_dir() or any(staging.iterdir())):
            raise ValueError(f"{staging}: staging must be absent or an empty directory")
        files, source = mobile_files(original)
        if not files:
            raise ValueError(f"{original}: no mobile files")
        new_top = max_mortal_level(args.source_root)
        texts = []
        for file in files:
            if file.is_symlink() or file.stat().st_size > MAX_SIZE:
                raise ValueError(f"{file}: unsafe input (symlink or over {MAX_SIZE} bytes)")
            texts.append((file, file.read_text(encoding="latin-1")))
        old_top = max([STOCK_TOP] + [highest_level(t, f) for f, t in texts])
        prepared = [(f, *process(t, f, min(old_top, new_top), new_top)) for f, t in texts]

        # Build everything in a sibling temporary directory, then move it
        # into place, so a failure never leaves a half-written staging tree.
        staging.parent.mkdir(parents=True, exist_ok=True)
        temp = pathlib.Path(tempfile.mkdtemp(prefix=".rebalance-", dir=staging.parent))
        (temp / "mob").mkdir()
        summary = []
        for src, data, changes in prepared:
            (temp / "mob" / src.name).write_text(data, encoding="latin-1")
            summary.extend({"source_file": src.name, **x} for x in changes)
        for index in ("index", "index.mini"):
            if (original / index).exists():
                shutil.copyfile(original / index, temp / "mob" / index)
        (temp / "level50-report.json").write_text(json.dumps({
            "mob_files": len(prepared), "file_selection": source,
            "old_top_level": old_top, "max_mortal_level": new_top,
            "changed_mobs": len(summary), "changes": summary,
            "notes": ["AC values are in file units: in-game AC is 10 times larger.",
                      "Only standard (S/E) mobile records are supported.",
                      "NPC scripts, shopkeepers, special NPCs and rewards need review.",
                      "Boot and combat-test a server before installing."]}, indent=2) + "\n")
        if staging.exists():
            staging.rmdir()          # empty, checked above
        os.replace(temp, staging)
        temp = None
        print(f"Staged {len(prepared)} files ({source}), {len(summary)} changed mobiles in {staging}")
    except (OSError, ValueError) as error:
        print(f"REJECTED: {error}", file=sys.stderr)
        return 1
    finally:
        if temp is not None:
            shutil.rmtree(temp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
