"""Regression tests for the SMAUG FUSS structural inventory."""
import importlib.util
import pathlib
import unittest

FILE = pathlib.Path(__file__).resolve().parents[1] / "tools/inventory_smaug_fuss.py"
spec = importlib.util.spec_from_file_location("inventory_smaug", FILE)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

EXAMPLE = """#FUSSAREA
#ROOM
Vnum 3405
Name Hall~
#EXIT
Direction north~
ToRoom 3406
#ENDEXIT
Reset M 0 3400 1 3405
#ENDROOM
#ROOM
Vnum 3406
Name Side room~
#ENDROOM
#MOBILE
Vnum 3400
Keywords guard~
#ENDMOBILE
#OBJECT
Vnum 3430
Keywords key~
#ENDOBJECT
"""

class SmaugInventoryTests(unittest.TestCase):
    def test_preserves_every_record_family_and_reset(self):
        found = mod.scan(EXAMPLE)
        self.assertEqual([3405, 3406], [x["vnum"] for x in found["room"]])
        self.assertEqual(3400, found["mobile"][0]["vnum"])
        self.assertEqual(3430, found["object"][0]["vnum"])
        self.assertEqual([3406], found["room"][0]["exit_destinations"])
        self.assertEqual("M", found["room"][0]["resets"][0]["command"])
        self.assertIn("Reset M 0 3400 1 3405", found["room"][0]["raw"])

    def test_duplicate_vnum_rejected(self):
        self.assertRaises(ValueError, mod.scan,
                          EXAMPLE + "#OBJECT\nVnum 3430\n#ENDOBJECT\n")

    def test_unclosed_record_rejected(self):
        self.assertRaises(ValueError, mod.scan, "#FUSSAREA\n#ROOM\nVnum 1\n")

    def test_wrong_format_rejected(self):
        self.assertRaises(ValueError, mod.scan, "#AREA\n#ROOM\nVnum 1\n#ENDROOM\n")

if __name__ == "__main__":
    unittest.main()
