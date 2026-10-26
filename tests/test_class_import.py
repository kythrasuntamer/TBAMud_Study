"""Test class ID invariants and skill prerequisite validation."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("validate_class_import",ROOT/"tools/validate_class_import.py")
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
BASE=json.loads((ROOT/"data/class_import_candidates.json").read_text())
class ClassImportTests(unittest.TestCase):
    def test_upstream_inventory_ids_stable(self):
        self.assertEqual([],mod.validate(BASE))
    def test_existing_saved_character_ids_cannot_change(self):
        m=copy.deepcopy(BASE);m["existing"][2]["id"]=7
        self.assertTrue(any("existing player class IDs" in x for x in mod.validate(m)))
    def test_missing_prerequisite_fails(self):
        m=copy.deepcopy(BASE)
        m["skill_trees"]=[{"class":"Ranger","skills":[{"key":"archery","level":5,"requires":["missing"]}]}]
        self.assertTrue(any("unresolved prerequisite" in x for x in mod.validate(m)))
    def test_cycle_fails(self):
        m=copy.deepcopy(BASE)
        m["skill_trees"]=[{"class":"Monk","skills":[
            {"key":"a","level":5,"requires":["b"]},
            {"key":"b","level":10,"requires":["a"]}]}]
        self.assertTrue(any("skill cycle" in x for x in mod.validate(m)))
    def test_immortal_only_skill_rejected(self):
        m=copy.deepcopy(BASE)
        m["skill_trees"]=[{"class":"Druid","skills":[{"key":"future","level":51,"requires":[]}]}]
        self.assertTrue(any("invalid skill level" in x for x in mod.validate(m)))
if __name__=="__main__":
    unittest.main()
