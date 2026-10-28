/* Executable tests of production immortal commands (act.wizard.c).
 *
 * act.wizard.c is linked with interpreter.c, utils.c and constants.c so the
 * argument splitters and lookup tables are the real ones. Everything else the
 * commands reach (world handlers, persistence, OLC, scripts) is a stub below.
 * Stubs that a test does not expect to reach fail the test; the rest record
 * the call so a test can assert on side effects such as "the pfile was not
 * rewritten" or "the character was taken out of the room before free".
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
#include "act.h"
#include "fight.h"
#include "spells.h"
#include "class.h"
#include "house.h"
#include "dg_scripts.h"
#include "quest.h"
#include "genzon.h"
#include "constants.h"
#include "modify.h"
#include "protocol.h"
#include <stdarg.h>

/* ---------------------------------------------------------------------
 * Structured diagnostics. utils.c owns tb_error_log_at(); the link wraps it
 * (--wrap) so tests can assert which guard fired.
 * --------------------------------------------------------------------- */
static int captured_errors;
static char captured_operation[64];
static char captured_detail[512];
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  va_list args;
  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func;
  captured_errors++;
  strlcpy(captured_operation, operation ? operation : "",
          sizeof(captured_operation));
  va_start(args, detail_fmt);
  if (vsnprintf(captured_detail, sizeof(captured_detail), detail_fmt, args) < 0)
    captured_detail[0] = '\0';
  va_end(args);
}

/* Everything sent to any character, concatenated. */
static char sent[MAX_STRING_LENGTH * 4];
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t len = strlen(sent);
  (void)ch;
  va_start(args, messg);
  if (len < sizeof(sent) &&
      vsnprintf(sent + len, sizeof(sent) - len, messg, args) < 0)
    sent[len] = '\0';
  va_end(args);
  return strlen(sent) - len;
}
/* page_string() is how the long listings (show, zcheck) reach the player. */
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  strlcat(sent, str, sizeof(sent));
}

#define UNREACHED() TEST_FAIL_MESSAGE(__func__)

/* ---------------------------------------------------------------------
 * World and player handlers: record the calls the tests assert on.
 * --------------------------------------------------------------------- */
static int char_to_room_calls, char_from_room_calls, free_char_calls;
static int save_char_calls, extract_final_calls, run_autowiz_calls;
static int load_char_result = -1;
static int load_char_level = 1;
static long load_char_idnum = 77;
static struct char_data *in_game_char;   /* returned by player lookups */

void char_to_room(struct char_data *ch, room_rnum room)
{
  char_to_room_calls++;
  IN_ROOM(ch) = room;
}
void char_from_room(struct char_data *ch)
{
  char_from_room_calls++;
  IN_ROOM(ch) = NOWHERE;
}
void free_char(struct char_data *ch)
{
  free_char_calls++;
  /* free_char() never unlinks from world[].people (db.c); a freed char still
   * in a room is the use-after-free G1-01 fixed. */
  TEST_ASSERT_EQUAL_INT_MESSAGE(NOWHERE, IN_ROOM(ch),
                                "free_char on a character still in a room");
  free(ch->player_specials);
  free(ch);
}
void extract_char_final(struct char_data *ch)
{
  (void)ch;
  extract_final_calls++;
  UNREACHED();
}
void save_char(struct char_data *ch) { (void)ch; save_char_calls++; }
void clear_char(struct char_data *ch)
{
  memset(ch, 0, sizeof(*ch));
  IN_ROOM(ch) = NOWHERE;
}
void new_mobile_data(struct char_data *ch) { (void)ch; }
int load_char(const char *name, struct char_data *ch)
{
  (void)name;
  if (load_char_result < 0)
    return load_char_result;
  GET_LEVEL(ch) = load_char_level;
  GET_IDNUM(ch) = load_char_idnum;
  return load_char_result;
}
room_rnum real_room(room_vnum vnum)
{
  room_rnum i;
  for (i = 0; i <= top_of_world; i++)
    if (world[i].number == vnum)
      return i;
  return NOWHERE;
}
struct char_data *get_player_vis(struct char_data *ch, char *name,
                                 int *number, int inroom)
{ (void)ch; (void)name; (void)number; (void)inroom; return in_game_char; }
struct char_data *get_char_vis(struct char_data *ch, char *name,
                               int *number, int where)
{ (void)ch; (void)name; (void)number; (void)where; return in_game_char; }
void run_autowiz(void) { run_autowiz_calls++; }

