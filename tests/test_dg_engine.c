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
#include "oasis.h"
#include "spells.h"
#include "protocol.h"
#include "tb_error_capture.h"

#define TEST_TRIG_VNUM 9000
#define NUM_TEST_ROOMS 3

static room_data test_rooms[NUM_TEST_ROOMS];
static struct zone_data test_zone;
static index_data test_trig_index;
static index_data *test_trig_table[1];
static trig_data test_proto;
/* One object prototype, vnum TEST_OBJ_VNUM in the test zone. */
#define TEST_OBJ_VNUM 150
static struct obj_data test_obj_proto;
static index_data test_obj_index;

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
  if (ch->desc) {
    /* Paged text (page_string) and a large output buffer, as
     * close_socket() would release them. */
    free(ch->desc->showstr_head);
    free(ch->desc->showstr_vector);
    if (ch->desc->large_outbuf) {
      free(ch->desc->large_outbuf->text);
      free(ch->desc->large_outbuf);
    }
    ProtocolDestroy(ch->desc->pProtocol);
    free(ch->desc);
  }
  if (ch->player_specials != &dummy_mob)
    free(ch->player_specials);
  free(ch->player.name);
  free(ch->player.short_descr);
  free(ch);
}

/* An implementor with a descriptor (can_edit_zone() requires one) and
 * builder access to every zone, for the immortal commands. */
