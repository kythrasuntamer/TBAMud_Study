/* Tests for the MEMORY_DEBUG allocator (src/zmalloc.c), linked as built.
 *
 * Detected heap misuse aborts the process after a structured TBERR record on
 * stderr, so each misuse case runs in a forked child whose stderr goes to a
 * pipe; the parent asserts that the child died of SIGABRT (SIGTRAP under
 * Fil-C, see ZM_ABORT_SIGNAL) and that the
 * expected operation was reported.  The functions are called directly (not
 * through the zmalloc.h malloc/free macros) so this file's own stdio and
 * Unity allocations stay on libc.
 *
 * The last section includes zmalloc.h and goes through the macros, as a
 * MEMORY_DEBUG server file does: calloc() expands with its arguments
 * parenthesised and overflow-checked, and (under ASan) the guard pads and
 * freed blocks are poisoned, so an overrun or use-after-free is reported at
 * the faulting access (zfree() keeps memory, so without the poisoning ASan
 * would see a freed block as live).
 */
#include "unity.h"

#include "conf.h"
#include "sysdep.h"
#include "asan_annotations.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The signal an abort() leaves in the child's wait status.  Fil-C's abort()
 * does not raise SIGABRT: it reports "filc user error: abort(3) called" and
 * panics, and the panic ends the process with SIGTRAP. */
#ifdef __FILC__
#define ZM_ABORT_SIGNAL SIGTRAP
#else
#define ZM_ABORT_SIGNAL SIGABRT
#endif

unsigned char *zmalloc(size_t, char *, int);
unsigned char *zcalloc(size_t, size_t, char *, int);
unsigned char *zrealloc(unsigned char *, size_t, char *, int);
void zfree(unsigned char *, char *, int);
void zmalloc_init(void);
void zmalloc_check(void);
char *zstrdup(const char *, char *, int);

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
  TEST_ASSERT_EQUAL_INT(0, fflush(stdout));
  TEST_ASSERT_EQUAL_INT(0, fflush(stderr));
  pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    (void)close(fds[0]);
    if (dup2(fds[1], STDERR_FILENO) < 0)
      _exit(98);
    fn();
    _exit(0);
  }
  TEST_ASSERT_EQUAL_INT(0, close(fds[1]));
  while (got + 1 < out_size &&
         (n = read(fds[0], out + got, out_size - 1 - got)) > 0)
    got += (size_t)n;
  out[got] = '\0';
  TEST_ASSERT_EQUAL_INT(0, close(fds[0]));
  TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
  return status;
}

static void assert_aborted_with(zm_case_fn fn, const char *operation)
{
  char records[8192], want[128];
  int status = run_child(fn, records, sizeof(records));

  TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == ZM_ABORT_SIGNAL,
                           records);
  TEST_ASSERT_TRUE(snprintf(want, sizeof(want), "operation=\"%s\"",
                            operation) < (int)sizeof(want));
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, want), records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "severity=FATAL"), records);
}

#ifdef TB_ASAN_ENABLED
/* Under ASan a poisoned guard pad or freed block makes the faulting access
 * itself the report (use-after-poison); zmalloc's own check never runs. */
static void assert_asan_poison_report(zm_case_fn fn)
{
  char records[8192];
  int status = run_child(fn, records, sizeof(records));

  TEST_ASSERT_FALSE_MESSAGE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                            records);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(records, "use-after-poison"), records);
}
#endif

/* ---- stderr capture for reports that do not abort (refused sizes) ---- */
static char captured[8192];
static int saved_stderr = -1;
static FILE *capture_file;

static void capture_begin(void)
{
  captured[0] = '\0';
  capture_file = tmpfile();
  TEST_ASSERT_NOT_NULL(capture_file);
  saved_stderr = dup(STDERR_FILENO);
  TEST_ASSERT_TRUE(saved_stderr >= 0);
  TEST_ASSERT_TRUE(dup2(fileno(capture_file), STDERR_FILENO) >= 0);
}

