/* Executable tests of production DG substitution (dg_variables.c var_subst)
 * and eval (dg_scripts.c process_eval).  A script line is expanded once by
 * script_driver() and, for eval, evaluated again; these tests drive the same
 * two calls.  Only trigger-local variables are used, so no entities exist.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_scripts.h"
#include "handler.h"
#include "interpreter.h"
#include "limits.h"
#include "fight.h"
#include "act.h"
#include "oasis.h"
#include "quest.h"
#include "spells.h"
#include "genobj.h"

/* ID counters live in db.c, which this fixture does not link. */
long max_mob_id = MOB_ID_BASE;
long max_obj_id = OBJ_ID_BASE;

/* World state and game functions the substitution code can reach for entity
 * fields.  These tests use trigger-local variables only, so reaching any of
 * them is a test failure. */
struct obj_data *object_list;
struct time_info_data time_info;
int top_of_trigt;
struct index_data **trig_index;
struct trig_data *trigger_list;
struct zone_data *zone_table;

#define UNREACHED() TEST_FAIL_MESSAGE("entity code reached")
void affect_total(struct char_data *ch) { (void)ch; UNREACHED(); }
int can_edit_zone(struct char_data *ch, zone_rnum rnum)
{ (void)ch; (void)rnum; UNREACHED(); return 0; }
int compute_armor_class(struct char_data *ch) { (void)ch; UNREACHED(); return 0; }
void extract_script(void *thing, int type) { (void)thing; (void)type; UNREACHED(); }
void extract_trigger(struct trig_data *t) { (void)t; UNREACHED(); }
int find_skill_num(char *name) { (void)name; UNREACHED(); return 0; }
room_rnum find_target_room(struct char_data *ch, char *rawroomstr)
{ (void)ch; (void)rawroomstr; UNREACHED(); return NOWHERE; }
void gain_exp(struct char_data *ch, int gain) { (void)ch; (void)gain; UNREACHED(); }
struct char_data *get_char_room_vis(struct char_data *ch, char *name, int *number)
{ (void)ch; (void)name; (void)number; UNREACHED(); return NULL; }
struct char_data *get_char_vis(struct char_data *ch, char *name, int *number, int where)
{ (void)ch; (void)name; (void)number; (void)where; UNREACHED(); return NULL; }
struct obj_data *get_obj_in_equip_vis(struct char_data *ch, char *arg, int *number,
                                      struct obj_data *equipment[])
{ (void)ch; (void)arg; (void)number; (void)equipment; UNREACHED(); return NULL; }
struct obj_data *get_obj_in_list_vis(struct char_data *ch, char *name, int *number,
                                     struct obj_data *list)
{ (void)ch; (void)name; (void)number; (void)list; UNREACHED(); return NULL; }
struct obj_data *get_obj_vis(struct char_data *ch, char *name, int *num)
{ (void)ch; (void)name; (void)num; UNREACHED(); return NULL; }
int get_number(char **name) { (void)name; UNREACHED(); return 0; }
int increase_gold(struct char_data *ch, int amt) { (void)ch; (void)amt; UNREACHED(); return 0; }
int is_complete(struct char_data *ch, qst_vnum vnum) { (void)ch; (void)vnum; UNREACHED(); return 0; }
int isname(const char *str, const char *namelist) { (void)str; (void)namelist; UNREACHED(); return 0; }
room_rnum obj_room(obj_data *obj) { (void)obj; UNREACHED(); return NOWHERE; }
bool oset_alias(struct obj_data *obj, char *argument) { (void)obj; (void)argument; UNREACHED(); return FALSE; }
bool oset_apply(struct obj_data *obj, char *argument) { (void)obj; (void)argument; UNREACHED(); return FALSE; }
bool oset_long_description(struct obj_data *obj, char *argument)
{ (void)obj; (void)argument; UNREACHED(); return FALSE; }
bool oset_short_description(struct obj_data *obj, char *argument)
{ (void)obj; (void)argument; UNREACHED(); return FALSE; }
trig_data *read_trigger(int nr) { (void)nr; UNREACHED(); return NULL; }
qst_rnum real_quest(qst_vnum vnum) { (void)vnum; UNREACHED(); return NOTHING; }
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; UNREACHED(); return NOWHERE; }
void update_pos(struct char_data *victim) { (void)victim; UNREACHED(); }
int valid_dg_target(char_data *ch, int bitvector) { (void)ch; (void)bitvector; UNREACHED(); return 0; }

