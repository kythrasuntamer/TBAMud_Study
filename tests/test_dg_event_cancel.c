/* Executable tests of the production DG event queue (src/dg_event.c):
 * cancelling an event from inside its own callback (D2-01).
 * Linked against the real dg_event.c; the game globals it needs (pulse,
 * free_mud_event, the logger and the checked allocator) are test-local.
 * Diagnostics are captured by operation name so each test asserts that a
 * guard failed loudly, not just that it did not crash.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "dg_event.h"
#include "mud_event.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

unsigned long pulse;

/* ---- diagnostic capture ------------------------------------------------ */
#define MAX_CAPTURED 16
static char captured_ops[MAX_CAPTURED][64];
static char captured_sev[MAX_CAPTURED][16];
static int n_captured;

void tb_error_log_at(const char *severity, const char *subsystem,
                     const char *operation, const char *resource, int errnum,
                     const char *source_file, int source_line,
                     const char *source_func, const char *detail_fmt, ...)
{
  (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func; (void)detail_fmt;
  if (n_captured < MAX_CAPTURED) {
    snprintf(captured_ops[n_captured], sizeof(captured_ops[0]), "%s", operation);
    snprintf(captured_sev[n_captured], sizeof(captured_sev[0]), "%s", severity);
    n_captured++;
  }
}

void basic_mud_log(const char *format, ...) { (void)format; }

static int captured(const char *op)
{
  int i;
  for (i = 0; i < n_captured; i++)
    if (!strcmp(captured_ops[i], op))
      return 1;
  return 0;
}

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *allocation;
  (void)file; (void)line; (void)func;
  if (element_size == 0 || count == 0 || count > SIZE_MAX / element_size)
    abort();
  allocation = calloc(count, element_size);
  if (!allocation)
    abort();
  return allocation;
}

/* Model of mud_event.c's free_mud_event(): it detaches the payload from the
 * event and frees it.  Counting calls proves the payload is freed once. */
static int mud_frees;
void free_mud_event(struct mud_event_data *pMudEvent)
{
  mud_frees++;
  pMudEvent->pEvent->event_obj = NULL;
  free(pMudEvent);
}

/* ---- fixtures ---------------------------------------------------------- */
static struct event *self_event;
static long self_return;
static int self_calls;

/* A mud event callback that (like event_whirlwind -> hit -> die ->
 * extract_char -> clear_char_event_list) cancels its own running event and
 * then asks to be re-run. */
static EVENTFUNC(cancel_self)
{
  (void)event_obj;
  self_calls++;
  event_cancel(self_event);
  event_cancel(self_event);    /* a second cancel must not double free */
  return self_return;
}

static struct event *attach_test_mud_event(EVENTFUNC(*func), long when)
{
  struct mud_event_data *m = calloc(1, sizeof(*m));
  struct event *e;
  TEST_ASSERT_NOT_NULL(m);
  m->iId = eNULL;
  e = event_create(func, m, when);
  TEST_ASSERT_NOT_NULL(e);
  e->isMudEvent = TRUE;
  m->pEvent = e;
  return e;
}

void setUp(void)
{
  n_captured = 0;
  mud_frees = 0;
  self_calls = 0;
  pulse = 0;
  event_init();
}

void tearDown(void)
{
  event_free_all();
}

/* D2-01: a callback that cancels its own running event must not cause a
 * use-after-free or double free in event_process(), and must not be
 * re-enqueued even when it returns > 0. */
static void test_cancel_running_event_is_deferred_and_not_requeued(void)
{
  self_return = 10;
  self_event = attach_test_mud_event(cancel_self, 1);

  pulse = 1;
  event_process();

  TEST_ASSERT_EQUAL_INT(1, self_calls);
  TEST_ASSERT_EQUAL_INT(1, mud_frees);
  TEST_ASSERT_TRUE(captured("event-cancel-running"));
  TEST_ASSERT_TRUE(captured("event-cancel-running-twice"));

  pulse = 11;
  event_process();
  TEST_ASSERT_EQUAL_INT(1, self_calls);
}

/* D2-09: cancelling NULL names the operation instead of a bare SYSERR. */
static void test_cancel_null_is_reported(void)
{
  event_cancel(NULL);
  TEST_ASSERT_TRUE(captured("event-cancel-null"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_cancel_null_is_reported);
  RUN_TEST(test_cancel_running_event_is_deferred_and_not_requeued);
  return UNITY_END();
}
