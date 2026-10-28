/* Executable tests of the production room script commands (dg_wldcmd.c),
 * and of the "all" loops shared with the mob and object commands, linked
 * against the real server objects and run in a tiny in-memory world
 * (dg_world_fixture.h).
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "dg_world_fixture.h"

void wld_command_interpreter(room_data *room, char *argument);
ACMD(do_mteleport);

void setUp(void) { dgw_setup(); }
void tearDown(void) { dgw_teardown(); }

static void wcmd(room_rnum r, const char *line)
{
  char *copy = strdup(line);
  TEST_ASSERT_NOT_NULL(copy);
  wld_command_interpreter(&world[r], copy);
  free(copy);
}

/* Room 101 gets an enter trigger that moves victim to room 102 whenever
 * anyone arrives. In 102, victim lands at the head of the occupant list, in
 * front of bystander: an "all" loop that saved victim as its next occupant
 * then walks room 102's list. */
static char enter_line[64];
static const char *enter_lines[] = { enter_line, NULL };

static void arm_enter_trigger(struct char_data *victim)
{
  snprintf(enter_line, sizeof(enter_line), "wteleport %c%ld 102", UID_CHAR,
           char_script_id(victim));
  world[1].script = dgw_script_with(WLD_TRIGGER, WTRIG_ENTER, enter_lines);
}

/* E1-10: 'wteleport all' must teleport only who was in the room. */
static void test_wteleport_all_ignores_characters_moved_by_callbacks(void)
{
  struct char_data *bystander = dgw_mob(DGW_MOB_OGRE, 2);
  struct char_data *victim = dgw_mob(DGW_MOB_GUARD, 0);
  struct char_data *leader = dgw_mob(DGW_MOB_GUARD, 0);   /* list head */

  arm_enter_trigger(victim);
  wcmd(0, "wteleport all 101");

  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(leader));
  TEST_ASSERT_EQUAL_INT(2, IN_ROOM(victim));
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, IN_ROOM(bystander),
      "wteleport all moved a character who was never in the room");
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("all-target-moved"));
}

/* E1-10: same loop in mteleport (the mob itself is moved too). */
static void test_mteleport_all_ignores_characters_moved_by_callbacks(void)
{
  struct char_data *bystander = dgw_mob(DGW_MOB_OGRE, 2);
  struct char_data *victim = dgw_mob(DGW_MOB_GUARD, 0);
  struct char_data *caller = dgw_mob(DGW_MOB_GUARD, 0);   /* list head */
  char *arg = strdup("all 101");

  arm_enter_trigger(victim);
  do_mteleport(caller, arg, 0, 0);
  free(arg);

  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(caller));
  TEST_ASSERT_EQUAL_INT(2, IN_ROOM(victim));
  TEST_ASSERT_EQUAL_INT(2, IN_ROOM(bystander));
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("all-target-moved"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_wteleport_all_ignores_characters_moved_by_callbacks);
  RUN_TEST(test_mteleport_all_ignores_characters_moved_by_callbacks);
  return UNITY_END();
}