/* dg_handler.c is not linked; this matches its list free. */
void free_varlist(struct trig_var_data *vd)
{
  struct trig_var_data *next;

  for (; vd; vd = next) {
    next = vd->next;
    free(vd->name);
    free(vd->value);
    free(vd);
  }
}

/* utils.c owns tb_error_log_at(); the link wraps it (--wrap) to count the
 * structured diagnostics the code under test emits. */
static int captured_errors;
static char captured_operation[64];
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func; (void)detail_fmt;
  captured_errors++;
  strlcpy(captured_operation, operation ? operation : "",
          sizeof(captured_operation));
}

static trig_data trig;

void setUp(void)
{
  memset(&trig, 0, sizeof(trig));
  captured_errors = 0;
  *captured_operation = '\0';
}
void tearDown(void)
{
  free_varlist(GET_TRIG_VARS(&trig));
  GET_TRIG_VARS(&trig) = NULL;
}

static const char *var(const char *name)
{
  struct trig_var_data *vd;

  for (vd = GET_TRIG_VARS(&trig); vd; vd = vd->next)
    if (!strcmp(vd->name, name))
      return vd->value;
  return NULL;
}

/* What script_driver() does for an eval line. */
static void run_eval(const char *line)
{
  char src[MAX_INPUT_LENGTH], cmd[MAX_INPUT_LENGTH];

  strlcpy(src, line, sizeof(src));
  var_subst(NULL, NULL, &trig, MOB_TRIGGER, src, cmd);
  process_eval(NULL, NULL, &trig, MOB_TRIGGER, cmd);
}

/* A player typing variable syntax must not have it evaluated by the eval's
 * second pass: before the fix "out" became the value of %secret%. */
static void player_percent_is_not_evaluated_by_eval(void)
{
  char shown[MAX_INPUT_LENGTH];

  add_var(&GET_TRIG_VARS(&trig), "secret", "leaked", 0);
  add_player_var(&GET_TRIG_VARS(&trig), "arg", "%secret%", 0);
  run_eval("eval out %arg%");

  TEST_ASSERT_NOT_NULL(var("out"));
  TEST_ASSERT_NULL(strstr(var("out"), "leaked"));
  /* The text survives and reads as typed once it leaves the engine. */
  strlcpy(shown, var("out"), sizeof(shown));
  dg_restore_percent(shown);
  TEST_ASSERT_EQUAL_STRING("%secret%", shown);
}

/* Player text used inside the %%name%% idiom stays a literal name part. */
static void player_percent_inside_dynamic_name_is_inert(void)
{
  add_var(&GET_TRIG_VARS(&trig), "secret", "leaked", 0);
  add_var(&GET_TRIG_VARS(&trig), "dir_north", "room-north", 0);
  add_player_var(&GET_TRIG_VARS(&trig), "arg", "north", 0);
  run_eval("eval where %%dir_%arg%%%");
  TEST_ASSERT_EQUAL_STRING("room-north", var("where"));

  /* Closes the name early and opens a new reference on the second pass. */
  add_player_var(&GET_TRIG_VARS(&trig), "arg", "x% %secret%", 0);
  run_eval("eval where %%dir_%arg%%%");
  TEST_ASSERT_NULL(strstr(var("where"), "leaked"));
}

/* The legacy idioms that rely on the second pass keep working. */
static void script_built_double_evaluation_still_works(void)
{
  add_var(&GET_TRIG_VARS(&trig), "txt3", "hello", 0);
  add_var(&GET_TRIG_VARS(&trig), "max", "3", 0);
  run_eval("eval direct %%txt%max%%%");
  TEST_ASSERT_EQUAL_STRING("hello", var("direct"));

  /* set speech %%txt[%max%]%% ... eval speech %speech% */
  add_var(&GET_TRIG_VARS(&trig), "speech", "%txt3%", 0);
  run_eval("eval speech %speech%");
  TEST_ASSERT_EQUAL_STRING("hello", var("speech"));
}

