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
#include "interpreter.h"
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
struct command_info *complete_cmd_info;
struct obj_data *get_obj_in_list_num(int num, struct obj_data *list)
{ (void)num; (void)list; return NULL; }
mob_rnum real_mobile(mob_vnum vnum) { return vnum == 500 ? 0 : NOBODY; }
size_t sprintbit(bitvector_t vektor, const char *names[], char *result,
                 size_t reslen)
{ (void)vektor; (void)names; return strlcpy(result, "NOBITS", reslen); }
/* do_quest's argument parsing: "word word" only. */
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  if (sscanf(argument, "%63s %63s", first_arg, second_arg) < 2)
    *second_arg = '\0';
  return argument + strlen(argument);
}
int search_block(char *arg, const char **list, int exact)
{
  int k;
  (void)exact;
  for (k = 0; *list[k] != '\n'; k++)
    if (!strcmp(arg, list[k]))
      return k;
  return -1;
}
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

/* H3-04: a quest row with an out-of-range type is reported by the trigger
 * check and shown as "<invalid>" by 'quest status', never used to index
 * quest_types[]. */
static void test_invalid_type_reported_and_named_safely(void)
{
  char cmd[MAX_INPUT_LENGTH] = "status 100";

  add_quest_fixture(0, 100, AQ_ROOM_FIND, 3001, 1);
  set_quest(&player, 0);
  quests[0].type = 9;

  autoquest_trigger_check(&player, NULL, NULL, 9);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("trigger-type", tb_captured_operation);

  TEST_ASSERT_EQUAL_STRING("<invalid>", QST_TYPE_NAME(0));
  GET_LEVEL(&player) = LVL_IMPL;
  do_quest(&player, cmd, 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "<invalid>"));
}

/* db.c's readers, reduced to what parse_quest needs: '~'-terminated strings
 * and the next non-blank line. */
char *fread_string(FILE *fl, const char *error)
{
  char buf[256];
  size_t n = 0;
  int c;
  (void)error;
  while ((c = fgetc(fl)) != EOF && c != '~' && n + 1 < sizeof(buf))
    buf[n++] = (char)c;
  buf[n] = '\0';
  while (c != EOF && c != '\n')
    c = fgetc(fl);
  return strdup(buf);
}
int get_line(FILE *fl, char *buf)
{
  char tmp[256];
  do {
    if (!fgets(tmp, sizeof(tmp), fl))
      return 0;
    tmp[strcspn(tmp, "\r\n")] = '\0';
  } while (!*tmp);
  strlcpy(buf, tmp, 256);
  return 1;
}
bitvector_t asciiflag_conv(char *flag) { (void)flag; return 0; }

/* H3-05: an unknown questmaster vnum and unknown record lines are reported
 * with the quest vnum instead of being dropped silently. */
static void test_parse_reports_unknown_qm_and_skipped_lines(void)
{
  static const char record[] =
    "name~\ndesc~\ninfo~\ndone~\nquit~\n"
    "1 777 0 3001 -1 -1 -1\n"
    "5 0 1 30 -1 0 1\n"
    "0 0 -1\n"
    "X junk\n"
    "S\n";
  struct aq_data *row;
  FILE *fl = fmemopen((void *)record, sizeof(record) - 1, "r");

  TEST_ASSERT_NOT_NULL(fl);
  aquest_table = quests;
  parse_quest(fl, 1234);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));

  TEST_ASSERT_EQUAL_INT(1, total_quests);
  row = &quests[0];
  TEST_ASSERT_EQUAL_INT(1234, row->vnum);
  TEST_ASSERT_EQUAL_INT(NOBODY, row->qm);
  TEST_ASSERT_EQUAL_INT(AQ_ROOM_FIND, row->type);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("parse-skip-line", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "quest vnum 1234"));
  free(row->name); free(row->desc); free(row->info);
  free(row->done); free(row->quit);
}

static int shop_calls;
static SPECIAL(fake_shop)
{
  (void)ch; (void)me; (void)cmd; (void)argument;
  shop_calls++;
  return TRUE;
}

/* H3-06: a questmaster's original spec proc survives a lower-vnum quest
 * added later by qedit (QST_FUNC NULL, inserted first in vnum order). */