/* ---------------------------------------------------------------------
 * Lookup tables owned by files this fixture does not link. Sized exactly as
 * in production so an out-of-range index is an ASan global overflow.
 * --------------------------------------------------------------------- */
struct attack_hit_type attack_hit_text[NUM_ATTACK_TYPES] = {
  {"hit", "hits"}, {"sting", "stings"}, {"whip", "whips"},
  {"slash", "slashes"}, {"bite", "bites"}, {"bludgeon", "bludgeons"},
  {"crush", "crushes"}, {"pound", "pounds"}, {"claw", "claws"},
  {"maul", "mauls"}, {"thrash", "thrashes"}, {"pierce", "pierces"},
  {"blast", "blasts"}, {"punch", "punches"}, {"stab", "stabs"}
};
const char *class_abbrevs[] = { "Mu", "Cl", "Th", "Wa", "Mo", "\n" };

/* ---------------------------------------------------------------------
 * Globals owned by unlinked files.
 * --------------------------------------------------------------------- */
time_t boot_time;
int buf_largecount, buf_overflows, buf_switches;
int circle_reboot, circle_shutdown;
const char *nrm = "", *grn = "", *cyn = "", *yel = "";
struct list_data *global_lists;
struct char_data *mob_proto;
socket_t mother_desc;
struct obj_data *obj_proto;
struct obj_data *object_list;
ush_int port;
struct shop_data *shop_index;
int top_of_p_table;
int top_of_trigt;
zone_rnum top_of_zone_table;
int top_shop;
qst_rnum total_quests;
struct index_data **trig_index;
struct zone_data *zone_table;

/* ---------------------------------------------------------------------
 * Functions the tests never expect to reach.
 * --------------------------------------------------------------------- */
