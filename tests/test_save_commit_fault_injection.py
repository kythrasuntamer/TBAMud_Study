"""Exercise the *actual* production save commit block with controlled failures.

Extracts the block from src/players.c, compiles it with a C test driver, and
executes it in a disposable temporary directory. Never accesses player files.
This is focused commit-boundary testing, not a full save_char integration test.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "players.c").read_text()
START = "  /* Report failures at their origin."
STOP = "  /* More char_to_store code"
assert SOURCE.count(START) == 1 and SOURCE.count(STOP) >= 1
BLOCK = SOURCE[SOURCE.index(START):SOURCE.index(STOP, SOURCE.index(START))]

C_DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define TRUE 1

static const char *fault;
static char reported[256];
static int input_write_error;
static int record_failure(const char *operation, int err) {
  if (!strcmp(fault, operation) ||\n      (!strcmp(fault, "rename+unlink") &&\n       (!strcmp(operation, "rename") || !strcmp(operation, "unlink")))) {\n    errno = err; return 1;\n  }
  return 0;
}
static int injected_ferror(FILE *f) {
  return input_write_error ? 1 : ferror(f);
}
static int injected_fflush(FILE *f) {
  if (record_failure("fflush", ENOSPC)) return EOF;
  return fflush(f);
}
static int injected_fclose(FILE *f) {
  if (record_failure("fclose", EIO)) {
    (void)fclose(f); /* close the fixture even on synthetic failure */
    errno = EIO;
    return EOF;
  }
  return fclose(f);
}
static int injected_rename(const char *a, const char *b) {
  if (record_failure("rename", EACCES)) return -1;
  return rename(a, b);
}
static int injected_unlink(const char *p) {
  if (record_failure("unlink", EACCES)) return -1;
  return remove(p);
}
static void note(const char *op) {
  if (!reported[0]) snprintf(reported, sizeof reported, "%s", op);
}
#define TB_ERROR_MSG(subsystem, op, resource, ...) note(op)
#define TB_ERROR_ERRNO(subsystem, op, resource, ...) note(op)
#define ferror injected_ferror
#define fflush injected_fflush
#define fclose injected_fclose
#define rename injected_rename
#define unlink injected_unlink

static int execute_commit(FILE *fl, const char *temporary_filename,
                          const char *filename, int *first_failure) {
  int save_failed = 0;
__BLOCK__
  *first_failure = reported[0] != '\0';
  return !save_failed;
}
#undef ferror
#undef fflush
#undef fclose
#undef rename
#undef unlink

static int read_equal(const char *filename, const char *expected) {
  char buf[128];
  FILE *f = fopen(filename, "rb");
  size_t n;
  if (!f) return 0;
  n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  return n == strlen(expected) && !memcmp(buf, expected, n);
}
int main(int argc, char **argv) {
  FILE *dest, *stage;
  int reported_failure, committed;
  if (argc != 2) return 3;
  fault = argv[1];
  dest = fopen("saved.plr", "wb");
  if (!dest) return 4;
  fwrite("ORIGINAL", 1, 8, dest);
  fclose(dest);
  stage = fopen("stage.tmp", "wb");
  if (!stage) return 5;
  fwrite("REPLACEMENT", 1, 11, stage);
  input_write_error = !strcmp(fault, "write");
  committed = execute_commit(stage, "stage.tmp", "saved.plr", &reported_failure);
  if (strcmp(fault, "none") == 0)
    return (!committed || !read_equal("saved.plr", "REPLACEMENT")) ? 6 : 0;
  if (committed || !reported_failure || !read_equal("saved.plr", "ORIGINAL"))
    return 7;
  if (!strcmp(fault, "rename+unlink"))\n    return read_equal("stage.tmp", "REPLACEMENT") ? 0 : 8;
  if (strcmp(reported, fault) != 0) return 9;
  return 0;
}
"""

class ProductionSaveCommitFaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if compiler is None:
            raise unittest.SkipTest("C compiler unavailable")
        cls.workspace = tempfile.TemporaryDirectory(prefix="tb-save-fi-")
        cls.root = Path(cls.workspace.name)
        src = cls.root / "driver.c"
        src.write_text(C_DRIVER.replace("__BLOCK__", BLOCK), encoding="utf-8")
        cls.exe = cls.root / "driver"
        result = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra",
                                 "-Werror", "-o", str(cls.exe), str(src)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "workspace"):
            cls.workspace.cleanup()

    def test_production_commit_block(self):
        for fault in ("none", "write", "fflush", "fclose", "rename", "rename+unlink"):
            with self.subTest(fault=fault):
                with tempfile.TemporaryDirectory(prefix="case-", dir=self.root) as work:
                    result = subprocess.run([str(self.exe), fault], cwd=work,
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     f"{fault}: {result.stdout} {result.stderr} code={result.returncode}")


if __name__ == "__main__":
    unittest.main()
