/* Regression tests for the production social editor (aedit.c).
 *
 * aedit.c, utils.c and random.c are linked with -ffunction-sections and
 * --gc-sections, so only the code these tests reach is kept.  The socials
 * and command tables, the OLC helpers (oasis.c), the social helpers
 * (act.social.c, interpreter.c) and the output functions are supplied
 * here.  aedit writes the socials file under LIB_MISC relative to the
 * working directory, so main() runs the suite inside a fresh temporary
 * directory.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "oasis.h"
#include "act.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* ---- output: everything sent to the builder since the last line ------- */

static char out_buf[MAX_STRING_LENGTH * 2];

static void out_append(const char *fmt, va_list args)
  __attribute__ ((format (printf, 1, 0)));

static void out_append(const char *fmt, va_list args)
{
  size_t used = strlen(out_buf);

  if (used < sizeof(out_buf) - 1 &&
      vsnprintf(out_buf + used, sizeof(out_buf) - used, fmt, args) < 0)
    out_buf[used] = '\0';
}

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  (void)d;
  va_start(args, txt);
  out_append(txt, args);
  va_end(args);
  return strlen(out_buf);
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  out_append(messg, args);
  va_end(args);
  return strlen(out_buf);
}

/* ---- OLC, interpreter and social collaborators ------------------------ */

const char *nrm = "", *grn = "", *cyn = "", *yel = "";
void get_char_colors(struct char_data *ch) { (void)ch; }
/* oasis.c's editor check: records what aedit asked for. */
static int editor_perm_asked = -1, editor_allowed = TRUE;
int can_use_editor(struct char_data *ch, int perm)
{ (void)ch; editor_perm_asked = perm; return editor_allowed; }
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return TRUE; }
int remove_from_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return TRUE; }
char *delete_doubledollar(char *string) { return string; }

/* interpreter.c: plain whitespace splitting and prefix matching. */
char *one_argument(char *argument, char *first_arg)
{
  size_t n = 0;

  while (*argument && isspace((unsigned char)*argument))
    argument++;
  while (*argument && !isspace((unsigned char)*argument) && n + 1 < MAX_INPUT_LENGTH)
    first_arg[n++] = *argument++;
  first_arg[n] = '\0';
  return argument;
}
int is_abbrev(const char *arg1, const char *arg2)
{
  return *arg1 && !strncmp(arg1, arg2, strlen(arg1));
}

const char *position_types[] = {
  "Dead", "Mortally wounded", "Incapacitated", "Stunned", "Sleeping",
  "Resting", "Sitting", "Fighting", "Standing", "\n"
};

struct social_messg *soc_mess_list = NULL;
int top_of_socialt = -1;
static struct command_info fixture_cmds[] = {
  { "RESERVED", "", 0, NULL, 0, 0 },
  { "look", "look", 0, NULL, 0, 0 },
  { "\n", "zzzzzzz", 0, NULL, 0, 0 }
};
struct command_info *complete_cmd_info = fixture_cmds;
void create_command_list(void) { }
void sort_commands(void) { }

/* As act.social.c: frees the strings, not the struct. */
void free_action(struct social_messg *mess)
{
  free(mess->command); free(mess->sort_as);
  free(mess->char_no_arg); free(mess->others_no_arg);
  free(mess->char_found); free(mess->others_found); free(mess->vict_found);
  free(mess->char_body_found); free(mess->others_body_found);
  free(mess->vict_body_found); free(mess->not_found);
  free(mess->char_auto); free(mess->others_auto);
  free(mess->char_obj_found); free(mess->others_obj_found);
  memset(mess, 0, sizeof(*mess));
}

/* The parts of oasis.c's cleanup_olc() aedit relies on.  It also frees the
 * OLC_ACTION struct after free_action(), so the fixture stays leak-clean. */
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  if (!d->olc)
    return;
  if (OLC_ACTION(d)) {
    if (cleanup_type == CLEANUP_ALL)
      free_action(OLC_ACTION(d));
    free(OLC_ACTION(d));
  }
  free(OLC_STORAGE(d));
  free(d->olc);
  d->olc = NULL;
  STATE(d) = CON_PLAYING;
}

/* ---- fixture ---------------------------------------------------------- */

static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;

static void social_fixture(void)
{
  top_of_socialt = 0;
  CREATE(soc_mess_list, struct social_messg, 1);
  soc_mess_list[0].command = strdup("smile");
  soc_mess_list[0].sort_as = strdup("smile");
  soc_mess_list[0].min_char_position = POS_RESTING;
  soc_mess_list[0].min_victim_position = POS_RESTING;
  soc_mess_list[0].char_no_arg = strdup("You smile.");
  soc_mess_list[0].char_found = strdup("You smile at $N.");
  soc_mess_list[0].others_found = strdup("$n smiles at $N.");
  config_info.operation.use_new_socials = 1;
}

void setUp(void)
{
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  memset(&builder_desc, 0, sizeof(builder_desc));
  builder.player_specials = &builder_specials;
  builder.player.name = (char *)"Builder";
  builder.player.level = LVL_IMPL;
  builder.char_specials.saved.idnum = 42;
  builder.nr = NOBODY;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
  builder_desc.connected = CON_PLAYING;
  descriptor_list = &builder_desc;
  social_fixture();
  editor_perm_asked = -1;
  editor_allowed = TRUE;
  out_buf[0] = '\0';
  tb_error_capture_reset();
}

void tearDown(void)
{
  int i;

  if (builder_desc.olc)
    cleanup_olc(&builder_desc, CLEANUP_ALL);
  for (i = 0; i <= top_of_socialt; i++)
    free_action(&soc_mess_list[i]);
  free(soc_mess_list);
  soc_mess_list = NULL;
  top_of_socialt = -1;
  descriptor_list = NULL;
}

