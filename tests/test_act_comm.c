/* Executable tests of the production communication commands (act.comm.c)
 * and socials (act.social.c). Both are linked with unreferenced sections discarded. act() is
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
/* CHECK_PLAYER_SPECIAL's reporter (utils.c, not linked here). */
void tb_player_special_on_mob_at(const struct char_data *ch, const char *field,
                                 const char *source_file, int source_line,
                                 const char *source_func)
{ (void)ch; (void)field; (void)source_file; (void)source_line; (void)source_func; }

int MAX(int a, int b) { return a > b ? a : b; }
struct social_messg *soc_mess_list = NULL;
int top_of_socialt = -1;

/* Argument splitters: the tests pass at most two short words. */
static char *copy_word(char *argument, char *out)
{
  size_t n = 0;

  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ' && n + 1 < MAX_INPUT_LENGTH)
    out[n++] = *argument++;
  out[n] = '\0';
  return argument;
}
char *one_argument(char *argument, char *first_arg)
{ return copy_word(argument, first_arg); }
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{ return copy_word(copy_word(argument, first_arg), second_arg); }

/* The social's victim: found by any name. */
static struct char_data *visible_victim;
struct char_data *get_char_vis(struct char_data *ch, char *name, int *number, int where)
{ (void)ch; (void)number; (void)where; return *name ? visible_victim : NULL; }
struct obj_data *get_obj_in_list_vis(struct char_data *ch, char *name, int *number,
                                     struct obj_data *list)
{ (void)ch; (void)name; (void)number; (void)list; return NULL; }

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void skip_spaces(char **string)
{
  while (**string == ' ')
    (*string)++;
}
/* Counts calls: legal_communication() decides whether parse_at() runs. */
static int parse_at_calls;
void parse_at(char *str) { (void)str; parse_at_calls++; }
void speech_mtrigger(char_data *actor, char *str) { (void)actor; (void)str; }
void speech_wtrigger(char_data *actor, char *str) { (void)actor; (void)str; }
int room_is_dark_at(room_rnum room, const char *src_file, int src_line, const char *src_func) { (void)src_file; (void)src_line; (void)src_func;  (void)room; return 0; }
/* History recorder: how many entries, for whom, and whether any was NULL. */
static int history_calls, history_null_calls;
static struct char_data *history_last_ch;
void add_history(struct char_data *ch, char *str, int type)
{
  (void)type;
  history_calls++;
  history_last_ch = ch;
  if (!str)
    history_null_calls++;
}
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{ (void)ch; (void)messg; return 0; }

/* act() recorder: the last format, its vict_obj and how many calls. */
static char act_format[MAX_STRING_LENGTH];
static const void *act_vict_obj;
static const void *act_obj;
static int act_calls;
/* act() delivers (returns text) only to this recipient; NULL otherwise. */
static void *act_delivers_to;
static char act_delivered[] = "delivered";
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func;
  (void)hide_invisible; (void)ch; (void)type;
  act_calls++;
  act_vict_obj = vict_obj;
  act_obj = obj;
  if (snprintf(act_format, sizeof(act_format), "%s", str ? str : "") < 0)
    act_format[0] = '\0';
  if (act_delivers_to && vict_obj == act_delivers_to)
    return act_delivered;
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
  parse_at_calls = 0;
  act_delivers_to = NULL;
  history_calls = history_null_calls = 0;
  history_last_ch = NULL;
  CONFIG_SPECIAL_IN_COMM = 0;
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

/* --- G2-08: body-part socials pass a string as act()'s obj ------------ */

static struct social_messg lick;

static void setup_lick_social(const char *char_body_found)
{
  static struct char_data victim;
  static struct player_special_data victim_specials;

  memset(&victim, 0, sizeof(victim));
  memset(&victim_specials, 0, sizeof(victim_specials));
  victim.player_specials = &victim_specials;
  GET_POS(&victim) = POS_STANDING;
  visible_victim = &victim;
  memset(&lick, 0, sizeof(lick));
  lick.command = (char *)"lick";
  lick.act_nr = 300;
  lick.char_found = (char *)"You lick $N.";
  lick.char_body_found = (char *)char_body_found;
  lick.others_body_found = (char *)"$n licks $N's $t.";
  lick.vict_body_found = (char *)"$n licks your $t.";
  soc_mess_list = &lick;
  top_of_socialt = 0;
}

