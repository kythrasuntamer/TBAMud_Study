"""Tests for global VNUM conflicts and reciprocal room exits."""
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

def load(name):
    file = ROOT / "tools" / (name + ".py")
    spec = importlib.util.spec_from_file_location(name, file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

audit = load("audit_world_vnums")
bridge = load("bridge_areas")

class ImportSafetyTests(unittest.TestCase):
    def test_duplicate_cross_file_vnum_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            old, new = root / "existing", root / "incoming"
            for folder, file, contents in [
                (old, "12.wld", "#1200\nRoom~\nS\n"),
                (new, "50.wld", "#1200\nDifferent~\nS\n"),
            ]:
                path = folder / "wld"
                path.mkdir(parents=True)
                (path / file).write_text(contents)
            result = audit.audit(old, new)
            self.assertTrue(any("COLLISION wld VNUM 1200" in x
                                for x in result["problems"]))

    def test_distinct_vnums_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            old, new = root / "old", root / "new"
            (old / "wld").mkdir(parents=True)
            (new / "wld").mkdir(parents=True)
            (old / "wld" / "1.wld").write_text("#100\nA~\nS\n")
            (new / "wld" / "2.wld").write_text("#200\nB~\nS\n")
            self.assertEqual([], audit.audit(old, new)["problems"])

    def test_bridge_adds_exit_and_refuses_duplicate(self):
        world = "#100\nStarting room~\nDescription~\n0 0 0 0 0 0\nS\n"
        modified = bridge.add_exit(world, 100, 1, 200)
        self.assertIn("D1\n~\n~\n0 -1 200\nS", modified)
        with self.assertRaises(ValueError):
            bridge.add_exit(modified, 100, 1, 300)

    def test_bridge_preserves_other_rooms(self):
        world = "#100\nFirst~\nS\n#101\nSecond~\nS\n"
        modified = bridge.add_exit(world, 100, 4, 200)
        self.assertTrue(modified.endswith("#101\nSecond~\nS\n"))

if __name__ == "__main__":
    unittest.main()
