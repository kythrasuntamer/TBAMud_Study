#!/usr/bin/env python3
"""Stage a level-50 combat rebalance of every indexed tbaMUD mobile file.

Does not edit live world files. Creature statistics, experience, combat output
and special-procedure balance require subsequent test-server review.
"""
import argparse
import json
import pathlib
import re
import shutil
import sys

HEADER = re.compile(r"(?m)^#(\d+)[ \t]*$")
DICE = re.compile(r"^(\d+)d(\d+)\+(-?\d+)$")
STATS = re.compile(r"^(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+d\d+\+-?\d+)\s+(\d+d\d+\+-?\d+)$")
NUMS = re.compile(r"^(\d+)\s+(\d+)$")
MAX_SIZE = 8 * 1024 * 1024

def target_level(level):
    """Keep starter mobs intact; stretch legacy 11-34 range toward 50."""
    if level <= 10 or level > 34:
        return level
    return min(50, 10 + round((level - 10) * 40 / 24))

def scale_dice(value, factor, maximum):
    match = DICE.fullmatch(value)
    if match is None:
        raise ValueError(f"unsupported dice expression {value!r}")
    count, sides, bonus = map(int, match.groups())
    if count > 10000 or sides > 10000 or abs(bonus) > 10000000:
        raise ValueError("unreasonably large dice expression")
    new_bonus = max(-maximum, min(maximum, round(bonus * factor)))
    return f"{count}d{sides}+{new_bonus}"

def process(text):
    matches = list(HEADER.finditer(text))
    if not matches:
        raise ValueError("no numbered mobile records")
    outputs, report = [], []
    for k, match in enumerate(matches):
        end = matches[k+1].start() if k+1 < len(matches) else len(text)
        block = text[match.start():end]
        lines = block.splitlines(keepends=True)
        # Skip the numeric VNUM header and exactly four tilde-terminated
        # text fields (keywords, short, long, description).
        line_no, count = 1, 0
        while line_no < len(lines) and count < 4:
            if lines[line_no].rstrip("\r\n").endswith("~"):
                count += 1
            line_no += 1
        if count != 4 or line_no + 2 >= len(lines):
            raise ValueError(f"mobile {match.group(1)} has invalid text layout")
        # The next row is flags/format, followed by 5-token combat stats.
        flagline = lines[line_no].strip()
        if not re.search(r"\b[ES]\s*$", flagline):
            raise ValueError(f"mobile {match.group(1)} uses unsupported format")
        index = line_no + 1
        stats = STATS.fullmatch(lines[index].strip())
        economy = NUMS.fullmatch(lines[index+1].strip())
        if stats is None or economy is None:
            raise ValueError(f"mobile {match.group(1)} lacks standard combat and economy rows")
        level, thaco, ac = map(int, stats.group(1,2,3))
        newlevel = target_level(level)
        if newlevel != level:
            factor = newlevel / level
            hp = scale_dice(stats.group(4), factor, 10000000)
            damage = scale_dice(stats.group(5), min(1.35, 1 + (factor-1)*0.4), 10000000)
            # Base combat values (THAC0/AC) have a distinct scale; cap
            # adjustments to avoid making converted mobs unhittable.
            new_thaco = max(-20, thaco - min(8, round((newlevel-level)*0.25)))
            new_ac = max(-100, ac - min(20, round((newlevel-level)*0.75)))
            newline = "\r\n" if lines[index].endswith("\r\n") else "\n"
            lines[index] = f"{newlevel} {new_thaco} {new_ac} {hp} {damage}" + newline
            gold, exp = map(int, economy.groups())
            # Leave currency unchanged; rebalance XP only, capped.
            adjusted_xp = min(2000000000, round(exp * factor))
            lines[index+1] = f"{gold} {adjusted_xp}" + newline
            report.append({"vnum":int(match.group(1)), "old_level":level,
                           "new_level":newlevel, "old_exp":exp,
                           "new_exp":adjusted_xp})
        outputs.append("".join(lines))
    return text[:matches[0].start()] + "".join(outputs), report

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--world", type=pathlib.Path, default=pathlib.Path("lib/world"))
    p.add_argument("--staging", type=pathlib.Path, required=True)
    args=p.parse_args()
    original=args.world/"mob"
    destination=args.staging/"mob"
    try:
        if not original.is_dir() or original.is_symlink():
            raise ValueError("missing mobile world directory")
        if args.staging.resolve() == args.world.resolve() or args.world.resolve() in args.staging.resolve().parents:
            raise ValueError("staging cannot be inside the live world")
        if args.staging.exists() and any(args.staging.iterdir()):
            raise ValueError("staging directory must be empty")
        files=sorted(original.glob("*.mob"))
        if not files: raise ValueError("no mobile files")
        prepared=[]
        for file in files:
            if file.is_symlink() or file.stat().st_size > MAX_SIZE:
                raise ValueError(f"unsafe input: {file}")
            before=file.read_text(encoding="latin-1")
            after,changes=process(before)
            prepared.append((file,after,changes))
        destination.mkdir(parents=True, exist_ok=False)
        summary=[]
        for source,data,changes in prepared:
            (destination/source.name).write_text(data,encoding="latin-1")
            summary.extend({"source_file":source.name,**x} for x in changes)
        for index in ("index","index.mini"):
            if (original/index).exists():
                shutil.copyfile(original/index,destination/index)
        (args.staging/"level50-report.json").write_text(json.dumps({
            "mob_files":len(prepared),"changed_mobs":len(summary),
            "changes":summary,
            "caveats":["Only standard mobile records are supported.",
                       "NPC scripts, shopkeepers, special NPCs and rewards need review.",
                       "Test server boot and combat verification required before installation."]},indent=2)+"\n")
        print(f"Staged {len(prepared)} files, {len(summary)} changed mobiles in {args.staging}")
    except (OSError,ValueError) as error:
        print(f"REJECTED: {error}",file=sys.stderr)
        return 1
    return 0

if __name__=="__main__":
    sys.exit(main())
