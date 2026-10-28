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

/* E1-13: a non-numeric target used to become vnum 0 (a real room). */
static void test_wteleport_non_numeric_target_is_refused(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);

  world[0].number = 0;                 /* make vnum 0 a real room here */
  wcmd(1, "wteleport all abc");
  wcmd(1, "wteleport all 0x65");
  wcmd(1, "wmove all abc");
  world[0].number = DGW_ROOM0;
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(m));
  TEST_ASSERT_EQUAL_INT(3, dgw_saw("parse-vnum"));
}

/* E1-V01: 70000 does not fit a vnum; it used to wrap to 4464 (here: wrap
 * onto room 101 = 65536 + 101 = 65637) and teleport everyone there. */
static void test_wteleport_vnum_overflow_is_refused(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);

  wcmd(0, "wteleport all 65637");
  wcmd(0, "wteleport all 99999999999");
  wcmd(0, "wteleport all -65435");
  wcmd(0, "wmove all 65637");
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(m));
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(stone));
  TEST_ASSERT_EQUAL_INT(4, dgw_saw("parse-vnum"));

  wcmd(0, "wteleport all 101");        /* a real vnum still works */
  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(m));
}

/* E1-13: load and damage arguments that do not parse are refused. */
static void test_wload_and_wdamage_bad_numbers(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  char line[64];

  wcmd(0, "wload obj 99999999999");
  wcmd(0, "wload obj 65737");          /* 65536 + 201 */
  wcmd(0, "wload mob 300 65637");      /* room target that wraps */
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_PTR(m, world[0].people);
  TEST_ASSERT_NULL(m->next_in_room);
  TEST_ASSERT_EQUAL_INT(3, dgw_saw("parse-vnum"));

  snprintf(line, sizeof(line), "wdamage %c%ld lots", UID_CHAR,
           char_script_id(m));
  wcmd(0, line);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("parse-number"));

  /* The room-target form works (it never matched before: the target
   * started with the separating space). */
  wcmd(0, "wload mob 301 101");
  TEST_ASSERT_NOT_NULL(world[1].people);
  TEST_ASSERT_EQUAL_INT(DGW_MOB_OGRE, GET_MOB_VNUM(world[1].people));
}

/* E1-14: an exit created by a non-room field leads nowhere (calloc made it
 * lead to rnum 0, a real room), and saying so; bad room/key values are
 * refused. */
static void test_wdoor_new_exit_leads_nowhere(void)
{
  struct room_direction_data *e;

  wcmd(0, "wdoor 100 up flags a");
  e = world[0].dir_option[UP];
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_INT(NOWHERE, e->to_room);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("door-create"));

  wcmd(0, "wdoor 100 up room 101");
  TEST_ASSERT_EQUAL_INT(1, e->to_room);
  wcmd(0, "wdoor 100 up room 65637");          /* wraps onto 101 */
  TEST_ASSERT_EQUAL_INT(NOWHERE, e->to_room);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("door-target"));

  wcmd(0, "wdoor 100 up key -1");
  TEST_ASSERT_EQUAL_INT(NOTHING, e->key);
  wcmd(0, "wdoor 100 up key 205");
  TEST_ASSERT_EQUAL_INT(205, e->key);
  wcmd(0, "wdoor 100 up key 70000");
  TEST_ASSERT_EQUAL_INT(205, e->key);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("parse-vnum"));

  wcmd(0, "wdoor 100 north room 102");          /* room field: no warning */
  TEST_ASSERT_EQUAL_INT(2, world[0].dir_option[NORTH]->to_room);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("door-create"));

  wcmd(0, "wdoor 100 up name hatch");
  wcmd(0, "wdoor 100 up description A hatch.");
  TEST_ASSERT_EQUAL_STRING("hatch", e->keyword);
  wcmd(0, "wdoor 100 up purge");
  TEST_ASSERT_NULL(world[0].dir_option[UP]);
}

static const char *global_var(room_rnum r, const char *name)
{
  struct trig_var_data *v;
  for (v = SCRIPT(&world[r])->global_vars; v; v = v->next)
    if (!strcmp(v->name, name))
      return v->value;
  return "";
}

/* E1-19: 'wload obj <vnum> <target>' updates %lastloaded% too. */
static void test_wload_with_target_sets_lastloaded(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  char expect[32];

  world[0].script = dgw_script();
  wcmd(0, "wload obj 201");
  snprintf(expect, sizeof(expect), "%c%ld", UID_CHAR,
           obj_script_id(world[0].contents));
  TEST_ASSERT_EQUAL_STRING(expect, global_var(0, "lastloaded"));

  wcmd(0, "wload obj 205 zzguard");
  TEST_ASSERT_NOT_NULL(m->carrying);
  snprintf(expect, sizeof(expect), "%c%ld", UID_CHAR,
           obj_script_id(m->carrying));
  TEST_ASSERT_EQUAL_STRING(expect, global_var(0, "lastloaded"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_wload_with_target_sets_lastloaded);
  RUN_TEST(test_wdoor_new_exit_leads_nowhere);
  RUN_TEST(test_wteleport_non_numeric_target_is_refused);
  RUN_TEST(test_wteleport_vnum_overflow_is_refused);
  RUN_TEST(test_wload_and_wdamage_bad_numbers);
  RUN_TEST(test_wteleport_all_ignores_characters_moved_by_callbacks);
  RUN_TEST(test_mteleport_all_ignores_characters_moved_by_callbacks);
  return UNITY_END();
}
