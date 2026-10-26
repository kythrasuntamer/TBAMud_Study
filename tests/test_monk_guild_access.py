"""Protect Monk guild access without weakening legacy guild restrictions."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
class MonkGuildAccessTests(unittest.TestCase):
    def test_monk_shares_warrior_guild_only(self):
        guards = (ROOT / "src/spec_procs.c").read_text()
        start = guards.index("SPECIAL(guild_guard)")
        end = guards.index("SPECIAL(puff)", start)
        body = guards[start:end]
        self.assertIn("GET_CLASS(ch) == guild_info[i].pc_class", body)
        self.assertIn("GET_CLASS(ch) == CLASS_MONK && guild_info[i].pc_class == CLASS_WARRIOR", body)
        self.assertIn("guild_info[i].guild_room", body)
        self.assertIn("guild_info[i].direction", body)
        self.assertIn("return (TRUE);", body)

    def test_dg_script_guard_allows_only_warrior_and_monk(self):
        scripts = (ROOT / "lib/world/trg/30.trg").read_text()
        guard = scripts.split("#3003\\n", 1)[1].split("#3004\\n", 1)[0]
        self.assertIn("if %direction% == east", guard)
        self.assertIn("if %actor.class% != Warrior && %actor.class% != Monk", guard)
        self.assertIn("return 0", guard)
        self.assertIn("The guard humiliates you", guard)
        for original in ("Magic User", "Cleric", "Thief"):
            self.assertIn("if %actor.class% != " + original, scripts)

    def test_warrior_guild_and_trainer_remain_assigned(self):
        classes = (ROOT / "src/class.c").read_text()
        assign = (ROOT / "src/spec_assign.c").read_text()
        self.assertIn("{ CLASS_WARRIOR,       3021,    EAST", classes)
        self.assertIn("ASSIGNMOB(3023, guild);", assign)
        self.assertIn("spell_level(SKILL_KICK, CLASS_MONK, 1);", classes)

if __name__ == "__main__":
    unittest.main()
