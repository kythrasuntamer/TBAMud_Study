/* Regression tests for act.movement.c (and the fight.c position changes it
 * depends on), linked against the production sources. See
 * gameplay_fixture.h for what is real and what is faked. */
#include "unity.h"
#include "gameplay_fixture.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "fight.h"
#include "act.h"
#include "dg_scripts.h"

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

ACMD(do_sit);

static char argbuf[MAX_INPUT_LENGTH];

static char *arg(const char *text)
{
  strlcpy(argbuf, text, sizeof(argbuf));
  return argbuf;
}

static struct obj_data *furniture(const char *name, room_rnum room)
{
  struct obj_data *obj = fx_new_obj(name, ITEM_FURNITURE);
  GET_OBJ_VAL(obj, 0) = 2;  /* seats */
  obj_to_room(obj, room);
  return obj;
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

/* H1-06: a fight stands a seated character up. The furniture must let go of
 * it, otherwise a later 'sit' links ch into a second list and the first
 * keeps a pointer to ch after ch is freed. */
static void test_fight_unseats_character_before_second_sit(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct char_data *other;
  struct obj_data *bench1 = furniture("bench1", 0);
  struct obj_data *bench2 = furniture("bench2", 0);

  do_sit(pc, arg("bench1"), 0, 0);
  TEST_ASSERT_EQUAL_PTR(bench1, SITTING(pc));
  TEST_ASSERT_EQUAL_INT(1, GET_OBJ_VAL(bench1, 1));

  set_fighting(pc, mob);
  TEST_ASSERT_EQUAL_INT(POS_FIGHTING, GET_POS(pc));
  TEST_ASSERT_NULL(SITTING(pc));
  TEST_ASSERT_NULL(OBJ_SAT_IN_BY(bench1));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(bench1, 1));
  stop_fighting(pc);

  do_sit(pc, arg("bench2"), 0, 0);
  TEST_ASSERT_EQUAL_PTR(bench2, SITTING(pc));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  fx_free_char(pc);  /* frees pc: bench1 must not still list it */
  other = fx_new_char("visitor", FALSE, 0);
  do_sit(other, arg("bench1"), 0, 0);
  TEST_ASSERT_EQUAL_PTR(bench1, SITTING(other));
  TEST_ASSERT_EQUAL_PTR(other, OBJ_SAT_IN_BY(bench1));
  TEST_ASSERT_NULL(NEXT_SITTING(other));
  fx_free_char(other);
  fx_free_char(mob);
}

/* H1-06: any other path that stands ch up without unseating it is caught
 * and reported by do_sit, which repairs the lists before linking. */
static void test_sit_reports_and_repairs_furniture_desync(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct obj_data *bench1 = furniture("bench1", 0);
  struct obj_data *bench2 = furniture("bench2", 0);

  do_sit(pc, arg("bench1"), 0, 0);
  GET_POS(pc) = POS_STANDING;  /* a buggy path: no char_from_furniture() */

  do_sit(pc, arg("bench2"), 0, 0);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("furniture-desync", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_NULL(OBJ_SAT_IN_BY(bench1));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(bench1, 1));
  TEST_ASSERT_EQUAL_PTR(bench2, SITTING(pc));
  fx_free_char(pc);
}

static struct room_direction_data *door_exit(room_rnum from, int dir, room_rnum to)
{
  struct room_direction_data *ex = calloc(1, sizeof(*ex));
  TEST_ASSERT_NOT_NULL(ex);
  ex->to_room = to;
  ex->key = NOTHING;
  ex->exit_info = EX_ISDOOR | EX_CLOSED;
  world[from].dir_option[dir] = ex;
  return ex;
}

/* A room DOOR script running 'wdoor <room> north purge': the exit is freed
 * and dir_option set to NULL before do_doorcmd uses it. */
static int purge_exit_hook(enum fx_trigger kind, struct char_data *actor,
                           struct obj_data *obj, int dir)
{
  (void)obj;
  if (kind == FX_DOOR_WTRIG) {
    free(world[IN_ROOM(actor)].dir_option[dir]);
    world[IN_ROOM(actor)].dir_option[dir] = NULL;
  }
  return 1;
}

