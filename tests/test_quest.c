/* Executable tests of the production autoquest code (quest.c): completion,
 * re-entry through reward objects, and chain/type lookups. quest.c is linked
 * as is; the world, prototypes and game functions it reaches are fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "quest.h"
#include <stdarg.h>
#include "tb_error_capture.h"

/* quest.c defines this with external linkage but quest.h does not declare it
 * (the questmaster spec proc is its only production caller). */
void quest_list(struct char_data *ch, struct char_data *qm,
                char argument[MAX_INPUT_LENGTH]);

/* db.c owns these; this fixture does not link db.c. */
struct aq_data *aquest_table;
qst_rnum total_quests;
struct obj_data *obj_proto;
struct char_data *mob_proto;
struct zone_data *zone_table;

static char sent[MAX_STRING_LENGTH * 4];
static int obj_to_char_calls, reward_objects;
static int extracted_objects;

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t len = strlen(sent);
  (void)ch;
  va_start(args, messg);
  if (len < sizeof(sent))
    (void)vsnprintf(sent + len, sizeof(sent) - len, messg, args);
  va_end(args);
  return strlen(sent);
}
void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  (void)source_file; (void)source_line; (void)source_func;
  return calloc(count ? count : 1, element_size ? element_size : 1);
}
int MAX(int a, int b) { return a > b ? a : b; }
int increase_gold(struct char_data *ch, int amt)
{ GET_GOLD(ch) += amt; return GET_GOLD(ch); }
void gain_exp(struct char_data *ch, int gain) { GET_EXP(ch) += gain; }

/* Object prototypes: vnum 3010 exists at rnum 0. */
#define FIXTURE_OBJ_VNUM 3010
static struct index_data obj_index_fixture[1];
obj_rnum real_object(obj_vnum vnum)
{ return vnum == FIXTURE_OBJ_VNUM ? 0 : NOTHING; }
struct obj_data *read_object(obj_vnum nr, int type)
{
  struct obj_data *obj;
  (void)type;
  if (nr != FIXTURE_OBJ_VNUM)
    return NULL;
  obj = calloc(1, sizeof(*obj));
  TEST_ASSERT_NOT_NULL(obj);
  obj->item_number = 0;
  obj->short_description = (char *)"a loaf of bread";
  reward_objects++;
  return obj;
}
/* The real obj_to_char() runs the AQ_OBJ_FIND trigger check (handler.c). */
static struct obj_data *given[64];
void obj_to_char(struct obj_data *object, struct char_data *ch)
{
  if (obj_to_char_calls < (int)(sizeof(given) / sizeof(given[0])))
    given[obj_to_char_calls] = object;
  if (++obj_to_char_calls > 8)
    TEST_FAIL_MESSAGE("obj_to_char recursion: reward re-entered completion");
  autoquest_trigger_check(ch, NULL, object, AQ_OBJ_FIND);
}
void extract_obj(struct obj_data *obj) { (void)obj; extracted_objects++; }

static struct room_data room;
static struct char_data player, npc[2];
static struct player_special_data pspec;
static struct index_data mob_index_fixture[1];
static struct aq_data quests[4];

static void add_quest_fixture(int idx, qst_vnum vnum, int type, int target,
                              int quantity)
{
  memset(&quests[idx], 0, sizeof(quests[idx]));
  quests[idx].vnum = vnum;
  quests[idx].type = type;
  quests[idx].target = target;
  quests[idx].qm = NOBODY;
  quests[idx].name = (char *)"quest name";
  quests[idx].desc = (char *)"quest desc";
  quests[idx].info = (char *)"quest info\r\n";
  quests[idx].done = (char *)"quest done";
  quests[idx].quit = (char *)"quest quit";
  quests[idx].value[6] = quantity;  /* QST_QUANTITY */
  quests[idx].value[4] = -1;        /* QST_TIME */
  quests[idx].obj_reward = NOTHING;
  quests[idx].prev_quest = NOTHING;
  quests[idx].next_quest = NOTHING;
  quests[idx].prereq = NOTHING;
  if ((qst_rnum)(idx + 1) > total_quests)
    total_quests = (qst_rnum)(idx + 1);
}

void setUp(void)
{
  int k;

  memset(&room, 0, sizeof(room));
  memset(&player, 0, sizeof(player));
  memset(&pspec, 0, sizeof(pspec));
  memset(npc, 0, sizeof(npc));
  memset(quests, 0, sizeof(quests));
  memset(given, 0, sizeof(given));
  world = &room;
  top_of_world = 0;
  room.number = 3001;
  aquest_table = quests;
  total_quests = 0;
  mob_index_fixture[0].vnum = 500;
  mob_index = mob_index_fixture;
  top_of_mobt = 0;
  obj_index_fixture[0].vnum = FIXTURE_OBJ_VNUM;
  obj_index = obj_index_fixture;
  top_of_objt = 0;

  player.player_specials = &pspec;
  GET_IDNUM(&player) = 42;
  player.in_room = 0;
  GET_QUEST(&player) = NOTHING;
  room.people = &player;
  for (k = 0; k < 2; k++) {
    SET_BIT_AR(MOB_FLAGS(&npc[k]), MOB_ISNPC);
    npc[k].nr = 0;
    npc[k].in_room = 0;
    npc[k].player_specials = &dummy_mob;
  }
  player.next_in_room = &npc[0];
  npc[0].next_in_room = &npc[1];
  sent[0] = '\0';
  obj_to_char_calls = reward_objects = extracted_objects = 0;
  tb_error_capture_reset();
}

