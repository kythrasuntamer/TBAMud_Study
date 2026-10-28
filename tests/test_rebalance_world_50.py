"""Contract tests for tools/rebalance_world_50.py (stage-and-report)."""
import importlib.util, json, os, pathlib, re, subprocess, sys, tempfile, unittest

ROOT = pathlib.Path(os.environ.get("TB_SOURCE_DIR", pathlib.Path(__file__).resolve().parents[1]))
TOOL = pathlib.Path(os.environ.get("TB_REBALANCE_TOOL", ROOT / "tools" / "rebalance_world_50.py"))
spec = importlib.util.spec_from_file_location("rb", TOOL)
rb = importlib.util.module_from_spec(spec); spec.loader.exec_module(rb)

def lvl_immort():
    m = re.search(r"#define LVL_IMMORT\s+(\d+)", (ROOT / "src/structs.h").read_text())
    return int(m.group(1))

MOB = ("#{v}\nkw~\nshort~\nlong\n~\ndesc\n~\n0 0 0 0 0 0 0 0 0 E\n"
       "{lvl} 10 5 2d8+{lvl} 1d4+1\n10 {exp}\n8 8 0\nE\n")

def mobfile(*lvls):
    return "".join(MOB.format(v=100 + i, lvl=l, exp=100 * l) for i, l in enumerate(lvls)) + "$\n"

def run(world, staging):
    return subprocess.run([sys.executable, "-I", str(TOOL), "--world", str(world),
                           "--staging", str(staging)], capture_output=True, text=True)

class Rebalance(unittest.TestCase):
    def test_levels_monotonic_and_mortal(self):
        top = lvl_immort() - 1
        prev = 0
        for lvl in range(1, top + 1):
            new = rb.target_level(lvl)
            self.assertGreaterEqual(new, prev, f"level {lvl} -> {new} below {prev}")
            self.assertLessEqual(new, top, f"level {lvl} -> {new} is an immortal level")
            prev = new

    def test_empty_zone_file_is_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            w = pathlib.Path(d, "world/mob"); w.mkdir(parents=True)
            (w / "1.mob").write_text(mobfile(20)); (w / "2.mob").write_text("$\n")
            r = run(w.parent, pathlib.Path(d, "stage"))
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_error_names_file_and_line(self):
        with tempfile.TemporaryDirectory() as d:
            w = pathlib.Path(d, "world/mob"); w.mkdir(parents=True)
            (w / "7.mob").write_text(mobfile(20).replace("1d4+1", "garbage"))
            r = run(w.parent, pathlib.Path(d, "stage"))
            self.assertNotEqual(r.returncode, 0)
            self.assertRegex(r.stderr, r"7\.mob:\d+")
            self.assertFalse(pathlib.Path(d, "stage").exists(), "partial staging left behind")

    def test_live_world_untouched_and_report(self):
        with tempfile.TemporaryDirectory() as d:
            w = pathlib.Path(d, "world/mob"); w.mkdir(parents=True)
            src = mobfile(5, 20, 30); (w / "3.mob").write_text(src)
            r = run(w.parent, pathlib.Path(d, "stage"))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual((w / "3.mob").read_text(), src)
            rep = json.loads(pathlib.Path(d, "stage/level50-report.json").read_text())
            self.assertEqual(rep["changed_mobs"], 2)

    def test_immortal_level_is_rejected_with_location(self):
        top = lvl_immort() - 1
        with tempfile.TemporaryDirectory() as d:
            w = pathlib.Path(d, "world/mob"); w.mkdir(parents=True)
            (w / "9.mob").write_text(mobfile(20, top + 1))
            r = run(w.parent, pathlib.Path(d, "stage"))
            self.assertNotEqual(r.returncode, 0)
            self.assertRegex(r.stderr, r"9\.mob:\d+: mobile #101 is level \d+, an immortal level")
            self.assertFalse(pathlib.Path(d, "stage").exists())

    def test_index_lists_files_and_missing_entry_fails(self):
        with tempfile.TemporaryDirectory() as d:
            w = pathlib.Path(d, "world/mob"); w.mkdir(parents=True)
            (w / "1.mob").write_text(mobfile(20))
            (w / "5.mob").write_text(mobfile(20))      # not listed: not staged
            (w / "index").write_text("1.mob\n$\n")
            r = run(w.parent, pathlib.Path(d, "stage"))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(pathlib.Path(d, "stage/mob/1.mob").exists())
            self.assertFalse(pathlib.Path(d, "stage/mob/5.mob").exists())
            (w / "index").write_text("1.mob\n2.mob\n$\n")
            r = run(w.parent, pathlib.Path(d, "stage2"))
            self.assertNotEqual(r.returncode, 0)
            self.assertRegex(r.stderr, r"index:2: listed file 2\.mob is missing")

    def test_ac_change_is_bounded_and_reported(self):
        with tempfile.TemporaryDirectory() as d:
            w = pathlib.Path(d, "world/mob"); w.mkdir(parents=True)
            (w / "4.mob").write_text(mobfile(34))
            r = run(w.parent, pathlib.Path(d, "stage"))
            self.assertEqual(r.returncode, 0, r.stderr)
            rep = json.loads(pathlib.Path(d, "stage/level50-report.json").read_text())
            change = rep["changes"][0]
            self.assertEqual(change["new"]["level"], lvl_immort() - 1)
            self.assertGreaterEqual(change["new"]["ac_file"], change["old"]["ac_file"] - 5)
            self.assertGreaterEqual(change["new"]["ac_file"], -30)
            for key in ("hp", "damage", "thac0", "exp"):
                self.assertIn(key, change["old"]); self.assertIn(key, change["new"])

if __name__ == "__main__":
    unittest.main()