static char_data *make_imm(void)
{
  char_data *ch;
  struct descriptor_data *d;

  TEST_ASSERT_TRUE(num_test_chars < MAX_TEST_CHARS);
  CREATE(ch, char_data, 1);
  clear_char(ch);
  CREATE(ch->player_specials, struct player_special_data, 1);
  ch->player.name = strdup("Imm");
  GET_LEVEL(ch) = LVL_IMPL;
  GET_IDNUM(ch) = 4242;
  GET_OLC_ZONE(ch) = ALL_PERMISSION;
  CREATE(d, struct descriptor_data, 1);
  d->output = d->small_outbuf;
  d->bufspace = SMALL_BUFSIZE - 1;
  d->connected = CON_PLAYING;
  d->character = ch;
  d->pProtocol = ProtocolCreate();
  ch->desc = d;
  ch->next = character_list;
  character_list = ch;
  char_to_room(ch, 0);
  test_chars[num_test_chars++] = ch;
  return ch;
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

  clear_object(&test_obj_proto);
  test_obj_proto.item_number = 0;
  test_obj_proto.name = "proto";
  test_obj_proto.short_description = "a prototype";
  test_obj_proto.description = "A prototype lies here.";
  memset(&test_obj_index, 0, sizeof(test_obj_index));
  test_obj_index.vnum = TEST_OBJ_VNUM;
  obj_proto = &test_obj_proto;
  obj_index = &test_obj_index;
  top_of_objt = 0;

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
  obj_proto = NULL;
  obj_index = NULL;
  top_of_objt = 0;
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

/* D1-07a: purging the owner's container freed the owner and the next line
 * dereferenced the freed object. script_driver() now checks its own owner's
 * UID after every command (dg_frame_owner_gone()). */
static void purging_owner_container_stops_script(void)
{
  obj_data *box = make_obj("box"), *owner = make_obj("owner");
  char first[64];
  const char *lines[] = { first, "%echo% still here", "context 7", NULL };
  trig_data *t;
  obj_data *ref = owner;

  obj_to_room(box, 0);
  obj_to_obj(owner, box);
  snprintf(first, sizeof(first), "%%purge%% %c%ld", UID_CHAR,
           obj_script_id(box));
  t = make_trigger(OBJ_TRIGGER, OTRIG_RANDOM, 100, lines);
  attach_to_script(&SCRIPT(owner), t);
  script_driver(&ref, t, OBJ_TRIGGER, TRIG_NEW);
  TEST_ASSERT_NULL(ref);
  TEST_ASSERT_NULL(object_list);
  /* obj_command_interpreter() (dg_objcmd.c) names the command that
   * destroyed its own owner indirectly as owner-extracted. */
  TEST_ASSERT_TRUE(tb_captured_has_op("owner-extracted"));
}

/* D1-07c: a nested trigger's self-purge used to end the outer object script
 * too, NULL its caller's live object and leave its trigger at depth 1. Each
 * frame now checks only its own owner. */
static void nested_self_purge_does_not_end_outer_script(void)
{
  const char *outer[] = { "%load% obj 150", "context 7", NULL };
  const char *inner[] = { "%purge% %self%", NULL };
  struct trig_proto_list proto_trig = { TEST_TRIG_VNUM, NULL };
  obj_data *owner = make_obj("owner"), *ref = owner, *dummy = make_obj("dummy");
  trig_data *t = make_trigger(OBJ_TRIGGER, OTRIG_RANDOM, 100, outer);

  /* The object prototype carries a load trigger that purges itself. Loading
   * it from the owner's script runs that trigger nested in the owner's. */
  test_obj_proto.proto_script = &proto_trig;

  attach_to_script(&SCRIPT(owner), t);
  /* The prototype's trigger is made last, so oload's read_trigger() copies
   * it; the instance made here is parked on an inert object. */
  attach_to_script(&SCRIPT(dummy), make_trigger(OBJ_TRIGGER, OTRIG_LOAD, 100, inner));
  obj_to_room(owner, 0);
  script_driver(&ref, t, OBJ_TRIGGER, TRIG_NEW);

  test_obj_proto.proto_script = NULL;
  TEST_ASSERT_EQUAL_INT(0, test_obj_index.number);   /* loaded and purged */
  TEST_ASSERT_EQUAL_PTR(owner, ref);
  TEST_ASSERT_EQUAL_INT(7, SCRIPT(owner)->context);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
}

/* D1-08: find_done() returned NULL for a block without 'done' nested near
 * the end, and find_case() / the 'break' handler followed it. */
static void switch_with_unterminated_nested_while_is_reported(void)
{
  const char *lines[] = { "switch 2", "case 1", "nop", "while 0", "nop", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_EQUAL_STRING("block-unterminated", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
}

static void break_without_done_is_reported(void)
{
  const char *lines[] = { "break", "while 0", "nop", "nop", NULL };
  trig_data *t = run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_EQUAL_STRING("block-unterminated", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
}

static void well_formed_switch_still_selects_its_case(void)
{
  const char *lines[] = { "switch 2", "case 1", "context 1", "break",
                          "case 2", "context 2", "break", "default",
                          "context 9", "break", "done", "nop end", NULL };
  char_data *mob = make_mob("mob");

  run_mob_script(mob, lines);
  TEST_ASSERT_EQUAL_INT(2, SCRIPT(mob)->context);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* D1-09: find_room() had no upper bound, so an object UID whose offset from
 * ROOM_ID_BASE is 100 mod 65536 named room vnum 100. */
static void object_uid_never_aliases_a_room(void)
{
  const char *quiet[] = { "nop", NULL };
  char second[64];
  const char *lines[] = { "set qflag done", second, NULL };
  obj_data *o;
  struct trig_var_data *v;

  max_obj_id = ROOM_ID_BASE + 153L * 65536 + 100;   /* 30027108 */
  o = make_obj("target");
  TEST_ASSERT_EQUAL_INT64(30027108, o->script_id);
  attach_to_script(&SCRIPT(o), make_trigger(OBJ_TRIGGER, OTRIG_RANDOM, 0, quiet));
  attach_to_script(&SCRIPT(&world[0]), make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, quiet));
  snprintf(second, sizeof(second), "remote qflag %ld", o->script_id);
  run_mob_script(make_mob("mob"), lines);

  TEST_ASSERT_NULL(SCRIPT(&world[0])->global_vars);
  v = SCRIPT(o)->global_vars;
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL_STRING("qflag", v->name);
  TEST_ASSERT_EQUAL_STRING("done", v->value);
}

static void wrapping_room_vnum_is_rejected(void)
{
  char name[16];

  snprintf(name, sizeof(name), "%d", 65536 + 100);
  TEST_ASSERT_NULL(get_room(name));
  TEST_ASSERT_EQUAL_STRING("uid-namespace", tb_captured_operation);
  snprintf(name, sizeof(name), "%c%ld", UID_CHAR, (long)ROOM_ID_BASE + 100);
  TEST_ASSERT_EQUAL_PTR(&world[0], get_room(name));
}

/* An object of the test prototype (vnum TEST_OBJ_VNUM, in the test zone). */
static obj_data *make_proto_obj(const char *name)
{
  obj_data *o = make_obj(name);

  o->item_number = 0;
  test_obj_index.number++;
  return o;
}

static void immortal_command(ACMD((*cmd)), char_data *ch, const char *text)
{
  char *arg = strdup(text);

  TEST_ASSERT_NOT_NULL(arg);
  cmd(ch, arg, 0, 0);
  free(arg);
}

/* D1-10: "detach obj <name>" printed the usage message but went on to call
 * remove_trigger(script, NULL), which ran strstr(NULL, "."). */
static void detach_object_without_trigger_argument_is_refused(void)
{
  const char *quiet[] = { "nop", NULL };
  char_data *imm = make_imm();
  obj_data *o = make_proto_obj("zzuniq");

  obj_to_room(o, 0);
  attach_to_script(&SCRIPT(o), make_trigger(OBJ_TRIGGER, OTRIG_RANDOM, 0, quiet));
  immortal_command(do_detach, imm, " obj zzuniq");
  TEST_ASSERT_NOT_NULL(SCRIPT(o));
  TEST_ASSERT_NOT_NULL(TRIGGERS(SCRIPT(o)));
  TEST_ASSERT_NOT_NULL(strstr(imm->desc->output, "must specify a trigger"));
}

static void detach_mobile_without_trigger_argument_is_refused(void)
{
  const char *quiet[] = { "nop", NULL };
  char_data *imm = make_imm(), *mob = make_mob("zzmob");

  attach_to_script(&SCRIPT(mob), make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, quiet));
  immortal_command(do_detach, imm, " mob zzmob");
  TEST_ASSERT_NOT_NULL(TRIGGERS(SCRIPT(mob)));
  TEST_ASSERT_NOT_NULL(strstr(imm->desc->output, "must specify a trigger"));
  TEST_ASSERT_NULL(strstr(imm->desc->output, "not found"));
}

/* D1-11: do_detach instantiated the prototype named by its third argument
 * with read_trigger() before validating anything, and never freed it. */
static void detach_does_not_instantiate_a_trigger(void)
{
  const char *quiet[] = { "nop", NULL };
  char_data *imm = make_imm();
  int before;

  attach_to_script(&SCRIPT(&test_rooms[0]),
                   make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, quiet));
  before = test_trig_index.number;
  immortal_command(do_detach, imm, "");
  immortal_command(do_detach, imm, " room");
  TEST_ASSERT_EQUAL_INT(before, test_trig_index.number);
  /* The report names the trigger that was really removed. */
  immortal_command(do_detach, imm, " room 100 9000");
  TEST_ASSERT_NULL(SCRIPT(&test_rooms[0]));
  TEST_ASSERT_EQUAL_INT(before - 1, test_trig_index.number);
  TEST_ASSERT_NOT_NULL(strstr(imm->desc->output,
                              "Trigger 9000 (test trigger) removed"));
}

/* D1-13: process_attach() instantiated the trigger before rejecting a
 * player target (script_players off) and leaked the instance. */
static void attach_to_player_is_refused_before_allocating(void)
{
  char line[64];
  const char *lines[] = { line, NULL };
  char_data *pc = make_mob("pc"), *mob = make_mob("mob");
  int before;

  REMOVE_BIT_AR(MOB_FLAGS(pc), MOB_ISNPC);
  GET_IDNUM(pc) = 77;
  snprintf(line, sizeof(line), "attach %d %ld", TEST_TRIG_VNUM, pc->script_id);
  before = test_trig_index.number;
  run_mob_script(mob, lines);
  /* Only the running trigger itself is live. */
  TEST_ASSERT_EQUAL_INT(before + 1, test_trig_index.number);
  TEST_ASSERT_NULL(SCRIPT(pc));
  TEST_ASSERT_EQUAL_STRING("attach-target", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "idnum 77"));
}

/* D1-14: when a script detached itself, script_driver() skipped freeing the
 * trigger's variables and then dropped the pointer. LSan (ASan build) and
 * Valgrind report the leak if it returns. */
static void self_detaching_script_frees_its_variables(void)
{
  const char *lines[] = { "set x 1", "set y 2", "detach all %self.id%", NULL };
  char_data *mob = make_mob("mob");

  run_mob_script(mob, lines);
  TEST_ASSERT_NULL(SCRIPT(mob));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* D1-15: extract looped num times even after the words ran out; with num
 * near INT_MAX that stalled the game loop for minutes, silently. */
static void extract_past_last_word_stops_and_reports(void)
{
  const char *ok[] = { "extract w 2 a b", NULL };
  const char *far[] = { "extract w 2000000000 a b", NULL };

  run_mob_script(make_mob("mob"), ok);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  run_mob_script(make_mob("mob2"), far);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("extract-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "9000"));
}

/* D1-16: do_stat_trigger() summed untruncated snprintf() returns, so a long
 * command list pushed its offset past sb and the overflow notice was written
 * beyond the buffer (ASan: stack-buffer-overflow WRITE). */
#define TSTAT_LINES 199
#define TSTAT_LINE_LEN 299
static void tstat_of_long_script_is_truncated_and_reported(void)
{
  static char text[TSTAT_LINES][TSTAT_LINE_LEN + 1];
  const char *lines[TSTAT_LINES + 1];
  char_data *imm = make_imm();
  int i;

  for (i = 0; i < TSTAT_LINES; i++) {
    memset(text[i], 'n', TSTAT_LINE_LEN);
    text[i][TSTAT_LINE_LEN] = '\0';
    lines[i] = text[i];
  }
  lines[TSTAT_LINES] = NULL;
  /* Attached only so tearDown frees the instance. */
  attach_to_script(&SCRIPT(imm), make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, lines));
  immortal_command(do_tstat, imm, " 9000");
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("tstat-truncate", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "9000"));
}

/* D1-17: read_saved_vars() parsed legacy plrvars lines into 32- and 16-byte
 * buffers with any_one_arg(), which writes up to MAX_INPUT_LENGTH bytes. */
static void legacy_saved_var_with_long_name_is_read_in_bounds(void)
{
  char dir[] = "/tmp/tb_plrvars_XXXXXX", cwd[4096], name[71];
  char_data *ch = make_mob("zzvars");
  FILE *fl;

  TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
  TEST_ASSERT_NOT_NULL(mkdtemp(dir));
  TEST_ASSERT_EQUAL_INT(0, chdir(dir));
  TEST_ASSERT_EQUAL_INT(0, mkdir("plrvars", 0700));
  TEST_ASSERT_EQUAL_INT(0, mkdir("plrvars/U-Z", 0700));
  memset(name, 'v', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  fl = fopen("plrvars/U-Z/zzvars.mem", "w");
  TEST_ASSERT_NOT_NULL(fl);
  fprintf(fl, "%s 12345678901234567890 value\n", name);
  fclose(fl);

  read_saved_vars(ch);

  remove("plrvars/U-Z/zzvars.mem");
  rmdir("plrvars/U-Z");
  rmdir("plrvars");
  TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
  rmdir(dir);
  TEST_ASSERT_NOT_NULL(SCRIPT(ch));
  TEST_ASSERT_NOT_NULL(SCRIPT(ch)->global_vars);
  TEST_ASSERT_EQUAL_STRING(name, SCRIPT(ch)->global_vars->name);
  TEST_ASSERT_EQUAL_STRING("value", SCRIPT(ch)->global_vars->value);
}

/* D1-18: %obj.extra% passed sizeof(str) (a pointer) as the capacity, so the
 * flag list was silently cut to seven bytes. */
static void obj_extra_lists_every_flag(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  obj_data *o = make_obj("thing");
  char out[MAX_INPUT_LENGTH];

  SET_BIT_AR(GET_OBJ_EXTRA(o), ITEM_GLOW);
  SET_BIT_AR(GET_OBJ_EXTRA(o), ITEM_HUM);
  SET_BIT_AR(GET_OBJ_EXTRA(o), ITEM_MAGIC);
  uid_var(t, "o", o->script_id);
  subst(mob, t, "x %o.extra%", out);
  TEST_ASSERT_EQUAL_STRING("x GLOW HUM MAGIC ", out);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* D1-20: setters added a script int to sbyte/sh_int fields in int and stored
 * the wrapped result; cha(250) wrapped before its 3..max clamp ran, and
 * wait(INT_MAX) overflowed int (UBSan). */
static void field_setters_saturate_instead_of_wrapping(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  uid_var(t, "a", mob->script_id);
  GET_HIT(mob) = 100;
  subst(mob, t, "%a.hitp(40000)%", out);
  TEST_ASSERT_EQUAL_INT(SHRT_MAX, GET_HIT(mob));
  TEST_ASSERT_EQUAL_STRING("field-clamp", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "hitp"));

  tb_error_capture_reset();
  mob->real_abils.cha = 10;
  subst(mob, t, "%a.cha(250)%", out);
  TEST_ASSERT_EQUAL_INT(25, mob->real_abils.cha);   /* the NPC game maximum */
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  subst(mob, t, "%a.wait(2147483647)%", out);
  TEST_ASSERT_EQUAL_INT(INT_MAX, GET_WAIT_STATE(mob));
  TEST_ASSERT_EQUAL_STRING("field-clamp", tb_captured_operation);
  GET_WAIT_STATE(mob) = 0;

  tb_error_capture_reset();
  script_damage(mob, -40000);
  TEST_ASSERT_EQUAL_INT(GET_MAX_HIT(mob), GET_HIT(mob));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* D1-21: dg_affect narrowed its duration to sh_int and its value to sbyte, so
 * a duration of 65536 stored -1 (permanent) and a value of 200 stored -56. */
static void dg_affect_out_of_range_is_refused(void)
{
  char line[96];
  const char *lines[] = { line, NULL };
  char_data *target = make_mob("target");

  snprintf(line, sizeof(line), "dg_affect %c%ld SANCT 1 65536", UID_CHAR,
           target->script_id);
  run_mob_script(make_mob("mob"), lines);
  TEST_ASSERT_NULL(target->affected);
  TEST_ASSERT_EQUAL_STRING("affect-range", tb_captured_operation);

  tb_error_capture_reset();
  snprintf(line, sizeof(line), "dg_affect %c%ld str 200 10", UID_CHAR,
           target->script_id);
  run_mob_script(make_mob("mob2"), lines);
  TEST_ASSERT_NULL(target->affected);
  TEST_ASSERT_EQUAL_STRING("affect-range", tb_captured_operation);

  tb_error_capture_reset();
  snprintf(line, sizeof(line), "dg_affect %c%ld str 2 10", UID_CHAR,
           target->script_id);
  run_mob_script(make_mob("mob3"), lines);
  TEST_ASSERT_NOT_NULL(target->affected);
  TEST_ASSERT_EQUAL_INT(2, target->affected->modifier);
  TEST_ASSERT_EQUAL_INT(9, target->affected->duration);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  affect_from_char(target, SPELL_DG_AFFECT);
}

/* D1-22: is_thief(on) on a mob set PLR_THIEF in the shared flag array, which
 * is MOB_SENTINEL, while reading back 0. */
static void player_flag_setters_ignore_mobs(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  uid_var(t, "a", mob->script_id);
  subst(mob, t, "%a.is_thief(on)%", out);
  TEST_ASSERT_FALSE(MOB_FLAGGED(mob, MOB_SENTINEL));
  TEST_ASSERT_EQUAL_STRING("0", out);
  TEST_ASSERT_EQUAL_STRING("field-target", tb_captured_operation);
}

/* D1-23: trig_wait_event() scanned rooms with i < top_of_world, so a wait in
 * the last room resumed as "unknown entity" and left the trigger at depth 1. */
static void wait_in_last_room_resumes(void)
{
  const char *lines[] = { "wait 1", "set done 1", NULL };
  room_data *room = &world[top_of_world];
  trig_data *t = make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, lines);
  int i;

  attach_to_script(&SCRIPT(room), t);
  script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_EQUAL_INT(1, GET_TRIG_DEPTH(t));
  for (i = 0; i < 4 * PASSES_PER_SEC && GET_TRIG_WAIT(t); i++) {
    pulse++;
    event_process();
  }
  TEST_ASSERT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* D1-24: eval saturated out-of-range operands and results and turned x / 0
 * into 0 without any report. */
static void saturated_arithmetic_is_reported(void)
{
  const char *ok[] = { "eval x 2 + 3", NULL };
  const char *big[] = { "eval x 2147483647 + 1", NULL };
  const char *zero[] = { "eval x 5 / 0", NULL };

  run_mob_script(make_mob("mob"), ok);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  run_mob_script(make_mob("mob2"), big);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("script-arith", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "result"));
  run_mob_script(make_mob("mob3"), zero);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "division by zero"));
}

