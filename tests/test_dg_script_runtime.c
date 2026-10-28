/* Executable tests of the production DG runtime: substitution
 * (dg_variables.c var_subst), eval (dg_scripts.c process_eval) and the
 * command loop (script_driver).  A script line is expanded once by
 * script_driver() and, for eval, evaluated again.  Entities are zeroed
 * fixtures registered in the production UID lookup table.
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
#include "comm.h"
#include "dg_event.h"

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

#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
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
/* Name lookups filter targets with this; every fixture is a valid target. */
int valid_dg_target(char_data *ch, int bitvector) { (void)ch; (void)bitvector; return 1; }

unsigned long pulse;
void do_dg_cast(void *go, struct script_data *sc, trig_data *t, int type, char *cmd)
{ (void)go; (void)sc; (void)t; (void)type; (void)cmd; UNREACHED(); }
void do_dg_affect(void *go, struct script_data *sc, trig_data *t, int type, char *cmd)
{ (void)go; (void)sc; (void)t; (void)type; (void)cmd; UNREACHED(); }
/* process_wait() schedules through this; record the delay it asked for. */
static long scheduled_delay;
struct event *event_create(EVENTFUNC(*func), void *event_obj, long when)
{
  (void)func;
  free(event_obj);
  scheduled_delay = when;
  return NULL;
}
int remove_var(struct trig_var_data **var_list, char *name)
{ (void)var_list; (void)name; UNREACHED(); return 0; }

/* dg_handler.c is not linked: script_driver() frames must stay balanced. */
static int execution_depth;
void dg_script_execution_enter(void) { execution_depth++; }
void dg_script_execution_leave(void)
{
  TEST_ASSERT_TRUE(execution_depth > 0);
  execution_depth--;
}

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

/* %mob.global(name)% passed a NULL field to find_replacement(), which
 * dereferenced it; the value it looked up was then overwritten anyway. */
static void mob_global_field_reads_the_variable(void)
{
  static char_data owner, target;
  static struct script_data target_script;
  static struct room_data room;
  char line[MAX_INPUT_LENGTH], out[MAX_INPUT_LENGTH];

  memset(&owner, 0, sizeof(owner));
  memset(&target, 0, sizeof(target));
  memset(&target_script, 0, sizeof(target_script));
  memset(&room, 0, sizeof(room));
  /* An empty one-room world: the name search scans the owner's room. */
  world = &room;
  IN_ROOM(&owner) = 0;
  IN_ROOM(&target) = NOWHERE;
  SET_BIT_AR(MOB_FLAGS(&target), MOB_ISNPC);
  target.script_id = MOB_ID_BASE + 7;
  SCRIPT(&target) = &target_script;
  add_var(&target_script.global_vars, "quest", "stage2", 0);
  init_lookup_table();
  add_to_lookup_table(target.script_id, &target);

  tb_snprintf(line, sizeof(line), "%c%ld", UID_CHAR, target.script_id);
  add_var(&GET_TRIG_VARS(&trig), "m", line, 0);

  strlcpy(line, "[%m.global(quest)%]", sizeof(line));
  var_subst(&owner, NULL, &trig, MOB_TRIGGER, line, out);
  TEST_ASSERT_EQUAL_STRING("[stage2]", out);

  strlcpy(line, "[%m.global(unset)%]", sizeof(line));
  var_subst(&owner, NULL, &trig, MOB_TRIGGER, line, out);
  TEST_ASSERT_EQUAL_STRING("[]", out);

  remove_from_lookup_table(target.script_id, &target);
  free_varlist(target_script.global_vars);
  world = NULL;
}

/* script_driver() fixtures: a two-line script whose first command frees or
 * extracts its owner, as purging the container holding an object does. */
static obj_data *purge_obj;
static char_data *purge_mob;
static int dispatched;

void obj_command_interpreter(obj_data *obj, char *argument)
{
  (void)argument;
  dispatched++;
  if (obj == purge_obj) {
    /* What extract_obj() does to the identity before freeing it. */
    remove_from_lookup_table(obj->script_id, obj);
    purge_obj = NULL;
  }
}
/* interpreter.c is linked for its string helpers; the link wraps (--wrap)
 * its two command entry points so script_driver() reaches these. */
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_script_command_interpreter(struct char_data *ch, char *arg)
{
  (void)arg;
  dispatched++;
  if (ch == purge_mob)
    SET_BIT_AR(MOB_FLAGS(ch), MOB_NOTDEADYET);   /* extract_char() */
  return 1;
}
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_command_interpreter(struct char_data *ch, char *argument)
{ (void)ch; (void)argument; UNREACHED(); }
void wld_command_interpreter(room_data *room, char *argument)
{ (void)room; (void)argument; UNREACHED(); }

