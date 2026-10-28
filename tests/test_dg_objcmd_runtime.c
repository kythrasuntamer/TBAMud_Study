/* Executable tests of the production object script commands (dg_objcmd.c),
 * linked against the real server objects and run in a tiny in-memory world
 * (dg_world_fixture.h). Each scenario is one an object trigger can reach;
 * the assertions check both that nothing is freed early, leaked or lost
 * (ASan/LSan, Valgrind and the fixture's index-count checks) and that each
 * rejecting guard emitted its structured diagnostic.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "dg_world_fixture.h"

void obj_command_interpreter(obj_data *obj, char *argument);

void setUp(void) { dgw_setup(); }
void tearDown(void) { dgw_teardown(); }

/* obj_command_interpreter() takes char *: pass a heap copy so ASan sees any
 * read past the terminator. */
static void ocmd(obj_data *obj, const char *line)
{
  char *copy = strdup(line);
  TEST_ASSERT_NOT_NULL(copy);
  obj_command_interpreter(obj, copy);
  free(copy);
}

static int room_count_objs(room_rnum r, obj_vnum vnum)
{
  struct obj_data *o;
  int n = 0;
  for (o = world[r].contents; o; o = o->next_content)
    if (GET_OBJ_VNUM(o) == vnum)
      n++;
  return n;
}

static void test_fixture_round_trip(void)
{
  struct obj_data *bag = dgw_obj(DGW_OBJ_BAG, 0);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, NOWHERE);
  obj_to_obj(stone, bag);
  TEST_ASSERT_EQUAL_INT(7, GET_OBJ_WEIGHT(bag));
  (void)dgw_mob(DGW_MOB_GUARD, 1);
}

/* E1-02: 'oat <room> opurge self' purges the temporary clone. The clone must
 * not be read after it is freed, and the real owner must survive (each
 * script frame checks only its own owner, by UID). */
static void test_oat_clone_purging_itself(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  long stone_uid = obj_script_id(stone);

  ocmd(stone, "oat 101 opurge self");

  TEST_ASSERT_TRUE(obj_uid_matches(stone_uid, stone));
  TEST_ASSERT_EQUAL_INT(0, room_count_objs(1, DGW_OBJ_STONE));
  TEST_ASSERT_EQUAL_INT(1, room_count_objs(0, DGW_OBJ_STONE));
}

/* E1-02: 'oat <room> omove <other>' moves the clone out of the 'at' room. It
 * must still be destroyed, not left behind as a duplicate. */
static void test_oat_clone_moved_away_is_still_destroyed(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);

  ocmd(stone, "oat 101 omove 102");

  TEST_ASSERT_EQUAL_INT(0, room_count_objs(1, DGW_OBJ_STONE));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, room_count_objs(2, DGW_OBJ_STONE),
      "oat left a duplicate of the owner in the world");
  TEST_ASSERT_EQUAL_INT(1, room_count_objs(0, DGW_OBJ_STONE));
}

/* E1-02: an owner without a prototype cannot be cloned; say so. */
static void test_oat_clone_failure_is_reported(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  obj_rnum saved = GET_OBJ_RNUM(stone);

  obj_index[saved].number--;    /* pretend it never had a prototype */
  stone->item_number = NOTHING;
  ocmd(stone, "oat 101 oecho hello");
  /* Give the stone its prototype back before asserting: a failed assertion
   * ends the test, and teardown would then free a NOTHING-rnum stone as
   * owning its strings -- the prototype's string literals (an invalid
   * free; ASan: bad-free, Fil-C: "cannot free global object"). */
  stone->item_number = saved;
  obj_index[saved].number++;
  TEST_ASSERT_EQUAL_INT(0, room_count_objs(1, DGW_OBJ_STONE)); /* no clone */
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("oat-clone"));
}

/* Run a one-trigger script on obj; returns the owner pointer as
 * script_driver() left it (NULL once it decided the owner is gone). */
static obj_data *run_obj_script(obj_data *obj, const char *const lines[])
{
  trig_data *t;
  SCRIPT(obj) = dgw_script_with(OBJ_TRIGGER, OTRIG_RANDOM, lines);
  t = TRIGGERS(SCRIPT(obj));
  dg_script_execution_enter();   /* as when called from a trigger handler */
  script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
  dg_script_execution_leave();
  return obj;
}