/* D1-26: trim formed tmpvar - 1 for an empty value (undefined behaviour the
 * sanitizers do not flag); check the edge cases of the index rewrite. */
static void trim_handles_empty_and_blank_values(void)
{
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  add_var(&GET_TRIG_VARS(t), "e", "", 0);
  add_var(&GET_TRIG_VARS(t), "b", "   ", 0);
  add_var(&GET_TRIG_VARS(t), "w", "  a b  ", 0);
  subst(mob, t, "x [%e.trim%][%b.trim%][%w.trim%]", out);
  TEST_ASSERT_EQUAL_STRING("x [][][a b]", out);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* D1-27: remote to a target without a script and a detach that matched
 * nothing returned silently. */
static void lost_remote_and_detach_miss_are_reported(void)
{
  const char *quiet[] = { "nop", NULL };
  char remote_line[64], detach_line[64];
  const char *remote_lines[] = { "set flag 1", remote_line, NULL };
  const char *detach_lines[] = { detach_line, NULL };
  char_data *bare = make_mob("bare"), *scripted = make_mob("scripted");

  attach_to_script(&SCRIPT(scripted), make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, quiet));
  snprintf(remote_line, sizeof(remote_line), "remote flag %ld", bare->script_id);
  run_mob_script(make_mob("mob"), remote_lines);
  TEST_ASSERT_EQUAL_STRING("remote-target", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "flag"));

  tb_error_capture_reset();
  snprintf(detach_line, sizeof(detach_line), "detach 4242 %ld",
           scripted->script_id);
  run_mob_script(make_mob("mob2"), detach_lines);
  TEST_ASSERT_EQUAL_STRING("detach-miss", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(TRIGGERS(SCRIPT(scripted)));
}