static struct cmdlist_element line2 = { "second", NULL, NULL, 0 };
static struct cmdlist_element line1 = { "first", NULL, &line2, 0 };

static void run_owned_script(void *go_address, int type)
{
  static struct index_data index = { 0 };
  static struct index_data *indexes[1] = { &index };

  index.vnum = 4242;
  trig_index = indexes;
  top_of_trigt = 1;
  trig.cmdlist = &line1;
  trig.attach_type = type;
  dispatched = 0;
  script_driver(go_address, &trig, type, TRIG_NEW);
  TEST_ASSERT_EQUAL_INT(0, execution_depth);
  trig_index = NULL;
  top_of_trigt = 0;
}

/* The flag-based check only noticed an object purging itself by name, and
 * a nested script cleared it; the frame now checks its own owner's UID. */
static void script_stops_when_its_object_owner_is_freed(void)
{
  static obj_data obj;
  static struct script_data script;
  obj_data *owner = &obj;

  memset(&obj, 0, sizeof(obj));
  memset(&script, 0, sizeof(script));
  init_lookup_table();
  SCRIPT(&obj) = &script;
  GET_OBJ_RNUM(&obj) = NOTHING;           /* no prototype */
  obj.script_id = OBJ_ID_BASE + 3;
  add_to_lookup_table(obj.script_id, &obj);
  purge_obj = &obj;

  run_owned_script(&owner, OBJ_TRIGGER);
  TEST_ASSERT_EQUAL_INT(1, dispatched);
  TEST_ASSERT_NULL(owner);        /* the caller learns the object is gone */
}

static void script_runs_on_while_its_object_owner_lives(void)
{
  static obj_data obj;
  static struct script_data script;
  obj_data *owner = &obj;

  memset(&obj, 0, sizeof(obj));
  memset(&script, 0, sizeof(script));
  init_lookup_table();
  SCRIPT(&obj) = &script;
  GET_OBJ_RNUM(&obj) = NOTHING;           /* no prototype */
  obj.script_id = OBJ_ID_BASE + 4;
  add_to_lookup_table(obj.script_id, &obj);
  purge_obj = NULL;

  run_owned_script(&owner, OBJ_TRIGGER);
  TEST_ASSERT_EQUAL_INT(2, dispatched);
  TEST_ASSERT_EQUAL_PTR(&obj, owner);
  remove_from_lookup_table(obj.script_id, &obj);
}

static void script_stops_when_its_mob_owner_is_extracted(void)
{
  static char_data mob;
  static struct script_data script;
  char_data *owner = &mob;

  memset(&mob, 0, sizeof(mob));
  memset(&script, 0, sizeof(script));
  init_lookup_table();
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_ISNPC);
  SCRIPT(&mob) = &script;
  GET_MOB_RNUM(&mob) = NOBODY;            /* no prototype */
  mob.script_id = MOB_ID_BASE + 5;
  add_to_lookup_table(mob.script_id, &mob);
  purge_mob = &mob;

  run_owned_script(&owner, MOB_TRIGGER);
  TEST_ASSERT_EQUAL_INT(1, dispatched);
  purge_mob = NULL;
  remove_from_lookup_table(mob.script_id, &mob);
}

/* A saved variable longer than one pfile line used to be written anyway;
 * on reload its tail was parsed as pfile tags.  It is now refused and
 * reported, and the rest still round-trips. */
