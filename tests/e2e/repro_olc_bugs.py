#!/usr/bin/env python3
"""Minimal reproducers for the OLC bugs found by soak_builder.py.

Each scenario boots a fresh server (scratch lib, empty player index so the
first character is the implementor), types a SHORT command script as that
implementor, shuts down with `shutdown die`, and then checks:
  * the LeakSanitizer report an ASan build prints at exit,
  * any runtime sanitizer report (ASan / UBSan), or -- with --vg-cmd -- any
    Valgrind error, and
  * for some scenarios, the world files the script saved.
One scenario per server process, so every finding is attributed to exactly
one command script.

Usage:
  repro_olc_bugs.py <circle binary> <source lib dir> [--vg-cmd TEMPLATE]
                    [scenario ...]
  --vg-cmd runs the server under Valgrind (same template as soak_builder.py,
  e.g. "valgrind --error-exitcode=99 ... --log-file={vg_log} {binary} -q -d
  {lib} {port}").  Without it, pass an ASan+UBSan build.

Scenarios (default: all):
  qedit-save         qedit <new vnum>, change the name, quit, save.
                     Leak: qedit's save path frees only the OLC quest struct
                     (CLEANUP_STRUCTS) although add_quest() deep-copied its
                     five strings.  Fixed by ef4d615 (F2-13).
  redit-quit         redit 3001 (a room with one prototype trigger), quit
                     without changes.
  redit-delete       redit 3001, delete it with the editor's X command.
  oedit-quit         oedit 3006 (an object with two prototype triggers), quit
                     without changes.
  medit-quit         medit 3012 (a mob with one prototype trigger), quit
                     without changes.
                     Leak (all four): the editor's private copy of the
                     entity's trigger list (OLC_SCRIPT, from
                     dg_olc_script_copy) is only released when a save hands
                     it to the prototype or an abandon re-attaches it; these
                     exits do neither, and cleanup_olc never frees
                     OLC_SCRIPT.  Fixed by 703f088 (F3-08) / 75bc7f0 (F1-09).
  sedit-copy, redit-copy, oedit-copy, medit-copy
                     open a NEW vnum, 'W'-copy an existing entity over it,
                     abandon.  Leak: the *_setup_existing() copy is built
                     straight over the live buffer, orphaning it (struct,
                     strings, trigger-list copy).  Fixed by 5845db0 (F1-10);
                     the base tip's 03012f8 fixes redit/oedit/medit only.
  medit-new-delete   save a new mob prototype, then delete it with X.
                     Leak: delete_mobile() "extracts" the prototype (it is
                     not in character_list, so nothing is freed; the log says
                     "Couldn't find 1 extractions") and shifts mob_proto over
                     it.  Fixed by 3b08462 (C1-23) / 64d803c.
  oedit-new-delete   save a new object prototype, then delete it with X.
                     Leak: delete_object() shifts obj_proto over the deleted
                     slot without freeing its strings.  Fixed by 50f326f
                     (F1-V02).
  shop-keeper-nobody save a NEW shop (keeper NOBODY), create and delete any
                     mob prototype, then `sedit save 30`.  delete_mobile()
                     renumbers every shop keeper with `keeper -= (keeper >=
                     deleted rnum)`, which also turns NOBODY (65535) into
                     65534; save_shops() then reads mob_index[65534] -- far
                     out of bounds (Valgrind: "Invalid read of size 2" at
                     genshp.c:410) -- and writes a garbage keeper vnum into
                     30.shp.  The check reads that file: shop 3010's keeper
                     must still be -1.  Fixed by 8cb7eba (C1-V01, C1-22) /
                     7b3d7e9.
  redit-new-delete   the same delete for a room; expected CLEAN (delete_room
                     frees the room's strings first) -- a negative control.
  control            log in and shut down only (must be clean).

Exit status follows regression-test convention: 0 when every requested
scenario is clean (the bug is fixed), 1 when any scenario reproduces, 2 on a
setup failure.  On audit/macro-inventory-and-dg-followup-20261027 as of
aeeb58d every scenario except the two controls is expected to exit 1.

Stdlib only.  Shares the server/telnet helpers in soak_builder.py.
"""
import argparse
import importlib.util
import os
import re
import shutil
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True   # no stray tests/e2e/__pycache__ in the tree
_spec = importlib.util.spec_from_file_location(
    "soak_builder", os.path.join(_HERE, "soak_builder.py"))
sb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sb)


def shop_keeper_field(lib, zone, shop_vnum):
    """Return the keeper field of `#shop_vnum~` in world/shp/<zone>.shp.

    Record layout (save_shops): products..-1, two profit lines, buy types..-1,
    seven message lines ending in '~', then temper, bitvector, KEEPER vnum,
    notrade, rooms..-1, four open/close hours."""
    path = os.path.join(lib, "world", "shp", f"{zone}.shp")
    try:
        lines = open(path, "r", errors="replace").read().splitlines()
    except OSError as e:
        return None, f"cannot read {path}: {e}"
    try:
        i = lines.index(f"#{shop_vnum}~")
    except ValueError:
        return None, f"shop #{shop_vnum} not in {path}"
    msgs = 0
    for j in range(i + 1, len(lines)):
        if lines[j].endswith("~"):
            msgs += 1
            if msgs == 7:
                return lines[j + 3].strip(), None
    return None, f"shop #{shop_vnum} record truncated in {path}"