/* D1-28: add_var()'s rejection of a dotted name was a plain log() line. */
static void dotted_variable_name_is_reported(void)
{
  struct trig_var_data *vars = NULL;

  add_var(&vars, "a.b", "x", 0);
  TEST_ASSERT_NULL(vars);
  TEST_ASSERT_EQUAL_STRING("var-name", tb_captured_operation);
}

/* D2-02: near the end of the pulse counter's range, when + pulse no longer
 * fits the event key. event_create() parked the wait forever (reported
 * without the trigger) and the trigger stayed suspended at depth 1. */
static void wait_past_the_pulse_range_is_rejected(void)
{
  const char *lines[] = { "set x 1", "wait 10", "set y 2", NULL };
  trig_data *t;

  pulse = (unsigned long)LONG_MAX - 5;
  t = run_mob_script(make_mob("mob"), lines);
  TEST_ASSERT_NULL(GET_TRIG_WAIT(t));
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_NULL(GET_TRIG_VARS(t));
  TEST_ASSERT_TRUE(tb_captured_has_op("wait-overflow"));
  TEST_ASSERT_TRUE(tb_captured_has_op("wait-invalid"));
  /* Rejected before event_create(), whose clamp (event_key()) would have
   * reported event/event-time without naming the trigger. */
  TEST_ASSERT_FALSE(tb_captured_has_op("event-time"));
}

