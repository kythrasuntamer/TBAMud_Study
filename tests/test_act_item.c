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

/* H1-02: the object's drop trigger purges the container ('opurge bag').
 * perform_put must not read the freed bag or link the sword into it. */
static void test_put_stops_when_trigger_purges_container(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *bag = fx_new_obj("bag", ITEM_CONTAINER);
  struct obj_data *sword = fx_new_obj("sword", ITEM_WEAPON);
  struct obj_data *spear = fx_new_obj("spear", ITEM_WEAPON);

  obj_to_char(bag, pc);
  obj_to_char(spear, pc);
  obj_to_char(sword, pc);
  doom(FX_DROP_OTRIG, bag, NULL);

  do_put(pc, arg("all bag"), 0, 0);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("put-target-changed", tb_captured_operation);
  TEST_ASSERT_EQUAL_PTR(pc, sword->carried_by);
  TEST_ASSERT_EQUAL_PTR(pc, spear->carried_by);
  TEST_ASSERT_NULL(sword->in_obj);
  fx_free_char(pc);
}

/* H1-02: a get trigger purges the container being emptied. */
static void test_get_from_container_stops_when_trigger_purges_it(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *bag = fx_new_obj("bag", ITEM_CONTAINER);

  obj_to_room(bag, 0);
  obj_to_obj(fx_new_obj("apple", ITEM_OTHER), bag);
  obj_to_obj(fx_new_obj("bread", ITEM_OTHER), bag);
  doom(FX_GET_OTRIG, bag, NULL);   /* extract_obj takes the contents too */

  do_get(pc, arg("all bag"), 0, 0);

  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("get-container-changed", tb_captured_operation);
  fx_free_char(pc);
}

/* H1-02: a get trigger that purges its own object is a deliberate script
 * outcome: nothing is picked up, nothing freed is touched, nothing logged. */
static void test_get_object_that_purges_itself(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *a = fx_new_obj("apple", ITEM_OTHER);

  obj_to_room(a, 0);
  doom(FX_GET_OTRIG, a, NULL);

  do_get(pc, arg("apple"), 0, 0);

  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

static struct obj_data *drinkcon(const char *name, int max, int now, int liquid)
{
  struct obj_data *o = fx_new_obj(name, ITEM_DRINKCON);
  GET_OBJ_VAL(o, 0) = max;
  GET_OBJ_VAL(o, 1) = now;
  GET_OBJ_VAL(o, 2) = liquid;
  GET_OBJ_WEIGHT(o) = now + 1;
  return o;
}

/* H1-07: a liquid type past drinks[] (world file or 'osetval 2 40') is
 * refused and reported before any table lookup. */
static void test_drink_rejects_liquid_out_of_range(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *bottle = drinkcon("bottle", 10, 5, 40);

  obj_to_char(bottle, pc);
  do_drink(pc, arg("bottle"), 0, SCMD_DRINK);

  TEST_ASSERT_EQUAL_INT(0, fx_trigger_calls[FX_CONSUME_OTRIG]);
  TEST_ASSERT_EQUAL_INT(5, GET_OBJ_VAL(bottle, 1));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("liquid-type-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "liquid 40"));
  fx_free_char(pc);
}

/* H1-07: a negative liquid type is refused by pour as well. */
static void test_pour_rejects_negative_liquid(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *bottle = drinkcon("bottle", 10, 5, -3);
  struct obj_data *cup = drinkcon("cup", 5, 0, 0);

  obj_to_char(cup, pc);
  obj_to_char(bottle, pc);
  do_pour(pc, arg("bottle cup"), 0, SCMD_POUR);

  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(cup, 1));
  TEST_ASSERT_EQUAL_STRING("liquid-type-range", tb_captured_operation);
  fx_free_char(pc);
}

/* H1-07: the name helpers refuse an out-of-range type and keep the name. */
static void test_name_to_drinkcon_rejects_bad_type(void)
{
  struct obj_data *cup = drinkcon("cup", 5, 0, 0);

  name_to_drinkcon(cup, NUM_LIQ_TYPES);
  TEST_ASSERT_EQUAL_STRING("cup", cup->name);
  TEST_ASSERT_EQUAL_STRING("liquid-type-range", tb_captured_operation);
}

/* H1-12: sacrificing a carried container spills its contents into the
 * sacrificer's room instead of orphaning them (IN_ROOM of a carried
 * object is NOWHERE). */
static void test_sac_carried_container_spills_into_room(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 1);
  struct obj_data *pack = fx_new_obj("backpack", ITEM_CONTAINER);
  struct obj_data *a = fx_new_obj("apple", ITEM_OTHER);
  struct obj_data *b = fx_new_obj("bread", ITEM_OTHER);

  obj_to_obj(a, pack);
  obj_to_obj(b, pack);
  obj_to_char(pack, pc);

  do_sac(pc, arg("backpack"), 0, 0);

  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(a));
  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(b));
  TEST_ASSERT_EQUAL_INT(0, fx_log_lines);       /* no obj_to_room SYSERR */
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

