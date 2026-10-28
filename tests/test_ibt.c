/* Regression tests for the production Ideas/Bugs/Typos code (ibt.c) and the
 * db.c string reader it loads with. ibt.c, db.c, interpreter.c and utils.c
 * are linked with -ffunction-sections/--gc-sections, so only the code these
 * tests reach is kept; the few outside symbols it needs are faked here.
 * test_stubs.c is not linked because db.c defines the world globals.
 *
 * IBT files live under LIB_MISC relative to the working directory, so main()
 * runs the suite inside a fresh temporary directory.
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
#include "oasis.h"
#include "ibt.h"
#include <stdarg.h>
#include <sys/stat.h>

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

/* ---- fakes for symbols outside the linked sources --------------------- */

static char sent[MAX_STRING_LENGTH];

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(sent, sizeof(sent), messg, args) < 0)
    sent[0] = '\0';
  va_end(args);
  return strlen(sent);
}
size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{ (void)d; (void)txt; return 0; }
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{ (void)src_file; (void)src_line; (void)src_func; (void)str;
  (void)hide_invisible; (void)ch; (void)obj; (void)vict_obj; (void)type;
  return NULL; }
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{ (void)d; (void)str; (void)keep_internal; }
void send_editor_help(struct descriptor_data *d) { (void)d; }
void parse_at(char *str) { (void)str; }
void smash_tilde(char *str) { (void)str; }
void string_write(struct descriptor_data *d, char **writeto, size_t len,
                  long mailto, void *data)
{
  (void)data;
  d->str = writeto;
  d->max_str = len;
  d->mail_to = mailto;
}
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  (void)cleanup_type;
  if (d->olc) {
    free_olc_ibt(d->olc->ibt);
    free(d->olc);
    d->olc = NULL;
  }
  STATE(d) = CON_PLAYING;
}
void clear_screen(struct descriptor_data *d) { (void)d; }
void get_char_colors(struct char_data *ch) { (void)ch; }

/* CMD_NAME reads complete_cmd_info[cmd]; a one-entry table keeps the full
 * cmd_info[] (and every do_* it points at) out of the link. */
static struct command_info test_cmds[] = {
  { "bug", "bug", POS_DEAD, NULL, 0, SCMD_BUG },
  { "\n", "zzzzzzz", 0, NULL, 0, 0 }
};
struct command_info *complete_cmd_info = test_cmds;
struct descriptor_data *descriptor_list = NULL;
FILE *logfile = NULL;
const char *nrm = "", *grn = "", *cyn = "", *yel = "";
const char *ibt_bits[] = { "Resolved", "Important", "InProgress", "\n" };

/* Argument helpers from interpreter.c (not linked; see CMakeLists.txt):
 * plain whitespace splitting is all do_ibt's subcommands need. */
static char *copy_word(char *argument, char *word)
{
  size_t n = 0;

  while (*argument && isspace((unsigned char)*argument))
    argument++;
  while (*argument && !isspace((unsigned char)*argument) && n + 1 < MAX_INPUT_LENGTH)
    word[n++] = *argument++;
  word[n] = '\0';
  return argument;
}
char *one_argument(char *argument, char *first_arg)
{
  argument = copy_word(argument, first_arg);
  while (*argument && isspace((unsigned char)*argument))
    argument++;
  return argument;
}
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  return one_argument(one_argument(argument, first_arg), second_arg);
}
int is_abbrev(const char *arg1, const char *arg2)
{
  return *arg1 && !strncmp(arg1, arg2, strlen(arg1));
}
int is_number(const char *str)
{
  if (*str == '-')
    str++;
  if (!*str)
    return 0;
  for (; *str; str++)
    if (!isdigit((unsigned char)*str))
      return 0;
  return 1;
}

/* ---- fixture ---------------------------------------------------------- */

struct player {
  struct char_data ch;
  struct player_special_data specials;
  struct descriptor_data desc;
};

static struct player pa, pb, imm;
static struct room_data room;
static int bug_cmd;