/* Run the event queue until t's wait has fired (at most four seconds). */
static void run_wait_out(trig_data *t)
{
  int i;

  for (i = 0; i < 4 * PASSES_PER_SEC && GET_TRIG_WAIT(t); i++) {
    pulse++;
    event_process();
  }
  TEST_ASSERT_NULL(GET_TRIG_WAIT(t));
}

/* D2-03: OLC add_room()/delete_room() RECREATE world[], which can move every
 * room, and update_wait_events() repoints only the rooms they shift. A room
 * wait kept the old &world[rnum], so it was dropped as an unknown owner (or
 * resumed on whatever room later reused the address). */
static void room_wait_survives_world_reallocation(void)
{
  const char *lines[] = { "wait 1", "set done 1", "global done", NULL };
  trig_data *t = make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, lines);
  room_data *old_world, *room;

  CREATE(old_world, room_data, NUM_TEST_ROOMS);
  memcpy(old_world, test_rooms, sizeof(test_rooms));
  world = old_world;
  room = &world[1];
  attach_to_script(&SCRIPT(room), t);
  script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));

  /* RECREATE(world) moved the array; nothing repointed this room's wait. */
  memcpy(test_rooms, old_world, sizeof(test_rooms));
  free(old_world);
  world = test_rooms;

  run_wait_out(t);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(SCRIPT(&world[1])->global_vars);
  TEST_ASSERT_EQUAL_STRING("done", SCRIPT(&world[1])->global_vars->name);
}

/* D2-03: a room wait whose room no longer resolves is reported with the
 * room's vnum and ends the trigger instead of leaving it at depth 1. */
static void room_wait_with_lost_room_is_reported(void)
{
  const char *lines[] = { "set x 1", "wait 1", "set y 2", NULL };
  trig_data *t = make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, lines);
  room_data *room = &world[top_of_world];

  attach_to_script(&SCRIPT(room), t);
  script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));

  world[top_of_world].number = 199;     /* vnum 102 no longer exists */
  run_wait_out(t);
  world[top_of_world].number = 100 + top_of_world;

  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_NULL(GET_TRIG_VARS(t));
  TEST_ASSERT_TRUE(TRIGGER_CHECK(t, WTRIG_RANDOM));
  TEST_ASSERT_EQUAL_STRING("wait-owner-lost", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 102"));
}

/* D1-19: lookups that index world[] by the owner's room never checked that
 * the owner is in one: %random.char% (mob and object owners), %random.dir%,
 * a mob's variable lookup by name and makeuid read world[NOWHERE]. Random
 * and time object triggers fire wherever the object is. */
