/* Executable tests of the production DG sub_write() message builder
 * (dg_comm.c): escape handling and token/buffer bounds. The recipient has an
 * act-trigger script and no descriptor, so SENDOK() passes; send_to_char()
 * is captured instead of reaching the network.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include <stdarg.h>
#include "tb_error_capture.h"

static char sent[MAX_STRING_LENGTH * 2];
static int sends, script_logs;

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(sent, sizeof(sent), messg, args) < 0)
    sent[0] = '\0';
  va_end(args);
  sends++;
  return strlen(sent);
}
void script_log(const char *format, ...) { (void)format; script_logs++; }
void basic_mud_log(const char *format, ...) { (void)format; }
int room_is_dark(room_rnum room) { (void)room; return 0; }
/* Name tokens resolve to nothing: sub_write renders "someone"/"something". */
struct char_data *get_char_in_room(room_data *room, char *name)
{ (void)room; (void)name; return NULL; }
struct char_data *get_char_room_vis(struct char_data *ch, char *name, int *number)
{ (void)ch; (void)name; (void)number; return NULL; }
obj_data *get_obj_in_room(room_data *room, char *name)
{ (void)room; (void)name; return NULL; }
struct obj_data *get_obj_in_list_vis(struct char_data *ch, char *name,
                                     int *number, struct obj_data *list)
{ (void)ch; (void)name; (void)number; (void)list; return NULL; }
struct obj_data *get_obj_in_equip_vis(struct char_data *ch, char *arg,
                                      int *number, struct obj_data *equipment[])
{ (void)ch; (void)arg; (void)number; (void)equipment; return NULL; }

static char_data speaker;
static struct script_data speaker_script;
static room_data room;

void setUp(void)
{
  memset(&speaker, 0, sizeof(speaker));
  memset(&speaker_script, 0, sizeof(speaker_script));
  memset(&room, 0, sizeof(room));
  speaker_script.types = MTRIG_ACT;
  speaker.script = &speaker_script;
  speaker.player_specials = &dummy_mob;
  GET_POS(&speaker) = POS_STANDING;
  speaker.in_room = 0;
  world = &room;
  top_of_world = 0;
  sent[0] = '\0';
  sends = script_logs = 0;
  tb_error_capture_reset();
}
void tearDown(void) { world = NULL; }

/* sub_write() modifies nothing in arg, but takes char *: use heap copies so
 * ASan reports any read past the terminator. */
static void deliver(const char *text)
{
  char *arg = strdup(text);
  TEST_ASSERT_NOT_NULL(arg);
  sub_write(arg, &speaker, TRUE, TO_CHAR);
  free(arg);
}

static void escaped_token_is_literal(void)
{
  deliver("a\\~b");
  TEST_ASSERT_EQUAL_STRING("a~b\n\r", sent);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}
/* A trailing backslash previously copied the NUL and read past it. */
static void trailing_backslash_stops_at_terminator(void)
{
  deliver("hello\\");
  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_STRING("hello\n\r", sent);
  /* Reported to the structured log and to builders. */
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(1, script_logs);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "unescaped backslash"));
}
/* One token per input byte previously could index tokens[] past its end. */
static void token_flood_is_bounded(void)
{
  char text[MAX_INPUT_LENGTH * 2 + 1];
  memset(text, '~', sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  deliver(text);
  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(1, script_logs);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "truncated"));
}
static void oversized_literal_is_bounded(void)
{
  size_t len = MAX_INPUT_LENGTH * 4;
  char *text = malloc(len + 1);
  TEST_ASSERT_NOT_NULL(text);
  memset(text, 'z', len);
  text[len] = '\0';
  deliver(text);
  free(text);
  TEST_ASSERT_EQUAL_INT(1, script_logs);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_TRUE(strlen(sent) < MAX_INPUT_LENGTH * 2 + 2);
}
/* A name longer than any_one_name()'s buffer is truncated and reported. */
static void oversized_name_is_reported(void)
{
  char text[MAX_INPUT_LENGTH + 64];
  text[0] = '~';
  memset(text + 1, 'n', sizeof(text) - 2);
  text[sizeof(text) - 1] = '\0';
  deliver(text);
  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("parse-name", tb_captured_operation);
}
static void missing_speaker_is_rejected(void)
{
  char text[] = "hello";
  sub_write(text, NULL, TRUE, TO_CHAR);
  TEST_ASSERT_EQUAL_INT(0, sends);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "invariant violated"));
}

/* E1-21: a misplaced speaker is named (vnum, UID, rnum), and an rnum past
 * the world table is rejected like NOWHERE instead of indexing world[]. */
static void misplaced_speaker_is_named(void)
{
  char text[] = "hello";
  SET_BIT_AR(MOB_FLAGS(&speaker), MOB_ISNPC);
  speaker.nr = NOBODY;
  speaker.script_id = 10000042;
  speaker.in_room = 5;           /* top_of_world is 0 */
  sub_write(text, &speaker, TRUE, TO_CHAR);
  TEST_ASSERT_EQUAL_INT(0, sends);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "uid 10000042 at rnum 5"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(misplaced_speaker_is_named);
  RUN_TEST(escaped_token_is_literal);
  RUN_TEST(trailing_backslash_stops_at_terminator);
  RUN_TEST(token_flood_is_bounded);
  RUN_TEST(oversized_literal_is_bounded);
  RUN_TEST(oversized_name_is_reported);
  RUN_TEST(missing_speaker_is_rejected);
  return UNITY_END();
}
