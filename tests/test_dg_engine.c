/* Executable tests of the production DG script engine: dg_scripts.c,
 * dg_variables.c and dg_misc.c, driven through script_driver(), var_subst()
 * and the ACMDs with every server source linked in (comm.c's main() is
 * renamed). The world is a hand-built fixture of three rooms in one zone and
 * one trigger prototype whose command list each test supplies.
 *
 * Each test asserts that the guard reported its failure with the expected
 * diagnostic operation (tests/tb_error_capture.h), not merely that ASan stays
 * quiet. Player-controlled inputs are heap copies so ASan sees over-reads.
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
#include "dg_scripts.h"
#include "dg_event.h"
#include "tb_error_capture.h"

#define TEST_TRIG_VNUM 9000
#define NUM_TEST_ROOMS 3

static room_data test_rooms[NUM_TEST_ROOMS];
static struct zone_data test_zone;
static index_data test_trig_index;
static index_data *test_trig_table[1];
static trig_data test_proto;

/* Owners created by a test; tearDown releases whatever is still listed. */
#define MAX_TEST_CHARS 8
static char_data *test_chars[MAX_TEST_CHARS];
static int num_test_chars;

static struct cmdlist_element *make_cmdlist(const char **lines)
{
  struct cmdlist_element *head = NULL, *tail = NULL, *c;

  for (; *lines; lines++) {
    CREATE(c, struct cmdlist_element, 1);
    c->cmd = strdup(*lines);
    TEST_ASSERT_NOT_NULL(c->cmd);
    if (tail)
      tail->next = c;
    else
      head = c;
    tail = c;
  }
  return head;
}

static void free_cmdlist(struct cmdlist_element *c)
{
  struct cmdlist_element *next;

  for (; c; c = next) {
    next = c->next;
    free(c->cmd);
    free(c);
  }
}

/* Command lists made by a test; tearDown frees them. Triggers share their
 * list with the prototype, as in the server, so a list must outlive every
 * trigger made from it. */
#define MAX_TEST_CMDLISTS 16
static struct cmdlist_element *test_cmdlists[MAX_TEST_CMDLISTS];
static int num_test_cmdlists;

/* Give the prototype a new command list and instantiate a trigger from it. */
static trig_data *make_trigger(int attach_type, long trigger_type, int narg,
                               const char **lines)
{
  trig_data *t;

  TEST_ASSERT_TRUE(num_test_cmdlists < MAX_TEST_CMDLISTS);
  test_proto.cmdlist = make_cmdlist(lines);
  test_cmdlists[num_test_cmdlists++] = test_proto.cmdlist;
  test_proto.attach_type = attach_type;
  test_proto.trigger_type = trigger_type;
  test_proto.narg = narg;
  t = read_trigger(0);
  TEST_ASSERT_NOT_NULL(t);
  return t;
}

static void attach_to_script(struct script_data **script, trig_data *t)
{
  if (!*script)
    CREATE(*script, struct script_data, 1);
  add_trigger(*script, t, -1);
}

static char_data *make_mob(const char *name)
{
  char_data *m;

  TEST_ASSERT_TRUE(num_test_chars < MAX_TEST_CHARS);
  CREATE(m, char_data, 1);
  clear_char(m);
  m->player_specials = &dummy_mob;
  SET_BIT_AR(MOB_FLAGS(m), MOB_ISNPC);
  m->player.name = strdup(name);
  m->player.short_descr = strdup(name);
  GET_MAX_HIT(m) = GET_HIT(m) = 100;
  m->next = character_list;
  character_list = m;
  char_to_room(m, 0);
  char_script_id(m);
  test_chars[num_test_chars++] = m;
  return m;
}

static void release_char(char_data *ch)
{
  char_data **pp;

  if (SCRIPT(ch))
    extract_script(ch, MOB_TRIGGER);
  if (IN_ROOM(ch) != NOWHERE)
    char_from_room(ch);
  if (ch->script_id)
    remove_from_lookup_table(ch->script_id, ch);
  for (pp = &character_list; *pp; pp = &(*pp)->next)
    if (*pp == ch) {
      *pp = ch->next;
      break;
    }
  free(ch->player.name);
  free(ch->player.short_descr);
  free(ch);
}

