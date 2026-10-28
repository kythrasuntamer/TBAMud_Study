#!/usr/bin/env python3
"""Read-only inventory of C preprocessor macros; no source rewriting.

Usage: python3 tools/audit_macros.py --root . --report macro-audit.txt
Scans project C/H files; skips generated/build/vendor directories.
"""
import argparse
from collections import defaultdict
from pathlib import Path
import re

DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(\([^\n]*?\))?(.*)$")
SKIP = {".git", "build", "vendor", "third_party", "external", "CMakeFiles"}
EXT = {".c", ".h"}
def audit(root):
    found = defaultdict(list)
    scanned = 0
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in EXT:
            continue
        rel = path.relative_to(root)
        if any(p in SKIP or p.startswith(("build-", "cmake-build-")) for p in rel.parts):
            continue
        scanned += 1
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            match = DEFINE.match(line)
            if match:
                name, params, body = match.groups()
                found[name].append((str(rel), number, (params or "") + body.strip()))
    return scanned, found

def render(scanned, found):
    total = sum(map(len, found.values()))
    lines = [f"Files scanned: {scanned}", f"Definitions: {total}",
             f"Distinct names: {len(found)}", "",
             "Repeated names (review conditions before consolidating):"]
    for name, defs in sorted(found.items()):
        if len(defs) < 2:
            continue
        lines.append(f"  {name} ({len(defs)})")
        for path, line, body in defs:
            lines.append(f"    {path}:{line} {body[:100]}")
    lines += ["", "Macro counts by source file:"]
    per_file = defaultdict(int)
    for defs in found.values():
        for path, _, _ in defs:
            per_file[path] += 1
    for path, count in sorted(per_file.items(), key=lambda x: (-x[1], x[0])):
        lines.append(f"  {count:5} {path}")
    lines += ["", "Audit note: repeated names may be intentional #if branches or",
              "header guards; this report does NOT prove duplicate active definitions.",
              "No source files were modified."]
    return "\n".join(lines) + "\n"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    scanned, found = audit(args.root.resolve())
    report = render(scanned, found)
    if args.report:
        args.report.write_text(report, encoding="utf-8")
    else:
        print(report, end="")
if __name__ == "__main__":
    main()