const char *ColourRGB(descriptor_t *d, const char *rgb) { (void)d; (void)rgb; return ""; }
const char *CopyoverGet(descriptor_t *d) { (void)d; UNREACHED(); return ""; }
void Crash_listrent(struct char_data *ch, char *name) { (void)ch; (void)name; UNREACHED(); }
void Crash_rentsave(struct char_data *ch, int cost) { (void)ch; (void)cost; UNREACHED(); }
int House_can_enter(struct char_data *ch, room_vnum house) { (void)ch; (void)house; return 1; }
void House_save_all(void) { UNREACHED(); }
void add_completed_quest(struct char_data *ch, qst_vnum vnum) { (void)ch; (void)vnum; UNREACHED(); }
void add_history(struct char_data *ch, char *msg, int type) { (void)ch; (void)msg; (void)type; }
void affect_remove(struct char_data *ch, struct affected_type *af) { (void)ch; (void)af; UNREACHED(); }
void affect_total(struct char_data *ch) { (void)ch; }
void appear(struct char_data *ch) { (void)ch; UNREACHED(); }
int can_edit_zone(struct char_data *ch, zone_rnum rnum) { (void)ch; (void)rnum; return 1; }
void clearMemory(struct char_data *ch) { (void)ch; UNREACHED(); }
void close_socket(struct descriptor_data *d) { (void)d; UNREACHED(); }
int count_quests(qst_vnum low, qst_vnum high) { (void)low; (void)high; return 0; }
const char *dg_display_value(const char *value, char *buf, size_t size)
{ strlcpy(buf, value, size); return buf; }
void do_sstat_character(char_data *ch, char_data *k) { (void)ch; (void)k; }
void do_sstat_object(char_data *ch, obj_data *j) { (void)ch; (void)j; }
void do_sstat_room(struct char_data *ch, room_data *r) { (void)ch; (void)r; }
int enter_wtrigger(room_data *room, char_data *actor, int dir)
{ (void)room; (void)actor; (void)dir; return 1; }
struct char_data *find_char(long n) { (void)n; return NULL; }
void find_uid_name(char *uid, char *name, size_t nlen) { (void)uid; strlcpy(name, "", nlen); }
void get_char_colors(struct char_data *ch) { (void)ch; }
int get_number(char **name) { (void)name; return 1; }
struct obj_data *get_obj_in_equip_vis(struct char_data *ch, char *arg, int *number,
                                      struct obj_data *equipment[])
{ (void)ch; (void)arg; (void)number; (void)equipment; return NULL; }
struct obj_data *get_obj_in_list_vis(struct char_data *ch, char *name, int *number,
                                     struct obj_data *list)
{ (void)ch; (void)name; (void)number; (void)list; return NULL; }
static struct obj_data *vis_obj;   /* returned by get_obj_vis() */
struct obj_data *get_obj_vis(struct char_data *ch, char *name, int *num)
{ (void)ch; (void)name; (void)num; return vis_obj; }
const char *get_spec_func_name(SPECIAL(*func)) { (void)func; return "None"; }
void hcontrol_list_houses(struct char_data *ch, char *arg) { (void)ch; (void)arg; UNREACHED(); }
int is_complete(struct char_data *ch, qst_vnum vnum) { (void)ch; (void)vnum; return 0; }
int is_name(const char *str, const char *namelist) { (void)str; (void)namelist; return 0; }
int level_exp(int chclass, int level) { (void)chclass; (void)level; return 0; }
void load_mtrigger(char_data *ch) { (void)ch; UNREACHED(); }
void load_otrigger(obj_data *obj) { (void)obj; UNREACHED(); }
void extract_obj(struct obj_data *obj) { (void)obj; UNREACHED(); }
void gain_exp_regardless(struct char_data *ch, int gain) { (void)ch; (void)gain; UNREACHED(); }
int hit_gain(struct char_data *ch) { (void)ch; return 0; }
int mana_gain(struct char_data *ch) { (void)ch; return 0; }
int move_gain(struct char_data *ch) { (void)ch; return 0; }
void obj_to_char(struct obj_data *object, struct char_data *ch) { (void)object; (void)ch; UNREACHED(); }
void obj_to_room(struct obj_data *object, room_rnum room) { (void)object; (void)room; UNREACHED(); }
bool oset_alias(struct obj_data *obj, char *argument) { (void)obj; (void)argument; UNREACHED(); return FALSE; }
bool oset_apply(struct obj_data *obj, char *argument) { (void)obj; (void)argument; UNREACHED(); return FALSE; }
bool oset_long_description(struct obj_data *obj, char *argument)
{ (void)obj; (void)argument; UNREACHED(); return FALSE; }
bool oset_short_description(struct obj_data *obj, char *argument)
{ (void)obj; (void)argument; UNREACHED(); return FALSE; }
void parse_at(char *str) { (void)str; }
int perform_set_dg_var(struct char_data *ch, struct char_data *vict, char *val_arg)
{ (void)ch; (void)vict; (void)val_arg; UNREACHED(); return 0; }
void print_zone(struct char_data *ch, zone_rnum rnum) { (void)ch; (void)rnum; }
struct char_data *read_mobile(mob_vnum nr, int type) { (void)nr; (void)type; UNREACHED(); return NULL; }
struct obj_data *read_object(obj_vnum nr, int type) { (void)nr; (void)type; UNREACHED(); return NULL; }
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; return NOBODY; }
obj_rnum real_object(obj_vnum vnum) { (void)vnum; return NOTHING; }
qst_rnum real_quest(qst_vnum vnum) { (void)vnum; return NOTHING; }
trig_rnum real_trigger(trig_vnum vnum) { (void)vnum; return NOTHING; }
zone_rnum real_zone(zone_vnum vnum)
{
  zone_rnum i;
  for (i = 0; i <= top_of_zone_table; i++)
    if (zone_table[i].number == vnum)
      return i;
  return NOWHERE;
}
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return NOWHERE; }
void remove_completed_quest(struct char_data *ch, qst_vnum vnum) { (void)ch; (void)vnum; UNREACHED(); }
void reset_zone(zone_rnum zone) { (void)zone; UNREACHED(); }
void roll_real_abils(struct char_data *ch) { (void)ch; UNREACHED(); }
int save_all(void) { UNREACHED(); return 0; }
int save_zone(zone_rnum zone_num) { (void)zone_num; UNREACHED(); return 0; }
void send_to_all(const char *messg, ...) { (void)messg; }
void send_to_room(room_rnum room, const char *messg, ...) { (void)room; (void)messg; }
void show_shops(struct char_data *ch, char *arg) { (void)ch; (void)arg; UNREACHED(); }
const char *skill_name(int num) { (void)num; return "skill"; }
void stop_fighting(struct char_data *ch) { (void)ch; }
int thaco(int class_num, int level) { (void)class_num; (void)level; return 20; }
void update_pos(struct char_data *victim) { (void)victim; }
int vnum_mobile(char *searchname, struct char_data *ch) { (void)searchname; (void)ch; return 0; }
int vnum_object(char *searchname, struct char_data *ch) { (void)searchname; (void)ch; return 0; }
int vnum_room(char *s, struct char_data *ch) { (void)s; (void)ch; return 0; }
int vnum_trig(char *s, struct char_data *ch) { (void)s; (void)ch; return 0; }
int write_to_descriptor(socket_t desc, const char *txt) { (void)desc; (void)txt; return 0; }
long char_script_id(char_data *ch) { (void)ch; return 0; }
long obj_script_id(obj_data *obj) { (void)obj; return 0; }