static void body_social_using_obj_code_is_refused(void)
{
  char *arg = strdup("bob ear");

  setup_lick_social("You lick $N's $p.");
  do_action(&speaker, arg, 300, 0);
  TEST_ASSERT_EQUAL_INT(0, act_calls);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("act-code", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "social 'lick' field char_body_found uses $p"));
  soc_mess_list = NULL;
  top_of_socialt = -1;
  free(arg);
}

static void body_social_using_t_runs(void)
{
  char *arg = strdup("bob ear");

  setup_lick_social("You lick $N's $t (costs $$1).");
  do_action(&speaker, arg, 300, 0);
  TEST_ASSERT_EQUAL_INT(3, act_calls);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  soc_mess_list = NULL;
  top_of_socialt = -1;
  free(arg);
}

/* --- G2-13: a social field read from a '#' line is NULL --------------- */

static void social_without_char_no_arg_is_reported(void)
{
  char *arg = strdup("");

  setup_lick_social("You lick $N's $t.");
  lick.char_no_arg = NULL;           /* fread_action() stored a '#' line */
  lick.others_no_arg = (char *)"$n licks the air.";
  do_action(&speaker, arg, 300, 0);
  TEST_ASSERT_EQUAL_INT(0, act_calls);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("social-field-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "social 'lick' has no char_no_arg"));
  soc_mess_list = NULL;
  top_of_socialt = -1;
  free(arg);
}

/* --- G2-14: history stores only what act() delivered ------------------ */

static void say_history_is_per_listener_and_never_null(void)
{
  static struct char_data awake, asleep;
  static struct player_special_data awake_sp, asleep_sp;
  char *text = strdup("hello");

  memset(&awake, 0, sizeof(awake));
  memset(&asleep, 0, sizeof(asleep));
  memset(&awake_sp, 0, sizeof(awake_sp));
  memset(&asleep_sp, 0, sizeof(asleep_sp));
  awake.player_specials = &awake_sp;
  asleep.player_specials = &asleep_sp;
  GET_POS(&awake) = POS_STANDING;
  GET_POS(&asleep) = POS_SLEEPING;
  speaker.next_in_room = &awake;
  awake.next_in_room = &asleep;
  act_delivers_to = &awake;     /* the speaker is linkless: TO_CHAR is NULL */

  do_say(&speaker, text, 0, 0);
  /* One act() per listener plus the speaker's own line. */
  TEST_ASSERT_EQUAL_INT(3, act_calls);
  TEST_ASSERT_EQUAL_INT(1, history_calls);
  TEST_ASSERT_EQUAL_PTR(&awake, history_last_ch);
  TEST_ASSERT_EQUAL_INT(0, history_null_calls);
  speaker.next_in_room = NULL;
  free(text);
}

/* --- G2-09: '@' as the last character of said text --------------------- */

static void trailing_at_sign_stays_inside_the_text(void)
{
  /* Exact-size heap copy: ASan reported the read one past the NUL. */
  char *text = strdup("hi@");

  CONFIG_SPECIAL_IN_COMM = 1;
  do_say(&speaker, text, 0, 0);
  TEST_ASSERT_EQUAL_INT(1, parse_at_calls);   /* still legal */
  TEST_ASSERT_EQUAL_STRING("You say, 'hi@'", act_format);
  free(text);

  text = strdup("bad @( code");
  do_say(&speaker, text, 0, 0);
  TEST_ASSERT_EQUAL_INT(1, parse_at_calls);   /* rejected: not parsed */
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
  RUN_TEST(body_social_using_obj_code_is_refused);
  RUN_TEST(body_social_using_t_runs);
  RUN_TEST(social_without_char_no_arg_is_reported);
  RUN_TEST(say_history_is_per_listener_and_never_null);
  RUN_TEST(trailing_at_sign_stays_inside_the_text);
  return UNITY_END();
}
