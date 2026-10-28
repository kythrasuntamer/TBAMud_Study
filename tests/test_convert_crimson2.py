"""Tests for tools/convert_crimson2.py: field mapping by meaning, fail-closed
references, merge/override/link recipes, and that the installed world is
exactly the recipe's output."""
import importlib.util
import json
import pathlib
import re
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("convert_crimson2", ROOT / "tools" / "convert_crimson2.py")
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)
RECIPE = ROOT / "imports" / "quarantine" / "crimson2" / "recipe.json"

ROOM = "#100\nA cave~\nDark.\n~\n9 {f1} {f2} 1 99 {sect}\n{exits}S\n#999999\n"
MOB = ("#100\nmob orc~\nthe orc~\nAn orc.\n~\nUgly.\n~\n"
       "8 1 136249361 0 -100 S\n5 20 5 2d8+10 1d6+1\n10 100\n8 8 1\n"
       "0 0 13 13 13 13 13 13\nATTACK_TYPE=108\n"
       "#101\nmob ogre~\nthe ogre~\nAn ogre.\n~\nBig.\n~\n"
       "8 1 136249361 0 -100 S\n9 20 5 2d8+10 1d6+1\n10 100\n8 8 1\n"
       "0 0 13 13 13 13 13 13\n#999999\n")
OBJ = ("#100\nobj axe~\n7\nan axe~\nAn axe.~\n~\n5 {f1} {f2} 8193\n0 2 6 3\n"
       "5 100 10\n#101\nobj scroll~\n1\n&Ga scroll&n~\nA scroll.~\n~\n2 0 0 1\n"
       "5 53 0 0\n1 10 1\nA\n18 100\n#999999\n")
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

    def recipe(self, base, area):
        path = base.parent / "recipe.json"
        path.write_text(json.dumps({"areas": {"T": dict(area, source="T")}}))
        return path

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

    def test_weapon_attack_scroll_spell_and_colour(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out = self.run_tool(self.area(td))
            text = (out / "obj" / "6.obj").read_text()
            lines = text.splitlines()
            self.assertIn("0 2 6 3", lines)            # Crimson slash 3 = tbaMUD slash 3
            self.assertIn("5 52 -1 -1", lines)         # SPELL_IDENTIFY 53 -> 52
            self.assertIn("@Ga scroll@n~", lines)      # Crimson &G..&n -> tbaMUD @G..@n
            self.assertNotIn("&G", text)
            self.assertIn("BareHandAttack: 6", (out / "mob" / "6.mob").read_text())

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

    def test_merge_converts_only_mapped_records_and_their_resets(self):
        # The orc (100) is not mapped; the ogre (101) becomes 7011 and keeps
        # the stock room 100; the orc's G follows an unselected M and is skipped.
        with tempfile.TemporaryDirectory() as td:
            base = self.area(td, resets="M 0 100 1 100\nG 1 100 1\nM 0 101 2 100\n")
            r = self.recipe(base, {"mode": "merge", "zone": 70,
                                   "map": {"mob": {"101": 7011}}})
            out = base.parent.parent / "out"
            self.assertEqual(0, conv.main(["--recipe", str(r), "--area", "T", "--out", str(out)]))
            mob = (out / "mob" / "70.mob.add").read_text()
            self.assertTrue(mob.startswith("#7011\nogre~"))
            self.assertNotIn("orc", mob)
            self.assertEqual("M 0 7011 2 100 \t(the ogre)\n", (out / "zon" / "70.zon.add").read_text())
            self.assertEqual("", (out / "obj" / "70.obj.add").read_text())

    def test_override_and_link_are_applied_and_link_cannot_replace_an_exit(self):
        with tempfile.TemporaryDirectory() as td:
            base = self.area(td)
            area = {"mode": "zone", "zone": 6, "offset": 500,
                    "overrides": {"obj": {"101": {"applies": [], "cost": 3, "reason": "test"}}},
                    "links": [{"room": 100, "dir": "up", "to": 3001, "description": "Up."}]}
            r = self.recipe(base, area)
            out = base.parent.parent / "out"
            self.assertEqual(0, conv.main(["--recipe", str(r), "--area", "T", "--out", str(out)]))
            obj = (out / "obj" / "6.obj").read_text()
            self.assertNotIn("\nA\n18 100", obj)
            self.assertIn("1 3 1 0", obj.splitlines())
            self.assertIn("D4\nUp.~\n~\n0 -1 3001\nS", (out / "wld" / "6.wld").read_text())
        with tempfile.TemporaryDirectory() as td:
            base = self.area(td, exits="D4\n~\n~\n0 -1 100\n")
            area["links"][0]["dir"] = "up"
            r = self.recipe(base, area)
            self.assertEqual(1, conv.main(["--recipe", str(r), "--area", "T",
                                           "--out", str(base.parent.parent / "out")]))

    def test_installed_world_is_the_recipes_output(self):
        # Zone 58 must equal the converter's output byte for byte, and zone 41
        # must end with exactly the merged Moria34 additions, so neither can
        # drift by hand edits.
        with tempfile.TemporaryDirectory() as td:
            out = pathlib.Path(td)
            for area in ("Shatterstone", "Moria34"):
                self.assertEqual(0, conv.main(["--recipe", str(RECIPE), "--area", area,
                                               "--out", str(out)]))
            world = ROOT / "lib" / "world"
            for ext in ("wld", "mob", "obj", "zon"):
                self.assertEqual((world / ext / f"58.{ext}").read_bytes(),
                                 (out / ext / f"58.{ext}").read_bytes(), f"58.{ext}")
            for ext, end in (("mob", "$\n"), ("obj", "$~\n"), ("zon", "S\n$\n")):
                frag = (out / ext / f"41.{ext}.add").read_text(encoding="latin-1")
                stock = (world / ext / f"41.{ext}").read_text(encoding="latin-1")
                self.assertTrue(frag)
                self.assertTrue(stock.endswith(frag + end), f"41.{ext} must end with the merge")

    def test_merged_records_ascend(self):
        # db.c discrete_load() reports a vnum that does not ascend: the merge
        # renumbers Crimson 4104 to 4109, after 4108, so order by new vnum.
        world = ROOT / "lib" / "world"
        for ext in ("mob", "obj"):
            vnums = [int(v) for v in re.findall(r"(?m)^#(\d+)$",
                     (world / ext / f"41.{ext}").read_text(encoding="latin-1"))]
            self.assertEqual(sorted(vnums), vnums, f"41.{ext}")

    def test_zone_47_is_not_installed(self):
        world = ROOT / "lib" / "world"
        for ext in ("wld", "mob", "obj", "zon"):
            self.assertFalse((world / ext / f"47.{ext}").exists())
            self.assertNotIn(f"47.{ext}", (world / ext / "index").read_text().split())


if __name__ == "__main__":
    unittest.main()