static void test_questmaster_finds_secondary_on_any_quest(void)
{
  char arg[MAX_INPUT_LENGTH] = "list";

  add_quest_fixture(0, 450, AQ_ROOM_FIND, 3001, 1);
  add_quest_fixture(1, 500, AQ_ROOM_FIND, 3001, 1);
  quests[0].qm = quests[1].qm = 500;
  quests[1].func = fake_shop;
  shop_calls = 0;

  TEST_ASSERT_TRUE(quest_qm_secondary(500) == fake_shop);
  TEST_ASSERT_TRUE(questmaster(&player, &npc[0], 0, arg));
  TEST_ASSERT_EQUAL_INT(1, shop_calls);

  quest_set_qm_secondary(500, fake_shop);
  TEST_ASSERT_TRUE(quests[0].func == fake_shop);
  quest_set_qm_secondary(500, questmaster);   /* never parks questmaster */
  TEST_ASSERT_TRUE(quests[0].func == fake_shop);
}

/* genqst.c fixtures for delete_quest(). */
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return NOWHERE; }
void add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; }
void *tb_realloc_array_at(void *ptr, size_t count, size_t element_size,
                          const char *source_file, int source_line,
                          const char *source_func)
{
  (void)source_file; (void)source_line; (void)source_func;
  return realloc(ptr, count * element_size);
}

/* H3-V01: deleting the last quest of questmaster mob vnum 500 restores the
 * spec proc on mob_index[real_mobile(500)], not on mob_index[500]. */
static void test_delete_last_quest_restores_qm_by_rnum(void)
{
  struct aq_data *table = calloc(1, sizeof(*table));

  TEST_ASSERT_NOT_NULL(table);
  table[0].vnum = 450;
  table[0].qm = 500;
  table[0].func = fake_shop;
  aquest_table = table;
  total_quests = 1;
  mob_index_fixture[0].func = questmaster;

  TEST_ASSERT_TRUE(delete_quest(0));

  TEST_ASSERT_EQUAL_INT(0, total_quests);
  TEST_ASSERT_TRUE(mob_index_fixture[0].func == fake_shop);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  mob_index_fixture[0].func = NULL;
}

/* H3-V01: a QM vnum with no prototype is reported and nothing is written. */
static void test_delete_last_quest_unknown_qm_reports(void)
{
  struct aq_data *table = calloc(1, sizeof(*table));

  TEST_ASSERT_NOT_NULL(table);
  table[0].vnum = 451;
  table[0].qm = 3005;
  aquest_table = table;
  total_quests = 1;

  TEST_ASSERT_TRUE(delete_quest(0));

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("delete-quest-qm", tb_captured_operation);
}

/* H3-07: a deleted reward object is reported to the log and the player. */
static void test_missing_reward_object_reports(void)
{
  add_quest_fixture(0, 100, AQ_ROOM_FIND, 3001, 1);
  quests[0].obj_reward = 4444;      /* no such prototype */
  set_quest(&player, 0);

  autoquest_trigger_check(&player, NULL, NULL, AQ_ROOM_FIND);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("reward-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "reward obj vnum 4444"));
  TEST_ASSERT_NOT_NULL(strstr(sent, "could not be found"));
}

/* H3-07: a quest count with no list is reported, not silently reset. */
static void test_completed_list_corrupt_reports(void)
{
  GET_NUM_QUESTS(&player) = 3;      /* but completed_quests == NULL */

  add_completed_quest(&player, 100);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("completed-list-corrupt", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(1, GET_NUM_QUESTS(&player));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_missing_reward_object_reports);
  RUN_TEST(test_completed_list_corrupt_reports);
  RUN_TEST(test_delete_last_quest_restores_qm_by_rnum);
  RUN_TEST(test_delete_last_quest_unknown_qm_reports);
  RUN_TEST(test_questmaster_finds_secondary_on_any_quest);
  /* parse_quest keeps its row index across calls: run it first, once. */
  RUN_TEST(test_parse_reports_unknown_qm_and_skipped_lines);
  RUN_TEST(test_invalid_type_reported_and_named_safely);
  RUN_TEST(test_quest_list_dangling_prev_reports);
  RUN_TEST(test_complete_dangling_next_reports);
  RUN_TEST(test_reward_equal_to_target_pays_once);
  RUN_TEST(test_mob_find_two_targets_completes_once);
  RUN_TEST(test_mob_find_does_not_count_against_chained_quest);
  RUN_TEST(test_complete_missing_quest_reports);
  return UNITY_END();
}