/* E1-03: an object whose script purges the container holding it is freed
 * with the container. script_driver() must stop instead of running the next
 * line (or reading SCRIPT()) on the freed owner. */
static void test_purging_own_container_stops_the_script(void)
{
  static const char *const lines[] = { "opurge zzbag", "oecho still here", NULL };
  struct obj_data *bag = dgw_obj(DGW_OBJ_BAG, 0);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, NOWHERE);
  obj_to_obj(stone, bag);

  TEST_ASSERT_NULL(run_obj_script(stone, lines));
  TEST_ASSERT_NULL(world[0].contents);
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("owner-extracted"));
}

/* E1-03: 'opurge' with no argument purges the room's objects; the owner is
 * inside a container on the floor, so it goes too. */
static void test_purging_the_room_from_inside_a_container_stops_the_script(void)
{
  static const char *const lines[] = { "opurge", "oecho still here", NULL };
  struct obj_data *bag = dgw_obj(DGW_OBJ_BAG, 0);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, NOWHERE);
  obj_to_obj(stone, bag);

  TEST_ASSERT_NULL(run_obj_script(stone, lines));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("owner-extracted"));
}

/* E1-03: 'opurge self' is the expected way to end; no warning for it. */
static void test_opurge_self_is_not_reported(void)
{
  static const char *const lines[] = { "opurge self", "oecho still here", NULL };
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);

  TEST_ASSERT_NULL(run_obj_script(stone, lines));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("owner-extracted"));
}

/* A script that purges something else keeps running with its owner. */
static void test_purging_something_else_keeps_the_owner(void)
{
  static const char *const lines[] = { "opurge zzbag", "oecho still here", NULL };
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  (void)dgw_obj(DGW_OBJ_BAG, 0);

  TEST_ASSERT_EQUAL_PTR(stone, run_obj_script(stone, lines));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("owner-extracted"));
}

/* E1-05: values used as array indexes elsewhere are range checked. */
static void test_osetval_rejects_out_of_range_index_values(void)
{
  struct obj_data *skin = dgw_obj(DGW_OBJ_SKIN, 0);
  struct obj_data *sword = dgw_obj(DGW_OBJ_SWORD, 0);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  int refused;

  ocmd(skin, "osetval 2 5000");
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(skin, 2));
  ocmd(skin, "osetval 2 -1");
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(skin, 2));
  ocmd(sword, "osetval 3 200");
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(sword, 3));
  refused = dgw_saw("osetval-range");

  ocmd(skin, "osetval 2 3");
  TEST_ASSERT_EQUAL_INT(3, GET_OBJ_VAL(skin, 2));
  ocmd(sword, "osetval 3 14");
  TEST_ASSERT_EQUAL_INT(14, GET_OBJ_VAL(sword, 3));
  ocmd(stone, "osetval 2 5000");   /* not an index for this type */
  TEST_ASSERT_EQUAL_INT(5000, GET_OBJ_VAL(stone, 2));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(3, refused);
  TEST_ASSERT_EQUAL_INT(3, dgw_saw("osetval-range"));
}

static struct char_data *seat(struct obj_data *bench)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, IN_ROOM(bench));
  SITTING(m) = bench;
  NEXT_SITTING(m) = OBJ_SAT_IN_BY(bench);
  OBJ_SAT_IN_BY(bench) = m;
  GET_OBJ_VAL(bench, 1)++;
  GET_POS(m) = POS_SITTING;
  return m;
}

/* E1-07: furniture transformed into furniture keeps its sitters. Before the
 * fix sitting_here was lost, the bench was freed with a sitter still
 * pointing at it, and the sitter standing up read the freed bench. */
static void test_otransform_keeps_sitters_on_furniture(void)
{
  struct obj_data *bench = dgw_obj(DGW_OBJ_BENCH, 0);
  struct char_data *m = seat(bench);

  ocmd(bench, "otransform 202");
  TEST_ASSERT_EQUAL_PTR(m, OBJ_SAT_IN_BY(bench));
  TEST_ASSERT_EQUAL_INT(1, GET_OBJ_VAL(bench, 1));

  extract_obj(bench);       /* unseats m */
  TEST_ASSERT_NULL(SITTING(m));
  char_from_furniture(m);
}

