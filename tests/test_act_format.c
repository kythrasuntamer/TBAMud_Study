/* Executable tests of the production act()/perform_act() formatter.
 * comm.c is linked with main() renamed and unreferenced sections discarded.
 * The recipient has an act-trigger script and no descriptor, so SENDOK()
 * passes and act() returns the formatted line without network output.
 * That copy is not capitalized; CAP() applies only to descriptor output.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "dg_scripts.h"

#include "protocol.h"
#include "tb_error_capture.h"

/* Fixtures for symbols the formatter reaches outside comm.c (db.c globals
 * are defined here because db.c is not linked). */
struct room_data *world = NULL;
room_rnum top_of_world = 0;
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL; /* recipient diagnostics name mob vnums */
mob_rnum top_of_mobt = 0;
struct index_data *obj_index = NULL; /* no-target diagnostics name obj vnums */
obj_rnum top_of_objt = 0;
void basic_mud_log(const char *format, ...) { (void)format; }
/* CHECK_PLAYER_SPECIAL's reporter (utils.c). */
void tb_player_special_on_mob_at(const struct char_data *ch, const char *field,
                                 const char *source_file, int source_line,
                                 const char *source_func)
{ (void)ch; (void)field; (void)source_file; (void)source_line; (void)source_func; }
int room_is_dark_at(room_rnum room, const char *source_file, int source_line,
                    const char *source_func)
{ (void)room; (void)source_file; (void)source_line; (void)source_func; return 0; }
char *fname(const char *namelist) { return (char *)namelist; }
/* Reached only for descriptor output, which these tests never produce. */
char *CAP(char *txt) { return txt; }

/* Act triggers: count deliveries per recipient and optionally run a hook,
 * standing in for a script that moves characters (mteleport). */
static void (*act_trigger_hook)(const char_data *ch);
static int act_trigger_calls[4];
static char_data *act_trigger_seen[4];
void act_mtrigger(const char_data *ch, char *str, char_data *actor,
                  char_data *victim, obj_data *object, obj_data *target,
                  char *arg)
{
  int i;
  (void)str; (void)actor; (void)victim; (void)object; (void)target; (void)arg;
  for (i = 0; i < 4; i++)
    if (act_trigger_seen[i] == ch || act_trigger_seen[i] == NULL) {
      act_trigger_seen[i] = (char_data *)ch;
      act_trigger_calls[i]++;
      break;
    }
  if (act_trigger_hook)
    act_trigger_hook(ch);
}
static int act_trigger_count(const char_data *ch)
{
  int i;
  for (i = 0; i < 4; i++)
    if (act_trigger_seen[i] == ch)
      return act_trigger_calls[i];
  return 0;
}

/* Script UIDs: a small registry standing in for dg_scripts.c. */
static char_data *uid_table[8];
long char_script_id(char_data *ch)
{
  long i;
  for (i = 0; i < 8 && uid_table[i]; i++)
    if (uid_table[i] == ch)
      return i + 1;
  if (i == 8)
    abort();
  uid_table[i] = ch;
  return i + 1;
}
struct char_data *find_char(long n)
{
  return (n >= 1 && n <= 8) ? uid_table[n - 1] : NULL;
}
const char *ProtocolOutput(descriptor_t *d, const char *data, int *length)
{ (void)d; (void)length; return data; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}

static char_data viewer;
static struct script_data viewer_script;

void setUp(void)
{
  memset(&viewer, 0, sizeof(viewer));
  memset(&viewer_script, 0, sizeof(viewer_script));
  viewer_script.types = MTRIG_ACT;
  viewer.script = &viewer_script;
  viewer.player_specials = &dummy_mob;
  GET_POS(&viewer) = POS_STANDING;
  tb_error_capture_reset();
}
void tearDown(void) {}

