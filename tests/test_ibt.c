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
  return UNITY_END();
}
