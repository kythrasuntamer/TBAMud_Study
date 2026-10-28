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

/* H1-20: a count of INT_MIN (or any count <= 0) is refused instead of
 * meaning "all" and overflowing 'howmany--'. */
static void test_negative_counts_are_refused(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *bob = fx_new_char("bob", FALSE, 0);
  struct obj_data *bag = fx_new_obj("bag", ITEM_CONTAINER);

  obj_to_room(fx_new_obj("bread", ITEM_FOOD), 0);
  obj_to_room(bag, 0);
  obj_to_obj(fx_new_obj("bread", ITEM_FOOD), bag);
  obj_to_char(fx_new_obj("bread", ITEM_FOOD), pc);

  do_get(pc, arg("-2147483648 bread"), 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "makes sense"));
  fx_last_send[0] = '\0';
  do_get(pc, arg("-1 bread bag"), 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "makes sense"));
  fx_last_send[0] = '\0';
  do_put(pc, arg("0 bread bag"), 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "makes sense"));
  fx_last_send[0] = '\0';
  do_give(pc, arg("-1 bread bob"), 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "makes sense"));

  /* Nothing moved. */
  TEST_ASSERT_NOT_NULL(pc->carrying);
  TEST_ASSERT_NULL(pc->carrying->next_content);
  TEST_ASSERT_NULL(bob->carrying);
  TEST_ASSERT_NOT_NULL(bag->contains);
  fx_free_char(bob);
  fx_free_char(pc);
}

/* H1-21: the junk reward goes through increase_gold(), which caps at
 * MAX_GOLD, instead of adding to GET_GOLD directly. */
static void test_junk_reward_respects_max_gold(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *gem = fx_new_obj("gem", ITEM_TREASURE);

  GET_OBJ_COST(gem) = 3200;          /* reward MIN(200, 3200 / 16) = 200 */
  obj_to_char(gem, pc);
  GET_GOLD(pc) = MAX_GOLD - 1;

  do_drop(pc, arg("gem"), 0, SCMD_JUNK);

  TEST_ASSERT_NULL(pc->carrying);
  TEST_ASSERT_EQUAL_INT(MAX_GOLD, GET_GOLD(pc));
  fx_free_char(pc);
}

/* A REMOVE script running '%force% %actor% remove plate': the nested
 * remove takes the item off before the outer one continues. */
static int nested_remove_hook(enum fx_trigger kind, struct char_data *actor,
                              struct obj_data *obj, int argn)
{
  (void)argn;
  if (kind == FX_REMOVE_OTRIG && obj && obj->worn_by == actor)
    obj_to_char(unequip_char(actor, obj->worn_on), actor);
  return 1;
}

/* H1-25: the outer remove must not unequip an empty slot. */
static void test_remove_rechecks_slot_after_trigger(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *plate = fx_new_obj("plate", ITEM_ARMOR);

  SET_BIT_AR(GET_OBJ_WEAR(plate), ITEM_WEAR_BODY);
  equip_char(pc, plate, WEAR_BODY);
  fx_hook = nested_remove_hook;

  do_remove(pc, arg("plate"), 0, 0);

  TEST_ASSERT_EQUAL_INT(0, fx_core_dumps);
  TEST_ASSERT_NULL(GET_EQ(pc, WEAR_BODY));
  TEST_ASSERT_EQUAL_PTR(plate, pc->carrying);
  TEST_ASSERT_NULL(plate->next_content);
  TEST_ASSERT_EQUAL_STRING("remove-slot-changed", tb_captured_operation);
  fx_free_char(pc);
}

/* db.c's real_room() over the fixture's rooms (vnums 3000 + rnum). */
room_rnum real_room(room_vnum vnum)
{
  return (vnum >= 3000 && vnum <= 3000 + top_of_world) ?
         (room_rnum) (vnum - 3000) : NOWHERE;
}

/* H1-27: with only donation room 3 configured, every roll but the junk
 * one must reach it (cases 1/2 used to try room 1 = NOWHERE and fail). */
static void test_donate_with_only_room_three(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *cup = fx_new_obj("cup", ITEM_OTHER);

  CONFIG_DON_ROOM_1 = NOWHERE;
  CONFIG_DON_ROOM_2 = NOWHERE;
  CONFIG_DON_ROOM_3 = 3001;
  obj_to_char(cup, pc);
  fx_rand_value = 1;               /* the first non-junk outcome */

  do_drop(pc, arg("cup"), 0, SCMD_DONATE);

  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(cup));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