static void saved_vars_never_exceed_one_pfile_line(void)
{
  static char_data player, reloaded;
  static struct script_data script;
  char long_value[READ_SIZE + 32], line[READ_SIZE];
  struct trig_var_data *vd;
  FILE *fp = tmpfile();

  TEST_ASSERT_NOT_NULL(fp);
  memset(&player, 0, sizeof(player));
  memset(&reloaded, 0, sizeof(reloaded));
  memset(&script, 0, sizeof(script));
  GET_IDNUM(&player) = 12;
  SCRIPT(&player) = &script;
  memset(long_value, 'x', sizeof(long_value) - 1);
  long_value[sizeof(long_value) - 1] = '\0';
  add_var(&script.global_vars, "long", long_value, 0);
  add_var(&script.global_vars, "broken", "a\nLevl: 34", 0);
  add_var(&script.global_vars, "stage", "2", 0);

  save_char_vars_ascii(fp, &player);
  TEST_ASSERT_EQUAL_INT(2, captured_errors);
  TEST_ASSERT_EQUAL_STRING("var-save", captured_operation);

  TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0L, SEEK_SET));
  TEST_ASSERT_EQUAL_INT(1, get_line(fp, line));
  TEST_ASSERT_EQUAL_STRING("Vars: 1", line);
  read_saved_vars_ascii(fp, &reloaded, 1);
  vd = SCRIPT(&reloaded)->global_vars;
  TEST_ASSERT_NOT_NULL(vd);
  TEST_ASSERT_EQUAL_STRING("stage", vd->name);
  TEST_ASSERT_EQUAL_STRING("2", vd->value);
  TEST_ASSERT_NULL(vd->next);
  TEST_ASSERT_EQUAL_INT(0, get_line(fp, line));   /* nothing else written */

  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  free_varlist(script.global_vars);
  free_varlist(SCRIPT(&reloaded)->global_vars);
  free(SCRIPT(&reloaded));
}

/* A huge wait used to reach the event queue as a key that wrapped
 * negative; it is now capped at one mud year. */
static void huge_wait_is_capped_at_one_mud_year(void)
{
  static struct cmdlist_element wait_line = { "wait 999999999999 s", NULL, NULL, 0 };
  static struct cmdlist_element short_wait = { "wait 3 s", NULL, NULL, 0 };
  static struct index_data index = { 0 };
  static struct index_data *indexes[1] = { &index };
  static obj_data obj;
  static struct script_data script;
  obj_data *owner = &obj;

  memset(&obj, 0, sizeof(obj));
  memset(&script, 0, sizeof(script));
  init_lookup_table();
  SCRIPT(&obj) = &script;
  GET_OBJ_RNUM(&obj) = NOTHING;
  obj.script_id = OBJ_ID_BASE + 6;
  add_to_lookup_table(obj.script_id, &obj);
  trig_index = indexes;
  top_of_trigt = 1;
  trig.attach_type = OBJ_TRIGGER;

  trig.cmdlist = &wait_line;
  script_driver(&owner, &trig, OBJ_TRIGGER, TRIG_NEW);
  TEST_ASSERT_EQUAL_INT(DG_MAX_WAIT_PULSES, scheduled_delay);

  trig.cmdlist = &short_wait;
  script_driver(&owner, &trig, OBJ_TRIGGER, TRIG_NEW);
  TEST_ASSERT_EQUAL_INT(3 * PASSES_PER_SEC, scheduled_delay);

  trig_index = NULL;
  top_of_trigt = 0;
  remove_from_lookup_table(obj.script_id, &obj);
}

/* The (subfield) buffer was shared by every reference on a line, so the
 * second reference saw "ab" instead of "b". */
static void each_reference_has_its_own_subfield(void)
{
  char line[] = "%x.contains(a)% %x.contains(b)%", out[MAX_INPUT_LENGTH];

  add_var(&GET_TRIG_VARS(&trig), "x", "b", 0);
  var_subst(NULL, NULL, &trig, MOB_TRIGGER, line, out);
  TEST_ASSERT_EQUAL_STRING("0 1", out);
}

/* Runs lines (NULL-terminated) as an object script; returns the number of
 * commands dispatched to the object's interpreter. */
static int run_object_lines(const char *const *lines)
{
  static struct cmdlist_element cl[8];
  static struct index_data index = { 0 };
  static struct index_data *indexes[1] = { &index };
  static obj_data obj;
  static struct script_data script;
  obj_data *owner = &obj;
  int n;

  memset(cl, 0, sizeof(cl));
  for (n = 0; lines[n]; n++) {
    TEST_ASSERT_TRUE(n < 8);
    cl[n].cmd = (char *)lines[n];
    cl[n].next = lines[n + 1] ? &cl[n + 1] : NULL;
  }
  memset(&obj, 0, sizeof(obj));
  memset(&script, 0, sizeof(script));
  init_lookup_table();
  SCRIPT(&obj) = &script;
  GET_OBJ_RNUM(&obj) = NOTHING;
  obj.script_id = OBJ_ID_BASE + 8;
  add_to_lookup_table(obj.script_id, &obj);
  trig_index = indexes;
  top_of_trigt = 1;
  trig.cmdlist = cl;
  trig.attach_type = OBJ_TRIGGER;
  purge_obj = NULL;
  dispatched = 0;

  script_driver(&owner, &trig, OBJ_TRIGGER, TRIG_NEW);

  TEST_ASSERT_EQUAL_INT(0, execution_depth);
  trig_index = NULL;
  top_of_trigt = 0;
  remove_from_lookup_table(obj.script_id, &obj);
  return dispatched;
}

