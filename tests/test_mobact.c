/* Executable tests of the production mobile_activity() (mobact.c): a mob
 * that a script purges during its own turn must stop acting. mobact.c is
 * linked as is; the world and the game functions it reaches are fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "fight.h"
#include "graph.h"
#include "act.h"
#include <stdarg.h>
#include "tb_error_capture.h"

void basic_mud_log(const char *format, ...) { (void)format; }
/* 0 makes the scavenger roll succeed; the default skips scavenging. */
static int scavenge_roll = 18;
/* utils.h maps rand_number() and room_is_dark() to their _at forms. */
int rand_number_at(int from, int to, const char *source_file, int source_line,
                   const char *source_func)
{ (void)source_file; (void)source_line; (void)source_func;
  if (from == 0 && to == 10)
    return scavenge_roll;         /* the scavenger's 1-in-11 roll */
  return from == 0 ? 18 : from; }
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }

static int hits;
void hit(struct char_data *ch, struct char_data *victim, int type)
{ (void)ch; (void)victim; (void)type; hits++; }
/* The hunt step is where the fixture's "script" purges the mob, as an
 * enter trigger reached through hunt_victim()/perform_move() would. */
static int purge_on_hunt;
void hunt_victim(struct char_data *ch)
{
  if (purge_on_hunt)
    SET_BIT_AR(MOB_FLAGS(ch), MOB_NOTDEADYET);
}
int perform_move(struct char_data *ch, int dir, int following)
{ (void)ch; (void)dir; (void)following; return 0; }
void stop_follower(struct char_data *ch) { (void)ch; }
int num_followers_charmed(struct char_data *ch) { (void)ch; return 0; }
/* The object the scavenger took out of the room, if any. */
static struct obj_data *taken_obj;
void obj_from_room(struct obj_data *object) { taken_obj = object; }
void obj_to_char(struct obj_data *object, struct char_data *ch) { (void)object; (void)ch; }
int room_is_dark_at(room_rnum room, const char *source_file, int source_line,
                    const char *source_func)
{ (void)room; (void)source_file; (void)source_line; (void)source_func;
  return 0; }
int find_command(const char *command) { (void)command; return 0; }
/* CAN_CARRY_W: all zero, so only weightless objects can be carried. */
const struct str_app_type str_app[26];

static struct room_data room;
static struct char_data mob, player;
static struct player_special_data pspec;
static struct index_data mob_index_fixture[1];

void setUp(void)
{
  memset(&room, 0, sizeof(room));
  memset(&mob, 0, sizeof(mob));
  memset(&player, 0, sizeof(player));
  memset(&pspec, 0, sizeof(pspec));
  world = &room;
  top_of_world = 0;
  mob_index_fixture[0].vnum = 3060;
  mob_index = mob_index_fixture;
  top_of_mobt = 0;

  SET_BIT_AR(MOB_FLAGS(&mob), MOB_ISNPC);
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_AGGRESSIVE);
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_SENTINEL);
  mob.nr = 0;
  mob.in_room = 0;
  mob.player_specials = &dummy_mob;
  GET_POS(&mob) = POS_STANDING;
  GET_LEVEL(&mob) = 10;

  player.player_specials = &pspec;
  player.in_room = 0;
  GET_POS(&player) = POS_STANDING;
  GET_LEVEL(&player) = 5;
  GET_IDNUM(&player) = 3;

  room.people = &mob;
  mob.next_in_room = &player;
  character_list = &mob;
  hits = 0;
  purge_on_hunt = 0;
  scavenge_roll = 18;
  taken_obj = NULL;
  tb_error_capture_reset();
}

void tearDown(void) { world = NULL; character_list = NULL; mob_index = NULL; }

/* Baseline: an aggressive mob attacks the player in its room. */
static void test_aggressive_mob_attacks(void)
{
  mobile_activity();
  TEST_ASSERT_EQUAL_INT(1, hits);
}

/* H3-17: purged during hunt_victim() (a DG trigger), the mob is pending
 * extraction and must not go on to attack. */
static void test_purged_mob_stops_acting(void)
{
  purge_on_hunt = 1;
  mobile_activity();
  TEST_ASSERT_EQUAL_INT(0, hits);
}

/* H2-06: a scavenger must not carry off furniture someone is sitting on.
 * obj_from_room() would unseat the sitter; do_get() refuses the same way. */
static void test_scavenger_leaves_occupied_furniture(void)
{
  struct obj_data chair;

  memset(&chair, 0, sizeof(chair));
  chair.item_number = NOTHING;
  chair.in_room = 0;
  SET_BIT_AR(GET_OBJ_WEAR(&chair), ITEM_WEAR_TAKE);
  GET_OBJ_COST(&chair) = 100;
  room.contents = &chair;
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_SCAVENGER);
  scavenge_roll = 0;

  OBJ_SAT_IN_BY(&chair) = &player;
  mobile_activity();
  TEST_ASSERT_NULL(taken_obj);

  /* Control: the same chair, empty, is taken. */
  OBJ_SAT_IN_BY(&chair) = NULL;
  mobile_activity();
  TEST_ASSERT_EQUAL_PTR(&chair, taken_obj);
  room.contents = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_aggressive_mob_attacks);
  RUN_TEST(test_purged_mob_stops_acting);
  RUN_TEST(test_scavenger_leaves_occupied_furniture);
  return UNITY_END();
}
