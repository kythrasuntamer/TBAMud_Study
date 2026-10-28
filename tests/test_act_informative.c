/* Executable tests of the production informative commands (act.informative.c).
 * The file is linked with unreferenced sections discarded, so only the
 * fixtures below are needed. db.c is not linked: its globals are defined here.
 * Inputs are heap copies so ASan reports any read past their end.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "act.h"
#include "quest.h"

#include "tb_error_capture.h"

/* --- db.c globals ------------------------------------------------------- */
struct help_index_element *help_table = NULL;
int top_of_helpt = 0;

struct aq_data *aquest_table = NULL;
qst_rnum total_quests = 0;

struct player_special_data dummy_mob;
struct index_data *mob_index = NULL;
mob_rnum top_of_mobt = 0;

void basic_mud_log(const char *format, ...) { (void)format; }

/* --- Fixtures for functions outside act.informative.c ------------------- */
static struct time_info_data fixed_time;
struct time_info_data *age(struct char_data *ch) { (void)ch; return &fixed_time; }
struct time_info_data *real_time_passed(time_t t2, time_t t1)
{ (void)t2; (void)t1; return &fixed_time; }
int compute_armor_class(struct char_data *ch) { (void)ch; return 100; }
int level_exp(int chclass, int level) { (void)chclass; return level * 1000; }
bool affected_by_spell(struct char_data *ch, int spell) { (void)ch; (void)spell; return FALSE; }
int room_is_dark(room_rnum room) { (void)room; return 0; }
/* Same lookup as quest.c's real_quest(): linear search by vnum. */
qst_rnum real_quest(qst_vnum vnum)
{
  int rnum;

  for (rnum = 0; rnum < total_quests; rnum++)
    if (aquest_table[rnum].vnum == vnum)
      return rnum;
  return NOTHING;
}

/* Output of the character under test, accumulated by send_to_char(). */
static char sent[MAX_STRING_LENGTH * 2];
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(sent);
  int n;

  (void)ch;
  va_start(args, messg);
  n = vsnprintf(sent + used, sizeof(sent) - used, messg, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}

void setUp(void)
{
  sent[0] = '\0';
  tb_error_capture_reset();
}
void tearDown(void) {}

/* --- G2-01: search_help bounds ----------------------------------------- */

static struct help_index_element *make_help_table(int count, const char **keys)
{
  /* Exactly 'count' entries, as the boot loader allocates: no slack slot. */
  struct help_index_element *t = calloc((size_t)count, sizeof(*t));
  int i;

  TEST_ASSERT_NOT_NULL(t);
  for (i = 0; i < count; i++) {
    t[i].keywords = strdup(keys[i]);
    t[i].entry = strdup("entry");
  }
  return t;
}

static void free_help_fixture(void)
{
  int i;

  for (i = 0; i < top_of_helpt; i++) {
    free(help_table[i].keywords);
    free(help_table[i].entry);
  }
  free(help_table);
  help_table = NULL;
  top_of_helpt = 0;
}

/* A key that sorts after every keyword used to read help_table[top_of_helpt]. */
static void search_help_key_after_last_entry_misses(void)
{
  const char *keys[] = { "ALPHA", "BETA", "GAMMA" };
  char *key = strdup("ZZZZ");

  help_table = make_help_table(3, keys);
  top_of_helpt = 3;
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help(key, LVL_IMPL));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(key);
  free_help_fixture();
}

static void search_help_finds_first_and_last_entry(void)
{
  const char *keys[] = { "ALPHA", "BETA", "GAMMA" };

  help_table = make_help_table(3, keys);
  top_of_helpt = 3;
  TEST_ASSERT_EQUAL_INT(0, search_help("alp", 0));
  TEST_ASSERT_EQUAL_INT(2, search_help("gamma", 0));
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help("AAA", 0));
  free_help_fixture();
}

static void search_help_empty_table_misses(void)
{
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help("anything", 0));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void search_help_null_table_with_count_is_reported(void)
{
  top_of_helpt = 4;
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help("x", 0));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("search-index", tb_captured_operation);
  top_of_helpt = 0;
}

/* --- G2-02: score with a quest vnum that is not loaded ----------------- */

static struct char_data *make_player(long idnum)
{
  struct char_data *ch = calloc(1, sizeof(*ch));

  TEST_ASSERT_NOT_NULL(ch);
  ch->player_specials = calloc(1, sizeof(*ch->player_specials));
  TEST_ASSERT_NOT_NULL(ch->player_specials);
  ch->player.name = strdup("Tester");
  ch->player.time.logon = time(0);
  ch->player.time.birth = time(0);
  GET_IDNUM(ch) = idnum;
  GET_LEVEL(ch) = 1;
  GET_POS(ch) = POS_STANDING;
  IN_ROOM(ch) = NOWHERE;
  GET_QUEST(ch) = NOTHING;
  return ch;
}

static void free_player(struct char_data *ch)
{
  free(ch->player.name);
  free(ch->player_specials);
  free(ch);
}

static void score_with_unloaded_quest_is_reported(void)
{
  struct char_data *ch = make_player(42);
  struct aq_data quest;

  memset(&quest, 0, sizeof(quest));
  quest.vnum = 100;
  quest.name = (char *)"Loaded quest";
  aquest_table = &quest;
  total_quests = 1;
  GET_QUEST(ch) = 9999;

  do_score(ch, (char *)"", 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "an unknown quest"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("score-quest-lookup", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "quest vnum 9999"));

  sent[0] = '\0';
  tb_error_capture_reset();
  GET_QUEST(ch) = 100;
  do_score(ch, (char *)"", 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Loaded quest"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  aquest_table = NULL;
  total_quests = 0;
  free_player(ch);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(search_help_key_after_last_entry_misses);
  RUN_TEST(search_help_finds_first_and_last_entry);
  RUN_TEST(search_help_empty_table_misses);
  RUN_TEST(search_help_null_table_with_count_is_reported);
  RUN_TEST(score_with_unloaded_quest_is_reported);
  return UNITY_END();
}
