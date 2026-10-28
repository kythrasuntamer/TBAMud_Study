/* Tests for the MEMORY_DEBUG allocator (src/zmalloc.c), linked as built.
 *
 * Detected heap misuse aborts the process after a structured TBERR record on
 * stderr, so each misuse case runs in a forked child whose stderr goes to a
 * pipe; the parent asserts that the child died of SIGABRT and that the
 * expected operation was reported.  The functions are called directly (not
 * through the zmalloc.h malloc/free macros) so this file's own stdio and
 * Unity allocations stay on libc.
 */
#include "unity.h"

#include "conf.h"
#include "sysdep.h"

#include <signal.h>
#include <sys/wait.h>

unsigned char *zmalloc(size_t, char *, int);
unsigned char *zcalloc(size_t, size_t, char *, int);
unsigned char *zrealloc(unsigned char *, size_t, char *, int);
void zfree(unsigned char *, char *, int);
void zmalloc_init(void);
void zmalloc_check(void);

#define ZM_FILE ((char *)__FILE__)

typedef void (*zm_case_fn)(void);

/* Run fn in a child with stderr captured.  Returns the child's wait status;
 * the captured records are in out. */
static int run_child(zm_case_fn fn, char *out, size_t out_size)
{
  int fds[2], status = 0;
  pid_t pid;
  size_t got = 0;
  ssize_t n;

  TEST_ASSERT_EQUAL_INT(0, pipe(fds));
  fflush(stdout);
  fflush(stderr);
  pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], STDERR_FILENO) < 0)
      _exit(98);
    fn();
    _exit(0);
  }
  close(fds[1]);
  while (got + 1 < out_size &&
         (n = read(fds[0], out + got, out_size - 1 - got)) > 0)
    got += (size_t)n;
  out[got] = '\0';
  close(fds[0]);
  TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
  return status;
}

static void assert_aborted_with(zm_case_fn fn, const char *operation)
{
  char records[8192], want[128];
  int status = run_child(fn, records, sizeof(records));

  TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                           records);
  snprintf(want, sizeof(want), "operation=\"%s\"", operation);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, want), records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "severity=FATAL"), records);
}

void setUp(void) {}
void tearDown(void) {}

/* ---- I2-17: detections are reported and abort, not just logged ---- */

static void case_double_free(void)
{
  unsigned char *p = zmalloc(16, ZM_FILE, __LINE__);
  zfree(p, ZM_FILE, __LINE__);
  zfree(p, ZM_FILE, __LINE__);
}

static void case_free_unknown(void)
{
  static unsigned char not_heap[8];
  zfree(not_heap, ZM_FILE, __LINE__);
}

static void case_end_pad_overrun(void)
{
  unsigned char *p = zmalloc(16, ZM_FILE, __LINE__);
  p[16] = 0xbb;			/* first byte of the end guard pad */
  zfree(p, ZM_FILE, __LINE__);
}

static void case_begin_pad_underrun(void)
{
  unsigned char *p = zmalloc(16, ZM_FILE, __LINE__);
  p[-1] = 0x0f;			/* last byte of the begin guard pad */
  zfree(p, ZM_FILE, __LINE__);
}

/* ---- I2-15: sizes are size_t; overflow is refused and reported ---- */

static void case_oversized_requests(void)
{
  /* Each must return NULL (exit 0 means all did). */
  if (zmalloc(SIZE_MAX - 1, ZM_FILE, __LINE__) != NULL)
    _exit(1);
  if (zcalloc(SIZE_MAX / 2, 4, ZM_FILE, __LINE__) != NULL)
    _exit(2);
  if (zrealloc(NULL, SIZE_MAX, ZM_FILE, __LINE__) != NULL)
    _exit(3);
  _exit(0);
}

void test_oversized_request_is_refused_and_reported(void)
{
  char records[8192];
  int status = run_child(case_oversized_requests, records, sizeof(records));

  TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                           records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "operation=\"zmalloc-size\""),
                               records);
}

void test_calloc_zero_fills_count_times_size(void)
{
  unsigned char *p = zcalloc(3, 5, ZM_FILE, __LINE__);
  int i;

  TEST_ASSERT_NOT_NULL(p);
  for (i = 0; i < 15; i++)
    TEST_ASSERT_EQUAL_UINT8(0, p[i]);
  zfree(p, ZM_FILE, __LINE__);
}

