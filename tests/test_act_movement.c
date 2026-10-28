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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_fight_unseats_character_before_second_sit);
  RUN_TEST(test_sit_reports_and_repairs_furniture_desync);
  return UNITY_END();
}
