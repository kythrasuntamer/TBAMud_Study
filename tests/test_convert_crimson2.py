"""Tests for tools/convert_crimson2.py: field mapping by meaning, fail-closed
references, and that the installed zones 47/58 are exactly its output."""
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("convert_crimson2", ROOT / "tools" / "convert_crimson2.py")
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)

ROOM = "#100\nA cave~\nDark.\n~\n9 {f1} {f2} 1 99 {sect}\n{exits}S\n#999999\n"
MOB = ("#100\nmob orc~\nthe orc~\nAn orc.\n~\nUgly.\n~\n"
       "8 1 136249361 0 -100 S\n5 20 5 2d8+10 1d6+1\n10 100\n8 8 1\n"
       "0 0 13 13 13 13 13 13\nATTACK_TYPE=108\n#999999\n")
OBJ = ("#100\nobj axe~\n7\nan axe~\nAn axe.~\n~\n5 {f1} {f2} 8193\n0 2 6 3\n"
       "5 100 10\n#101\nobj scroll~\n1\na scroll~\nA scroll.~\n~\n2 0 0 1\n"
       "5 53 0 0\n1 10 1\n#999999\n")
ZON = "#0\nTest~\n199 10 2 0 0 0\n{resets}S\n$~\n#999999\n"


class Crimson2Conversion(unittest.TestCase):
    def area(self, td, f1=8, f2=0, sect=1, exits="", resets="M 0 100 1 100\n",
             o1=0, o2=0):
        base = pathlib.Path(td) / "src" / "T"
        base.parent.mkdir(parents=True)
        base.with_suffix(".wld").write_text(ROOM.format(f1=f1, f2=f2, sect=sect, exits=exits))
        base.with_suffix(".mob").write_text(MOB)
        base.with_suffix(".obj").write_text(OBJ.format(f1=o1, f2=o2))
        base.with_suffix(".zon").write_text(ZON.format(resets=resets))
        return base

    def run_tool(self, base, *extra, offset="500"):
        out = base.parent.parent / "out"
        rc = conv.main([str(base), "--zone", "6", "--offset", offset, "--out", str(out), *extra])
        return rc, out

    def test_flags_mapped_by_meaning(self):
        # Crimson OBJ1_ANTI_THIEF (13) / ANTI_CLERIC (14) are tbaMUD 14 / 13;
        # OBJ1_MINLVL20 (bit 20) becomes the object level.
        with tempfile.TemporaryDirectory() as td:
            rc, out = self.run_tool(self.area(td, o1=(1 << 13) | (1 << 20)))
            self.assertEqual(0, rc)
            lines = (out / "obj" / "6.obj").read_text().splitlines()
            self.assertEqual(f"5 {1 << 14} 0 0 0 8193 0 0 0 0 0 0 0", lines[5])
            self.assertEqual("5 100 10 20", lines[7])
            self.assertEqual("axe~", lines[1])            # "obj" prefix dropped

    def test_room_sector_and_flags(self):
        # SECT_UNDERWATER 8 -> tbaMUD 9; RM1_BFS_MARK (runtime) dropped
        # silently; RM1_NOPKILL is PvP and is reported.
        with tempfile.TemporaryDirectory() as td:
            base = self.area(td, f1=(1 << 3) | (1 << 20) | (1 << 15), sect=8)
            rc, out = self.run_tool(base)
            self.assertEqual(0, rc)
            self.assertIn("6 8 0 0 0 9", (out / "wld" / "6.wld").read_text().splitlines())

    def test_weapon_attack_and_scroll_spell(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out = self.run_tool(self.area(td))
            lines = (out / "obj" / "6.obj").read_text().splitlines()
            self.assertIn("0 2 6 3", lines)            # Crimson slash 3 = tbaMUD slash 3
            self.assertIn("5 52 -1 -1", lines)         # SPELL_IDENTIFY 53 -> 52
            mob = (out / "mob" / "6.mob").read_text()
            self.assertIn("BareHandAttack: 6", mob)    # Crimson TYPE_CRUSH 108 -> 6

    def test_external_exit_fails_unless_dropped(self):
        exits = "D1\n~\n~\n0 -1 4099\n"
        with tempfile.TemporaryDirectory() as td:
            rc, out = self.run_tool(self.area(td, exits=exits))
            self.assertEqual(1, rc)
            self.assertFalse(out.exists())
        with tempfile.TemporaryDirectory() as td:
            rc, out = self.run_tool(self.area(td, exits=exits), "--drop-external")
            self.assertEqual(0, rc)
            self.assertNotIn("D1", (out / "wld" / "6.wld").read_text())

    def test_unresolved_reset_reference_fails_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out = self.run_tool(self.area(td, resets="M 0 9001 1 100\n"))
            self.assertEqual(1, rc)
            self.assertFalse(out.exists())

    def test_vnum_past_idxtype_fails(self):
        with tempfile.TemporaryDirectory() as td:
            rc, _ = self.run_tool(self.area(td), offset="65500")
            self.assertEqual(1, rc)

    def test_installed_zones_are_the_tools_output(self):
        # lib/world/*/47.* and 58.* must be exactly what the converter makes
        # from the quarantined sources, so they cannot drift by hand edits.
        q = ROOT / "imports" / "quarantine" / "crimson2"
        with tempfile.TemporaryDirectory() as td:
            for src, zone, off, name in (
                    ("Moria34", 47, 600, "Mines of Moria, Levels 3-4 (Crimson 2)"),
                    ("Shatterstone", 58, 0, "Shatterstone (Crimson 2, unfinished)")):
                rc = conv.main([str(q / src), "--zone", str(zone), "--offset", str(off),
                                "--out", td, "--name", name, "--drop-external"])
                self.assertEqual(0, rc)
                for ext in ("wld", "mob", "obj", "zon"):
                    made = (pathlib.Path(td) / ext / f"{zone}.{ext}").read_bytes()
                    shipped = (ROOT / "lib" / "world" / ext / f"{zone}.{ext}").read_bytes()
                    self.assertEqual(shipped, made, f"{zone}.{ext}")


if __name__ == "__main__":
    unittest.main()
