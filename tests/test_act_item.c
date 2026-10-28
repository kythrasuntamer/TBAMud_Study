/* Regression tests for act.item.c, linked against the production sources.
 * See gameplay_fixture.h for what is real and what is faked. A DG trigger
 * is played by fx_hook: it can extract or move objects exactly as a script
 * would, and extract_obj() really frees them, so a stale pointer in the code
 * under test is a use-after-free that ASan and Valgrind report. */
#include "unity.h"
#include "gameplay_fixture.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "act.h"
#include "dg_scripts.h"

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

static char argbuf[MAX_INPUT_LENGTH];

static char *arg(const char *text)
{
  strlcpy(argbuf, text, sizeof(argbuf));
  return argbuf;
}

/* Objects the next hooked trigger extracts (a script's 'opurge'), and the
 * trigger kind that fires it. Cleared once used. */
#define MAX_DOOMED 4
static struct obj_data *doomed[MAX_DOOMED];
static enum fx_trigger doom_on;

static int purge_hook(enum fx_trigger kind, struct char_data *actor,
                      struct obj_data *obj, int argn)
{
  int i;
  (void)actor; (void)obj; (void)argn;
  if (kind != doom_on)
    return 1;
  for (i = 0; i < MAX_DOOMED; i++)
    if (doomed[i]) {
      extract_obj(doomed[i]);
      doomed[i] = NULL;
    }
  return 1;
}

static void doom(enum fx_trigger kind, struct obj_data *a, struct obj_data *b)
{
  memset(doomed, 0, sizeof(doomed));
  doomed[0] = a;
  doomed[1] = b;
  doom_on = kind;
  fx_hook = purge_hook;
}

static int carried_count(struct char_data *ch)
{
  int n = 0;
  struct obj_data *o;
  for (o = ch->carrying; o; o = o->next_content)
    n++;
  return n;
}

void setUp(void)
{
  fx_setup(2);
  tb_error_capture_reset();
}

void tearDown(void)
{
  fx_teardown();
}

/* H1-01: 'get all' where the first object's GET trigger purges the rest of
 * the room. The loop must not follow the saved next_content. */
static void test_get_all_survives_trigger_purging_siblings(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *a = fx_new_obj("apple", ITEM_OTHER);
  struct obj_data *b = fx_new_obj("bread", ITEM_OTHER);
  struct obj_data *c = fx_new_obj("cheese", ITEM_OTHER);

  /* obj_to_room appends, so the room list is a, b, c. */
  obj_to_room(a, 0);
  obj_to_room(b, 0);
  obj_to_room(c, 0);
  doom(FX_GET_OTRIG, b, c);

  do_get(pc, arg("all"), 0, 0);

  TEST_ASSERT_EQUAL_PTR(a, pc->carrying);
  TEST_ASSERT_EQUAL_INT(1, carried_count(pc));
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("item-iter-stale", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "during get"));
  fx_free_char(pc);
}

/* H1-01: the same through a container ('get all bag'). */
static void test_get_all_from_container_survives_purge(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *bag = fx_new_obj("bag", ITEM_CONTAINER);
  struct obj_data *a = fx_new_obj("apple", ITEM_OTHER);
  struct obj_data *b = fx_new_obj("bread", ITEM_OTHER);
  struct obj_data *c = fx_new_obj("cheese", ITEM_OTHER);

  obj_to_room(bag, 0);
  /* obj_to_obj prepends: contents are a, b, c. */
  obj_to_obj(c, bag);
  obj_to_obj(b, bag);
  obj_to_obj(a, bag);
  doom(FX_GET_OTRIG, b, c);

  do_get(pc, arg("all bag"), 0, 0);

  TEST_ASSERT_EQUAL_PTR(a, pc->carrying);
  TEST_ASSERT_NULL(bag->contains);
  TEST_ASSERT_EQUAL_STRING("item-iter-stale", tb_captured_operation);
  fx_free_char(pc);
}

/* H1-01: 'drop all' where a drop trigger purges the next inventory item. */
static void test_drop_all_survives_trigger_purging_next(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *a = fx_new_obj("apple", ITEM_OTHER);
  struct obj_data *b = fx_new_obj("bread", ITEM_OTHER);
  struct obj_data *c = fx_new_obj("cheese", ITEM_OTHER);

  /* obj_to_char prepends: inventory is a, b, c. */
  obj_to_char(c, pc);
  obj_to_char(b, pc);
  obj_to_char(a, pc);
  doom(FX_DROP_OTRIG, b, NULL);

  do_drop(pc, arg("all"), 0, SCMD_DROP);

  /* a dropped; b purged mid-walk; the walk stops loudly, c stays. */
  TEST_ASSERT_EQUAL_PTR(a, world[0].contents);
  TEST_ASSERT_EQUAL_PTR(c, pc->carrying);
  TEST_ASSERT_EQUAL_STRING("item-iter-stale", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "during drop"));
  fx_free_char(pc);
}

/* H1-01: 'give all' where the give trigger purges the next item. */
static void test_give_all_survives_trigger_purging_next(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *bob = fx_new_char("bob", FALSE, 0);
  struct obj_data *a = fx_new_obj("apple", ITEM_OTHER);
  struct obj_data *b = fx_new_obj("bread", ITEM_OTHER);

  obj_to_char(b, pc);
  obj_to_char(a, pc);
  doom(FX_GIVE_OTRIG, b, NULL);

  do_give(pc, arg("all bob"), 0, 0);

  TEST_ASSERT_EQUAL_PTR(a, bob->carrying);
  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_EQUAL_STRING("item-iter-stale", tb_captured_operation);
  fx_free_char(pc);
  fx_free_char(bob);
}

/* H1-01: a walk with no interference reports nothing. */
static void test_get_all_without_triggers_is_silent(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  obj_to_room(fx_new_obj("apple", ITEM_OTHER), 0);
  obj_to_room(fx_new_obj("bread", ITEM_OTHER), 0);

  do_get(pc, arg("all"), 0, 0);

  TEST_ASSERT_EQUAL_INT(2, carried_count(pc));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_get_all_survives_trigger_purging_siblings);
  RUN_TEST(test_get_all_from_container_survives_purge);
  RUN_TEST(test_drop_all_survives_trigger_purging_next);
  RUN_TEST(test_give_all_survives_trigger_purging_next);
  RUN_TEST(test_get_all_without_triggers_is_silent);
  return UNITY_END();
}
