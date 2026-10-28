/* Executable tests of production handler.c primitives (fname, furniture,
 * affect_join, obj_from_room). Linked against src/handler.c with unrelated
 * sections discarded; only the externals these paths reach are supplied by
 * test_stubs.c or below. Fixtures are disposable heap/static objects.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "spells.h"
#include "dg_scripts.h"
#include <stdlib.h>
#include <string.h>
#include "tb_error_capture.h"

void basic_mud_log(const char *format, ...) { (void)format; }

void setUp(void) { tb_error_capture_reset(); }
void tearDown(void) {}

/* H2-04: a first keyword longer than fname()'s static buffer must be
 * truncated (not written past the buffer) and reported. */
static void test_fname_long_keyword_truncated_and_reported(void)
{
  size_t n = READ_SIZE + 44;
  char *kw = malloc(n + 1);
  char *out;

  TEST_ASSERT_NOT_NULL(kw);
  memset(kw, 'a', n);
  kw[n] = '\0';
  out = fname(kw);
  TEST_ASSERT_EQUAL_size_t(READ_SIZE - 1, strlen(out));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("fname-truncate", tb_captured_operation);
  free(kw);
}

static void test_fname_normal_keyword_silent(void)
{
  char *kw = strdup("sword long");

  TEST_ASSERT_NOT_NULL(kw);
  TEST_ASSERT_EQUAL_STRING("sword", fname(kw));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(kw);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_fname_long_keyword_truncated_and_reported);
  RUN_TEST(test_fname_normal_keyword_silent);
  return UNITY_END();
}