/* ---------------------------------------------------------------------
 * Fixtures: a two-room world and an implementor with a descriptor.
 * --------------------------------------------------------------------- */
static struct room_data rooms[2];
static struct zone_data zones[1];
static struct char_data imm;
static struct player_special_data imm_specials;
static struct descriptor_data imm_desc;

static void reset_counters(void)
{
  captured_errors = 0;
  captured_operation[0] = captured_detail[0] = '\0';
  sent[0] = '\0';
  char_to_room_calls = char_from_room_calls = free_char_calls = 0;
  save_char_calls = extract_final_calls = run_autowiz_calls = 0;
}

void setUp(void)
{
  memset(rooms, 0, sizeof(rooms));
  memset(zones, 0, sizeof(zones));
  rooms[0].number = 3000;
  rooms[1].number = 3014;
  world = rooms;
  top_of_world = 1;
  zones[0].number = 30;
  zones[0].bot = 3000;
  zones[0].top = 3099;
  zone_table = zones;
  top_of_zone_table = 0;

  memset(&imm, 0, sizeof(imm));
  memset(&imm_specials, 0, sizeof(imm_specials));
  memset(&imm_desc, 0, sizeof(imm_desc));
  imm.player_specials = &imm_specials;
  imm.player.name = (char *)"Implo";
  GET_LEVEL(&imm) = LVL_IMPL;
  GET_IDNUM(&imm) = 1;
  IN_ROOM(&imm) = 0;
  imm.desc = &imm_desc;
  imm_desc.character = &imm;

  in_game_char = NULL;
  vis_obj = NULL;
  load_char_result = -1;
  load_char_level = 1;
  load_char_idnum = 77;
  reset_counters();
}
void tearDown(void) {}

/* Run a command with a writable copy of its argument, as the interpreter
 * does. */
#define RUN_CMD(fn, who, text) do { \
  char cmd_arg_[MAX_INPUT_LENGTH]; \
  strlcpy(cmd_arg_, (text), sizeof(cmd_arg_)); \
  fn((who), cmd_arg_, 0, 0); \
} while (0)

/* G1-01: "set file <name> room <vnum>" used to char_to_room() the temporary
 * file copy and then free it, leaving a freed node in world[].people. */
static void test_set_file_room_is_refused_and_reported(void)
{
  load_char_result = 5;
  RUN_CMD(do_set, &imm, "file bobby room 3014");
  TEST_ASSERT_EQUAL_INT(0, char_to_room_calls);
  TEST_ASSERT_EQUAL_INT(0, save_char_calls);
  TEST_ASSERT_EQUAL_INT(1, free_char_calls);
  TEST_ASSERT_EQUAL_STRING("set-file-room", captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(captured_detail, "idnum 77"));
  TEST_ASSERT_NULL(rooms[1].people);
}

/* G1-04: "stat file" released its temp copy with extract_char_final(),
 * which saves the stale copy and deletes the player's crash file. The
 * extract_char_final() stub fails the test if it is reached. */
