/* Regression tests for tb_strftime() (sysdep.h): dst is a NUL-terminated
 * string on every return, and each failure is reported as a TBERR record
 * with operation "strftime" at the caller (G1-19, B2-26).
 *
 * localtime()/gmtime() return NULL for a time_t they cannot represent; the
 * callers (do_last, list_llog_entries, remove_player, house and mail
 * listings) pass that NULL straight in and print dst with "%s".
 */
#include "unity.h"

#include "conf.h"
#include "sysdep.h"

/* Redirect stderr (where TBERR records go) into a tmpfile around a call. */
static FILE *stderr_capture_begin(int *saved_fd)
{
  FILE *capture = tmpfile();

  TEST_ASSERT_NOT_NULL(capture);
  TEST_ASSERT_EQUAL_INT(0, fflush(stderr));
  *saved_fd = dup(STDERR_FILENO);
  TEST_ASSERT_TRUE(*saved_fd >= 0);
  TEST_ASSERT_TRUE(dup2(fileno(capture), STDERR_FILENO) >= 0);
  return capture;
}

static void stderr_capture_end(FILE *capture, int saved_fd, char *out,
                               size_t out_size)
{
  size_t got;

  TEST_ASSERT_EQUAL_INT(0, fflush(stderr));
  TEST_ASSERT_TRUE(dup2(saved_fd, STDERR_FILENO) >= 0);
  TEST_ASSERT_EQUAL_INT(0, close(saved_fd));
  TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0L, SEEK_SET));
  got = fread(out, 1, out_size - 1, capture);
  TEST_ASSERT_FALSE(ferror(capture));
  out[got] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(capture));
}

void setUp(void) { }
void tearDown(void) { }

/* G1-19 / B2-26: the NULL struct tm path used to return without writing
 * dst, so the caller printed whatever the stack held. */
void test_null_tm_leaves_empty_string_and_reports(void)
{
  char records[2048];
  char timestr[25];
  int saved_fd;
  FILE *capture;

  memset(timestr, 'X', sizeof(timestr));   /* no terminator anywhere */
  capture = stderr_capture_begin(&saved_fd);
  tb_strftime(timestr, sizeof(timestr), "%a %b %d %H:%M:%S %Y", NULL);
  stderr_capture_end(capture, saved_fd, records, sizeof(records));

  TEST_ASSERT_EQUAL_STRING("", timestr);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "operation=\"strftime\""),
                               records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "NULL struct tm"), records);
  /* The record names this file, i.e. the caller, not sysdep.h. */
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "test_strftime.c:"), records);
}

/* The real-world shape: localtime() of a time_t far past year 2^31. */
void test_unrepresentable_time_gives_terminated_string(void)
{
  char records[2048];
  char timestr[25];
  time_t huge = (time_t) 0x7fffffffffffffffLL;
  struct tm *tm_ptr;
  int saved_fd;
  FILE *capture;

  memset(timestr, 'X', sizeof(timestr));
  capture = stderr_capture_begin(&saved_fd);
  tm_ptr = localtime(&huge);
  tb_strftime(timestr, sizeof(timestr), "%a %b %d %H:%M:%S %Y", tm_ptr);
  stderr_capture_end(capture, saved_fd, records, sizeof(records));

  /* Whatever localtime() made of it, dst is a terminated string. */
  TEST_ASSERT_NOT_NULL(memchr(timestr, '\0', sizeof(timestr)));
  if (tm_ptr == NULL) {
    TEST_ASSERT_EQUAL_STRING("", timestr);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "operation=\"strftime\""),
                                 records);
  }
}

/* A destination too small for the result is emptied and reported. */
void test_small_buffer_is_emptied_and_reported(void)
{
  char records[2048];
  char small[4];
  time_t now = (time_t) 86400;
  struct tm *tm_ptr = gmtime(&now);
  int saved_fd;
  FILE *capture;

  TEST_ASSERT_NOT_NULL(tm_ptr);
  memset(small, 'X', sizeof(small));
  capture = stderr_capture_begin(&saved_fd);
  tb_strftime(small, sizeof(small), "%Y-%m-%d", tm_ptr);
  stderr_capture_end(capture, saved_fd, records, sizeof(records));

  TEST_ASSERT_EQUAL_STRING("", small);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "operation=\"strftime\""),
                               records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "buffer too small"), records);
}

/* The normal path formats and reports nothing. */
void test_valid_time_formats_silently(void)
{
  char records[2048];
  char buf[32];
  time_t day_one = (time_t) 86400;
  struct tm *tm_ptr = gmtime(&day_one);
  int saved_fd;
  FILE *capture;

  TEST_ASSERT_NOT_NULL(tm_ptr);
  capture = stderr_capture_begin(&saved_fd);
  tb_strftime(buf, sizeof(buf), "%Y-%m-%d", tm_ptr);
  stderr_capture_end(capture, saved_fd, records, sizeof(records));

  TEST_ASSERT_EQUAL_STRING("1970-01-02", buf);
  TEST_ASSERT_NULL_MESSAGE(strstr(records, "TBERR"), records);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_null_tm_leaves_empty_string_and_reports);
  RUN_TEST(test_unrepresentable_time_gives_terminated_string);
  RUN_TEST(test_small_buffer_is_emptied_and_reported);
  RUN_TEST(test_valid_time_formats_silently);
  return UNITY_END();
}