static obj_data *make_obj(const char *name)
{
  obj_data *o;

  CREATE(o, obj_data, 1);
  clear_object(o);
  o->name = strdup(name);
  o->short_description = strdup(name);
  o->description = strdup(name);
  o->next = object_list;
  object_list = o;
  obj_script_id(o);
  return o;
}

static void uid_var(trig_data *t, const char *name, long uid)
{
  char buf[32];

  snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, uid);
  add_var(&GET_TRIG_VARS(t), name, buf, 0);
}

static const char *trig_var(trig_data *t, const char *name)
{
  struct trig_var_data *v;

  for (v = GET_TRIG_VARS(t); v; v = v->next)
    if (!strcmp(v->name, name))
      return v->value;
  return NULL;
}

void setUp(void)
{
  int i;

  logfile = fopen("/dev/null", "w");
  memset(test_rooms, 0, sizeof(test_rooms));
  for (i = 0; i < NUM_TEST_ROOMS; i++) {
    test_rooms[i].number = 100 + i;
    test_rooms[i].zone = 0;
    test_rooms[i].name = "test room";
  }
  world = test_rooms;
  top_of_world = NUM_TEST_ROOMS - 1;

  memset(&test_zone, 0, sizeof(test_zone));
  test_zone.number = 1;
  test_zone.bot = 100;
  test_zone.top = 199;
  zone_table = &test_zone;
  top_of_zone_table = 0;

  memset(&test_proto, 0, sizeof(test_proto));
  test_proto.nr = 0;
  test_proto.name = "test trigger";
  memset(&test_trig_index, 0, sizeof(test_trig_index));
  test_trig_index.vnum = TEST_TRIG_VNUM;
  test_trig_index.proto = &test_proto;
  test_trig_table[0] = &test_trig_index;
  trig_index = test_trig_table;
  top_of_trigt = 1;

  character_list = NULL;
  object_list = NULL;
  num_test_chars = 0;
  init_lookup_table();
  event_init();
  pulse = 0;
  tb_error_capture_reset();
}

void tearDown(void)
{
  int i;

  while (object_list)
    extract_obj(object_list);
  for (i = 0; i < num_test_chars; i++)
    release_char(test_chars[i]);
  num_test_chars = 0;
  for (i = 0; i < NUM_TEST_ROOMS; i++)
    if (SCRIPT(&test_rooms[i]))
      extract_script(&test_rooms[i], WLD_TRIGGER);
  event_free_all();
  for (i = 0; i < num_test_cmdlists; i++)
    free_cmdlist(test_cmdlists[i]);
  num_test_cmdlists = 0;
  test_proto.cmdlist = NULL;
  world = NULL;
  top_of_world = 0;
  trig_index = NULL;
  top_of_trigt = 0;
  if (logfile)
    fclose(logfile);
  logfile = NULL;
}

/* D1-01: %var.car% copied the first word of a variable into the 492-byte
 * replacement buffer with no bound. %speech% is raw player text. */
static void car_of_overlong_word_is_truncated_and_reported(void)
{
  const char *lines[] = { "eval word %speech.car%", NULL };
  char_data *mob = make_mob("mob");
  trig_data *t = make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, lines);
  char *speech = malloc(507);

  TEST_ASSERT_NOT_NULL(speech);
  memset(speech, 'x', 506);
  speech[506] = '\0';
  attach_to_script(&SCRIPT(mob), t);
  add_var(&GET_TRIG_VARS(t), "speech", speech, 0);
  free(speech);
  /* Keep the result: run the line directly rather than through the driver,
   * which frees the trigger's variables when the script ends. */
  {
    char out[MAX_INPUT_LENGTH];
    char *line = strdup("eval word %speech.car%");
    TEST_ASSERT_NOT_NULL(line);
    var_subst(mob, SCRIPT(mob), t, MOB_TRIGGER, line, out);
    free(line);
    TEST_ASSERT_TRUE(strlen(out) < MAX_INPUT_LENGTH);
  }
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("field-truncate", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "car"));
}

/* Run a one-trigger mob script from the top. */
static trig_data *run_mob_script(char_data *mob, const char **lines)
{
  trig_data *t = make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, lines);

  attach_to_script(&SCRIPT(mob), t);
  script_driver(&mob, t, MOB_TRIGGER, TRIG_NEW);
  return t;
}

