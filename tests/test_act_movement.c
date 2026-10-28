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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_move_into_room_with_bad_sector);
  RUN_TEST(test_door_trigger_purging_exit_is_reported);
  RUN_TEST(test_autokey_stops_after_failed_unlock);
  RUN_TEST(test_door_open_without_interference);
  RUN_TEST(test_fight_unseats_character_before_second_sit);
  RUN_TEST(test_sit_reports_and_repairs_furniture_desync);
  return UNITY_END();
}