/* A chained field binds the intermediate value instead of evaluating it:
 * "a-b" used to be computed as a - b = 0, so its length was 1. */
static void chained_field_value_is_not_evaluated(void)
{
  add_player_var(&GET_TRIG_VARS(&trig), "arg", "a-b c", 0);
  run_eval("eval len %arg.car.strlen%");
  TEST_ASSERT_EQUAL_STRING("3", var("len"));
}

/* A reference with no closing '%' used to step past the line's terminator
 * and keep substituting stale stack bytes; with a full-length line that read
 * past the end of var_subst()'s buffer. */
static void substitute_full_line(const char *tail, char *out)
{
  char line[MAX_INPUT_LENGTH];
  size_t fill = sizeof(line) - 1 - strlen(tail);

  memset(line, 'a', fill);
  strlcpy(line + fill, tail, sizeof(line) - fill);
  TEST_ASSERT_EQUAL_size_t(MAX_INPUT_LENGTH - 1, strlen(line));
  var_subst(NULL, NULL, &trig, MOB_TRIGGER, line, out);
}

static void unterminated_reference_stops_at_line_end(void)
{
  char out[MAX_INPUT_LENGTH];

  add_var(&GET_TRIG_VARS(&trig), "x", "X", 0);
  substitute_full_line("%x", out);
  TEST_ASSERT_EQUAL_CHAR('X', out[strlen(out) - 1]);
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("var-unterminated", captured_operation);
}

static void unclosed_paren_stops_at_line_end(void)
{
  char out[MAX_INPUT_LENGTH];

  substitute_full_line("%x.y(z", out);
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("var-unterminated", captured_operation);
}

static void closed_reference_reports_nothing(void)
{
  char line[] = "say %x% done", out[MAX_INPUT_LENGTH];

  add_var(&GET_TRIG_VARS(&trig), "x", "X", 0);
  var_subst(NULL, NULL, &trig, MOB_TRIGGER, line, out);
  TEST_ASSERT_EQUAL_STRING("say X done", out);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

/* car used to copy the whole first word regardless of the destination size;
 * a heap destination lets ASan see the overflow. */
static void car_is_bounded_by_destination(void)
{
  char word[64];
  char *str = malloc(16);
  struct trig_var_data *vd;

  TEST_ASSERT_NOT_NULL(str);
  memset(word, 'w', sizeof(word) - 1);
  word[sizeof(word) - 1] = '\0';
  add_var(&GET_TRIG_VARS(&trig), "long", word, 0);
  vd = GET_TRIG_VARS(&trig);

  TEST_ASSERT_TRUE(text_processed("car", "", vd, str, 16));
  TEST_ASSERT_EQUAL_size_t(15, strlen(str));
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("field-truncated", captured_operation);

  /* A first word that fits is copied whole and silently. */
  add_var(&GET_TRIG_VARS(&trig), "long", "short rest", 0);
  TEST_ASSERT_TRUE(text_processed("car", "", vd, str, 16));
  TEST_ASSERT_EQUAL_STRING("short", str);
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  free(str);
}

static void restore_round_trips_neutralize(void)
{
  char text[] = "50% off, 100%% sure";

  dg_neutralize_percent(text);
  TEST_ASSERT_NULL(strchr(text, '%'));
  dg_restore_percent(text);
  TEST_ASSERT_EQUAL_STRING("50% off, 100%% sure", text);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(player_percent_is_not_evaluated_by_eval);
  RUN_TEST(player_percent_inside_dynamic_name_is_inert);
  RUN_TEST(script_built_double_evaluation_still_works);
  RUN_TEST(chained_field_value_is_not_evaluated);
  RUN_TEST(restore_round_trips_neutralize);
  RUN_TEST(unterminated_reference_stops_at_line_end);
  RUN_TEST(unclosed_paren_stops_at_line_end);
  RUN_TEST(closed_reference_reports_nothing);
  RUN_TEST(car_is_bounded_by_destination);
  return UNITY_END();
}
