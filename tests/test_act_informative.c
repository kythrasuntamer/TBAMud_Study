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
#include "class.h"
#include "handler.h"
#include "dg_scripts.h"

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
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
FILE *logfile = NULL;
struct weather_data weather_info;
struct descriptor_data *descriptor_list = NULL;
/* Text files paged by do_gen_ps (loaded by db.c at boot). */
char *credits, *news, *motd, *imotd, *help, *info, *wizlist, *immlist,
     *background, *handbook, *policies, *ihelp;


/* --- Fixtures for functions outside act.informative.c ------------------- */
int compute_armor_class(struct char_data *ch) { (void)ch; return 100; }
int level_exp(int chclass, int level) { (void)chclass; return level * 1000; }
bool affected_by_spell(struct char_data *ch, int spell) { (void)ch; (void)spell; return FALSE; }
struct config_data config_info;
struct index_data **trig_index = NULL;
bool can_see_map(struct char_data *ch) { (void)ch; return FALSE; }
void str_and_map(char *str, struct char_data *ch, room_vnum target_room)
{ (void)str; (void)ch; (void)target_room; }
/* Commands under test are called with at most one short word. */
char *one_argument(char *argument, char *first_arg)
{
  size_t n = 0;

  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ' && n + 1 < MAX_INPUT_LENGTH)
    first_arg[n++] = *argument++;
  first_arg[n] = '\0';
  return argument;
}
void skip_spaces(char **string)
{
  while (**string == ' ')
    (*string)++;
}
void half_chop(char *string, char *arg1, char *arg2)
{
  char *rest = one_argument(string, arg1);

  while (*rest == ' ')
    rest++;
  strlcpy(arg2, rest, MAX_INPUT_LENGTH);
}
/* Every class letter selects every class. */
bitvector_t find_class_bitvector(const char *arg)
{ (void)arg; return (1 << NUM_CLASSES) - 1; }
/* The real class table (class.c is not linked). */
const char *class_abbrevs[] = { "Wi", "Cl", "Th", "Wa", "Mo", "\n" };
struct happyhour happy_data;
/* Name match for 'where' (handler.c is not linked): substring is enough. */
int isname(const char *str, const char *namelist)
{ return namelist && strstr(namelist, str) != NULL; }
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
  ch->player.title = (char *)"";
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

/* --- G2-05: a room sector outside sector_types[] ---------------------- */