static void make_player(struct player *p, const char *name, long idnum, int level)
{
  memset(p, 0, sizeof(*p));
  p->ch.player.name = (char *)name;
  p->ch.player.level = level;
  p->ch.player_specials = &p->specials;
  p->ch.char_specials.saved.idnum = idnum;
  p->ch.nr = NOBODY;
  p->ch.desc = &p->desc;
  p->ch.in_room = 0;
  p->desc.character = &p->ch;
  p->desc.connected = CON_PLAYING;
  p->desc.desc_num = (int)idnum;
}

void setUp(void)
{
  make_player(&pa, "Alice", 1, 1);
  make_player(&pb, "Bob", 2, 1);
  make_player(&imm, "Imm", 3, LVL_GRGOD);
  pa.desc.next = &pb.desc;
  pb.desc.next = &imm.desc;
  descriptor_list = &pa.desc;
  memset(&room, 0, sizeof(room));
  room.number = 3001;
  world = &room;
  top_of_world = 0;
  sent[0] = '\0';
  tb_error_capture_reset();
}

void tearDown(void)
{
  free_ibt_lists();
  descriptor_list = NULL;
  world = NULL;
}

static void ibt_cmd(struct player *p, const char *args)
{
  char buf[MAX_INPUT_LENGTH];
  strlcpy(buf, args, sizeof(buf));
  do_ibt(&p->ch, buf, bug_cmd, SCMD_BUG);
}

/* What string_add() does with a typed line: allocate or extend *d->str. */
static void type_line(struct player *p, const char *text)
{
  TEST_ASSERT_NOT_NULL(p->desc.str);
  if (!*p->desc.str)
    *p->desc.str = strdup(text);
}

/* What string_add()+playing_string_cleanup() do on '/a'. */
static void abort_write(struct player *p)
{
  clean_ibt_list(SCMD_BUG, &p->desc);
  p->desc.str = NULL;
}

static int count_bugs(void)
{
  int n = 0;
  IBT_DATA *b;
  for (b = first_bug; b; b = b->next)
    n++;
  return n;
}

/* C1-01: B aborting a bug freed every empty bug, including the one A was
 * still writing; A's next line wrote into the freed node (ASan
 * heap-use-after-free before the fix). */
static void test_abort_frees_only_the_aborting_writers_node(void)
{
  ibt_cmd(&pa, "submit alice bug");
  ibt_cmd(&pb, "submit bob bug");
  TEST_ASSERT_EQUAL_INT(2, count_bugs());

  abort_write(&pb);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  TEST_ASSERT_EQUAL_PTR(&first_bug->body, pa.desc.str);
  TEST_ASSERT_EQUAL_STRING("ibt-pending-free", tb_captured_operation);

  type_line(&pa, "my bug text");
  TEST_ASSERT_EQUAL_STRING("my bug text", first_bug->body);
  pa.desc.str = NULL;
}

/* C1-01: an aborted submission is dropped even after text was typed, and
 * empty nodes nobody is writing (left by a disconnect) are swept. */
static void test_abort_drops_own_text_and_orphans(void)
{
  ibt_cmd(&pa, "submit orphan");
  pa.desc.str = NULL;                       /* A disconnected mid-write */
  ibt_cmd(&pb, "submit bob bug");
  type_line(&pb, "typed then aborted");
  abort_write(&pb);
  TEST_ASSERT_EQUAL_INT(0, count_bugs());
}

/* Submit a complete bug as p (what 'bug submit' + typing + save leaves). */
static void submit_bug(struct player *p, const char *title, const char *body)
{
  char args[MAX_INPUT_LENGTH];
  snprintf(args, sizeof(args), "submit %s", title);
  ibt_cmd(p, args);
  type_line(p, body);
  p->desc.str = NULL;
}

static void olc_line(struct player *p, const char *text)
{
  char buf[MAX_INPUT_LENGTH];
  strlcpy(buf, text, sizeof(buf));
  ibtedit_parse(&p->desc, buf);
}

/* C1-13(a): 'bug remove N' freed a record whose submitter was still
 * writing it; the submitter's next line wrote into freed memory. */
