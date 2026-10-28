#include "unity.h"

#include "conf.h"
#include "sysdep.h"

static const char *dest_path = "tb-test-dest.txt";
static const char *temp_path = "tb-test-temp.txt";
/* An operator's own backup next to dest: tb_replace_file_checked() must
 * never delete it (it used "<dest>.bak" as its scratch name). */
static const char *operator_bak_path = "tb-test-dest.txt.bak";
/* The fallback's scratch backup: "<dest>.tbreplace.<pid>". */
static char backup_path[128];

static void cleanup_files(void)
{
  (void)remove(dest_path);
  (void)remove(temp_path);
  (void)remove(backup_path);
  (void)remove(operator_bak_path);
}

/* Run tb_replace_file_checked() with stderr (where the TBERR records go)
 * redirected to a file, and return the captured records in out. */
static int replace_capturing(char *out, size_t out_size)
{
  FILE *capture = tmpfile();
  int saved_fd, result;
  size_t got;

  TEST_ASSERT_NOT_NULL(capture);
  fflush(stderr);
  saved_fd = dup(STDERR_FILENO);
  TEST_ASSERT_TRUE(saved_fd >= 0);
  TEST_ASSERT_TRUE(dup2(fileno(capture), STDERR_FILENO) >= 0);
  result = tb_replace_file_checked(temp_path, dest_path);
  TEST_ASSERT_TRUE(dup2(saved_fd, STDERR_FILENO) >= 0);
  TEST_ASSERT_EQUAL_INT(0, close(saved_fd));
  rewind(capture);
  got = fread(out, 1, out_size - 1, capture);
  out[got] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(capture));
  return result;
}

static void write_text(const char *path, const char *text)
{
  FILE *fp = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), fp));
  TEST_ASSERT_EQUAL_INT(0, fflush(fp));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

static void assert_file_text(const char *path, const char *expected)
{
  char buf[128];
  FILE *fp = fopen(path, "rb");
  size_t got;

  TEST_ASSERT_NOT_NULL(fp);
  got = fread(buf, 1, sizeof(buf) - 1, fp);
  TEST_ASSERT_FALSE(ferror(fp));
  buf[got] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_STRING(expected, buf);
}

static void assert_file_missing(const char *path)
{
  FILE *fp = fopen(path, "rb");

  if (fp != NULL) {
    (void)fclose(fp);
    TEST_FAIL_MESSAGE("file unexpectedly exists");
  }
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

void setUp(void)
{
  if (snprintf(backup_path, sizeof(backup_path), "%s.tbreplace.%ld",
               dest_path, (long)getpid()) >= (int)sizeof(backup_path))
    TEST_FAIL_MESSAGE("backup path too long");
  cleanup_files();
  tb_test_set_failpoints(0);
}

void tearDown(void)
{
  tb_test_set_failpoints(0);
  cleanup_files();
}

void test_error_reporter_preserves_errno(void)
{
  errno = EACCES;
  tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                            "unit-test", "preserve errno",
                            ENOSPC, __FILE__, __LINE__, __func__);
  TEST_ASSERT_EQUAL_INT(EACCES, errno);
}

void test_error_reporter_escapes_record_breaking_context(void)
{
  char escaped[128];

  TEST_ASSERT_GREATER_THAN_UINT32(
      0, (uint32_t)tb_diag_escape(escaped, sizeof(escaped),
                                  "line1\nline2\t\"quoted\"\\tail"));
  TEST_ASSERT_EQUAL_STRING("line1\\nline2\\t\\\"quoted\\\"\\\\tail", escaped);
}

void test_error_reporter_never_emits_partial_escape(void)
{
  char tiny[2];
  char exact[3];

  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tb_diag_escape(tiny, sizeof(tiny), "\""));
  TEST_ASSERT_EQUAL_STRING("", tiny);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)tb_diag_escape(exact, sizeof(exact), "\""));
  TEST_ASSERT_EQUAL_STRING("\\\"", exact);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tb_diag_escape(tiny, sizeof(tiny), "\n"));
  TEST_ASSERT_EQUAL_STRING("", tiny);
}

void test_error_reporter_exposes_symbolic_errno(void)
{
#ifdef ENOENT
  TEST_ASSERT_EQUAL_STRING("ENOENT", tb_errno_name(ENOENT));
#endif
  TEST_ASSERT_EQUAL_STRING("NONE", tb_errno_name(0));
}

void test_checked_int_parser_accepts_complete_number(void)
{
  int value = 0;
  TEST_ASSERT_TRUE(tb_parse_int_checked(" 42 \t", &value));
  TEST_ASSERT_EQUAL_INT(42, value);
}

void test_checked_int_parser_rejects_trailing_garbage(void)
{
  int value = 123;
  TEST_ASSERT_FALSE(tb_parse_int_checked("42oops", &value));
  TEST_ASSERT_EQUAL_INT(123, value);
}