def check_shop_keeper(lib):
    keeper, err = shop_keeper_field(lib, 30, 3010)
    if err:
        return [err]
    if keeper != "-1":
        return [f"shop 3010 keeper saved as {keeper!r}, expected '-1' (NOBODY "
                f"was renumbered by delete_mobile and read out of bounds)"]
    return []


SCENARIOS = {
    "qedit-save":   (["qedit 3050", "1", "Leak Quest", "q", "y"], None),
    "redit-quit":   (["redit 3001", "q"], None),
    "redit-delete": (["redit 3001", "x", "y"], None),
    "oedit-quit":   (["oedit 3006", "q"], None),
    "medit-quit":   (["medit 3012", "q"], None),
    # 'W' (copy) into a buffer that setup_new already allocated, then abandon.
    "sedit-copy":   (["sedit 3010", "w", "3000", "q", "n"], None),
    "redit-copy":   (["redit 3070", "w", "3040", "q", "n"], None),
    "oedit-copy":   (["oedit 3007", "w", "3000", "q", "n"], None),
    "medit-copy":   (["medit 3014", "w", "3060", "q", "n"], None),
    # Save a NEW prototype, then delete it with the editor's X command.
    "medit-new-delete": (["medit 3014", "2", "leakmob soak", "q", "y",
                          "medit 3014", "x", "y"], None),
    "oedit-new-delete": (["oedit 3007", "1", "leakobj soak", "q", "y",
                          "oedit 3007", "x", "y"], None),
    "shop-keeper-nobody": (["sedit 3010", "5", "1.1", "q", "y",
                            "medit 3014", "2", "keeperbug mob", "q", "y",
                            "medit 3014", "x", "y",
                            "sedit save 30"], check_shop_keeper),
    "redit-new-delete": (["redit 3070", "1", "Leak room", "q", "y",
                          "redit 3070", "x", "y"], None),
    "control":      (["look"], None),
}

WAIT = 30
QUIET, CAP = 0.4, 15


def run(binary, src_lib, name, script, check, vg_cmd):
    work = tempfile.mkdtemp(prefix=f"tb-repro-{name}-")
    server = sb.Server(binary, src_lib, work, sb.free_port(),
                       valgrind=bool(vg_cmd), vg_cmd=vg_cmd)
    wait, quiet, cap = (600, 2.5, 120) if vg_cmd else (WAIT, QUIET, CAP)
    if not server.boot(900 if vg_cmd else 300):
        return None, work, "boot failed"
    imp = None
    try:
        imp = sb.create_char(server.port, "Implo", wait)
        for line in script:
            imp.send(line)
            imp.drain(quiet, cap)
        imp.send("shutdown die")
        if server.wait_exit(300 if vg_cmd else 60) is None:
            server.stop()
    except SystemExit as e:
        server.stop()
        return None, work, f"driver abort: {e}"
    finally:
        if imp:
            imp.close()

    log = open(server.log_path, "r", errors="replace").read()
    leaks = []
    if "ERROR: LeakSanitizer" in log:
        report = log[log.index("ERROR: LeakSanitizer"):]
        for m in re.finditer(r"(Direct|Indirect) leak of (\d+) byte.*?\n((?:\s+#\d+ .*\n)+)",
                             report):
            frames = [re.sub(r".*/src/", "", f.strip()) for f in m.group(3).splitlines()]
            frames = [f.split(" in ", 1)[-1] for f in frames[1:4]]
            leaks.append(f"{m.group(1)} {m.group(2)} B <- " + " <- ".join(frames))
    other = [f[1].splitlines()[0][:160] for f in sb.scan_sanitizer(server)
             if sb.is_runtime_finding(f[1])]
    if vg_cmd:
        for src, snip in sb.scan_valgrind(server, server.proc.returncode):
            other.append(" | ".join(l.strip() for l in snip.splitlines()[:3])[:200])
    problems = check(server.lib) if check else []
    return {"rc": server.proc.returncode, "leaks": leaks, "other": other,
            "problems": problems}, work, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("binary")
    ap.add_argument("src_lib")
    ap.add_argument("--vg-cmd", default=None)
    ap.add_argument("scenario", nargs="*")
    args = ap.parse_args()
    vg_cmd = args.vg_cmd                  # Server substitutes {vg_log} etc.
    names = args.scenario or list(SCENARIOS)
    status = 0
    for name in names:
        if name not in SCENARIOS:
            sys.exit(f"unknown scenario {name!r}; choose from {', '.join(SCENARIOS)}")
        script, check = SCENARIOS[name]
        res, work, err = run(args.binary, args.src_lib, name, script, check, vg_cmd)
        print(f"== {name}: script {script}")
        if err:
            print(f"   SETUP FAILURE: {err} (scratch {work})")
            status = max(status, 2)
            continue
        if res["leaks"] or res["other"] or res["problems"]:
            print(f"   REPRODUCES (server rc={res['rc']}):")
            for item in res["leaks"] + res["other"] + res["problems"]:
                print(f"     {item}")
            print(f"   scratch kept: {work}")
            status = max(status, 1)
        else:
            print(f"   clean (server rc={res['rc']})")
            shutil.rmtree(work, ignore_errors=True)
    sys.exit(status)


if __name__ == "__main__":
    main()
