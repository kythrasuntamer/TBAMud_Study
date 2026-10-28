/* Executable tests of the production DG event queue (dg_event.c): an event's
 * queue key (delay + pulse) selects a bucket by key % NUM_EVENT_QUEUES, so a
 * delay that wraps the key negative used to index the buckets out of bounds.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "dg_event.h"
#include "mud_event.h"
#include "tb_error_capture.h"

unsigned long pulse;

void basic_mud_log(const char *format, ...) { (void)format; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}

/* Not reached: these events are plain, not mud events. */
void free_mud_event(struct mud_event_data *pMudEvent)
{ (void)pMudEvent; TEST_FAIL_MESSAGE("free_mud_event reached"); }

static EVENTFUNC(never_runs)
{
  (void)event_obj;
  TEST_FAIL_MESSAGE("event ran");
  return 0;
}

void setUp(void)
{
  pulse = 100;
  event_init();
  tb_error_capture_reset();
}
void tearDown(void) { event_free_all(); }

static void huge_delay_is_clamped_not_wrapped(void)
{
  struct event *e = event_create(never_runs, NULL, LONG_MAX);

  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("event-time", tb_captured_operation);
  /* Due at the latest representable pulse, still in the future. */
  TEST_ASSERT_TRUE(event_time(e) > 0);
  event_cancel(e);
}

static void ordinary_delay_is_untouched(void)
{
  struct event *e = event_create(never_runs, NULL, 50);

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(50, event_time(e));
  event_cancel(e);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(huge_delay_is_clamped_not_wrapped);
  RUN_TEST(ordinary_delay_is_untouched);
  return UNITY_END();
}