/* E1-07: furniture transformed into something else unseats its sitters,
 * and says so. */
static void test_otransform_to_non_furniture_unseats(void)
{
  struct obj_data *bench = dgw_obj(DGW_OBJ_BENCH, 0);
  struct char_data *m = seat(bench);

  ocmd(bench, "otransform 206");
  TEST_ASSERT_NULL(SITTING(m));
  TEST_ASSERT_NULL(OBJ_SAT_IN_BY(bench));
  extract_obj(bench);
  char_from_furniture(m);
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("otransform"));
}

/* E1-07: carried and container weights follow the new weight. */
static void test_otransform_recomputes_weights(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *bag = dgw_obj(DGW_OBJ_BAG, NOWHERE);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, NOWHERE);
  struct obj_data *loose = dgw_obj(DGW_OBJ_STONE, NOWHERE);

  obj_to_char(bag, m);
  obj_to_obj(stone, bag);
  obj_to_char(loose, m);
  TEST_ASSERT_EQUAL_INT(9, IS_CARRYING_W(m));     /* 5 + 2 + 2 */

  ocmd(stone, "otransform 206");                   /* 2 -> 20, in the bag */
  TEST_ASSERT_EQUAL_PTR(bag, stone->in_obj);
  TEST_ASSERT_EQUAL_INT(25, GET_OBJ_WEIGHT(bag));
  TEST_ASSERT_EQUAL_INT(27, IS_CARRYING_W(m));

  ocmd(loose, "otransform 206");                   /* 2 -> 20, carried */
  TEST_ASSERT_EQUAL_PTR(m, loose->carried_by);
  TEST_ASSERT_EQUAL_INT(45, IS_CARRYING_W(m));
  TEST_ASSERT_EQUAL_INT(2, IS_CARRYING_N(m));     /* bag, rock */

  ocmd(bag, "otransform 205");                     /* bag 5 -> chest 10 */
  TEST_ASSERT_EQUAL_PTR(stone, bag->contains);
  TEST_ASSERT_EQUAL_INT(30, GET_OBJ_WEIGHT(bag));
  TEST_ASSERT_EQUAL_INT(50, IS_CARRYING_W(m));
}

/* E1-07 / E1-V02: the transformed object counts as an instance of the new
 * prototype only (teardown also checks every count returns to zero). */
static void test_otransform_moves_the_instance_count(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  obj_rnum stone_r = GET_OBJ_RNUM(stone);

  ocmd(stone, "otransform 206");
  TEST_ASSERT_EQUAL_INT(0, obj_index[stone_r].number);
  TEST_ASSERT_EQUAL_INT(1, obj_index[real_object(DGW_OBJ_ROCK)].number);
}

/* E1-08: an unresolvable omove target leaves the object where it was. */
static void test_omove_to_invalid_room_keeps_the_object(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, NOWHERE);
  int refused;
  obj_to_char(stone, m);

  ocmd(stone, "omove 99999");
  TEST_ASSERT_EQUAL_PTR(m, stone->carried_by);
  TEST_ASSERT_EQUAL_PTR(stone, m->carrying);
  refused = dgw_saw("omove-target");

  ocmd(stone, "omove 102");
  TEST_ASSERT_NULL(stone->carried_by);
  TEST_ASSERT_EQUAL_INT(2, IN_ROOM(stone));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, refused);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("omove-target"));
}

/* E1-09: 'oteleport all' from an object that is not in any room read
 * world[NOWHERE].people (far past the end of the world table). */
static void test_oteleport_all_from_unplaced_object(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, NOWHERE);

  ocmd(stone, "oteleport all 101");
  obj_to_room(stone, 0);   /* so teardown finds it placed */
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("oteleport-origin"));
}

/* E1-09: teleporting everyone into the room they are in is refused, not
 * logged and then done anyway (re-firing every enter trigger). */
static void test_oteleport_all_to_own_room_is_refused(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct char_data *other = dgw_mob(DGW_MOB_OGRE, 0);

  ocmd(stone, "oteleport all 100");
  TEST_ASSERT_EQUAL_PTR(other, world[0].people);   /* order untouched */
  TEST_ASSERT_EQUAL_PTR(m, other->next_in_room);

  ocmd(stone, "oteleport all 101");
  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(m));
  TEST_ASSERT_EQUAL_INT(1, IN_ROOM(other));
}