static void capture_end(void)
{
  size_t got;

  TEST_ASSERT_TRUE(dup2(saved_stderr, STDERR_FILENO) >= 0);
  close(saved_stderr);
  saved_stderr = -1;
  TEST_ASSERT_EQUAL_INT(0, fseek(capture_file, 0L, SEEK_SET));
  got = fread(captured, 1, sizeof(captured) - 1, capture_file);
  TEST_ASSERT_EQUAL_INT(0, ferror(capture_file));
  captured[got] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(capture_file));
  capture_file = NULL;
}

static void assert_reported(const char *operation)
{
  char needle[96];

  TEST_ASSERT_TRUE(snprintf(needle, sizeof(needle), "operation=\"%s\"",
                            operation) < (int)sizeof(needle));
  if (strstr(captured, needle) == NULL)
    TEST_FAIL_MESSAGE(captured[0] ? captured : "nothing was reported");
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

/* ---- I2-V01: blocks keep malloc()'s alignment ---- */

struct zm_wide { double d; long l; void *p; long double ld; };

void test_blocks_are_max_aligned(void)
{
  struct zm_wide *w;
  unsigned char *p;
  size_t align = __alignof__(long double);

  if (__alignof__(long long) > align)
    align = __alignof__(long long);
  p = zmalloc(sizeof(*w), ZM_FILE, __LINE__);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)((uintptr_t)p % align));
  w = (struct zm_wide *)p;
  w->d = 1.5;			/* UBSan: misaligned access before the fix */
  w->l = 2;
  w->p = w;
  w->ld = 3.0L;
  p = zrealloc(p, 3 * sizeof(*w), ZM_FILE, __LINE__);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)((uintptr_t)p % align));
  TEST_ASSERT_TRUE(((struct zm_wide *)p)->d == 1.5);
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

#ifdef TB_ASAN_ENABLED
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

/* Without ASan the pad check in zfree() catches the overrun; with ASan the
 * poisoned pad makes the write itself the report. */
void test_end_pad_overrun_is_fatal(void)
{
#ifdef TB_ASAN_ENABLED
  assert_asan_poison_report(case_end_pad_overrun);
#else
  assert_aborted_with(case_end_pad_overrun, "zmalloc-pad");
#endif
}

void test_begin_pad_underrun_is_fatal(void)
{
#ifdef TB_ASAN_ENABLED
  assert_asan_poison_report(case_begin_pad_underrun);
#else
  assert_aborted_with(case_begin_pad_underrun, "zmalloc-pad");
#endif
}

/* SI-5: strdup(NULL) used to crash inside strlen() with no caller named. */
static void case_strdup_null(void)
{
  (void)zstrdup(NULL, ZM_FILE, __LINE__);
}

static void test_strdup_null_fails_loudly(void)
{
  assert_aborted_with(case_strdup_null, "zstrdup-null");
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

/* =====================================================================
 * From here on malloc/calloc/realloc/free are the zmalloc.h macros, as in a
 * MEMORY_DEBUG server file (SI-5).
 * ===================================================================== */
#include "zmalloc.h"

static void test_calloc_parenthesises_arguments(void)
{
  int extra = 3;
  unsigned char *block = calloc(2, extra + 1);
  int i;

  TEST_ASSERT_NOT_NULL(block);
  /* The old expansion zmalloc((n*x)) allocated 2*3+1 = 7 bytes here. */
  for (i = 0; i < 8; i++)
    TEST_ASSERT_EQUAL_UINT8(0, block[i]);
#ifdef TB_ASAN_ENABLED
  TEST_ASSERT_FALSE(__asan_address_is_poisoned(block + 7));
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block + 8));
#endif
  free(block);
}

/* The pads used to be 4 bytes, so every block was only 4-byte aligned. */
static void test_blocks_are_malloc_aligned(void)
{
  void *a = malloc(1);
  void *b = malloc(24);

  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)a % 16);
  TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)b % 16);
  b = realloc(b, 100);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)b % 16);
  free(a);
  free(b);
}

