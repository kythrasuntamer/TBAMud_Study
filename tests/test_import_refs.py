"""Regression tests for staged world cross-reference checking."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location("validate_import_refs",Path(__file__).resolve().parents[1]/"tools/validate_import_refs.py")
validator=importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)

class ImportReferenceTests(unittest.TestCase):
    def test_rejects_unresolved_room_exit(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td); old=base/"old"; new=base/"new"
            (old/"wld").mkdir(parents=True);(new/"wld").mkdir(parents=True)
            (old/"wld"/"1.wld").write_text("#100\nOriginal~\nDescription~\n0 0 0 0 0 0\nS\n")
            (new/"wld"/"2.wld").write_text("#200\nImported~\nDescription~\n0 0 0 0 0 0\nD0\n~\n~\n0 -1 99999\nS\n")
            result=validator.validate(old,new)
            self.assertTrue(any("unresolved exit" in p for p in result["problems"]))
    def test_accepts_remapped_reference(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td);old=base/"old";new=base/"new"
            old.mkdir();(new/"wld").mkdir(parents=True)
            (new/"wld"/"2.wld").write_text("#200\nRoom~\nDesc~\n0 0 0 0 0 0\nD0\n~\n~\n0 -1 201\nS\n#201\nNext~\nDesc~\n0 0 0 0 0 0\nS\n")
            result=validator.validate(old,new)
            self.assertEqual([],result["problems"])
    def test_reset_checks_object_and_mobile_vnums(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td);old=base/"old";new=base/"new"
            old.mkdir();(new/"zon").mkdir(parents=True)
            (new/"zon"/"2.zon").write_text("#2\nArea~\n299 30 2\nM 0 9001 1 200\nO 0 8000 1 200\nS\n")
            result=validator.validate(old,new)
            self.assertTrue(any("unresolved mob 9001" in p for p in result["problems"]))
            self.assertTrue(any("unresolved obj 8000" in p for p in result["problems"]))
if __name__=="__main__":
    unittest.main()