void test_checked_int_parser_rejects_overflow(void)
{
  int value = 123;
  TEST_ASSERT_FALSE(tb_parse_int_checked("999999999999999999999999", &value));
  TEST_ASSERT_EQUAL_INT(123, value);
}

void test_replace_file_commits_new_contents(void)
{
  write_text(dest_path, "old");
  write_text(temp_path, "new");

  TEST_ASSERT_TRUE(tb_replace_file_checked(temp_path, dest_path));
  assert_file_text(dest_path, "new");
  assert_file_missing(temp_path);
}

void test_replace_file_rolls_back_after_commit_failure(void)
{
  write_text(dest_path, "old");
  write_text(temp_path, "new");

  tb_test_set_failpoints(TB_FAIL_REPLACE_INITIAL | TB_FAIL_REPLACE_COMMIT);
  TEST_ASSERT_FALSE(tb_replace_file_checked(temp_path, dest_path));

  assert_file_text(dest_path, "old");
  assert_file_text(temp_path, "new");
  assert_file_missing(backup_path);
}

void test_replace_file_reports_rollback_failure_without_destroying_backup(void)
{
  write_text(dest_path, "old");
  write_text(temp_path, "new");

  tb_test_set_failpoints(TB_FAIL_REPLACE_INITIAL |
                         TB_FAIL_REPLACE_COMMIT |
                         TB_FAIL_REPLACE_ROLLBACK);
  TEST_ASSERT_FALSE(tb_replace_file_checked(temp_path, dest_path));

  assert_file_text(backup_path, "old");
  assert_file_text(temp_path, "new");
}

/* I2-14: a missing temp file (ENOENT) is a real error on POSIX; it used to
 * fall into the backup dance, which deleted "<dest>.bak" first. */
void test_replace_file_missing_temp_keeps_operator_backup(void)
{
  char records[4096];

  write_text(dest_path, "old");
  write_text(operator_bak_path, "operator");

  TEST_ASSERT_FALSE(replace_capturing(records, sizeof(records)));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
  assert_file_text(dest_path, "old");
  assert_file_text(operator_bak_path, "operator");
  assert_file_missing(backup_path);
  TEST_ASSERT_NOT_NULL(strstr(records, "operation=\"rename\""));
  TEST_ASSERT_NOT_NULL(strstr(records, "dest=tb-test-dest.txt"));
}

/* I2-14: a fallback that recovers from a failed first rename is reported. */
void test_replace_file_reports_recovered_fallback(void)
{
  char records[4096];

  write_text(dest_path, "old");
  write_text(temp_path, "new");
  write_text(operator_bak_path, "operator");

  tb_test_set_failpoints(TB_FAIL_REPLACE_INITIAL);
  TEST_ASSERT_TRUE(replace_capturing(records, sizeof(records)));
  assert_file_text(dest_path, "new");
  assert_file_text(operator_bak_path, "operator");
  assert_file_missing(backup_path);
  TEST_ASSERT_NOT_NULL(strstr(records, "operation=\"rename-fallback\""));
}

/* I2-14: a failed rollback names the backup that now holds the good copy. */
void test_replace_file_rollback_failure_names_backup(void)
{
  char records[4096];

  write_text(dest_path, "old");
  write_text(temp_path, "new");

  tb_test_set_failpoints(TB_FAIL_REPLACE_INITIAL | TB_FAIL_REPLACE_COMMIT |
                         TB_FAIL_REPLACE_ROLLBACK);
  TEST_ASSERT_FALSE(replace_capturing(records, sizeof(records)));
  TEST_ASSERT_NOT_NULL(strstr(records, "operation=\"rename-rollback\""));
  TEST_ASSERT_NOT_NULL(strstr(records, backup_path));
}

/* =========================================================
 * main
 * ========================================================= */

int main(void)
{
  UNITY_BEGIN();

  RUN_TEST(test_error_reporter_preserves_errno);
  RUN_TEST(test_error_reporter_escapes_record_breaking_context);
  RUN_TEST(test_error_reporter_exposes_symbolic_errno);
  RUN_TEST(test_error_reporter_never_emits_partial_escape);
  RUN_TEST(test_checked_int_parser_accepts_complete_number);
  RUN_TEST(test_checked_int_parser_rejects_trailing_garbage);
  RUN_TEST(test_checked_int_parser_rejects_overflow);
  RUN_TEST(test_replace_file_commits_new_contents);
  RUN_TEST(test_replace_file_rolls_back_after_commit_failure);
  RUN_TEST(test_replace_file_reports_rollback_failure_without_destroying_backup);
  RUN_TEST(test_replace_file_missing_temp_keeps_operator_backup);
  RUN_TEST(test_replace_file_reports_recovered_fallback);
  RUN_TEST(test_replace_file_rollback_failure_names_backup);

  return UNITY_END();
}
