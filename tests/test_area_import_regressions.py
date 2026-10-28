"""Regression tests for the area-import tools: zone-reset field layout,
key/exit sentinels, shop headers and the SMAUG room-line layout."""
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

refs = load("validate_import_refs")
vnums = load("audit_world_vnums")
smaug = load("convert_smaug_rooms")

class ImportToolRegressions(unittest.TestCase):
    def tree(self, td, files):
        root = pathlib.Path(td)
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        return root

    def test_reset_if_flag_is_not_a_vnum(self):
        # 'M 0 9001 1 200': 0 is the if-flag, 9001 the mob, 200 the room.
        with tempfile.TemporaryDirectory() as td:
            new = self.tree(td, {"new/zon/2.zon": "#2\nA~\n299 30 2\nM 0 9001 1 200\nT 1 2 777 200\nS\n$~\n",
                                 "new/wld/2.wld": "#200\nR~\nD~\n0 0 0 0 0 0\nS\n$~\n"})
            problems = refs.validate(new / "missing", new / "new")["problems"]
            self.assertIn("unresolved mob 9001 in zone 2:4", problems)
            self.assertIn("unresolved trg 777 in zone 2:5", problems)
            self.assertFalse(any("mob 0" in p or "wld 1 " in p for p in problems), problems)

    def test_key_zero_and_room_zero_are_sentinels(self):
        # setup_dir(): to_room 0/-1 -> NOWHERE; stock rooms use key 0 for none.
        with tempfile.TemporaryDirectory() as td:
            root = self.tree(td, {"new/wld/2.wld": "#200\nR~\nD~\n0 0 0 0 0 0\nD0\n~\n~\n0 0 0\nS\n$~\n"})
            self.assertEqual([], refs.validate(root / "old", root / "new")["problems"])

    def test_shop_vnum_collision_detected(self):
        with tempfile.TemporaryDirectory() as td:
            root = self.tree(td, {"old/shp/30.shp": "CircleMUD v3.0 Shop File~\n#3000~\n$~\n",
                                  "new/shp/90.shp": "CircleMUD v3.0 Shop File~\n#3000~\n$~\n"})
            problems = vnums.audit(root / "old", root / "new")["problems"]
            self.assertTrue(any("COLLISION shp VNUM 3000" in p for p in problems), problems)

    def test_smaug_sector_is_last_field_and_mapped_to_tba(self):
        rooms = smaug.parse_area("#FUSSAREA\n#ROOM\nVnum 10\nName Sky~\nSector air~\n#ENDROOM\n")
        lines = smaug.convert(rooms, 100).splitlines()
        self.assertEqual("0 0 0 0 0 8", lines[3])  # SECT_FLYING, sector last

    def test_smaug_vnum_beyond_idxtype_rejected(self):
        rooms = smaug.parse_area("#FUSSAREA\n#ROOM\nVnum 10\nName X~\n#ENDROOM\n")
        with self.assertRaises(ValueError):
            smaug.convert(rooms, 500000)

fetch = load("fetch_mud_assets")


class FetchAndAuditRegressions(unittest.TestCase):
    def test_catalog_entry_must_pin_a_commit(self):
        with self.assertRaises(ValueError):
            fetch.check_entry({"repo": "a/b"}, allow_unhashed=True)
        with self.assertRaises(ValueError):
            fetch.check_entry({"repo": "a/b", "ref": "HEAD"}, allow_unhashed=True)
        fetch.check_entry({"repo": "a/b", "ref": "0" * 40}, allow_unhashed=True)

    def test_unhashed_entry_refused_by_default(self):
        with self.assertRaises(ValueError):
            fetch.check_entry({"repo": "a/b", "ref": "0" * 40})

    def test_bad_repo_field_rejected(self):
        with self.assertRaises(ValueError):
            fetch.check_entry({"repo": "a/../b", "ref": "0" * 40}, allow_unhashed=True)

    def test_unsafe_member_rejects_whole_archive(self):
        # A good file followed by a traversal entry: nothing may be returned
        # (the old code wrote the good file before failing on the bad one).
        import io, tarfile
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            for name in ("o-r-1/area/good.are", "o-r-1/area/../../evil.are"):
                data = b"#AREA\n"
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        with self.assertRaises(ValueError):
            fetch.scan_archive(buf.getvalue(), {"area_glob": "area/*.are"})

    def test_helper_script_in_world_dir_is_a_note(self):
        with tempfile.TemporaryDirectory() as td:
            root = self.tree(td, {"old/shp/30.shp": "CircleMUD v3.0 Shop File~\n#3000~\n$~\n",
                                  "old/shp/shop-convert.pl": "#!/usr/bin/perl\n",
                                  "new/wld/2.wld": "#200\nR~\nD~\n0 0 0 0 0 0\nS\n$~\n"})
            report = vnums.audit(root / "old", root / "new")
            self.assertEqual([], report["problems"])
            self.assertTrue(any("shop-convert.pl" in n for n in report["notes"]))

    tree = ImportToolRegressions.tree

if __name__ == "__main__":
    unittest.main()
