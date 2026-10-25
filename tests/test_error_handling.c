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
  TEST_ASSERT_NULL(fopen(temp_path, "rb"));
}

void test_replace_file_rolls_back_after_commit_failure(void)
{
  write_text(dest_path, "old");
  write_text(temp_path, "new");

  tb_test_set_failpoints(TB_FAIL_REPLACE_INITIAL | TB_FAIL_REPLACE_COMMIT);
  TEST_ASSERT_FALSE(tb_replace_file_checked(temp_path, dest_path));

  assert_file_text(dest_path, "old");
  assert_file_text(temp_path, "new");
  TEST_ASSERT_NULL(fopen(backup_path, "rb"));
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