void tearDown(void)
{
  int k;
  for (k = 0; k < (int)(sizeof(given) / sizeof(given[0])); k++)
    free(given[k]);
  free(pspec.saved.completed_quests);
  pspec.saved.completed_quests = NULL;
  world = NULL;
  aquest_table = NULL;
  total_quests = 0;
}

/* H3-01: two NPCs of the target vnum stand in the room. The first completes
 * the quest (and clears it); the second must not run completion again with
 * GET_QUEST == NOTHING (aquest_table[NOTHING]). */
static void test_mob_find_two_targets_completes_once(void)
{
  add_quest_fixture(0, 100, AQ_MOB_FIND, 500, 1);
  quests[0].value[0] = 7;           /* QST_POINTS */
  set_quest(&player, 0);

  autoquest_trigger_check(&player, NULL, NULL, AQ_MOB_FIND);

  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(7, GET_QUESTPOINTS(&player));
  TEST_ASSERT_EQUAL_INT(1, GET_NUM_QUESTS(&player));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H3-01: a chained follow-on quest must not be counted down by the remaining
 * matches of the quest that just completed. */
static void test_mob_find_does_not_count_against_chained_quest(void)
{
  add_quest_fixture(0, 100, AQ_MOB_FIND, 500, 1);
  quests[0].next_quest = 101;
  add_quest_fixture(1, 101, AQ_MOB_KILL, 500, 3);
  set_quest(&player, 0);

  autoquest_trigger_check(&player, NULL, NULL, AQ_MOB_FIND);

  TEST_ASSERT_EQUAL_INT(101, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(3, GET_QUEST_COUNTER(&player));
}

/* H3-01: completion for a quest that no longer resolves is reported, the
 * player's quest is cleared, and no table entry is read. */
static void test_complete_missing_quest_reports(void)
{
  add_quest_fixture(0, 100, AQ_MOB_FIND, 500, 1);
  GET_QUEST(&player) = 999;
  GET_QUEST_COUNTER(&player) = 1;

  generic_complete_quest(&player);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("complete-missing-quest", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(0, GET_QUESTPOINTS(&player));
}

/* H3-02: an Object quest whose reward object is also its target. Handing
 * out the reward runs the AQ_OBJ_FIND check again (obj_to_char); it must not
 * complete the quest a second time. */
static void test_reward_equal_to_target_pays_once(void)
{
  add_quest_fixture(0, 100, AQ_OBJ_FIND, FIXTURE_OBJ_VNUM, 1);
  quests[0].obj_reward = FIXTURE_OBJ_VNUM;
  quests[0].value[0] = 5;           /* QST_POINTS */
  quests[0].gold_reward = 10;
  set_quest(&player, 0);

  obj_to_char(read_object(FIXTURE_OBJ_VNUM, VIRTUAL), &player);

  TEST_ASSERT_EQUAL_INT(2, reward_objects);   /* the pickup + one reward */
  TEST_ASSERT_EQUAL_INT(2, obj_to_char_calls);
  TEST_ASSERT_EQUAL_INT(5, GET_QUESTPOINTS(&player));
  TEST_ASSERT_EQUAL_INT(10, GET_GOLD(&player));
  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(1, GET_NUM_QUESTS(&player));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H3-03: 'quest list 1' at the questmaster for a quest whose prev_quest
 * names a deleted quest must print a marker and report the dangling link,
 * not read aquest_table[NOTHING].name. */
static void test_quest_list_dangling_prev_reports(void)
{
  char arg[MAX_INPUT_LENGTH] = "1";

  add_quest_fixture(0, 11, AQ_ROOM_FIND, 3001, 1);
  quests[0].qm = 500;
  quests[0].prev_quest = 99;

  quest_list(&player, &npc[0], arg);

  TEST_ASSERT_NOT_NULL(strstr(sent, "(missing quest 99)"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("dangling-chain", tb_captured_operation);
}

/* H3-03: a completed quest whose next_quest dangles reports the link and
 * leaves the player without a quest. */
static void test_complete_dangling_next_reports(void)
{
  add_quest_fixture(0, 100, AQ_ROOM_FIND, 3001, 1);
  quests[0].next_quest = 98;
  set_quest(&player, 0);

  autoquest_trigger_check(&player, NULL, NULL, AQ_ROOM_FIND);

  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("dangling-chain", tb_captured_operation);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_quest_list_dangling_prev_reports);
  RUN_TEST(test_complete_dangling_next_reports);
  RUN_TEST(test_reward_equal_to_target_pays_once);
  RUN_TEST(test_mob_find_two_targets_completes_once);
  RUN_TEST(test_mob_find_does_not_count_against_chained_quest);
  RUN_TEST(test_complete_missing_quest_reports);
  return UNITY_END();
}