static void unplaced_owner_room_lookups_are_reported(void)
{
  const char *idle[] = { "nop", NULL };
  const char *mk[] = { "makeuid found obj sword", NULL };
  obj_data *loose = make_obj("loose");         /* in no room, held by none */
  trig_data *ot = make_trigger(OBJ_TRIGGER, OTRIG_RANDOM, 100, idle);
  char_data *mob = make_mob("mob");
  trig_data *mt;
  char out[MAX_INPUT_LENGTH];
  char *line;

  attach_to_script(&SCRIPT(loose), ot);
  line = strdup("x %random.char% %random.dir%");
  TEST_ASSERT_NOT_NULL(line);
  var_subst(loose, SCRIPT(loose), ot, OBJ_TRIGGER, line, out);
  free(line);
  TEST_ASSERT_EQUAL_STRING("x  ", out);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("owner-unplaced", tb_captured_ops[0]);
  TEST_ASSERT_EQUAL_STRING("owner-unplaced", tb_captured_ops[1]);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum"));

  char_from_room(mob);
  mt = idle_mob_trigger(mob);
  tb_error_capture_reset();
  subst(mob, mt, "x %random.char%", out);
  TEST_ASSERT_EQUAL_STRING("x ", out);
  TEST_ASSERT_EQUAL_STRING("owner-unplaced", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum"));

  tb_error_capture_reset();
  add_var(&GET_TRIG_VARS(mt), "target", "nobody", 0);
  subst(mob, mt, "x %target.name%", out);
  TEST_ASSERT_EQUAL_STRING("owner-unplaced", tb_captured_operation);

  tb_error_capture_reset();
  run_mob_script(mob, mk);
  TEST_ASSERT_EQUAL_STRING("owner-unplaced", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "makeuid obj"));
}

/* E1-20: masound, mpurge and mload index world[IN_ROOM(ch)], and a mob's
 * script dispatched them (and every other command) without checking that
 * the mob stands in a room. */
static void unplaced_mob_script_runs_no_command(void)
{
  const char *lines[] = { "set before 1", "masound hello", "set after 1", NULL };
  char_data *mob = make_mob("mob");
  trig_data *t;

  char_from_room(mob);
  t = run_mob_script(mob, lines);
  TEST_ASSERT_EQUAL_STRING("mob-cmd-unplaced", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "line 2"));
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_NULL(GET_TRIG_VARS(t));
}

/* DGDOC-1: %x.level(N)% clamped every target below LVL_IMMORT, so any
 * builder's script demoted an immortal who set it off; the clamp itself
 * was silent. */
static void level_setter_never_changes_an_immortal(void)
{
  char_data *imm = make_imm();
  char_data *mob = make_mob("mob");
  trig_data *t = idle_mob_trigger(mob);
  char out[MAX_INPUT_LENGTH];

  uid_var(t, "imm", char_script_id(imm));
  subst(mob, t, "x %imm.level(1)%", out);
  TEST_ASSERT_EQUAL_INT(LVL_IMPL, GET_LEVEL(imm));
  TEST_ASSERT_EQUAL_STRING("field-target", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "idnum 4242"));

  tb_error_capture_reset();
  GET_LEVEL(mob) = 5;
  uid_var(t, "m", char_script_id(mob));
  subst(mob, t, "x %m.level(12)%", out);
  TEST_ASSERT_EQUAL_INT(12, GET_LEVEL(mob));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  subst(mob, t, "x %m.level(500)%", out);
  TEST_ASSERT_EQUAL_INT(LVL_IMMORT - 1, GET_LEVEL(mob));
  TEST_ASSERT_EQUAL_STRING("field-clamp", tb_captured_operation);
}

/* DGDOC-2: do_attach checked the zone of the room the builder stood in, not
 * the target's, so a builder in their own zone could attach a trigger to a
 * mob or object anywhere (do_detach already checked the target). */
static void attach_checks_the_target_zone(void)
{
  char_data *b = make_imm(), *mob = make_mob("zzmob");
  obj_data *mine = make_proto_obj("zzmine"), *stray = make_obj("zzstray");

  GET_LEVEL(b) = LVL_BUILDER;      /* zone 1's builder, standing in zone 1 */
  GET_OLC_ZONE(b) = 1;
  obj_to_room(mine, 0);
  obj_to_room(stray, 0);           /* no prototype: belongs to no zone */

  immortal_command(do_attach, b, " mob 9000 zzmob");
  TEST_ASSERT_NULL(SCRIPT(mob));
  TEST_ASSERT_EQUAL_STRING("attach-denied", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));

  tb_error_capture_reset();
  immortal_command(do_attach, b, " obj 9000 zzstray");
  TEST_ASSERT_NULL(SCRIPT(stray));
  TEST_ASSERT_EQUAL_STRING("attach-denied", tb_captured_operation);

  tb_error_capture_reset();
  immortal_command(do_attach, b, " obj 9000 zzmine");
  TEST_ASSERT_NOT_NULL(SCRIPT(mine));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Give owner's script (made if missing) a global variable named flag. */
static void give_global_flag(struct script_data **script)
{
  if (!*script)
    CREATE(*script, struct script_data, 1);
  add_var(&(*script)->global_vars, "flag", "1", 0);
}

/* DGDOC-3: vdelete (a builder command) deleted the global variables of any
 * room, mob, object or player by UID, a player's saved quest flags
 * included, with no zone or level check. */
