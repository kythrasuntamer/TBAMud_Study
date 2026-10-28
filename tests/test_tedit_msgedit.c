/* Executable tests of the production text-file editor (tedit.c) and
 * combat-message editor (msgedit.c).
 *
 * The server reads tedit's texts on every connection or login (GREETINGS,
 * motd...), so an edit must never leave one NULL, and neither editor may
 * stop the server when a save fails.  Both files are linked as they are;
 * the string editor, OLC cleanup and the other collaborators they reach
 * are reduced to small models here, and unrelated sections are discarded
 * with --gc-sections.  Saves run inside a private temporary directory with
 * the lib/ layout (text/, misc/) when a test wants them to succeed.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "oasis.h"
#include "genolc.h"
#include "improved-edit.h"
#include "modify.h"
#include "interpreter.h"
#include "spells.h"
#include "msgedit.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tb_error_capture.h"

/* tedit.c and msgedit.c entry points without a header of their own. */
ACMD(do_tedit);
ACMD(do_msgedit);
void tedit_string_cleanup(struct descriptor_data *d, int terminator);

/* ------------------------------------------------------------------ */
/* Globals the two files reference (test_stubs.c owns GREETINGS, motd, */
/* imotd, background, motdmod, newsmod and descriptor_list).           */
/* ------------------------------------------------------------------ */
char *credits = NULL, *news = NULL, *help = NULL, *ihelp = NULL,
     *info = NULL, *wizlist = NULL, *immlist = NULL, *handbook = NULL,
     *policies = NULL;
struct message_list fight_messages[MAX_MESSAGES];
struct spell_info_type spell_info[TOP_SPELL_DEFINE + 1];
const char *nrm = "", *grn = "", *cyn = "", *yel = "";

/* ------------------------------------------------------------------ */
/* Collaborators.                                                      */
/* ------------------------------------------------------------------ */
static char out_buf[16384];

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  size_t used = strlen(out_buf);
  (void)d;
  if (used >= sizeof(out_buf) - 1)
    return 0;
  va_start(args, txt);
  if (vsnprintf(out_buf + used, sizeof(out_buf) - used, txt, args) < 0)
    out_buf[used] = '\0';
  va_end(args);
  return strlen(out_buf) - used;
}

int MIN(int a, int b) { return a < b ? a : b; }
int MAX(int a, int b) { return a > b ? a : b; }
void basic_mud_log(const char *format, ...) { (void)format; }
static int mudlogs;
void mudlog(int type, int level, int file, const char *str, ...)
{
  (void)type; (void)level; (void)file; (void)str;
  mudlogs++;
}
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *allocation;
  (void)file; (void)line; (void)func;
  allocation = calloc(count ? count : 1, element_size ? element_size : 1);
  if (!allocation) abort();
  return allocation;
}
char *delete_doubledollar(char *string) { return string; }
char *fread_action(FILE *fl, int nr) { (void)fl; (void)nr; return NULL; }
int genolc_checkstring(struct descriptor_data *d, char *arg) { (void)d; (void)arg; return TRUE; }
void get_char_colors(struct char_data *ch) { (void)ch; }
void clear_screen(struct descriptor_data *d) { (void)d; }
void strip_cr(char *buffer)
{
  int rpos, wpos;
  for (rpos = 0, wpos = 0; buffer[rpos]; rpos++) {
    buffer[wpos] = buffer[rpos];
    wpos += (buffer[rpos] != '\r');
  }
  buffer[wpos] = '\0';
}
char *one_argument(char *argument, char *first_arg)
{
  while (*argument && isspace((unsigned char)*argument)) argument++;
  while (*argument && !isspace((unsigned char)*argument)) *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}

/* modify.c string_write() with the improved editor: the descriptor edits
 * *writeto and keeps data as the abort copy. */
void string_write(struct descriptor_data *d, char **writeto, size_t len, long mailto, void *data)
{
  d->str = writeto;
  d->max_str = len;
  d->mail_to = mailto;
  d->backstr = (char *)data;
}

/* oasis.c cleanup_olc(), reduced to the parts tedit and msgedit own. */
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  (void)cleanup_type;
  if (!d->olc)
    return;
  if (OLC_MSG_LIST(d))
    free_message_list(OLC_MSG_LIST(d));
  free(OLC_STORAGE(d));
  free(d->olc);
  d->olc = NULL;
  STATE(d) = CON_PLAYING;
}

/* ------------------------------------------------------------------ */
/* Fixtures.                                                           */
/* ------------------------------------------------------------------ */
static struct descriptor_data builder_desc;
static struct char_data builder;
static struct player_special_data builder_specials;

void setUp(void)
{
  tb_error_capture_reset();
  out_buf[0] = '\0';
  mudlogs = 0;
  memset(&builder_desc, 0, sizeof(builder_desc));
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  builder.player_specials = &builder_specials;
  builder.player.name = "Builder";
  builder.player.level = LVL_IMPL;
  GET_IDNUM(&builder) = 4242;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
  STATE(&builder_desc) = CON_PLAYING;
  spell_info[0].name = "!UNUSED!";
}

/* Saves write relative to the lib directory: a test that saves runs in a
 * fresh temporary directory, holding the subdirectory (text/, misc/) its
 * save needs when the save should work.  tearDown() removes it, so a
 * failed assertion leaves nothing behind. */
static char saved_cwd[4096];
static char scratch_dir[64];
static const char *scratch_subdir, *scratch_file;