/* 'aedit <arg>' as typed by the builder. */
static void aedit_cmd(const char *arg)
{
  char buf[MAX_INPUT_LENGTH];

  strlcpy(buf, arg, sizeof(buf));
  out_buf[0] = '\0';
  do_oasis_aedit(&builder, buf, 0, 0);
}

/* One line typed into the open editor. */
static void type(const char *line)
{
  char buf[MAX_INPUT_LENGTH];

  TEST_ASSERT_EQUAL_INT(CON_AEDIT, STATE(&builder_desc));
  strlcpy(buf, line, sizeof(buf));
  out_buf[0] = '\0';
  aedit_parse(&builder_desc, buf);
}

#define NAME_30 "abcdefghijabcdefghijabcdefghij"

/* I1-01: 'aedit <name>' started adding a social with a name of any length;
 * one such name widened every column of the socials listing until it
 * overran its buffer. */
static void test_aedit_refuses_overlong_new_social_name(void)
{
  aedit_cmd(" " NAME_30 "k");
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&builder_desc));
  TEST_ASSERT_EQUAL_STRING("social-name-length", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "length 31 over 30"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "at most 30 characters"));

  aedit_cmd(" " NAME_30);
  TEST_ASSERT_NOT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(AEDIT_CONFIRM_ADD, OLC_MODE(&builder_desc));
}

/* I1-01: the command and sort-as prompts stored a name of any length. */
static void test_aedit_refuses_overlong_command_and_sort_names(void)
{
  aedit_cmd(" smile");
  TEST_ASSERT_EQUAL_INT(AEDIT_CONFIRM_EDIT, OLC_MODE(&builder_desc));
  type("y");
  type("n");
  TEST_ASSERT_EQUAL_INT(AEDIT_ACTION_NAME, OLC_MODE(&builder_desc));
  type(NAME_30 NAME_30);
  TEST_ASSERT_EQUAL_INT(AEDIT_ACTION_NAME, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_STRING("smile", OLC_ACTION(&builder_desc)->command);
  TEST_ASSERT_EQUAL_STRING("social-name-length", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "field command input length 60 over 30"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "at most 30 characters"));
  type("grin");
  TEST_ASSERT_EQUAL_STRING("grin", OLC_ACTION(&builder_desc)->command);

  type("1");
  tb_error_capture_reset();
  type(NAME_30 "k");
  TEST_ASSERT_EQUAL_INT(AEDIT_SORT_AS, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_STRING("smile", OLC_ACTION(&builder_desc)->sort_as);
  TEST_ASSERT_EQUAL_STRING("social-name-length", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "field sort_as"));
  type(NAME_30);
  TEST_ASSERT_EQUAL_STRING(NAME_30, OLC_ACTION(&builder_desc)->sort_as);
}

/* G2-13: a social saved without char_no_arg (printed when it is used with
 * no argument) or, when it takes a target, without others_found (printed
 * by gemote) cannot run that way.  The save still goes ahead, but the
 * builder and the log are told. */
static void test_aedit_save_reports_missing_required_messages(void)
{
  free(soc_mess_list[0].others_found);
  soc_mess_list[0].others_found = NULL;
  aedit_cmd(" smile");
  type("y");
  type("a");
  type("");                               /* clears char_no_arg */
  type("q");
  TEST_ASSERT_EQUAL_INT(AEDIT_CONFIRM_SAVESTRING, OLC_MODE(&builder_desc));
  tb_error_capture_reset();
  type("y");

  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_NULL(soc_mess_list[0].char_no_arg);       /* saved anyway */
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("social-field-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "social 'smile' saved without its others_found message"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "a) Char [NO ARG] is empty"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "g) Others [VICT] is empty"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "Action saved to disk"));
}

/* G2-13: a complete social saves without a warning. */
static void test_aedit_save_of_complete_social_is_quiet(void)
{
  aedit_cmd(" smile");
  type("y");
  type("5");                              /* toggle hidden: a change */
  type("q");
  tb_error_capture_reset();
  type("y");

  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NULL(strstr(out_buf, "is empty"));
}

/* F3-20: aedit asked can_edit_zone() with the AEDIT_PERMISSION sentinel,
 * which is honoured only while the world has fewer than 1000 zones; it
 * now asks can_use_editor() for the socials editor. */
static void test_aedit_asks_the_socials_editor_permission(void)
{
  editor_allowed = FALSE;
  aedit_cmd(" smile");
  TEST_ASSERT_EQUAL_INT(AEDIT_PERMISSION, editor_perm_asked);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "You don't have access to editing socials."));

  editor_allowed = TRUE;
  aedit_cmd(" smile");
  TEST_ASSERT_NOT_NULL(builder_desc.olc);
}

int main(void)
{
  char dir[] = "/tmp/tb_test_aedit.XXXXXX";
  int failures;

  if (!mkdtemp(dir) || chdir(dir) != 0 || mkdir("misc", 0700) != 0) {
    perror("test_aedit fixture directory");
    return 1;
  }
  logfile = stderr;

  UNITY_BEGIN();
  RUN_TEST(test_aedit_refuses_overlong_new_social_name);
  RUN_TEST(test_aedit_refuses_overlong_command_and_sort_names);
  RUN_TEST(test_aedit_save_reports_missing_required_messages);
  RUN_TEST(test_aedit_save_of_complete_social_is_quiet);
  RUN_TEST(test_aedit_asks_the_socials_editor_permission);
  failures = UNITY_END();

  (void)remove("misc/socials.new");
  (void)rmdir("misc");
  if (chdir("/") == 0)
    (void)rmdir(dir);
  return failures;
}
