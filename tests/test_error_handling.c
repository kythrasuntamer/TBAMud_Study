#include "unity.h"

#include "conf.h"
#include "sysdep.h"

static const char *dest_path = "tb-test-dest.txt";
static const char *temp_path = "tb-test-temp.txt";
static const char *backup_path = "tb-test-dest.txt.bak";

static void cleanup_files(void)
{
  (void)remove(dest_path);
  (void)remove(temp_path);
  (void)remove(backup_path);
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

void test_diagnostic_truncation_is_explicit(void)
{
  char small[5];
  char exact[5];

  TEST_ASSERT_TRUE(tb_diag_escape_checked(small, sizeof(small), "abcdef"));
  TEST_ASSERT_EQUAL_STRING("abcd", small);
  TEST_ASSERT_FALSE(tb_diag_escape_checked(exact, sizeof(exact), "abcd"));
  TEST_ASSERT_EQUAL_STRING("abcd", exact);
  TEST_ASSERT_TRUE(tb_diag_escape_checked(small, sizeof(small), "ab\nxyz"));
  TEST_ASSERT_EQUAL_STRING("ab\\nx", small);
  TEST_ASSERT_TRUE(tb_diag_escape_checked(NULL, 0, "text"));
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

/* =========================================================
 * main
 * ========================================================= */

int main(void)
{
  UNITY_BEGIN();

  RUN_TEST(test_error_reporter_preserves_errno);
  RUN_TEST(test_error_reporter_escapes_record_breaking_context);
  RUN_TEST(test_error_reporter_exposes_symbolic_errno);
  RUN_TEST(test_diagnostic_truncation_is_explicit);
  RUN_TEST(test_error_reporter_never_emits_partial_escape);
  RUN_TEST(test_checked_int_parser_accepts_complete_number);
  RUN_TEST(test_checked_int_parser_rejects_trailing_garbage);
  RUN_TEST(test_checked_int_parser_rejects_overflow);
  RUN_TEST(test_replace_file_commits_new_contents);
  RUN_TEST(test_replace_file_rolls_back_after_commit_failure);
  RUN_TEST(test_replace_file_reports_rollback_failure_without_destroying_backup);

  return UNITY_END();
}