static void test_remove_refuses_record_being_written(void)
{
  ibt_cmd(&pa, "submit in progress");
  ibt_cmd(&imm, "remove 1");
  TEST_ASSERT_EQUAL_STRING("remove-busy", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  type_line(&pa, "still mine");
  TEST_ASSERT_EQUAL_STRING("still mine", first_bug->body);
  pa.desc.str = NULL;
}

/* C1-13(b): ibtedit saved by list position, so removing an earlier record
 * during the session made the save overwrite the following record. */
static void test_ibtedit_save_follows_record_not_position(void)
{
  submit_bug(&pa, "first", "body one");
  submit_bug(&pa, "second", "body two");
  submit_bug(&pa, "third", "body three");

  do_oasis_ibtedit(&imm.ch, (char[]){"2"}, bug_cmd, SCMD_BUG);
  TEST_ASSERT_EQUAL_INT(CON_IBTEDIT, STATE(&imm.desc));
  olc_line(&imm, "3");
  olc_line(&imm, "second, edited");

  /* Another GRGOD removes record 1; "second" is now at position 1. */
  pb.ch.player.level = LVL_GRGOD;
  ibt_cmd(&pb, "remove 1");
  TEST_ASSERT_EQUAL_INT(2, count_bugs());

  olc_line(&imm, "q");
  olc_line(&imm, "y");
  TEST_ASSERT_EQUAL_STRING("second, edited", first_bug->text);
  TEST_ASSERT_EQUAL_STRING("body two", first_bug->body);
  TEST_ASSERT_EQUAL_STRING("third", first_bug->next->text);

  /* A record removed during the session is reported, not retargeted. */
  do_oasis_ibtedit(&imm.ch, (char[]){"2"}, bug_cmd, SCMD_BUG);
  olc_line(&imm, "3");
  olc_line(&imm, "gone");
  imm.ch.player.level = LVL_GRGOD;
  STATE(&imm.desc) = CON_PLAYING;  /* let remove pass the editor check */
  {
    struct oasis_olc_data *olc = imm.desc.olc;
    imm.desc.olc = NULL;
    ibt_cmd(&pb, "remove 2");
    imm.desc.olc = olc;
  }
  STATE(&imm.desc) = CON_IBTEDIT;
  tb_error_capture_reset();
  olc_line(&imm, "q");
  olc_line(&imm, "y");
  TEST_ASSERT_EQUAL_STRING("edit-target-gone", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  TEST_ASSERT_EQUAL_STRING("second, edited", first_bug->text);
}

/* C1-13: a record open in ibtedit cannot be removed under the editor. */
static void test_remove_refuses_record_open_in_ibtedit(void)
{
  submit_bug(&pa, "first", "body one");
  do_oasis_ibtedit(&imm.ch, (char[]){"1"}, bug_cmd, SCMD_BUG);
  pb.ch.player.level = LVL_GRGOD;
  ibt_cmd(&pb, "remove 1");
  TEST_ASSERT_EQUAL_STRING("remove-busy", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  olc_line(&imm, "q");
}

/* C1-12: removing a record leaked its admin notes (LeakSanitizer and
 * Valgrind report the strdup before the fix). */
static void test_remove_frees_admin_notes(void)
{
  submit_bug(&pa, "with notes", "body");
  first_bug->notes = strdup("admin notes");
  ibt_cmd(&imm, "remove 1");
  TEST_ASSERT_EQUAL_INT(0, count_bugs());
}

static void write_bugs_file(const char *content)
{
  FILE *fl = fopen(BUGS_FILE, "wb");

  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_INT(1, fputs(content, fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

/* C1-09: a blank line in a body is saved as a lone "\r\n"; reading it back,
 * fread_clean_string() walked to tmp[-1] (ASan stack-buffer-underflow
 * before the fix). */
static void test_load_body_with_blank_line(void)
{
  write_bugs_file("Text      title~\n"
                  "Body      line1\r\n\r\nline3\r\n~\n"
                  "Name      Alice~\n"
                  "Level     1\n"
                  "Room      3001\n"
                  "Flags     0 0 0 0\n"
                  "End\n");
  load_ibt_file(SCMD_BUG);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  TEST_ASSERT_EQUAL_STRING("line1\r\n\r\nline3\r\n", first_bug->body);
}

#define GOOD_RECORD(title) \
  "Text      " title "~\n" \
  "Body      body of " title "\n~\n" \
  "Name      Alice~\n" \
  "Level     1\n" \
  "Room      3001\n" \
  "Flags     0 0 0 0\n" \
  "End\n"

/* C1-V02: a body cut off before its '~' (crash or full disk during save)
 * made fread_clean_string() exit(1) at every boot. It is now reported and
 * the record dropped; this one is the last in the file. */
static void test_load_truncated_body_is_dropped_not_fatal(void)
{
  write_bugs_file(GOOD_RECORD("first")
                  "Text      second~\n"
                  "Body      partial text\n");
  load_ibt_file(SCMD_BUG);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  TEST_ASSERT_EQUAL_STRING("first", first_bug->text);
  TEST_ASSERT_EQUAL_STRING("ibt-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "record 1 field Body"));
}

/* C1-10: 'Body' at end of file logged read_ibt's uninitialised stack
 * buffer as the error context (Valgrind: conditional jump on uninitialised
 * value). The context is now the file, record and field. */
static void test_load_body_keyword_at_eof_names_record(void)
{
  write_bugs_file(GOOD_RECORD("first") "Body\n");
  load_ibt_file(SCMD_BUG);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  TEST_ASSERT_EQUAL_STRING("ibt-load", tb_captured_operation);
}

/* C1-11: a keyword of MAX_STRING_LENGTH bytes makes fread_word() return
 * NULL, which read_ibt dereferenced (SEGV before the fix). The record is
 * dropped and the ones after it still load. */
static void test_load_overlong_keyword_drops_only_that_record(void)
{
  size_t n = MAX_STRING_LENGTH + 10, used;
  char *content = malloc(n + 1024);

  TEST_ASSERT_NOT_NULL(content);
  used = strlcpy(content, GOOD_RECORD("first"), n + 1024);
  memset(content + used, 'A', n);
  used += n;
  strlcpy(content + used, "\nEnd\n" GOOD_RECORD("third"), n + 1024 - used);
  write_bugs_file(content);
  free(content);

  load_ibt_file(SCMD_BUG);
  TEST_ASSERT_EQUAL_INT(2, count_bugs());
  TEST_ASSERT_EQUAL_STRING("first", first_bug->text);
  TEST_ASSERT_EQUAL_STRING("third", first_bug->next->text);
  TEST_ASSERT_EQUAL_STRING("ibt-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "field keyword"));
}

/* Empty Notes ("~" alone) is a valid, empty string, not a failure. */
static void test_load_empty_notes_is_valid(void)
{
  write_bugs_file("Text      t~\nBody      b\n~\nNotes     ~\nEnd\n");
  load_ibt_file(SCMD_BUG);
  TEST_ASSERT_EQUAL_INT(1, count_bugs());
  TEST_ASSERT_EQUAL_STRING("", first_bug->notes);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

int main(void)
{
  char dir[] = "/tmp/tb_test_ibt.XXXXXX";

  if (!mkdtemp(dir) || chdir(dir) != 0 || mkdir("misc", 0700) != 0) {
    perror("test_ibt fixture directory");
    return 1;
  }
  logfile = stderr;

  bug_cmd = 0;

  UNITY_BEGIN();
  RUN_TEST(test_abort_frees_only_the_aborting_writers_node);
  RUN_TEST(test_abort_drops_own_text_and_orphans);
  RUN_TEST(test_remove_refuses_record_being_written);
  RUN_TEST(test_ibtedit_save_follows_record_not_position);
  RUN_TEST(test_remove_refuses_record_open_in_ibtedit);
  RUN_TEST(test_remove_frees_admin_notes);
  RUN_TEST(test_load_body_with_blank_line);
  RUN_TEST(test_load_truncated_body_is_dropped_not_fatal);
  RUN_TEST(test_load_body_keyword_at_eof_names_record);
  RUN_TEST(test_load_overlong_keyword_drops_only_that_record);
  RUN_TEST(test_load_empty_notes_is_valid);
  return UNITY_END();
}
