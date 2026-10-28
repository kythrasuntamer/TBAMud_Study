/* Executable tests of string_add()'s abort path (modify.c) with the real
 * improved editor (improved-edit.c): '/a' from each kind of editor.
 * Red before the fix: every in-game abort logged "SYSERR: string_add:
 * Aborting write from unknown origin.", and aborting a note on paper kept the
 * half-written text instead of restoring the old note. */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "improved-edit.h"
#include "modify.h"
#include "mail.h"
#include <stdarg.h>

/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* log() as modify.c sees it (linked with --wrap=basic_mud_log). */
static int syserr_logs;
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_basic_mud_log(const char *format, ...);
void __wrap_basic_mud_log(const char *format, ...)
{
  if (format && !strncmp(format, "SYSERR", 6))
    syserr_logs++;
}

/* interpreter.c helpers modify.c calls; the editor input has no "$$". */
char *delete_doubledollar(char *string)
{
  return string;
}
char *any_one_arg(char *argument, char *first_arg)
{
  *first_arg = '\0';
  return argument;
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  (void)ch; (void)messg;
  return 0;
}

size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{
  (void)t; (void)txt;
  return 0;
}

static struct descriptor_data d;
static struct char_data ch;
static struct player_special_data ps;

void setUp(void)
{
  memset(&d, 0, sizeof(d));
  memset(&ch, 0, sizeof(ch));
  memset(&ps, 0, sizeof(ps));
  ch.player_specials = &ps;
  ch.desc = &d;
  GET_IDNUM(&ch) = 42;
  d.character = &ch;
  d.desc_num = 7;
  STATE(&d) = CON_PLAYING;
  syserr_logs = 0;
  tb_error_capture_reset();
}

void tearDown(void) {}

static void type_line(const char *text)
{
  char line[MAX_INPUT_LENGTH];

  strlcpy(line, text, sizeof(line));
  string_add(&d, line);
}

/* do_write(): the note's old text is handed over as backstr; '/a' must put
 * it back and free nothing the paper still owns. */
static void test_note_abort_restores_previous_text(void)
{
  char *note = strdup("Meet at the fountain.\r\n");

  string_write(&d, &note, MAX_NOTE_LENGTH, 0, strdup(note));
  type_line("Scribbled over it.");
  type_line("/a");

  TEST_ASSERT_EQUAL_STRING("Meet at the fountain.\r\n", note);
  TEST_ASSERT_NULL(d.str);
  TEST_ASSERT_NULL(d.backstr);
  TEST_ASSERT_EQUAL_INT(0, syserr_logs);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(note);
}

/* mail: playing_string_cleanup() owns the abort; nothing to report. */
static void test_mail_abort_is_not_reported(void)
{
  char **mailwrite;

  CREATE(mailwrite, char *, 1);
  SET_BIT_AR(PLR_FLAGS(&ch), PLR_MAILING);
  string_write(&d, mailwrite, MAX_MAIL_SIZE, 1001, NULL);
  type_line("Dear Brena,");
  type_line("/a");

  TEST_ASSERT_NULL(d.str);             /* cleanup freed the buffer and slot */
  TEST_ASSERT_FALSE(PLR_FLAGGED(&ch, PLR_MAILING));
  TEST_ASSERT_EQUAL_INT(0, syserr_logs);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* An editor session in a state no handler knows is reported where it
 * happens, naming the descriptor, the player and the state. */
static void test_unknown_origin_abort_is_reported(void)
{
  char *text = NULL;

  STATE(&d) = CON_MENU;
  string_write(&d, &text, MAX_NOTE_LENGTH, 0, strdup("saved copy"));
  type_line("typed");
  type_line("/a");

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("string-abort", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "desc #7 player idnum 42 state"));
  TEST_ASSERT_NULL(d.backstr);         /* the saved copy is not leaked */
  TEST_ASSERT_EQUAL_INT(0, syserr_logs);
  free(text);
}

/* A note started on blank paper (no backstr): '/a' leaves the paper blank.
 * Red before: the abandoned text stayed on the paper. */
static void test_blank_note_abort_leaves_paper_blank(void)
{
  char *note = NULL;

  string_write(&d, &note, MAX_NOTE_LENGTH, 0, NULL);
  type_line("A half-written draft.");
  type_line("/a");

  TEST_ASSERT_NULL(note);
  TEST_ASSERT_NULL(d.str);
  TEST_ASSERT_EQUAL_INT(0, syserr_logs);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* The link drops mid-note (close_socket()/release_booted_editor()): the paper
 * gets its old text back and the saved copy is not leaked (LeakSanitizer).
 * Red before: close_socket() skipped backstr while PLR_WRITING, leaking it
 * and leaving the half-written text on the paper. */
static void test_link_drop_mid_note_restores_old_text(void)
{
  char *note = strdup("Meet at the fountain.\r\n");

  string_write(&d, &note, MAX_NOTE_LENGTH, 0, strdup(note));
  type_line("Scribbled over it.");

  TEST_ASSERT_TRUE(string_abandon_in_game_edit(&d));
  TEST_ASSERT_EQUAL_STRING("Meet at the fountain.\r\n", note);
  TEST_ASSERT_NULL(d.str);
  TEST_ASSERT_NULL(d.backstr);
  free(note);
}

/* Mail has no backstr: the helper leaves it to close_socket()'s mail code. */
static void test_link_drop_mid_mail_is_left_to_mail_cleanup(void)
{
  char **mailwrite;

  CREATE(mailwrite, char *, 1);
  SET_BIT_AR(PLR_FLAGS(&ch), PLR_MAILING);
  string_write(&d, mailwrite, MAX_MAIL_SIZE, 1001, NULL);
  type_line("Dear Brena,");

  TEST_ASSERT_FALSE(string_abandon_in_game_edit(&d));
  TEST_ASSERT_TRUE(d.str == mailwrite);
  free(*mailwrite);
  free(mailwrite);
  d.str = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_note_abort_restores_previous_text);
  RUN_TEST(test_mail_abort_is_not_reported);
  RUN_TEST(test_unknown_origin_abort_is_reported);
  RUN_TEST(test_blank_note_abort_leaves_paper_blank);
  RUN_TEST(test_link_drop_mid_note_restores_old_text);
  RUN_TEST(test_link_drop_mid_mail_is_left_to_mail_cleanup);
  return UNITY_END();
}