static void look_reports_room_sector_out_of_range(void)
{
  struct char_data *ch = make_player(7);

  enter_test_room(ch);
  GET_LEVEL(ch) = LVL_IMPL;
  SET_BIT_AR(PRF_FLAGS(ch), PRF_SHOWVNUMS);
  test_room.sector_type = -5;   /* parse_room let negative sectors through */
  look_at_room(ch, TRUE);
  TEST_ASSERT_NOT_NULL(strstr(sent, "[ <invalid> ]"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("sector-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3001 has sector -5"));

  sent[0] = '\0';
  tb_error_capture_reset();
  test_room.sector_type = NUM_ROOM_SECTORS;
  look_at_room(ch, TRUE);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);

  sent[0] = '\0';
  tb_error_capture_reset();
  test_room.sector_type = SECT_FIELD;
  look_at_room(ch, TRUE);
  TEST_ASSERT_NOT_NULL(strstr(sent, "[ Field ]"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  leave_test_room(ch);
  free_player(ch);
}

/* --- G2-06: 'areas' with more zone text than its buffer ---------------- */

static void areas_truncates_long_listing_and_reports(void)
{
  struct char_data *ch = make_player(7);
  struct descriptor_data d;
  int nzones = 400, i;
  char name[251];

  memset(&d, 0, sizeof(d));
  ch->desc = &d;
  d.character = ch;
  memset(name, 'z', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  zone_table = calloc((size_t)nzones, sizeof(*zone_table));
  TEST_ASSERT_NOT_NULL(zone_table);
  for (i = 0; i < nzones; i++) {
    zone_table[i].name = strdup(name);
    zone_table[i].min_level = -1;
    zone_table[i].max_level = -1;
    SET_BIT_AR(zone_table[i].zone_flags, ZONE_GRID);
  }
  top_of_zone_table = nzones - 1;

  do_areas(ch, (char *)"", 0, 0);   /* stack-buffer-overflow before the fix */
  TEST_ASSERT_NOT_NULL(strstr(sent, "***OVERFLOW***"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("areas-truncated", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "400 of 400 zones"));

  /* A listing that fits is not marked or reported. */
  sent[0] = '\0';
  tb_error_capture_reset();
  top_of_zone_table = 2;
  do_areas(ch, (char *)"", 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "3 areas found."));
  TEST_ASSERT_NULL(strstr(sent, "OVERFLOW"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  for (i = 0; i < nzones; i++)
    free(zone_table[i].name);
  free(zone_table);
  zone_table = NULL;
  top_of_zone_table = 0;
  ch->desc = NULL;
  free_player(ch);
}

/* --- G2-12: a player class outside class_abbrevs[] -------------------- */

static void who_reports_class_out_of_range(void)
{
  struct char_data *ch = make_player(7);
  struct char_data *odd = make_player(8);
  struct descriptor_data odd_desc;

  enter_test_room(ch);
  viewer_desc.character = ch;
  STATE(&viewer_desc) = CON_PLAYING;
  memset(&odd_desc, 0, sizeof(odd_desc));
  odd_desc.character = odd;
  STATE(&odd_desc) = CON_PLAYING;
  odd->desc = &odd_desc;
  IN_ROOM(odd) = 0;
  ch->next_in_room = odd;
  free(odd->player.name);
  odd->player.name = strdup("Victim");
  GET_CLASS(odd) = 40;          /* corrupt 'Clas' in the player file */
  descriptor_list = &odd_desc;

  do_who(ch, (char *)"", 0, 0);
  /* class_abbrevs[40] read an unrelated global before the fix. */
  TEST_ASSERT_NOT_NULL(strstr(sent, "[ 1 ??] Victim"));
  TEST_ASSERT_TRUE(tb_captured_errors >= 1);
  TEST_ASSERT_EQUAL_STRING("class-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 8 has class 40"));

  /* 'who -c' shifted 1 << 40 (UBSan) before the fix; it now matches nothing. */
  sent[0] = '\0';
  tb_error_capture_reset();
  do_who(ch, (char *)"-c mctwk", 0, 0);
  TEST_ASSERT_NULL(strstr(sent, "Victim"));
  TEST_ASSERT_TRUE(tb_captured_errors >= 1);
  TEST_ASSERT_EQUAL_STRING("class-range", tb_captured_operation);

  descriptor_list = NULL;
  ch->next_in_room = NULL;
  odd->desc = NULL;
  leave_test_room(ch);
  free_player(odd);
  free_player(ch);
}

/* --- G2-15: immortal 'where' with more matches than its buffer ------- */

static void where_marks_and_reports_full_listing(void)
{
  struct char_data *ch = make_player(7);
  struct char_data *mobs;
  char room_name[201];
  int nmobs = 400, i;

  enter_test_room(ch);
  viewer_desc.character = ch;
  GET_LEVEL(ch) = LVL_IMMORT;
  SET_BIT_AR(PRF_FLAGS(ch), PRF_HOLYLIGHT);
  memset(room_name, 'r', sizeof(room_name) - 1);
  room_name[sizeof(room_name) - 1] = '\0';
  test_room.name = room_name;
  mobs = calloc((size_t)nmobs, sizeof(*mobs));
  TEST_ASSERT_NOT_NULL(mobs);
  for (i = 0; i < nmobs; i++) {
    SET_BIT_AR(MOB_FLAGS(&mobs[i]), MOB_ISNPC);
    mobs[i].player.name = (char *)"guard";
    mobs[i].player.short_descr = (char *)"the city guard";
    mobs[i].player_specials = &dummy_mob;
    mobs[i].nr = NOBODY;
    IN_ROOM(&mobs[i]) = 0;
    mobs[i].next = i + 1 < nmobs ? &mobs[i + 1] : NULL;
  }
  character_list = mobs;

  do_where(ch, (char *)"guard", 0, 0);
  /* The marker used to land after the truncation NUL (or be cut short). */
  TEST_ASSERT_NOT_NULL(strstr(sent, "***OVERFLOW***"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("where-truncated", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "where 'guard'"));

  /* A listing that fits is neither marked nor reported. */
  sent[0] = '\0';
  tb_error_capture_reset();
  mobs[2].next = NULL;
  do_where(ch, (char *)"guard", 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "M   3. the city guard"));
  TEST_ASSERT_NULL(strstr(sent, "OVERFLOW"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  character_list = NULL;
  free(mobs);
  leave_test_room(ch);
  free_player(ch);
}

/* --- G2-18: a drink container holding more than its capacity -------- */

/* do_look's word parsing (interpreter.c and handler.c are not linked). */
int is_abbrev(const char *arg1, const char *arg2)
{
  if (!*arg1)
    return 0;
  for (; *arg1 && *arg2; arg1++, arg2++)
    if (LOWER(*arg1) != LOWER(*arg2))
      return 0;
  return !*arg1;
}
int search_block(char *arg, const char **list, int exact)
{ (void)arg; (void)list; (void)exact; return -1; }   /* no direction words */
int get_number(char **name) { (void)name; return 1; }
char *fname(const char *namelist)
{
  static char holder[MAX_INPUT_LENGTH];

  strlcpy(holder, namelist, sizeof(holder));
  return holder;
}

static struct obj_data *found_obj;
int generic_find(char *arg, bitvector_t bitvector, struct char_data *ch,
                 struct char_data **tar_ch, struct obj_data **tar_obj)
{
  (void)arg; (void)bitvector; (void)ch;
  *tar_ch = NULL;
  *tar_obj = found_obj;
  return found_obj ? FIND_OBJ_INV : 0;
}

static void look_in_overfull_drinkcon_is_reported(void)
{
  struct char_data *ch = make_player(7);
  struct obj_data flask;

  enter_test_room(ch);
  memset(&flask, 0, sizeof(flask));
  flask.item_number = NOTHING;
  flask.name = (char *)"flask";
  GET_OBJ_TYPE(&flask) = ITEM_DRINKCON;
  GET_OBJ_VAL(&flask, 0) = 5;     /* capacity */
  GET_OBJ_VAL(&flask, 1) = 9;     /* amount: corrupt */
  found_obj = &flask;

  do_look(ch, (char *)"in flask", 0, SCMD_LOOK);
  TEST_ASSERT_NOT_NULL(strstr(sent, "somewhat murky"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("drinkcon-invariant", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "amount 9 outside 0..capacity 5"));

  /* A consistent container is described without a report. */
  sent[0] = '\0';
  tb_error_capture_reset();
  GET_OBJ_VAL(&flask, 1) = 5;
  do_look(ch, (char *)"in flask", 0, SCMD_LOOK);
  TEST_ASSERT_NOT_NULL(strstr(sent, "full of a"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  found_obj = NULL;
  leave_test_room(ch);
  free_player(ch);
}

/* --- G2-19: an unhandled subcommand names the command and player ----- */

static void gen_ps_unknown_subcmd_is_reported(void)
{
  struct char_data *ch = make_player(31);

  do_gen_ps(ch, (char *)"", 123, 999);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("bad-subcmd", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "player idnum 31 do_gen_ps cmd 123 subcmd 999"));
  free_player(ch);
}

/* --- G2-14: add_history with act()'s NULL result ---------------------- */

static void history_null_text_is_reported_not_stored(void)
{
  struct char_data *ch = make_player(9);
  struct txt_block *h;

  add_history(ch, NULL, HIST_SAY);
  TEST_ASSERT_NULL(GET_HISTORY(ch, HIST_SAY));
  TEST_ASSERT_NULL(GET_HISTORY(ch, HIST_ALL));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("history-null", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "history type 1"));

  tb_error_capture_reset();
  add_history(ch, (char *)"Bob says, 'hi'", HIST_SAY);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(GET_HISTORY(ch, HIST_SAY));
  TEST_ASSERT_NOT_NULL(strstr(GET_HISTORY(ch, HIST_SAY)->text, "Bob says"));
  TEST_ASSERT_NOT_NULL(GET_HISTORY(ch, HIST_ALL));
  for (h = GET_HISTORY(ch, HIST_SAY); h; h = GET_HISTORY(ch, HIST_SAY)) {
    GET_HISTORY(ch, HIST_SAY) = h->next;
    free(h->text);
    free(h);
  }
  for (h = GET_HISTORY(ch, HIST_ALL); h; h = GET_HISTORY(ch, HIST_ALL)) {
    GET_HISTORY(ch, HIST_ALL) = h->next;
    free(h->text);
    free(h);
  }
  free_player(ch);
}

/* --- G2-17: look_at_room for a viewer outside a valid room ------------ */

/* look_at_room() indexed world[IN_ROOM(ch)] with no check, so a caller
 * that passed an unplaced viewer read past world[] (ASan global overflow
 * here) and nothing named the caller. */
static void look_at_unplaced_viewer_is_reported_at_caller(void)
{
  struct char_data *ch = make_player(8);
  int call_line;

  enter_test_room(ch);
  IN_ROOM(ch) = 1;                      /* one past top_of_world */
  call_line = __LINE__ + 1;
  look_at_room(ch, TRUE);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("look-unplaced", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(call_line, tb_captured_line);   /* the caller */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 8"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room rnum 1,"));
  TEST_ASSERT_EQUAL_STRING("", sent);

  tb_error_capture_reset();
  IN_ROOM(ch) = NOWHERE;
  look_at_room(ch, TRUE);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("look-unplaced", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("", sent);

  /* A placed viewer still sees the room, with no report. */
  tb_error_capture_reset();
  IN_ROOM(ch) = 0;
  look_at_room(ch, TRUE);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Test Room"));

  test_room.people = NULL;
  leave_test_room(ch);
  free_player(ch);
}

/* --- G2-16: an object script without triggers ------------------------- */

/* The showvnums and 'where' object markers tested only SCRIPT(obj) before
 * reading TRIGGERS(SCRIPT(obj))->next (the character markers also tested
 * TRIGGERS). An object's script always holds a trigger, so a trigger-less
 * one is reported, and no marker is shown. */
static void object_script_without_triggers_is_reported(void)
{
  struct char_data *ch = make_player(10);
  struct script_data sc;
  struct obj_data *obj;

  enter_test_room(ch);
  viewer_desc.character = ch;
  GET_LEVEL(ch) = LVL_IMMORT;
  SET_BIT_AR(PRF_FLAGS(ch), PRF_SHOWVNUMS);
  SET_BIT_AR(PRF_FLAGS(ch), PRF_HOLYLIGHT);
  obj = make_room_obj("orb", "an orb", "An orb hums here.");
  memset(&sc, 0, sizeof(sc));
  SCRIPT(obj) = &sc;                    /* no triggers */
  obj->script_id = 4242;

  look_at_room(ch, TRUE);               /* show_obj_to_char() */
  TEST_ASSERT_NOT_NULL(strstr(sent, "An orb hums here."));
  TEST_ASSERT_NULL(strstr(sent, "[T"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("script-empty", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "uid 4242"));

  sent[0] = '\0';
  tb_error_capture_reset();
  object_list = obj;
  do_where(ch, (char *)"orb", 0, 0);    /* print_object_location() */
  TEST_ASSERT_NOT_NULL(strstr(sent, "an orb"));
  TEST_ASSERT_NULL(strstr(sent, "[T"));
  TEST_ASSERT_EQUAL_STRING("script-empty", tb_captured_operation);

  object_list = NULL;
  SCRIPT(obj) = NULL;
  free_room_objs();
  test_room.people = NULL;
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
  RUN_TEST(look_reports_room_sector_out_of_range);
  RUN_TEST(areas_truncates_long_listing_and_reports);
  RUN_TEST(who_reports_class_out_of_range);
  RUN_TEST(where_marks_and_reports_full_listing);
  RUN_TEST(look_in_overfull_drinkcon_is_reported);
  RUN_TEST(gen_ps_unknown_subcmd_is_reported);
  RUN_TEST(history_null_text_is_reported_not_stored);
  RUN_TEST(look_at_unplaced_viewer_is_reported_at_caller);
  RUN_TEST(object_script_without_triggers_is_reported);
  return UNITY_END();
}
