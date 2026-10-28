/* Executable tests of split_argument() (oasis.c), the key/value splitter
 * load_config() uses for the lines of the game configuration file.
 * oasis.c is linked with unreferenced sections discarded (see
 * tests/CMakeLists.txt); tb_error_capture.h supplies tb_error_log_at().
 * Lines are heap copies so ASan reports any read past their end.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "oasis.h"
#include "tb_error_capture.h"

void setUp(void) { tb_error_capture_reset(); }
void tearDown(void) {}

static char *heap_line(const char *text)
{
  char *line = strdup(text);

  TEST_ASSERT_NOT_NULL(line);
  return line;
}

/* The load_config case: spaces dropped from the key, value moved to the
 * front of the line. */
static void split_argument_splits_key_and_value(void)
{
  char *line = heap_line("auto_save = 1");
  char tag[MAX_INPUT_LENGTH];

  split_argument(line, tag);
  TEST_ASSERT_EQUAL_STRING("auto_save", tag);
  TEST_ASSERT_EQUAL_STRING("1", line);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(line);
}

/* A key of exactly sizeof(tag) - 1 bytes fits. */
static void split_argument_keeps_key_that_just_fits(void)
{
  char *line = heap_line("abcdefg=2");
  char tag[8];

  split_argument(line, tag);
  TEST_ASSERT_EQUAL_STRING("abcdefg", tag);
  TEST_ASSERT_EQUAL_STRING("2", line);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(line);
}

/* F3-19: the key was copied into tag with no bound, so a key longer than
 * tag overflowed it (ASan stack-buffer-overflow here before the fix). It is
 * now refused, not cut to a prefix that could name another setting, and
 * reported with both sizes. */
static void split_argument_refuses_key_longer_than_tag(void)
{
  char *line = heap_line("pk_allowed_and_then_some = 1");
  char tag[8];

  split_argument(line, tag);
  TEST_ASSERT_EQUAL_STRING("", tag);
  TEST_ASSERT_EQUAL_STRING("1", line);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("tag-too-long", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "key of 24 bytes"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "8-byte buffer"));
  free(line);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(split_argument_splits_key_and_value);
  RUN_TEST(split_argument_keeps_key_that_just_fits);
  RUN_TEST(split_argument_refuses_key_longer_than_tag);
  return UNITY_END();
}
