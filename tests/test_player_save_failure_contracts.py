"""Conservative source-contract guards for staged character save integrity.

These do NOT replace runtime syscall fault injection. They protect the
production save path's commit ordering, failure handling, and persistent tags.
"""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src" / "players.c").read_text()
SAVE = SOURCE.split("void save_char(", 1)[1].split("/** Separate a 4-character id tag", 1)[0]


class SaveFailurePathContracts(unittest.TestCase):
    def test_destination_is_never_opened_for_truncation(self):
        self.assertNotIn('fopen(filename, "w")', SAVE)
        self.assertNotIn('freopen(filename, "w"', SAVE)

    def test_staged_creation_precedes_all_writes_and_commit(self):
        self.assertLess(SAVE.index("mkstemp(temporary_filename)"),
                        SAVE.index('fdopen(temp_fd, "w")'))
        self.assertLess(SAVE.index('fdopen(temp_fd, "w")'),
                        SAVE.index('rename(temporary_filename, filename)'))

    def test_staged_path_truncation_is_rejected_before_creation(self):
        self.assertIn("path_len < 0 || (size_t)path_len >= sizeof(temporary_filename)", SAVE)
        self.assertIn('TB_ERROR_MSG("player", "snprintf"', SAVE)
        self.assertLess(SAVE.index("path_len < 0"), SAVE.index("mkstemp(temporary_filename)"))

    def test_each_io_failure_has_own_diagnostic(self):
        for operation in ("fflush", "fclose", "rename", "unlink"):
            with self.subTest(operation=operation):
                self.assertIn(f'TB_ERROR_ERRNO("player", "{operation}"', SAVE)
        self.assertIn('TB_ERROR_MSG("player", "write"', SAVE)

    def test_failed_save_cannot_attempt_replace(self):
        self.assertIn(
            "if (!save_failed && rename(temporary_filename, filename) != 0)",
            SAVE)
        self.assertIn(
            "if (save_failed && unlink(temporary_filename) != 0)",
            SAVE)

    def test_index_update_is_after_commit_success(self):
        failure_return = SAVE.index("if (save_failed)\n    return;")
        index_update = SAVE.index("/* update the player in the player index */")
        self.assertLess(failure_return, index_update)

    def test_endgame_fields_are_still_serialized(self):
        for name in ("EgAP", "EgXP", "EgHP", "EgMP"):
            with self.subTest(field=name):
                self.assertIn(f'"{name}: %d', SAVE)


if __name__ == "__main__":
    unittest.main()
