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
struct room_data *world = NULL;
room_rnum top_of_world = 0;
struct obj_data *object_list = NULL;
struct char_data *character_list = NULL;
struct index_data *obj_index = NULL;
obj_rnum top_of_objt = 0;
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
struct config_data config_info;
struct index_data **trig_index = NULL;
bool can_see_map(struct char_data *ch) { (void)ch; return FALSE; }
void str_and_map(char *str, struct char_data *ch, room_vnum target_room)
{ (void)str; (void)ch; (void)target_room; }
void sprintbitarray(int bitvector[], const char *names[], int maxar,
                    char *result, size_t reslen)
{ (void)bitvector; (void)names; (void)maxar; if (reslen) *result = '\0'; }
/* act() output is not under test; the formatter has its own tests. */
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func; (void)str;
  (void)hide_invisible; (void)ch; (void)obj; (void)vict_obj; (void)type;
  return NULL;
}
/* Paged output is captured like direct output. */
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)keep_internal;
  send_to_char(d ? d->character : NULL, "%s", str);
}
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

/* --- G2-03: objects and mobs with NULL display strings ----------------- */

/* One lit room (vnum 3001) the viewer stands in. */
static struct room_data test_room;
static struct descriptor_data viewer_desc;

static void enter_test_room(struct char_data *ch)
{
  memset(&test_room, 0, sizeof(test_room));
  test_room.number = 3001;
  test_room.name = (char *)"Test Room";
  test_room.description = (char *)"A plain room.\r\n";
  test_room.light = 1;
  world = &test_room;
  top_of_world = 0;
  memset(&viewer_desc, 0, sizeof(viewer_desc));
  ch->desc = &viewer_desc;
  IN_ROOM(ch) = 0;
  test_room.people = ch;
}

static void leave_test_room(struct char_data *ch)
{
  ch->desc = NULL;
  world = NULL;
}

static struct obj_data *make_room_obj(const char *name, const char *shortd,
                                      const char *longd)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));

  TEST_ASSERT_NOT_NULL(obj);
  obj->item_number = NOTHING;
  obj->name = name ? strdup(name) : NULL;
  obj->short_description = shortd ? strdup(shortd) : NULL;
  obj->description = longd ? strdup(longd) : NULL;
  IN_ROOM(obj) = 0;
  obj->next_content = test_room.contents;
  test_room.contents = obj;
  return obj;
}

static void free_room_objs(void)
{
  struct obj_data *obj, *next;

  for (obj = test_room.contents; obj; obj = next) {
    next = obj->next_content;
    free(obj->name);
    free(obj->short_description);
    free(obj->description);
    free(obj);
  }
  test_room.contents = NULL;
}

static void look_skips_object_without_long_description(void)
{
  struct char_data *ch = make_player(7);

  enter_test_room(ch);
  make_room_obj("sword", "a sword", "A sword lies here.");
  make_room_obj("ghost", "a ghost item", NULL);
  look_at_room(ch, TRUE);
  TEST_ASSERT_NOT_NULL(strstr(sent, "A sword lies here."));
  TEST_ASSERT_NULL(strstr(sent, "ghost"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("obj-string-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3001"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "description NULL"));
  free_room_objs();
  leave_test_room(ch);
  free_player(ch);
}

/* The grouping loop strcmp()s every earlier object against the current one. */
static void look_groups_past_object_without_short_description(void)
{
  struct char_data *ch = make_player(7);

  enter_test_room(ch);
  make_room_obj("coin", "a coin", "A coin lies here.");
  make_room_obj("coin", NULL, "A broken coin lies here.");
  make_room_obj("coin", "a coin", "A coin lies here.");
  look_at_room(ch, TRUE);
  TEST_ASSERT_NOT_NULL(strstr(sent, "( 2) "));
  TEST_ASSERT_NULL(strstr(sent, "broken"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("obj-string-missing", tb_captured_operation);
  free_room_objs();
  leave_test_room(ch);
  free_player(ch);
}

static void look_skips_mob_without_short_description(void)
{
  struct char_data *ch = make_player(7);
  struct char_data *mob = make_player(0);
  struct index_data mob_idx;

  memset(&mob_idx, 0, sizeof(mob_idx));
  mob_idx.vnum = 3010;
  mob_index = &mob_idx;
  top_of_mobt = 0;
  enter_test_room(ch);
  SET_BIT_AR(MOB_FLAGS(mob), MOB_ISNPC);
  mob->nr = 0;
  free(mob->player_specials);
  mob->player_specials = &dummy_mob;
  IN_ROOM(mob) = 0;
  ch->next_in_room = mob;
  look_at_room(ch, TRUE);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("mob-string-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3010"));
  ch->next_in_room = NULL;
  mob->player_specials = NULL;
  free(mob->player.name);
  free(mob);
  mob_index = NULL;
  leave_test_room(ch);
  free_player(ch);
}

/* --- G2-04: a mob position outside positions[] ------------------------- */

static void look_reports_mob_position_out_of_range(void)
{
  struct char_data *ch = make_player(7);
  struct char_data *mob = make_player(0);
  struct index_data mob_idx;

  memset(&mob_idx, 0, sizeof(mob_idx));
  mob_idx.vnum = 3011;
  mob_index = &mob_idx;
  enter_test_room(ch);
  SET_BIT_AR(MOB_FLAGS(mob), MOB_ISNPC);
  mob->nr = 0;
  free(mob->player_specials);
  mob->player_specials = &dummy_mob;
  mob->player.short_descr = (char *)"a confused guard";
  GET_POS(mob) = 20;          /* as stored by parse_simple_mob from the file */
  GET_DEFAULT_POS(mob) = 20;  /* long_descr is NULL, so the position line runs */
  IN_ROOM(mob) = 0;
  ch->next_in_room = mob;
  look_at_room(ch, TRUE);
  TEST_ASSERT_NOT_NULL(strstr(sent, "A confused guard is here."));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("position-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3011"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "position 20"));
  ch->next_in_room = NULL;
  mob->player_specials = NULL;
  free(mob->player.name);
  free(mob);
  mob_index = NULL;
  leave_test_room(ch);
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
  RUN_TEST(look_skips_object_without_long_description);
  RUN_TEST(look_groups_past_object_without_short_description);
  RUN_TEST(look_skips_mob_without_short_description);
  RUN_TEST(look_reports_mob_position_out_of_range);
  return UNITY_END();
}