static void test_stat_file_frees_copy_without_extracting(void)
{
  load_char_result = 0;
  RUN_CMD(do_stat, &imm, "file bobby");
  TEST_ASSERT_EQUAL_INT(0, extract_final_calls);
  TEST_ASSERT_EQUAL_INT(0, save_char_calls);
  TEST_ASSERT_EQUAL_INT(0, char_to_room_calls);
  TEST_ASSERT_EQUAL_INT(1, free_char_calls);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

/* G1-V01: the level refusal must come before any side effect, and be
 * reported. */
static void test_stat_file_outranked_is_denied_before_side_effects(void)
{
  GET_LEVEL(&imm) = LVL_IMMORT;
  load_char_result = 0;
  load_char_level = LVL_IMPL;
  RUN_CMD(do_stat, &imm, "file bobby");
  TEST_ASSERT_EQUAL_INT(0, extract_final_calls);
  TEST_ASSERT_EQUAL_INT(0, save_char_calls);
  TEST_ASSERT_EQUAL_INT(1, free_char_calls);
  TEST_ASSERT_EQUAL_STRING("stat-file-denied", captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Sorry"));
}

/* G1-04: a player who is in the world (here linkdead) but not visible to
 * the caller must not be shown from the stale disk copy. */
static void test_stat_file_of_player_in_world_is_refused(void)
{
  static struct char_data online;
  static struct player_special_data online_specials;
  memset(&online, 0, sizeof(online));
  online.player_specials = &online_specials;
  GET_IDNUM(&online) = 77;
  character_list = &online;
  load_char_result = 0;
  RUN_CMD(do_stat, &imm, "file bobby");
  character_list = NULL;
  TEST_ASSERT_EQUAL_INT(1, free_char_calls);
  TEST_ASSERT_EQUAL_STRING("stat-file-online", captured_operation);
}

/* G1-05: "force <linkdead player> return" (or a DG mforce) runs do_return
 * with ch->desc == NULL; it dereferenced ch->desc->original. */
static void test_return_and_switch_without_descriptor_are_reported(void)
{
  imm.desc = NULL;
  RUN_CMD(do_return, &imm, "");
  TEST_ASSERT_EQUAL_STRING("command-no-desc", captured_operation);
  TEST_ASSERT_EQUAL_INT(0, save_char_calls);

  reset_counters();
  RUN_CMD(do_switch, &imm, "bobby");
  TEST_ASSERT_EQUAL_STRING("command-no-desc", captured_operation);
}

/* G1-05: return_to_char() on a body that is not switched is reported, not
 * dereferenced. */
static void test_return_to_char_requires_switched_body(void)
{
  return_to_char(&imm);   /* imm_desc.original is NULL */
  TEST_ASSERT_EQUAL_STRING("return-to-char", captured_operation);
  TEST_ASSERT_EQUAL_PTR(&imm, imm_desc.character);
}

/* An object outside the world with no prototype (rnum NOTHING). */
static void make_obj(struct obj_data *obj, int type)
{
  memset(obj, 0, sizeof(*obj));
  obj->item_number = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  obj->name = (char *)"sword";
  obj->short_description = (char *)"a sword";
  GET_OBJ_TYPE(obj) = type;
}

/* G1-08: weapon value[3] indexed attack_hit_text[] unchecked. */
static void test_stat_weapon_bad_attack_type_is_reported(void)
{
  static struct obj_data sword;
  make_obj(&sword, ITEM_WEAPON);
  GET_OBJ_VAL(&sword, 3) = 120;
  vis_obj = &sword;
  RUN_CMD(do_stat, &imm, "object sword");
  TEST_ASSERT_EQUAL_STRING("stat-attack-type", captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Message type: <invalid>"));

  reset_counters();
  GET_OBJ_VAL(&sword, 3) = -1;
  RUN_CMD(do_stat, &imm, "object sword");
  TEST_ASSERT_EQUAL_STRING("stat-attack-type", captured_operation);

  reset_counters();
  GET_OBJ_VAL(&sword, 3) = 3;
  RUN_CMD(do_stat, &imm, "object sword");
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Message type: slash"));
}

int main(void)
{
  /* basic_mud_log()/mudlog() are the real utils.c ones; keep them quiet. */
  logfile = fopen("/dev/null", "w");
  UNITY_BEGIN();
  RUN_TEST(test_set_file_room_is_refused_and_reported);
  RUN_TEST(test_stat_file_frees_copy_without_extracting);
  RUN_TEST(test_stat_file_outranked_is_denied_before_side_effects);
  RUN_TEST(test_stat_file_of_player_in_world_is_refused);
  RUN_TEST(test_return_and_switch_without_descriptor_are_reported);
  RUN_TEST(test_return_to_char_requires_switched_body);
  RUN_TEST(test_stat_weapon_bad_attack_type_is_reported);
  {
    int failures = UNITY_END();
    if (logfile)
      fclose(logfile);
    return failures;
  }
}