static void vdelete_checks_the_builders_authority(void)
{
  char_data *b = make_imm(), *mob = make_mob("zzmob"), *pc = make_mob("zzpc");
  char cmd[64];

  GET_LEVEL(b) = LVL_BUILDER;      /* zone 1's builder */
  GET_OLC_ZONE(b) = 1;
  REMOVE_BIT_AR(MOB_FLAGS(pc), MOB_ISNPC);
  GET_LEVEL(pc) = LVL_IMPL;        /* in zone 1, but outranks the builder */
  give_global_flag(&SCRIPT(mob));  /* no prototype: belongs to no zone */
  give_global_flag(&SCRIPT(pc));
  give_global_flag(&SCRIPT(&world[0]));

  snprintf(cmd, sizeof(cmd), " flag %ld", mob->script_id);
  immortal_command(do_vdelete, b, cmd);
  TEST_ASSERT_NOT_NULL(SCRIPT(mob)->global_vars);
  TEST_ASSERT_EQUAL_STRING("vdelete-denied", tb_captured_operation);

  tb_error_capture_reset();
  snprintf(cmd, sizeof(cmd), " flag %ld", pc->script_id);
  immortal_command(do_vdelete, b, cmd);
  TEST_ASSERT_NOT_NULL(SCRIPT(pc)->global_vars);
  TEST_ASSERT_EQUAL_STRING("vdelete-denied", tb_captured_operation);

  tb_error_capture_reset();
  snprintf(cmd, sizeof(cmd), " flag %ld", room_script_id(&world[0]));
  immortal_command(do_vdelete, b, cmd);
  TEST_ASSERT_NULL(SCRIPT(&world[0])->global_vars);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* DGDOC-4: extract_char() only flags a character, and its waits fire until
 * extract_pending_chars() ends the pulse, so a wait expiring in the pulse
 * its mob died resumed the script for the dead mob. */
static void wait_of_an_extracted_mob_does_not_resume(void)
{
  const char *lines[] = { "set after 1", "wait 1", "global after", NULL };
  char_data *mob = make_mob("mob");
  trig_data *t = run_mob_script(mob, lines);

  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(t));
  extract_char(mob);               /* killed or purged this pulse */
  run_wait_out(t);
  TEST_ASSERT_NULL(SCRIPT(mob)->global_vars);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_NULL(GET_TRIG_VARS(t));
  TEST_ASSERT_EQUAL_STRING("owner-extracted", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "suspended run ended"));
}

/* DGDOC-4: only 'mpurge self' stopped a mob's script; a mob killed by its
 * own command (die() -> extract_char()) went on running its remaining
 * lines. */
static void script_stops_once_its_mob_is_extracted(void)
{
  const char *lines[] = { "set after 1", "mdamage %self% 1000", "global after", NULL };
  char_data *owner = make_mob("zzowner");
  trig_data *t = run_mob_script(owner, lines);

  TEST_ASSERT_TRUE(MOB_FLAGGED(owner, MOB_NOTDEADYET));
  TEST_ASSERT_NULL(SCRIPT(owner)->global_vars);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_EQUAL_STRING("owner-extracted", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "line 2"));
}

/* 'mpurge' of itself is the deliberate way for a mob's script to end its
 * owner (stock triggers write "%purge% %self%"): the run stops after that
 * line through the normal cleanup, and nothing is reported. */
static void mob_purging_itself_stops_without_a_report(void)
{
  const char *lines[] = { "set after 1", "%purge% %self%", "global after", NULL };
  char_data *owner = make_mob("zzpurger");
  trig_data *t = run_mob_script(owner, lines);

  TEST_ASSERT_TRUE(MOB_FLAGGED(owner, MOB_NOTDEADYET));
  TEST_ASSERT_NULL(SCRIPT(owner)->global_vars);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(t));
  TEST_ASSERT_NULL(GET_TRIG_VARS(t));
  TEST_ASSERT_FALSE(tb_captured_has_op("owner-extracted"));
}

/* DGDOC-5: a damage trigger's result is the damage dealt, and a script that
 * ends without 'return' made script_driver() return 1, so a damage trigger
 * that merely reacted to hits cut every hit on its mob to 1 point. */
static void damage_trigger_without_return_keeps_the_damage(void)
{
  const char *react[] = { "set seen %damage%", NULL };
  const char *halve[] = { "eval half %damage% / 2", "return %half%", NULL };
  const char *plain[] = { "set x 1", NULL };
  char_data *attacker = make_mob("attacker");
  char_data *reacting = make_mob("reacting"), *halving = make_mob("halving");
  char_data *other = make_mob("other");
  trig_data *t;

  attach_to_script(&SCRIPT(reacting), make_trigger(MOB_TRIGGER, MTRIG_DAMAGE, 100, react));
  attach_to_script(&SCRIPT(halving), make_trigger(MOB_TRIGGER, MTRIG_DAMAGE, 100, halve));
  TEST_ASSERT_EQUAL_INT(37, damage_mtrigger(attacker, reacting, 37, 0));
  TEST_ASSERT_EQUAL_INT(20, damage_mtrigger(attacker, halving, 40, 0));

  /* The default applies to that one run only. */
  t = make_trigger(MOB_TRIGGER, MTRIG_SPEECH, 0, plain);
  attach_to_script(&SCRIPT(other), t);
  TEST_ASSERT_EQUAL_INT(1, script_driver(&other, t, MOB_TRIGGER, TRIG_NEW));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Run a one-trigger script on room world[1] from the top. */
static void run_room_script(const char **lines)
{
  room_data *room = &world[1];
  trig_data *t = make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, lines);

  attach_to_script(&SCRIPT(room), t);
  script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
}

/* Value of world[1]'s global variable name, or NULL. */
static const char *room_global(const char *name)
{
  struct trig_var_data *v;

  for (v = SCRIPT(&world[1]) ? SCRIPT(&world[1])->global_vars : NULL; v; v = v->next)
    if (!strcmp(v->name, name))
      return v->value;
  return NULL;
}

/* DGDOC-6: control keywords were matched as prefixes, so a line that only
 * began with one (here "elsewhere") opened the else branch, and a glued
 * "elseif%x% == 3" was taken as a plain else. "endif" still ends a block. */
static void control_keywords_match_whole_words(void)
{
  const char *elsewhere[] = { "if 0", "set a 1", "elsewhere", "set a 2",
                              "end", "global a", NULL };
  const char *glued[] = { "set x 2", "if %x% == 1", "set d 1",
                          "elseif%x% == 3", "set d 2", "else", "set d 3",
                          "end", "global d", NULL };
  const char *endif[] = { "if 1", "set c 1", "endif", "global c", NULL };

  run_room_script(elsewhere);
  TEST_ASSERT_NULL(room_global("a"));
  run_room_script(glued);
  TEST_ASSERT_EQUAL_STRING("3", room_global("d"));
  run_room_script(endif);
  TEST_ASSERT_EQUAL_STRING("1", room_global("c"));
}

