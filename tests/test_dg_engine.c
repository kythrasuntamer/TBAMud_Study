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

/* Give the prototype a new command list and instantiate a trigger from it.
 * The instance shares the prototype's command list, as in the server. */
static trig_data *make_trigger(int attach_type, long trigger_type, int narg,
                               const char **lines)
{
  trig_data *t;

  free_cmdlist(test_proto.cmdlist);
  test_proto.cmdlist = make_cmdlist(lines);
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
  free_cmdlist(test_proto.cmdlist);
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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(car_of_overlong_word_is_truncated_and_reported);
  return UNITY_END();
}