/* ---- I2-18: realloc of an untracked block ---- */

static void case_realloc_untracked(void)
{
  static unsigned char not_heap[8] = "keepme";
  (void)zrealloc(not_heap, 16, ZM_FILE, __LINE__);
}

void test_realloc_of_untracked_pointer_is_fatal(void)
{
  assert_aborted_with(case_realloc_untracked, "zrealloc-untracked");
}

void test_realloc_null_allocates(void)
{
  unsigned char *p = zrealloc(NULL, 8, ZM_FILE, __LINE__);

  TEST_ASSERT_NOT_NULL(p);
  memcpy(p, "abcdefg", 8);
  p = zrealloc(p, 64, ZM_FILE, __LINE__);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("abcdefg", (char *)p);
  zfree(p, ZM_FILE, __LINE__);
}

/* ---- I2-16: freed blocks are retained, so check misuse explicitly ---- */

static void case_realloc_after_free(void)
{
  unsigned char *p = zmalloc(16, ZM_FILE, __LINE__);
  zfree(p, ZM_FILE, __LINE__);
  (void)zrealloc(p, 32, ZM_FILE, __LINE__);
}

void test_realloc_after_free_is_fatal(void)
{
  assert_aborted_with(case_realloc_after_free, "zrealloc-after-free");
}

#if defined(__SANITIZE_ADDRESS__)
static volatile unsigned char zm_sink;

static void case_read_after_free(void)
{
  unsigned char *p = zmalloc(16, ZM_FILE, __LINE__);
  zfree(p, ZM_FILE, __LINE__);
  zm_sink = p[0];
}

/* Under ASan a freed (retained) block is poisoned, so a stale read is a
 * reported use-after-poison instead of a silent success. */
void test_read_after_free_is_seen_by_asan(void)
{
  char records[8192];
  int status = run_child(case_read_after_free, records, sizeof(records));

  TEST_ASSERT_FALSE_MESSAGE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                            records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "use-after-poison"), records);
}
#endif

void test_double_free_is_fatal(void)
{
  assert_aborted_with(case_double_free, "zfree-double");
}

void test_free_of_untracked_pointer_is_fatal(void)
{
  assert_aborted_with(case_free_unknown, "zfree-unknown");
}

void test_end_pad_overrun_is_fatal(void)
{
  assert_aborted_with(case_end_pad_overrun, "zmalloc-pad");
}

void test_begin_pad_underrun_is_fatal(void)
{
  assert_aborted_with(case_begin_pad_underrun, "zmalloc-pad");
}

static void case_clean_alloc_free(void)
{
  unsigned char *p = zmalloc(32, ZM_FILE, __LINE__);

  if (p == NULL)
    _exit(97);
  memset(p, 'x', 32);
  zfree(p, ZM_FILE, __LINE__);
}

void test_clean_alloc_free_reports_nothing(void)
{
  char records[4096];
  int status = run_child(case_clean_alloc_free, records, sizeof(records));

  TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                           records);
  TEST_ASSERT_NULL_MESSAGE(strstr(records, "TBERR"), records);
}

int main(void)
{
  UNITY_BEGIN();
  zmalloc_init();

  RUN_TEST(test_double_free_is_fatal);
  RUN_TEST(test_free_of_untracked_pointer_is_fatal);
  RUN_TEST(test_end_pad_overrun_is_fatal);
  RUN_TEST(test_begin_pad_underrun_is_fatal);
  RUN_TEST(test_clean_alloc_free_reports_nothing);
  RUN_TEST(test_realloc_of_untracked_pointer_is_fatal);
  RUN_TEST(test_realloc_null_allocates);
  RUN_TEST(test_realloc_after_free_is_fatal);
  RUN_TEST(test_oversized_request_is_refused_and_reported);
  RUN_TEST(test_calloc_zero_fills_count_times_size);
#if defined(__SANITIZE_ADDRESS__)
  RUN_TEST(test_read_after_free_is_seen_by_asan);
#endif

  zmalloc_check();		/* releases every tracked block */
  return UNITY_END();
}