/* D1-02: an unbounded wait made event_create() form a negative queue key
 * (head[-4] in queue_enq) and 'wait until' overflowed a multiplication.
 * Oversized waits now saturate and are capped at one mud year (the audit
 * branch's policy, see huge_wait_is_capped_at_one_mud_year in
 * test_dg_script_runtime.c), and the cap is reported as dg/wait-range. */
static void wait_beyond_one_mud_year_is_capped_and_reported(void)
{
  const char *lines[] = { "wait 9223372036854775807", "nop after", NULL };
  trig_data *t;

  pulse = 12345;
  t = run_mob_script(make_mob("mob"), lines);
  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_STRING("wait-range", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
}

static void scaled_wait_overflow_is_capped_and_reported(void)
{
  const char *lines[] = { "wait 99999999999999999 t", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_STRING("wait-range", tb_captured_operation);
}

static void wait_until_out_of_day_is_rejected(void)
{
  const char *lines[] = { "wait until 99999999999999999", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_TRUE(tb_captured_has_op("wait-range"));
}

static void wait_until_midnight_is_accepted(void)
{
  const char *lines[] = { "wait until 24:00", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void nonpositive_wait_waits_one_pulse_with_warning(void)
{
  const char *lines[] = { "wait -5", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("wait-range", tb_captured_operation);
}

/* D1-12: a rejected wait returned from script_driver() with depth 1 and the
 * variables kept, so TRIGGER_CHECK() refused the trigger until extraction. */
static void rejected_wait_ends_trigger_cleanly(void)
{
  const char *lines[] = { "set x 1", "wait 5 minutes", "set y 2", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_NULL(GET_TRIG_VARS(t));
  TEST_ASSERT_TRUE(TRIGGER_CHECK(t, MTRIG_SPEECH));
  TEST_ASSERT_EQUAL_STRING("wait-invalid", tb_captured_operation);
}

/* An overflowing wait is not rejected on this branch: it saturates and is
 * capped at one mud year (reported as wait-range), so the trigger is
 * legitimately suspended on a real event, not wedged without one. */
static void overflowing_wait_is_capped_not_wedged(void)
{
  const char *lines[] = { "wait 99999999999999999 t", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_TRUE(tb_captured_has_op("wait-range"));
  TEST_ASSERT_FALSE(tb_captured_has_op("wait-invalid"));
}

static void accepted_wait_keeps_trigger_suspended(void)
{
  const char *lines[] = { "set x 1", "wait 1", "set y 2", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_INT(1, GET_TRIG_DEPTH(t));
  TEST_ASSERT_EQUAL_STRING("1", trig_var(t, "x"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* var_subst() on a heap copy of text, so ASan sees any over-read. */
static void subst(char_data *mob, trig_data *t, const char *text, char *out)
{
  char *line = strdup(text);

  TEST_ASSERT_NOT_NULL(line);
  var_subst(mob, SCRIPT(mob), t, MOB_TRIGGER, line, out);
  free(line);
}

static trig_data *idle_mob_trigger(char_data *mob)
{
  const char *lines[] = { "nop", NULL };
  trig_data *t = make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, lines);

  attach_to_script(&SCRIPT(mob), t);
  return t;
}

/* D1-04: a lone '%' ending the line opens no variable; it used to be
 * dropped silently. */
static void lone_percent_at_line_end_is_kept(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  subst(mob, t, "say 50%", out);
  TEST_ASSERT_EQUAL_STRING("say 50%", out);
  TEST_ASSERT_EQUAL_STRING("subst-unterminated", tb_captured_operation);
}

/* D1-05: the subfield buffer was never reset between variables, so the
 * second variable on a line received the first one's subfield. */
static void each_variable_gets_its_own_subfield(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  uid_var(t, "actor", char_script_id(mob));
  GET_GOLD(mob) = 0;
  subst(mob, t, "say %actor.gold(5)% coins, total %actor.gold%", out);
  TEST_ASSERT_EQUAL_STRING("say 5 coins, total 5", out);
  TEST_ASSERT_EQUAL_INT(5, GET_GOLD(mob));
  subst(mob, t, "say %actor.gold(5)% then %actor.name%", out);
  TEST_ASSERT_EQUAL_STRING("say 10 then mob", out);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void unbalanced_close_paren_is_reported(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  subst(mob, t, "say %self.name)%", out);
  TEST_ASSERT_EQUAL_STRING("subst-paren", tb_captured_operation);
}

/* D1-29: a chained field (%a.b.c%) renamed the variable to "tmpvr" in place
 * inside var_subst's copy, overwriting the next field when var.field was
 * shorter than five bytes. */
static void chained_field_after_short_name_is_kept(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  uid_var(t, "m", char_script_id(mob));
  subst(mob, t, "x %m.id.strlen%", out);
  TEST_ASSERT_EQUAL_STRING("x 8", out);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void failed_chain_link_is_reported(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  subst(mob, t, "x %nosuchvar.name.car%", out);
  TEST_ASSERT_EQUAL_STRING("subst-chain", tb_captured_operation);
}

static obj_data *make_scripted_obj(const char *name, long trig_type, int narg,
                                   const char **lines)
{
  obj_data *o = make_obj(name);
  trig_data *t = make_trigger(OBJ_TRIGGER, trig_type, narg, lines);

  obj_to_room(o, 0);
  attach_to_script(&SCRIPT(o), t);
  return o;
}

/* D1-06: script_trigger_check() read obj->next after random_otrigger(obj),
 * and stock random triggers purge their own object. */
static void random_trigger_self_purge_keeps_scanning(void)
{
  const char *quiet[] = { "nop", NULL };
  const char *purge[] = { "%purge% %self%", NULL };
  obj_data *tail = make_scripted_obj("tail", OTRIG_RANDOM, 100, quiet);
  long tail_uid = obj_script_id(tail);

  make_scripted_obj("purger", OTRIG_RANDOM, 100, purge);
  script_trigger_check();
  TEST_ASSERT_EQUAL_PTR(tail, object_list);
  TEST_ASSERT_NULL(tail->next);
  TEST_ASSERT_TRUE(obj_uid_matches(tail_uid, tail));
}

/* The purger also purges the object after it. The dispatcher snapshots
 * every candidate as (UID, address) and revalidates each before firing, so
 * the purged one is skipped without being read; nothing is left. */
static void random_trigger_purging_next_object_is_safe(void)
{
  const char *quiet[] = { "nop", NULL };
  obj_data *tail = make_scripted_obj("tail", OTRIG_RANDOM, 100, quiet);
  char first[64];
  const char *purge[] = { first, "%purge% %self%", NULL };

  snprintf(first, sizeof(first), "%%purge%% %c%ld", UID_CHAR,
           obj_script_id(tail));
  make_scripted_obj("purger", OTRIG_RANDOM, 100, purge);
  script_trigger_check();
  TEST_ASSERT_NULL(object_list);
  TEST_ASSERT_FALSE(tb_captured_has_op("random-dispatch"));
}

static void time_trigger_self_purge_keeps_scanning(void)
{
  const char *quiet[] = { "nop", NULL };
  const char *purge[] = { "%purge% %self%", NULL };
  obj_data *tail = make_scripted_obj("tail", OTRIG_TIME, 5, quiet);

  make_scripted_obj("purger", OTRIG_TIME, 5, purge);
  time_info.hours = 5;
  check_time_triggers();
  TEST_ASSERT_EQUAL_PTR(tail, object_list);
  TEST_ASSERT_NULL(tail->next);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(car_of_overlong_word_is_truncated_and_reported);
  RUN_TEST(wait_beyond_one_mud_year_is_capped_and_reported);
  RUN_TEST(scaled_wait_overflow_is_capped_and_reported);
  RUN_TEST(wait_until_out_of_day_is_rejected);
  RUN_TEST(wait_until_midnight_is_accepted);
  RUN_TEST(nonpositive_wait_waits_one_pulse_with_warning);
  RUN_TEST(rejected_wait_ends_trigger_cleanly);
  RUN_TEST(overflowing_wait_is_capped_not_wedged);
  RUN_TEST(accepted_wait_keeps_trigger_suspended);
  RUN_TEST(lone_percent_at_line_end_is_kept);
  RUN_TEST(each_variable_gets_its_own_subfield);
  RUN_TEST(unbalanced_close_paren_is_reported);
  RUN_TEST(chained_field_after_short_name_is_kept);
  RUN_TEST(failed_chain_link_is_reported);
  RUN_TEST(random_trigger_self_purge_keeps_scanning);
  RUN_TEST(random_trigger_purging_next_object_is_safe);
  RUN_TEST(time_trigger_self_purge_keeps_scanning);
  return UNITY_END();
}