static void plain_message_gets_line_ending(void)
{
  const char *out = act("hello", FALSE, &viewer, NULL, NULL, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQUAL_STRING("hello\r\n", out);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}
static void text_substitution_is_copied(void)
{
  const char *out = act("says '$T'", FALSE, &viewer, NULL, "hi", TO_CHAR);
  TEST_ASSERT_EQUAL_STRING("says 'hi'\r\n", out);
}
/* A trailing '$' previously advanced past the format's terminator. */
static void trailing_dollar_stops_at_terminator(void)
{
  /* Heap copy so ASan sees any read past the end of the format. */
  char *fmt = strdup("cost 5$");
  TEST_ASSERT_NOT_NULL(fmt);
  const char *out = act(fmt, FALSE, &viewer, NULL, NULL, TO_CHAR);
  free(fmt); /* out is the formatter's own copy */
  TEST_ASSERT_EQUAL_STRING("cost 5\r\n", out);
  /* Reported with the format as the call-site key. */
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format=\"cost 5$\""));
  /* The act() caller's own location is named. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_act_format.c:"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, " trailing_dollar_stops_at_terminator"));
}
/* $T text longer than the output buffer previously overflowed the stack. */
static void oversized_substitution_is_truncated(void)
{
  size_t len = MAX_STRING_LENGTH * 2;
  char *huge = malloc(len + 1);
  TEST_ASSERT_NOT_NULL(huge);
  memset(huge, 'x', len);
  huge[len] = '\0';
  const char *out = act("$T tail", FALSE, &viewer, NULL, huge, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  size_t out_len = strlen(out);
  TEST_ASSERT_EQUAL_size_t(MAX_STRING_LENGTH - 1, out_len);
  TEST_ASSERT_EQUAL_STRING("\r\n", out + out_len - 2);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-output", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format=\"$T tail\""));
  free(huge);
}
static void illegal_code_is_reported_escaped(void)
{
  const char *out = act("bad $Z \"quote\"\n", FALSE, &viewer, NULL, NULL, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-parse", tb_captured_operation);
  /* Quotes and control characters cannot break the log record. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format=\"bad $Z ?quote??\""));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "0x5a at offset 5"));
  /* The recipient identity pair is expanded, not printed literally. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "(recipient player idnum 0)"));
  TEST_ASSERT_NULL(strstr(tb_captured_detail, "DIAG_CHAR_FMT"));
}
static void oversized_literal_is_truncated(void)
{
  size_t len = MAX_STRING_LENGTH * 2;
  char *huge = malloc(len + 1);
  TEST_ASSERT_NOT_NULL(huge);
  memset(huge, 'y', len);
  huge[len] = '\0';
  const char *out = act(huge, FALSE, &viewer, NULL, NULL, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQUAL_size_t(MAX_STRING_LENGTH - 1, strlen(out));
  free(huge);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  /* The excerpt is bounded and marked as cut. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "yyy...\""));
}

/* TO_ROOM delivery when an act trigger moves recipients (mteleport): mob_a's
 * trigger sends mob_b and then mob_a itself to room 3002, where mob_c is.
 * The old loop then followed mob_a->next_in_room through room 3002's list,
 * delivering to mob_b and mob_c in the wrong room. */
static struct room_data rooms[2];
static char_data actor, mob_a, mob_b, mob_c;
static struct script_data mob_script;

static void move_to_room(char_data *ch, room_rnum dest)
{
  char_data **p = &world[IN_ROOM(ch)].people;
  while (*p && *p != ch)
    p = &(*p)->next_in_room;
  if (*p)
    *p = ch->next_in_room;
  ch->in_room = dest;
  ch->next_in_room = world[dest].people; /* char_to_room() prepends */
  world[dest].people = ch;
}
static void mob_a_teleports_mob_b(const char_data *ch)
{
  if (ch == &mob_a && IN_ROOM(&mob_a) == 0) {
    move_to_room(&mob_b, 1);
    move_to_room(&mob_a, 1);
  }
}
static void make_mob(char_data *m)
{
  memset(m, 0, sizeof(*m));
  SET_BIT_AR(MOB_FLAGS(m), MOB_ISNPC);
  m->nr = NOBODY;
  m->script = &mob_script;
  m->player_specials = &dummy_mob;
  GET_POS(m) = POS_STANDING;
}
static void recipient_moved_by_trigger_is_skipped(void)
{
  memset(rooms, 0, sizeof(rooms));
  memset(uid_table, 0, sizeof(uid_table));
  memset(act_trigger_calls, 0, sizeof(act_trigger_calls));
  memset(act_trigger_seen, 0, sizeof(act_trigger_seen));
  memset(&mob_script, 0, sizeof(mob_script));
  mob_script.types = MTRIG_ACT;
  rooms[0].number = 3001;
  rooms[1].number = 3002;
  world = rooms;
  top_of_world = 1;

  memset(&actor, 0, sizeof(actor));
  actor.player_specials = &dummy_mob;
  actor.player.name = (char *)"Bob";
  GET_POS(&actor) = POS_STANDING;
  make_mob(&mob_a);
  make_mob(&mob_b);
  make_mob(&mob_c);
  /* Room 3001: actor, mob_a, mob_b. Room 3002: mob_c. */
  mob_c.in_room = 1;
  rooms[1].people = &mob_c;
  actor.in_room = mob_a.in_room = mob_b.in_room = 0;
  rooms[0].people = &actor;
  actor.next_in_room = &mob_a;
  mob_a.next_in_room = &mob_b;
  act_trigger_hook = mob_a_teleports_mob_b;

  act("$n waves.", FALSE, &actor, NULL, NULL, TO_ROOM);

  act_trigger_hook = NULL;
  TEST_ASSERT_EQUAL_INT(1, act_trigger_count(&mob_a));
  TEST_ASSERT_EQUAL_INT(0, act_trigger_count(&mob_b));
  TEST_ASSERT_EQUAL_INT(0, act_trigger_count(&mob_c));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("recipient-moved", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "left room vnum 3001"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_act_format.c:"));
  world = NULL;
  top_of_world = 0;
}

/* A nested act(..., DG_NO_TRIG) from an act trigger (do_say in a script)
 * cleared the file-global dg_act_check for the rest of the outer TO_ROOM
 * delivery, so later recipients silently lost their act triggers. */
static void mob_a_says_without_triggers(const char_data *ch)
{
  if (ch == &mob_a)
    act("You say 'hi'", FALSE, &mob_a, NULL, NULL, TO_CHAR | DG_NO_TRIG);
}
static void nested_no_trig_act_keeps_outer_triggers(void)
{
  memset(rooms, 0, sizeof(rooms));
  memset(uid_table, 0, sizeof(uid_table));
  memset(act_trigger_calls, 0, sizeof(act_trigger_calls));
  memset(act_trigger_seen, 0, sizeof(act_trigger_seen));
  memset(&mob_script, 0, sizeof(mob_script));
  mob_script.types = MTRIG_ACT;
  rooms[0].number = 3001;
  world = rooms;
  top_of_world = 1;
  memset(&actor, 0, sizeof(actor));
  actor.player_specials = &dummy_mob;
  actor.player.name = (char *)"Bob";
  GET_POS(&actor) = POS_STANDING;
  make_mob(&mob_a);
  make_mob(&mob_b);
  actor.in_room = mob_a.in_room = mob_b.in_room = 0;
  rooms[0].people = &actor;
  actor.next_in_room = &mob_a;
  mob_a.next_in_room = &mob_b;
  act_trigger_hook = mob_a_says_without_triggers;

  act("$n waves.", FALSE, &actor, NULL, NULL, TO_ROOM);

  act_trigger_hook = NULL;
  TEST_ASSERT_EQUAL_INT(1, act_trigger_count(&mob_a));
  TEST_ASSERT_EQUAL_INT(1, act_trigger_count(&mob_b));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  world = NULL;
  top_of_world = 0;
}

/* A room message with nobody placed was logged as a bare SYSERR. */
static void room_act_without_target_is_reported(void)
{
  viewer.in_room = NOWHERE;
  TEST_ASSERT_NULL(act("$n waves.", FALSE, &viewer, NULL, NULL, TO_ROOM));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("no-target", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_act_format.c:"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "actor player idnum 0"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(plain_message_gets_line_ending);
  RUN_TEST(text_substitution_is_copied);
  RUN_TEST(trailing_dollar_stops_at_terminator);
  RUN_TEST(oversized_substitution_is_truncated);
  RUN_TEST(illegal_code_is_reported_escaped);
  RUN_TEST(oversized_literal_is_truncated);
  RUN_TEST(recipient_moved_by_trigger_is_skipped);
  RUN_TEST(nested_no_trig_act_keeps_outer_triggers);
  RUN_TEST(room_act_without_target_is_reported);
  return UNITY_END();
}
