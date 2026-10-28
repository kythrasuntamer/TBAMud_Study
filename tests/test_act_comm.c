/* Executable tests of the production communication commands (act.comm.c).
 * act.comm.c is linked with unreferenced sections discarded. act() is
 * replaced by a recorder, so the tests see exactly which format each
 * command hands to the formatter (perform_act has its own tests).
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "act.h"
#include "dg_scripts.h"

#include "tb_error_capture.h"

/* --- Globals owned by files that are not linked ------------------------ */
struct room_data *world = NULL;
struct char_data *character_list = NULL;
struct descriptor_data *descriptor_list = NULL;
struct config_data config_info;
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL;
mob_rnum top_of_mobt = 0;
struct time_info_data time_info;
struct command_info *complete_cmd_info = NULL;
int MAX(int a, int b) { return a > b ? a : b; }

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void skip_spaces(char **string)
{
  while (**string == ' ')
    (*string)++;
}
void parse_at(char *str) { (void)str; }
void speech_mtrigger(char_data *actor, char *str) { (void)actor; (void)str; }
void speech_wtrigger(char_data *actor, char *str) { (void)actor; (void)str; }
int room_is_dark(room_rnum room) { (void)room; return 0; }
void add_history(struct char_data *ch, char *str, int type)
{ (void)ch; (void)str; (void)type; }
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{ (void)ch; (void)messg; return 0; }

/* act() recorder: the last format, its vict_obj and how many calls. */
static char act_format[MAX_STRING_LENGTH];
static const void *act_vict_obj;
static int act_calls;
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func;
  (void)hide_invisible; (void)ch; (void)obj; (void)type;
  act_calls++;
  act_vict_obj = vict_obj;
  if (snprintf(act_format, sizeof(act_format), "%s", str ? str : "") < 0)
    act_format[0] = '\0';
  return NULL;
}

static struct char_data speaker;
static struct player_special_data speaker_specials;
static struct room_data room;

void setUp(void)
{
  memset(&speaker, 0, sizeof(speaker));
  memset(&speaker_specials, 0, sizeof(speaker_specials));
  memset(&room, 0, sizeof(room));
  speaker.player_specials = &speaker_specials;
  speaker.player.name = (char *)"Speaker";
  GET_IDNUM(&speaker) = 77;
  GET_POS(&speaker) = POS_STANDING;
  IN_ROOM(&speaker) = 0;
  room.people = &speaker;
  world = &room;
  act_format[0] = '\0';
  act_vict_obj = NULL;
  act_calls = 0;
  tb_error_capture_reset();
}
void tearDown(void) {}

/* --- G2-07: caller text spliced into act() formats ---------------------- */

/* A forced "qsay $N is here" passed the text as the format AND as vict_obj,
 * so perform_act read the string's bytes as a char_data. */
static void qsay_from_script_cannot_inject_act_codes(void)
{
  char *text = strdup("$N is here");

  SET_BIT_AR(PRF_FLAGS(&speaker), PRF_QUEST);
  do_qcomm(&speaker, text, 0, SCMD_QSAY);
  TEST_ASSERT_EQUAL_STRING("You quest-say, '$$N is here'", act_format);
  TEST_ASSERT_NULL(act_vict_obj);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("act-code-in-text", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 77"));
  free(text);
}

static void qecho_text_is_no_longer_a_format(void)
{
  char *text = strdup("$p glows");

  SET_BIT_AR(PRF_FLAGS(&speaker), PRF_QUEST);
  do_qcomm(&speaker, text, 0, SCMD_QECHO);
  TEST_ASSERT_EQUAL_STRING("$$p glows", act_format);
  TEST_ASSERT_NULL(act_vict_obj);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  free(text);
}

/* process_input() doubles '$' typed on a socket: that text is unchanged. */
static void socket_text_with_escaped_dollars_is_unchanged(void)
{
  char *text = strdup("costs 5$$ or $$$$");

  do_say(&speaker, text, 0, 0);
  TEST_ASSERT_EQUAL_STRING("You say, 'costs 5$$ or $$$$'", act_format);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(text);
}

static void tell_text_cannot_reach_the_victim_pointer(void)
{
  char out[64];
  char *text = strdup("$T $t $N");

  ACT_ESCAPE_TEXT(&speaker, text, out);
  TEST_ASSERT_EQUAL_STRING("$$T $$t $$N", out);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "3 unescaped"));
  free(text);
}

static void escape_truncates_between_pairs_and_reports(void)
{
  char out[6];
  char *text = strdup("ab$$cd");

  ACT_ESCAPE_TEXT(&speaker, text, out);
  /* "ab$$" fits (4 + NUL); "c" would too, but never half a pair. */
  TEST_ASSERT_EQUAL_STRING("ab$$c", out);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("act-text-truncated", tb_captured_operation);
  free(text);

  tb_error_capture_reset();
  text = strdup("abcd$");
  ACT_ESCAPE_TEXT(&speaker, text, out);
  /* A lone '$' needs two bytes; only one is left, so it is dropped whole. */
  TEST_ASSERT_EQUAL_STRING("abcd", out);
  free(text);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(qsay_from_script_cannot_inject_act_codes);
  RUN_TEST(qecho_text_is_no_longer_a_format);
  RUN_TEST(socket_text_with_escaped_dollars_is_unchanged);
  RUN_TEST(tell_text_cannot_reach_the_victim_pointer);
  RUN_TEST(escape_truncates_between_pairs_and_reports);
  return UNITY_END();
}
