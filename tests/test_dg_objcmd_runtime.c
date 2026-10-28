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
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("oat-clone"));
  stone->item_number = saved;
  obj_index[saved].number++;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_fixture_round_trip);
  RUN_TEST(test_oat_clone_purging_itself);
  RUN_TEST(test_oat_clone_moved_away_is_still_destroyed);
  RUN_TEST(test_oat_clone_failure_is_reported);
  return UNITY_END();
}