/* H1-03: the door trigger purges the exit it was fired for. */
static void test_door_trigger_purging_exit_is_reported(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  door_exit(0, NORTH, 1);
  fx_hook = purge_exit_hook;

  do_gen_door(pc, arg("door north"), 0, SCMD_OPEN);

  TEST_ASSERT_EQUAL_INT(1, fx_trigger_calls[FX_DOOR_WTRIG]);
  TEST_ASSERT_NULL(world[0].dir_option[NORTH]);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("door-target-changed", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3000 dir 0"));
  fx_free_char(pc);
}

/* H1-03: AUTOKEY unlocks then opens; when the unlock's trigger removes the
 * door, the open must not run on it. */
static void test_autokey_stops_after_failed_unlock(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct room_direction_data *ex = door_exit(0, NORTH, 1);
  struct obj_data *key = fx_new_obj("key", ITEM_KEY);
  static struct index_data key_index[1];
  static struct obj_data key_proto[1];

  /* has_key() matches by vnum, so give the key a prototype index entry. */
  key_index[0].vnum = 3099;
  obj_index = key_index;
  obj_proto = key_proto;
  key->item_number = 0;
  ex->exit_info |= EX_LOCKED;
  ex->key = 3099;
  SET_BIT_AR(PRF_FLAGS(pc), PRF_AUTOKEY);
  obj_to_char(key, pc);
  fx_hook = purge_exit_hook;

  do_gen_door(pc, arg("door north"), 0, SCMD_OPEN);

  /* Only the unlock ran its triggers; the open was never attempted. */
  TEST_ASSERT_EQUAL_INT(1, fx_trigger_calls[FX_DOOR_MTRIG]);
  TEST_ASSERT_EQUAL_INT(1, fx_trigger_calls[FX_DOOR_WTRIG]);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("door-target-changed", tb_captured_operation);
  fx_free_char(pc);
  obj_index = NULL;
  obj_proto = NULL;
}

/* H1-03: an ordinary open with no interference still works and is silent. */
static void test_door_open_without_interference(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct room_direction_data *ex = door_exit(0, NORTH, 1);

  do_gen_door(pc, arg("door north"), 0, SCMD_OPEN);

  TEST_ASSERT_FALSE(IS_SET(ex->exit_info, EX_CLOSED));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(pc);
}

/* H1-14: a .wld sector past movement_loss[] (sector 10 passed the
 * loader's off-by-one check) costs 1 and is reported, not read OOB. */
static void test_move_into_room_with_bad_sector(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct room_direction_data *ex = door_exit(0, NORTH, 1);
  static struct zone_data zone[1];

  zone[0].min_level = zone[0].max_level = -1;
  zone_table = zone;                 /* both rooms are in zone 0 */
  ex->exit_info = 0;                 /* a plain open exit */
  world[1].sector_type = NUM_ROOM_SECTORS;
  GET_MOVE(pc) = GET_MAX_MOVE(pc) = 50;

  TEST_ASSERT_EQUAL_INT(1, do_simple_move(pc, NORTH, 0));

  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(pc));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("sector-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3001 sector 10"));
  fx_free_char(pc);
  zone_table = NULL;
}

/* #41: a move is checked before any world[]/zone_table[] access and again
 * after every special and leave trigger; a bad index is refused and reported
 * at the stage that found it.  Red before: master read past both arrays. */
static struct zone_data move_zone[1];

static struct char_data *open_north_exit_to_room_1(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct room_direction_data *ex = door_exit(0, NORTH, 1);

  memset(move_zone, 0, sizeof(move_zone));
  move_zone[0].min_level = move_zone[0].max_level = -1;
  zone_table = move_zone;
  top_of_zone_table = 0;
  ex->exit_info = 0;
  GET_MOVE(pc) = GET_MAX_MOVE(pc) = 50;
  return pc;
}

static void assert_move_refused_at(struct char_data *pc, const char *stage)
{
  char want[64];

  TEST_ASSERT_EQUAL_INT(0, do_simple_move(pc, NORTH, 0));
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(pc));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("move-index", tb_captured_operation);
  TEST_ASSERT_TRUE(snprintf(want, sizeof(want), "not valid at %s;", stage)
                   < (int)sizeof(want));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, want));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum"));
}