static void enter_scratch_dir(const char *subdir, const char *file)
{
  TEST_ASSERT_NOT_NULL(getcwd(saved_cwd, sizeof(saved_cwd)));
  strlcpy(scratch_dir, "/tmp/tb-tedit-msgedit-XXXXXX", sizeof(scratch_dir));
  TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
  TEST_ASSERT_EQUAL_INT(0, chdir(scratch_dir));
  scratch_subdir = subdir;
  scratch_file = file;
  if (subdir)
    TEST_ASSERT_EQUAL_INT(0, mkdir(subdir, 0700));
}

/* Remove the file a save may have written (and its temporary), the
 * subdirectory and the directory itself. */
static void leave_scratch_dir(void)
{
  char temp_name[256];

  if (!scratch_dir[0])
    return;
  if (scratch_file) {
    (void)remove(scratch_file);
    if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", scratch_file) < (int)sizeof(temp_name))
      (void)remove(temp_name);
  }
  if (scratch_subdir)
    (void)rmdir(scratch_subdir);
  if (chdir(saved_cwd) != 0 || rmdir(scratch_dir) != 0)
    TEST_FAIL_MESSAGE("could not remove the scratch directory");
  scratch_dir[0] = '\0';
}

void tearDown(void)
{
  if (builder_desc.olc)
    cleanup_olc(&builder_desc, CLEANUP_ALL);
  free(builder_desc.backstr);
  builder_desc.backstr = NULL;
  free_messages();
  leave_scratch_dir();
}

static const char *slurp(const char *path)
{
  static char buf[4096];
  FILE *fl = fopen(path, "r");
  size_t n;

  if (!fl)
    return NULL;
  n = fread(buf, 1, sizeof(buf) - 1, fl);
  buf[n] = '\0';
  (void)fclose(fl);
  return buf;
}

/* ------------------------------------------------------------------ */
/* O6d: tedit never leaves a live text NULL.                           */
/* ------------------------------------------------------------------ */

/* The editor works on a private copy: improved-edit's '/c' (free the
 * buffer, NULL it) must not touch GREETINGS, which comm.c strlen()s for
 * every new connection, and saving the cleared buffer publishes "" with a
 * warning instead of NULL. */
static void test_tedit_clear_then_save_never_leaves_greetings_null(void)
{
  char cmd[] = " greetings";

  enter_scratch_dir("text", GREETINGS_FILE);
  GREETINGS = strdup("Welcome!\r\n");
  do_tedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_TEDIT, STATE(&builder_desc));
  TEST_ASSERT_TRUE(builder_desc.str != &GREETINGS);

  free(*builder_desc.str);             /* '/c' */
  *builder_desc.str = NULL;
  TEST_ASSERT_EQUAL_STRING("Welcome!\r\n", GREETINGS);

  free(builder_desc.backstr);          /* string_add()'s save */
  builder_desc.backstr = NULL;
  tedit_string_cleanup(&builder_desc, STRINGADD_SAVE);
  TEST_ASSERT_NOT_NULL(GREETINGS);
  TEST_ASSERT_EQUAL_STRING("", GREETINGS);
  TEST_ASSERT_EQUAL_STRING("tedit-empty", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_EQUAL_STRING("", slurp(GREETINGS_FILE));
  TEST_ASSERT_NULL(builder_desc.olc);

  free(GREETINGS);
  GREETINGS = NULL;
}

/* Text typed and then dropped (lost link: close_socket() calls
 * cleanup_olc()) never reaches the live global. */
static void test_tedit_lost_link_leaves_live_text(void)
{
  char cmd[] = " motd";
  char *before;

  motd = strdup("Old motd\r\n");
  before = motd;
  do_tedit(&builder, cmd, 0, 0);
  free(*builder_desc.str);             /* the builder typed a new text */
  *builder_desc.str = strdup("Half a new motd");

  cleanup_olc(&builder_desc, CLEANUP_ALL);
  TEST_ASSERT_TRUE(motd == before);
  TEST_ASSERT_EQUAL_STRING("Old motd\r\n", motd);

  free(motd);
  motd = NULL;
}

/* ------------------------------------------------------------------ */
/* O6c: a msgedit save that cannot be written is refused and reported; */
/* it used to exit(1) the whole server from the editor.                */
/* ------------------------------------------------------------------ */
static void test_msgedit_failed_save_is_reported_and_server_keeps_running(void)
{
  char cmd[] = " 1", yes[] = "y";

  enter_scratch_dir(NULL, NULL);       /* no misc/: the file cannot be opened */
  do_msgedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_MSGEDIT, STATE(&builder_desc));
  OLC_VAL(&builder_desc) = 1;          /* there are unsaved changes */
  OLC_MODE(&builder_desc) = MSGEDIT_CONFIRM_SAVE;
  out_buf[0] = '\0';

  msgedit_parse(&builder_desc, yes);

  TEST_ASSERT_EQUAL_STRING("msgedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "slot 1 editor player idnum 4242"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "writing " MESS_FILE " FAILED; the file is unchanged"));
  TEST_ASSERT_NULL(strstr(out_buf, "Messages saved."));
  TEST_ASSERT_EQUAL_INT(1, OLC_VAL(&builder_desc));
  TEST_ASSERT_EQUAL_INT(CON_MSGEDIT, STATE(&builder_desc));
}

/* With misc/ present the save commits the file and says nothing. */
static void test_msgedit_save_commits_the_messages_file(void)
{
  enter_scratch_dir("misc", MESS_FILE);
  TEST_ASSERT_TRUE(save_messages_to_disk());
  TEST_ASSERT_NOT_NULL(strstr(slurp(MESS_FILE), "Combat Message File"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_tedit_clear_then_save_never_leaves_greetings_null);
  RUN_TEST(test_tedit_lost_link_leaves_live_text);
  RUN_TEST(test_msgedit_failed_save_is_reported_and_server_keeps_running);
  RUN_TEST(test_msgedit_save_commits_the_messages_file);
  return UNITY_END();
}