/* find_done() used to return NULL when a nested block ran to the end, and
 * break/switch then followed cl->next from NULL. */
static void break_without_done_ends_the_script(void)
{
  static const char *const lines[] = { "break", "while 1", "nop x", NULL };

  TEST_ASSERT_EQUAL_INT(0, run_object_lines(lines));
  TEST_ASSERT_EQUAL_STRING("block-unterminated", captured_operation);
}

static void switch_with_unclosed_nested_while_ends_the_script(void)
{
  static const char *const lines[] = { "switch 1", "while 0", "nop x", NULL };

  TEST_ASSERT_EQUAL_INT(0, run_object_lines(lines));
  TEST_ASSERT_EQUAL_STRING("block-unterminated", captured_operation);
}

static void switch_still_selects_its_case(void)
{
  static const char *const lines[] = {
    "switch 2", "case 1", "first", "case 2", "second", "done", NULL };

  TEST_ASSERT_EQUAL_INT(1, run_object_lines(lines));
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

/* "remote x %self.id%" re-adds a variable from its own value: add_var()
 * freed the old value and then copied from it. */
static void add_var_accepts_its_own_value(void)
{
  struct trig_var_data *list = NULL;

  add_var(&list, "x", "stage two", 0);
  add_var(&list, "x", list->value, 0);
  TEST_ASSERT_EQUAL_STRING("stage two", list->value);
  add_var(&list, "x", list->value + 6, 0);
  TEST_ASSERT_EQUAL_STRING("two", list->value);
  TEST_ASSERT_NULL(list->next);
  free_varlist(list);
}

/* page_string() is a weak stub; keep what tstat would page. */
static char *paged;
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  free(paged);
  paged = strdup(str);
}

/* tstat added snprintf's would-be length, so one long line carried len
 * past the 48K buffer and the next write went out of bounds. */
static void tstat_of_a_long_script_stays_in_bounds(void)
{
  enum { LINES = 120, WIDTH = 480 };
  static struct cmdlist_element cl[LINES];
  static char text[LINES][WIDTH + 1];
  static trig_data proto;
  static struct index_data index;
  static struct index_data *indexes[1] = { &index };
  static char_data imm;
  static struct player_special_data imm_specials;
  char arg[] = "4242";
  int i;

  memset(&proto, 0, sizeof(proto));
  memset(&imm, 0, sizeof(imm));
  memset(&imm_specials, 0, sizeof(imm_specials));
  imm.player_specials = &imm_specials;      /* colour preferences */
  for (i = 0; i < LINES; i++) {
    memset(text[i], 'c', WIDTH);
    text[i][WIDTH] = '\0';
    cl[i].cmd = text[i];
    cl[i].next = i + 1 < LINES ? &cl[i + 1] : NULL;
  }
  proto.name = "long script";
  proto.cmdlist = cl;
  index.vnum = 4242;
  index.proto = &proto;
  trig_index = indexes;
  top_of_trigt = 1;

  do_tstat(&imm, arg, 0, 0);

  TEST_ASSERT_NOT_NULL(paged);
  TEST_ASSERT_TRUE(strlen(paged) < MAX_STRING_LENGTH);
  TEST_ASSERT_NOT_NULL(strstr(paged, "*** Overflow - script too long! ***"));
  free(paged);
  paged = NULL;
  trig_index = NULL;
  top_of_trigt = 0;
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
  RUN_TEST(mob_global_field_reads_the_variable);
  RUN_TEST(script_stops_when_its_object_owner_is_freed);
  RUN_TEST(script_runs_on_while_its_object_owner_lives);
  RUN_TEST(script_stops_when_its_mob_owner_is_extracted);
  RUN_TEST(saved_vars_never_exceed_one_pfile_line);
  RUN_TEST(huge_wait_is_capped_at_one_mud_year);
  RUN_TEST(each_reference_has_its_own_subfield);
  RUN_TEST(break_without_done_ends_the_script);
  RUN_TEST(switch_with_unclosed_nested_while_ends_the_script);
  RUN_TEST(switch_still_selects_its_case);
  RUN_TEST(add_var_accepts_its_own_value);
  RUN_TEST(tstat_of_a_long_script_stays_in_bounds);
  return UNITY_END();
}