/* E1-09 (same class): a target character who is in no room. */
static void test_object_target_character_in_no_room(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 1);
  char cmd[64];

  char_from_room(m);
  tb_snprintf(cmd, sizeof(cmd), "omove %c%ld", UID_CHAR, char_script_id(m));
  ocmd(stone, cmd);
  char_to_room(m, 1);      /* so teardown finds it placed */
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(stone));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("target-room"));
}

/* E1-13: numeric object-command arguments are parsed strictly. */
static void test_obj_numeric_arguments_are_checked(void)
{
  struct obj_data *stone = dgw_obj(DGW_OBJ_STONE, 0);
  int bad_numbers, bad_vnums;

  GET_OBJ_TIMER(stone) = 7;
  ocmd(stone, "otimer 99999999999");
  TEST_ASSERT_EQUAL_INT(7, GET_OBJ_TIMER(stone));
  ocmd(stone, "osetval 0 99999999999");
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(stone, 0));
  bad_numbers = dgw_saw("parse-number");

  ocmd(stone, "otransform 65737");     /* 65536 + 201 */
  TEST_ASSERT_EQUAL_INT(DGW_OBJ_STONE, GET_OBJ_VNUM(stone));
  ocmd(stone, "oload obj 65738");
  ocmd(stone, "omove 65637");
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(stone));
  bad_vnums = dgw_saw("parse-vnum");

  ocmd(stone, "otimer 12");
  TEST_ASSERT_EQUAL_INT(12, GET_OBJ_TIMER(stone));
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(2, bad_numbers);
  TEST_ASSERT_EQUAL_INT(3, bad_vnums);
}

/* E1-22: an o-command rejection is reported as cmd-rejected with the
 * object's vnum; olog, the script's own message, is not. */
static void test_obj_rejection_is_reported_and_olog_is_not(void)
{
  struct obj_data *o = dgw_obj(DGW_OBJ_STONE, 0);

  ocmd(o, "olog just a note");
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("cmd-rejected"));
  ocmd(o, "oload obj 99999");
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("cmd-rejected"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("cmd-rejected"), "obj vnum 201"));
}

/* An unplaced object's commands that need its room refuse loudly
 * (VALID_OBJ_ROOM); bare 'opurge' used to return without a word. */
static void test_unplaced_object_room_commands_are_reported(void)
{
  struct obj_data *o = dgw_obj(DGW_OBJ_STONE, NOWHERE);

  ocmd(o, "opurge");
  ocmd(o, "oecho hello");
  ocmd(o, "oload obj 201");
  DGW_REQUIRE_DIAG_CAPTURE();
  TEST_ASSERT_EQUAL_INT(3, dgw_saw("cmd-rejected"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_fixture_round_trip);
  RUN_TEST(test_oat_clone_purging_itself);
  RUN_TEST(test_oat_clone_moved_away_is_still_destroyed);
  RUN_TEST(test_oat_clone_failure_is_reported);
  RUN_TEST(test_purging_own_container_stops_the_script);
  RUN_TEST(test_purging_the_room_from_inside_a_container_stops_the_script);
  RUN_TEST(test_opurge_self_is_not_reported);
  RUN_TEST(test_purging_something_else_keeps_the_owner);
  RUN_TEST(test_osetval_rejects_out_of_range_index_values);
  RUN_TEST(test_otransform_keeps_sitters_on_furniture);
  RUN_TEST(test_otransform_to_non_furniture_unseats);
  RUN_TEST(test_otransform_recomputes_weights);
  RUN_TEST(test_otransform_moves_the_instance_count);
  RUN_TEST(test_omove_to_invalid_room_keeps_the_object);
  RUN_TEST(test_oteleport_all_from_unplaced_object);
  RUN_TEST(test_oteleport_all_to_own_room_is_refused);
  RUN_TEST(test_object_target_character_in_no_room);
  RUN_TEST(test_obj_numeric_arguments_are_checked);
  RUN_TEST(test_obj_rejection_is_reported_and_olog_is_not);
  RUN_TEST(test_unplaced_object_room_commands_are_reported);
  return UNITY_END();
}
