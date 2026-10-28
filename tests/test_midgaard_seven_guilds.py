"""Regression checks for the seven Midgaard guild locations."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
ROOMS = (3034, 3035, 3036, 3037, 3038, 3039, 3055, 3056, 3057)
MENTORS = ((3014, 3035), (3015, 3036), (3016, 3037),
           (3017, 3038), (3018, 3039), (3019, 3055), (3028, 3056))

class MidgaardGuildTests(unittest.TestCase):
    def test_rooms_unique_ordered_and_present(self):
        data = (ROOT / "lib/world/wld/30.wld").read_text()
        ids = [int(x) for x in re.findall(r"(?m)^#(\d+)$", data)]
        self.assertEqual(ids, sorted(set(ids)))
        for room in ROOMS:
            self.assertIn(room, ids)

    def test_mentors_spawn_and_monk_is_trainable(self):
        mob = (ROOT / "lib/world/mob/30.mob").read_text()
        zone = (ROOT / "lib/world/zon/30.zon").read_text()
        ids = [int(x) for x in re.findall(r"(?m)^#(\d+)$", mob)]
        self.assertEqual(ids, sorted(set(ids)))
        for mentor, room in MENTORS:
            self.assertIn(mentor, ids)
            self.assertIn(f"M 0 {mentor} 1 {room} ", zone)
        assign = (ROOT / "src/spec_assign.c").read_text()
        self.assertIn("ASSIGNMOB(3014, guild);", assign)
        self.assertIn("ASSIGNMOB(3023, guild);", assign)

    def test_necromancer_study_connected_and_class_registered(self):
        world = (ROOT / "lib/world/wld/30.wld").read_text()
        mobs = (ROOT / "lib/world/mob/30.mob").read_text()
        zone = (ROOT / "lib/world/zon/30.zon").read_text()
        structs = (ROOT / "src/structs.h").read_text()
        self.assertIn("#3067\nThe Necromancer's Study~", world)
        self.assertIn("0 -1 3067", world)
        self.assertIn("0 -1 3038", world)
        self.assertIn("#3058\nYe Olde Water Shoppe~", world)
        self.assertIn("#3029\nnecromancer mentor scholar~", mobs)
        self.assertIn("M 0 3029 1 3067", zone)
        self.assertIn("#define CLASS_NECROMANCER 11", structs)
        self.assertRegex(structs, r"#define NUM_CLASSES\s+12\b")

    def test_all_mentor_spawn_rooms_exist(self):
        world = (ROOT / "lib/world/wld/30.wld").read_text()
        zone = (ROOT / "lib/world/zon/30.zon").read_text()
        rooms = {int(x) for x in re.findall(r"(?m)^#(\d+)$", world)}
        for mentor, room in MENTORS + ((3029, 3067),):
            self.assertIn(room, rooms)
            self.assertIn(f"M 0 {mentor} 1 {room} ", zone)

    def test_midgaard_connection(self):
        world = (ROOT / "lib/world/wld/30.wld").read_text()
        self.assertIn("0 -1 3057", world)
        self.assertIn("0 -1 3000", world)
        self.assertIn("0 -1 3034", world)

if __name__ == "__main__":
    unittest.main()