/* Refused, not fatal: the caller's own allocation-failure path runs. */
static void test_calloc_overflow_fails_loudly(void)
{
  void *block;

  capture_begin();
  block = calloc(SIZE_MAX / 2, 4);
  capture_end();
  TEST_ASSERT_NULL(block);
  assert_reported("zmalloc-size");
}

/* A negative int size converts to a size_t that cannot be padded. */
static void test_negative_size_fails_loudly(void)
{
  void *block;

  capture_begin();
  block = malloc(-1);
  capture_end();
  TEST_ASSERT_NULL(block);
  assert_reported("zmalloc-size");
}

static void case_double_free_via_macros(void)
{
  char *block = malloc(16);

  if (block == NULL)
    _exit(97);
  free(block);
  free(block);
}

static void test_double_free_is_reported(void)
{
  assert_aborted_with(case_double_free_via_macros, "zfree-double");
}

/* An interior pointer of a live block is not a block zmalloc handed out. */
static void case_free_interior_pointer(void)
{
  char *block = malloc(16);

  if (block == NULL)
    _exit(97);
  free(block + 4);
}

static void test_unknown_pointer_free_is_reported(void)
{
  assert_aborted_with(case_free_interior_pointer, "zfree-unknown");
}

static void test_realloc_keeps_contents(void)
{
  char *block = malloc(8);

  TEST_ASSERT_NOT_NULL(block);
  memcpy(block, "abcdefg", 8);
  block = realloc(block, 64);
  TEST_ASSERT_NOT_NULL(block);
  TEST_ASSERT_EQUAL_STRING("abcdefg", block);
#ifdef TB_ASAN_ENABLED
  TEST_ASSERT_FALSE(__asan_address_is_poisoned(block + 63));
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block + 64));
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block - 1));
#endif
  free(block);
}

#ifdef TB_ASAN_ENABLED
/* Under ASan the guards are poisoned, so an overrun aborts at the write and
 * a use-after-free aborts at the access; check the shadow state directly. */
static void test_asan_poisons_pads_and_freed_blocks(void)
{
  unsigned char *block = malloc(32);

  TEST_ASSERT_NOT_NULL(block);
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block - 1));
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block + 32));
  TEST_ASSERT_NULL(__asan_region_is_poisoned(block, 32));
  free(block);
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block));
  TEST_ASSERT_TRUE(__asan_address_is_poisoned(block + 31));
}
#else
/* Without ASan the pads are the only overrun detector: corrupting one must
 * reach the diagnostic stream when the block is freed. */
static void case_pad_overrun_via_macros(void)
{
  unsigned char *block = malloc(32);

  if (block == NULL)
    _exit(97);
  block[32] = 0xbb;
  free(block);
}

static void test_pad_overrun_is_reported(void)
{
  assert_aborted_with(case_pad_overrun_via_macros, "zmalloc-pad");
}
#endif

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
  RUN_TEST(test_blocks_are_max_aligned);
#ifdef TB_ASAN_ENABLED
  RUN_TEST(test_read_after_free_is_seen_by_asan);
#endif
  RUN_TEST(test_strdup_null_fails_loudly);

  /* Through the zmalloc.h macros (SI-5). */
  RUN_TEST(test_calloc_parenthesises_arguments);
  RUN_TEST(test_blocks_are_malloc_aligned);
  RUN_TEST(test_calloc_overflow_fails_loudly);
  RUN_TEST(test_negative_size_fails_loudly);
  RUN_TEST(test_double_free_is_reported);
  RUN_TEST(test_unknown_pointer_free_is_reported);
  RUN_TEST(test_realloc_keeps_contents);
#ifdef TB_ASAN_ENABLED
  RUN_TEST(test_asan_poisons_pads_and_freed_blocks);
#else
  RUN_TEST(test_pad_overrun_is_reported);
#endif

  zmalloc_check();		/* releases every tracked block */
  return UNITY_END();
}