/* DGDOC-6: the every-30-passes loop pause counted in cmdlist->loops, which
 * every instance of a trigger shares (and which kept its count after a loop
 * ended), so a second instance paused after 6 passes on the first's count.
 * The pause itself must still happen. */
static void while_pause_counts_per_trigger_instance(void)
{
  const char *lines[] = { "set i 0", "while %i% < 25", "eval i %i% + 1",
                          "done", NULL };
  const char *long_loop[] = { "set i 0", "while %i% < 40", "eval i %i% + 1",
                              "done", NULL };
  room_data *ra = &world[1], *rb = &world[2], *rc = &world[0];
  trig_data *a = make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, lines);
  trig_data *b = read_trigger(0), *c;

  TEST_ASSERT_TRUE(a->cmdlist == b->cmdlist);
  attach_to_script(&SCRIPT(ra), a);
  attach_to_script(&SCRIPT(rb), b);
  script_driver(&ra, a, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_NULL(GET_TRIG_WAIT(a));
  script_driver(&rb, b, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_NULL(GET_TRIG_WAIT(b));
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_DEPTH(b));

  c = make_trigger(WLD_TRIGGER, WTRIG_RANDOM, 0, long_loop);
  attach_to_script(&SCRIPT(rc), c);
  script_driver(&rc, c, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_NOT_NULL(GET_TRIG_WAIT(c));     /* paused at 30 passes */
  TEST_ASSERT_EQUAL_INT(30, GET_TRIG_LOOPS(c));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Player-file fuzzer case vars-noname: read_saved_vars() ignored
 * get_filename()'s result and fopen()ed its unwritten buffer for a player
 * whose pfile left the name empty; save_char_vars() would also unlink and
 * create that garbage path. */
static void nameless_player_variable_file_is_refused(void)
{
  char_data *pc = make_mob("pc");
  char *name = pc->player.name;

  REMOVE_BIT_AR(MOB_FLAGS(pc), MOB_ISNPC);     /* a player ... */
  GET_IDNUM(pc) = 77;
  pc->player.name = NULL;                       /* ... with no Name tag */
  read_saved_vars(pc);
  TEST_ASSERT_EQUAL_STRING("vars-filename", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "idnum 77"));
  TEST_ASSERT_NOT_NULL(SCRIPT(pc));            /* as when there is no file */

  tb_error_capture_reset();
  add_var(&SCRIPT(pc)->global_vars, "flag", "1", 0);
  save_char_vars(pc);
  TEST_ASSERT_EQUAL_STRING("vars-filename", tb_captured_operation);
  pc->player.name = name;
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
  RUN_TEST(purging_owner_container_stops_script);
  RUN_TEST(nested_self_purge_does_not_end_outer_script);
  RUN_TEST(switch_with_unterminated_nested_while_is_reported);
  RUN_TEST(break_without_done_is_reported);
  RUN_TEST(well_formed_switch_still_selects_its_case);
  RUN_TEST(object_uid_never_aliases_a_room);
  RUN_TEST(wrapping_room_vnum_is_rejected);
  RUN_TEST(detach_object_without_trigger_argument_is_refused);
  RUN_TEST(detach_mobile_without_trigger_argument_is_refused);
  RUN_TEST(detach_does_not_instantiate_a_trigger);
  RUN_TEST(attach_to_player_is_refused_before_allocating);
  RUN_TEST(self_detaching_script_frees_its_variables);
  RUN_TEST(extract_past_last_word_stops_and_reports);
  RUN_TEST(tstat_of_long_script_is_truncated_and_reported);
  RUN_TEST(legacy_saved_var_with_long_name_is_read_in_bounds);
  RUN_TEST(obj_extra_lists_every_flag);
  RUN_TEST(field_setters_saturate_instead_of_wrapping);
  RUN_TEST(dg_affect_out_of_range_is_refused);
  RUN_TEST(player_flag_setters_ignore_mobs);
  RUN_TEST(wait_in_last_room_resumes);
  RUN_TEST(saturated_arithmetic_is_reported);
  RUN_TEST(trim_handles_empty_and_blank_values);
  RUN_TEST(lost_remote_and_detach_miss_are_reported);
  RUN_TEST(dotted_variable_name_is_reported);
  RUN_TEST(wait_past_the_pulse_range_is_rejected);
  RUN_TEST(room_wait_survives_world_reallocation);
  RUN_TEST(room_wait_with_lost_room_is_reported);
  RUN_TEST(unplaced_owner_room_lookups_are_reported);
  RUN_TEST(unplaced_mob_script_runs_no_command);
  RUN_TEST(level_setter_never_changes_an_immortal);
  RUN_TEST(attach_checks_the_target_zone);
  RUN_TEST(vdelete_checks_the_builders_authority);
  RUN_TEST(wait_of_an_extracted_mob_does_not_resume);
  RUN_TEST(script_stops_once_its_mob_is_extracted);
  RUN_TEST(mob_purging_itself_stops_without_a_report);
  RUN_TEST(damage_trigger_without_return_keeps_the_damage);
  RUN_TEST(control_keywords_match_whole_words);
  RUN_TEST(while_pause_counts_per_trigger_instance);
  RUN_TEST(nameless_player_variable_file_is_refused);
  return UNITY_END();
}