/* H1-13: a unique object from a rent file has rnum NOTHING; emptying it
 * must not read obj_proto[NOTHING]. */
static void test_name_from_drinkcon_with_no_prototype(void)
{
  struct obj_data *flask = drinkcon("flask", 5, 0, 0);

  free(flask->name);
  flask->name = strdup("flask water");
  TEST_ASSERT_EQUAL_INT(NOTHING, GET_OBJ_RNUM(flask));

  name_from_drinkcon(flask);

  TEST_ASSERT_EQUAL_STRING("flask", flask->name);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H1-13: an rnum past the prototype table is reported, name untouched. */
static void test_name_from_drinkcon_with_bad_rnum(void)
{
  struct obj_data *flask = drinkcon("flask", 5, 0, 0);

  flask->item_number = 77;   /* top_of_objt is 0 in this test */
  name_from_drinkcon(flask);
  flask->item_number = NOTHING;

  TEST_ASSERT_EQUAL_STRING("flask", flask->name);
  TEST_ASSERT_EQUAL_STRING("drinkcon-proto", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "rnum 77"));
}

static int refuse_drop_hook(enum fx_trigger kind, struct char_data *actor,
                            struct obj_data *obj, int argn)
{
  (void)actor; (void)obj; (void)argn;
  return kind == FX_DROP_WTRIG ? 0 : 1;
}

static int purge_dropped_hook(enum fx_trigger kind, struct char_data *actor,
                              struct obj_data *obj, int argn)
{
  (void)actor; (void)argn;
  if (kind == FX_DROP_WTRIG && obj)
    extract_obj(obj);
  return 1;
}

/* H1-17: a room drop trigger that refuses a gold drop: the coins stay in
 * the purse and the player is told. */
static void test_drop_gold_refused_by_room_trigger(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  GET_GOLD(pc) = 50;
  fx_hook = refuse_drop_hook;
  do_drop(pc, arg("10 coins"), 0, SCMD_DROP);

  TEST_ASSERT_EQUAL_INT(1, fx_trigger_calls[FX_DROP_WTRIG]);
  TEST_ASSERT_EQUAL_INT(50, GET_GOLD(pc));
  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "can't drop"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

/* H1-17: an allowed gold drop moves the coins from purse to floor. */
static void test_drop_gold_allowed(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  GET_GOLD(pc) = 50;
  do_drop(pc, arg("10 coins"), 0, SCMD_DROP);

  TEST_ASSERT_EQUAL_INT(40, GET_GOLD(pc));
  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_NOT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_INT(ITEM_MONEY, GET_OBJ_TYPE(world[0].contents));
  TEST_ASSERT_EQUAL_INT(10, GET_OBJ_VAL(world[0].contents, 0));
  fx_free_char(pc);
}

/* H1-17: a drop script that purges the coins is reported. */
static void test_drop_gold_purged_by_room_trigger(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  GET_GOLD(pc) = 50;
  fx_hook = purge_dropped_hook;
  do_drop(pc, arg("10 coins"), 0, SCMD_DROP);

  TEST_ASSERT_EQUAL_INT(40, GET_GOLD(pc));
  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_STRING("drop-gold-vanished", tb_captured_operation);
  fx_free_char(pc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_drop_gold_refused_by_room_trigger);
  RUN_TEST(test_drop_gold_allowed);
  RUN_TEST(test_drop_gold_purged_by_room_trigger);
  RUN_TEST(test_name_from_drinkcon_with_no_prototype);
  RUN_TEST(test_name_from_drinkcon_with_bad_rnum);
  RUN_TEST(test_sac_carried_container_spills_into_room);
  RUN_TEST(test_drink_rejects_liquid_out_of_range);
  RUN_TEST(test_pour_rejects_negative_liquid);
  RUN_TEST(test_name_to_drinkcon_rejects_bad_type);
  RUN_TEST(test_put_stops_when_trigger_purges_container);
  RUN_TEST(test_get_from_container_stops_when_trigger_purges_it);
  RUN_TEST(test_get_object_that_purges_itself);
  RUN_TEST(test_get_all_survives_trigger_purging_siblings);
  RUN_TEST(test_get_all_from_container_survives_purge);
  RUN_TEST(test_drop_all_survives_trigger_purging_next);
  RUN_TEST(test_give_all_survives_trigger_purging_next);
  RUN_TEST(test_get_all_without_triggers_is_silent);
  return UNITY_END();
}