static void test_move_through_exit_past_world_is_refused(void)
{
  struct char_data *pc = open_north_exit_to_room_1();

  W_EXIT(0, NORTH)->to_room = top_of_world + 5;
  assert_move_refused_at(pc, "exit");
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "dir 0"));
  fx_free_char(pc);
  zone_table = NULL;
}

static void test_move_into_room_past_zone_table_is_refused(void)
{
  struct char_data *pc = open_north_exit_to_room_1();

  world[1].zone = 3;
  assert_move_refused_at(pc, "exit");
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 3 (top 0)"));
  fx_free_char(pc);
  zone_table = NULL;
}

/* A leave trigger that renumbers the destination's zone mid-move. */
static int rezone_on_leave_hook(enum fx_trigger kind, struct char_data *actor,
                                struct obj_data *obj, int dir)
{
  (void)actor; (void)obj; (void)dir;
  if (kind == FX_LEAVE_MTRIG)
    world[1].zone = 3;
  return 1;
}

static void test_leave_trigger_invalidating_zone_is_refused(void)
{
  struct char_data *pc = open_north_exit_to_room_1();

  fx_hook = rezone_on_leave_hook;
  assert_move_refused_at(pc, "leave-mtrigger");
  fx_hook = NULL;
  fx_free_char(pc);
  zone_table = NULL;
}

/* The greet trigger fired for 'trigger_mover' (a mob's script in the room
 * it enters) makes 'released' stop following, as an mfollow would; that
 * frees released's follow_type node. */
static struct char_data *trigger_mover, *released;
static int release_on_greet_hook(enum fx_trigger kind, struct char_data *actor,
                                 struct obj_data *obj, int dir)
{
  (void)obj; (void)dir;
  if (kind == FX_GREET_MTRIG && actor == trigger_mover && released &&
      released->master) {
    stop_follower(released);
    released = NULL;
  }
  return 1;
}

/* Ledger finding: perform_move() kept the next follow_type node across each
 * follower's move, whose triggers can release that next follower: the loop
 * then read a freed node (ASan heap-use-after-free). Followers are now
 * walked from a snapshot and re-checked, so the released one stays behind
 * and is reported. */
static void test_follower_released_by_trigger_is_skipped(void)
{
  struct char_data *leader = fx_new_char("hero", FALSE, 0);
  struct char_data *first = fx_new_char("dog", TRUE, 0);
  struct char_data *second = fx_new_char("cat", TRUE, 0);
  static struct zone_data zone[1];

  zone[0].min_level = zone[0].max_level = -1;
  zone_table = zone;
  door_exit(0, NORTH, 1)->exit_info = 0;   /* a plain open exit */
  GET_MOVE(leader) = GET_MAX_MOVE(leader) = 50;
  add_follower(first, leader);             /* followers: second, first */
  add_follower(second, leader);
  trigger_mover = second;                  /* moves first ... */
  released = first;                        /* ... and releases first */
  fx_hook = release_on_greet_hook;

  TEST_ASSERT_EQUAL_INT(1, perform_move(leader, NORTH, 0));

  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(leader));
  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(second));
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(first));
  TEST_ASSERT_NULL(first->master);
  TEST_ASSERT_EQUAL_STRING("follower-changed", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "was released by a trigger"));
  stop_follower(second);
  fx_free_char(second);
  fx_free_char(first);
  fx_free_char(leader);
  zone_table = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_follower_released_by_trigger_is_skipped);
  RUN_TEST(test_move_into_room_with_bad_sector);
  RUN_TEST(test_move_through_exit_past_world_is_refused);
  RUN_TEST(test_move_into_room_past_zone_table_is_refused);
  RUN_TEST(test_leave_trigger_invalidating_zone_is_refused);
  RUN_TEST(test_door_trigger_purging_exit_is_reported);
  RUN_TEST(test_autokey_stops_after_failed_unlock);
  RUN_TEST(test_door_open_without_interference);
  RUN_TEST(test_fight_unseats_character_before_second_sit);
  RUN_TEST(test_sit_reports_and_repairs_furniture_desync);
  return UNITY_END();
}