/* H1-27: a configured donation room that does not exist is reported. */
static void test_donate_reports_missing_room(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  CONFIG_DON_ROOM_1 = 3063;        /* no such room */
  CONFIG_DON_ROOM_2 = NOWHERE;
  CONFIG_DON_ROOM_3 = NOWHERE;
  obj_to_char(fx_new_obj("cup", ITEM_OTHER), pc);

  do_drop(pc, arg("cup"), 0, SCMD_DONATE);

  TEST_ASSERT_NOT_NULL(pc->carrying);
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "can't donate"));
  TEST_ASSERT_EQUAL_STRING("donation-room-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "vnum 3063"));
  fx_free_char(pc);
}

/* A drop script that hands the object to someone else ('%force% %actor%
 * give sword bob'). */
static struct char_data *receiver;
static int give_away_hook(enum fx_trigger kind, struct char_data *actor,
                          struct obj_data *obj, int argn)
{
  (void)argn;
  if (kind == FX_DROP_OTRIG && obj && obj->carried_by == actor) {
    obj_from_char(obj);
    obj_to_char(obj, receiver);
  }
  return 1;
}

/* OBJ_STILL_HELD_BY in perform_drop: an object the drop trigger moved to
 * another character is not dropped (from that character's hands). */
static void test_drop_skips_object_moved_by_trigger(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *sword = fx_new_obj("sword", ITEM_WEAPON);

  receiver = fx_new_char("bob", FALSE, 1);
  obj_to_char(sword, pc);
  fx_hook = give_away_hook;

  do_drop(pc, arg("sword"), 0, SCMD_DROP);

  TEST_ASSERT_EQUAL_PTR(receiver, sword->carried_by);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_STRING("drop-target-changed", tb_captured_operation);
  fx_free_char(receiver);
  fx_free_char(pc);
}

/* H1-27: the boot-time check reports each configured donation room that
 * does not exist, and is quiet about unset and existing ones, before any
 * player tries to donate. */
static void test_boot_check_reports_missing_donation_room(void)
{
  CONFIG_DON_ROOM_1 = NOWHERE;     /* not configured: not an error */
  CONFIG_DON_ROOM_2 = 3063;        /* no such room */
  CONFIG_DON_ROOM_3 = 3001;        /* exists */

  check_donation_rooms();

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("donation-room-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "donation room 2 is vnum 3063"));

  CONFIG_DON_ROOM_2 = NOWHERE;
  tb_error_capture_reset();
  check_donation_rooms();
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H1-26: weight_change_object() skipped a worn object (create water on a
 * held drink container, TAR_OBJ_EQUIP) with a bare SYSERR line, so the
 * container's weight drifted from its contents. Equipment is not counted in
 * IS_CARRYING_W, so only the object changes. */
static void test_weight_change_on_worn_object(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *flask = fx_new_obj("flask", ITEM_DRINKCON);
  int carried;

  SET_BIT_AR(GET_OBJ_WEAR(flask), ITEM_WEAR_HOLD);
  equip_char(pc, flask, WEAR_HOLD);
  TEST_ASSERT_EQUAL_PTR(flask, GET_EQ(pc, WEAR_HOLD));
  carried = IS_CARRYING_W(pc);

  weight_change_object(flask, 5);

  TEST_ASSERT_EQUAL_INT(6, GET_OBJ_WEIGHT(flask));
  TEST_ASSERT_EQUAL_INT(carried, IS_CARRYING_W(pc));
  TEST_ASSERT_EQUAL_PTR(flask, GET_EQ(pc, WEAR_HOLD));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

/* H1-26: an object that is nowhere breaks the callers' contract. It is
 * still changed (no holder can drift) and reported with its vnum and UID;
 * it used to be a SYSERR line naming nothing. */
static void test_weight_change_on_unplaced_object_is_reported(void)
{
  struct obj_data *flask = fx_new_obj("flask", ITEM_DRINKCON);

  weight_change_object(flask, -1);

  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_WEIGHT(flask));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("weight-change", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, " uid "));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "changed by -1"));
}

/* H1-26: do_pour with a subcommand other than pour or fill logged a SYSERR
 * line naming no character, and the player was told nothing. */
static void test_pour_with_unknown_subcommand_is_reported(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  do_pour(pc, arg("flask"), 0, 42);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("pour-subcmd", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "subcommand 42"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum"));
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "nothing was poured"));
  fx_free_char(pc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_weight_change_on_worn_object);
  RUN_TEST(test_weight_change_on_unplaced_object_is_reported);
  RUN_TEST(test_pour_with_unknown_subcommand_is_reported);
  RUN_TEST(test_boot_check_reports_missing_donation_room);
  RUN_TEST(test_drop_skips_object_moved_by_trigger);
  RUN_TEST(test_donate_with_only_room_three);
  RUN_TEST(test_donate_reports_missing_room);
  RUN_TEST(test_remove_rechecks_slot_after_trigger);
  RUN_TEST(test_junk_reward_respects_max_gold);
  RUN_TEST(test_negative_counts_are_refused);
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
