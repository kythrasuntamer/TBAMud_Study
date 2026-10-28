"""Protect zone 301 post office and sorted room lookup invariants."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Zone301RoomOrdering(unittest.TestCase):
    def test_room_vnums_strictly_increasing(self):
        data = (ROOT / "lib/world/wld/301.wld").read_text()
        rooms = [int(value) for value in re.findall(r"(?m)^#(\d+)\s*$", data)]
        self.assertGreater(len(rooms), 0)
        self.assertEqual(rooms, sorted(set(rooms)),
                         "World rooms must be ordered for binary-search lookup")

    def test_post_office_and_mailbox_spawn_preserved(self):
        world = (ROOT / "lib/world/wld/301.wld").read_text()
        zone = (ROOT / "lib/world/zon/301.zon").read_text()
        self.assertRegex(world, r"(?m)^#30170\s*$\nThe Post Office~")
        self.assertRegex(zone, r"(?m)^M\s+0\s+30128\s+1\s+30170\b")
        self.assertLess(world.index("#30170"), world.index("#30171"))

if __name__ == "__main__":
    unittest.main()
