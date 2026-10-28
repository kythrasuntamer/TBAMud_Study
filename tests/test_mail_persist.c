/* Executable tests of the production mud-mail store (mail.c).
 *
 * MAIL_FILE is a server-written text file that can be truncated or damaged
 * on disk. The tests run scan_file()/has_mail()/read_delete() and the
 * postmaster's receive command against a scratch lib/etc tree (the test
 * chdir()s into it, since MAIL_FILE is a relative path) and assert both on
 * the file contents and on the structured diagnostic.
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
#include "mail.h"
#include <stdarg.h>
#include <sys/stat.h>
#include "tb_error_capture.h"
#include "tb_persist_support.h"

int no_mail = 0;
struct obj_data *object_list;

static char scratch_dir[256];
static char start_dir[1024];
static int objects_given;

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void parse_at(char *str) { (void)str; }

int touch(const char *path)
{
  FILE *fl = fopen(path, "a");
  if (fl == NULL)
    return -1;
  return fclose(fl);
}

/* Same contract as utils.c get_line(). */
int get_line(FILE *fl, char *buf)
{
  char temp[READ_SIZE];
  int lines = 0;
  size_t sl;

  do {
    if (!fgets(temp, READ_SIZE, fl))
      return 0;
    lines++;
  } while (*temp == '*' || *temp == '\n' || *temp == '\r');
  sl = strlen(temp);
  while (sl > 0 && (temp[sl - 1] == '\n' || temp[sl - 1] == '\r'))
    temp[--sl] = '\0';
  memcpy(buf, temp, sl + 1);
  return lines;
}

char *get_name_by_id(long id) { (void)id; return NULL; }
/* Referenced by the postmaster's "mail" command, which these tests do not run. */
long get_id_by_name(const char *name) { (void)name; return -1; }
int decrease_gold(struct char_data *ch, int amt) { (void)ch; return amt; }
char *one_argument(char *argument, char *first_arg) { *first_arg = '\0'; return argument; }
void string_write(struct descriptor_data *d, char **txt, size_t len, long mailto, void *data)
{ (void)d; (void)txt; (void)len; (void)mailto; (void)data; }

/* Body reader with fread_string()'s result: the text before '~'. */
char *fread_string(FILE *fl, const char *error)
{
  char line[READ_SIZE], buf[MAX_STRING_LENGTH];
  (void)error;
  buf[0] = '\0';
  while (fgets(line, sizeof(line), fl)) {
    char *tilde = strchr(line, '~');
    if (tilde) {
      *tilde = '\0';
      strlcat(buf, line, sizeof(buf));
      return *buf ? strdup(buf) : NULL;
    }
    strlcat(buf, line, sizeof(buf));
  }
  return NULL;
}

struct obj_data *create_obj(void)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));
  TEST_ASSERT_NOT_NULL(obj);
  obj->item_number = NOTHING;
  return obj;
}

static void free_mail_obj(struct obj_data *obj)
{
  free(obj->name);
  free(obj->short_description);
  free(obj->description);
  free(obj->action_description);
  free(obj);
}

void obj_to_char(struct obj_data *object, struct char_data *ch)
{
  object->next_content = ch->carrying;
  ch->carrying = object;
  object->carried_by = ch;
  objects_given++;
}

/* ------------------------------------------------------------------ */
static void write_mail_file(const char *body)
{
  FILE *fl = fopen(MAIL_FILE, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fputs(body, fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

static char *slurp(const char *path)
{
  static char buf[4096];
  size_t n;
  FILE *fl = fopen(path, "r");
  TEST_ASSERT_NOT_NULL(fl);
  n = fread(buf, 1, sizeof(buf) - 1, fl);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  return buf;
}

static const char three_with_bad_middle[] =
  "### 7 1 1000\nfirst for seven\n~\n"
  "### 8 x 1000\nbroken header\n~\n"
  "### 9 1 1000\nlast for nine\n~\n";

void setUp(void)
{
  if (scratch_dir[0] == '\0') {
    const char *base = getenv("TMPDIR");
    char etc[300];
    TEST_ASSERT_NOT_NULL(getcwd(start_dir, sizeof(start_dir)));
    TEST_ASSERT_TRUE(snprintf(scratch_dir, sizeof(scratch_dir), "%s/tb_mail_XXXXXX",
                              base && *base ? base : "/tmp") > 0);
    TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
    TEST_ASSERT_TRUE(snprintf(etc, sizeof(etc), "%s/etc", scratch_dir) > 0);
    TEST_ASSERT_EQUAL_INT(0, mkdir(etc, 0700));
  }
  TEST_ASSERT_EQUAL_INT(0, chdir(scratch_dir));
  no_mail = 0;
  objects_given = 0;
  tb_error_capture_reset();
}

void tearDown(void)
{
  (void)remove(MAIL_FILE);
  (void)remove(MAIL_FILE_TMP);
  TEST_ASSERT_EQUAL_INT(0, chdir(start_dir));
}

/* B2-05: a damaged header used to look like end of file, so read_delete()
 * renamed the partial rewrite over MAIL_FILE and dropped every later
 * message. The live file must survive untouched. */
static void test_read_delete_keeps_file_on_damaged_record(void)
{
  char *text = NULL;

  write_mail_file(three_with_bad_middle);
  TEST_ASSERT_FALSE(read_delete(7, &text));
  TEST_ASSERT_NULL(text);

  TEST_ASSERT_EQUAL_STRING(three_with_bad_middle, slurp(MAIL_FILE));
  TEST_ASSERT_TRUE(no_mail);
  TEST_ASSERT_EQUAL_STRING("parse-header", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "recipient idnum 7"));
}

/* B2-05: scan_file() honours its contract and reports a damaged file. */
static void test_scan_file_reports_damage(void)
{
  write_mail_file(three_with_bad_middle);
  TEST_ASSERT_FALSE(scan_file());
  TEST_ASSERT_EQUAL_STRING("scan", tb_captured_operation);
}

static void test_scan_file_counts_good_file(void)
{
  write_mail_file("### 7 1 1000\nhello\n~\n");
  TEST_ASSERT_TRUE(scan_file());
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* The postmaster's "receive" command, reached through SPECIAL(postmaster). */
static struct command_info receive_cmd[] = {
  { "receive", "receive", POS_STANDING, NULL, 0, 0 },
};
struct command_info *complete_cmd_info = receive_cmd;

static struct char_data player, mailman;
static struct player_special_data player_specials;
static struct descriptor_data desc;

static void receive_mail(void)
{
  memset(&player, 0, sizeof(player));
  memset(&mailman, 0, sizeof(mailman));
  memset(&player_specials, 0, sizeof(player_specials));
  memset(&desc, 0, sizeof(desc));
  player.player_specials = &player_specials;
  player.desc = &desc;
  GET_IDNUM(&player) = 7;
  GET_LEVEL(&player) = 1;
  mailman.player_specials = &player_specials;
  SET_BIT_AR(MOB_FLAGS(&mailman), MOB_ISNPC);
  TEST_ASSERT_TRUE(postmaster(&player, &mailman, 0, (char *)""));
}

static void free_inventory(void)
{
  while (player.carrying) {
    struct obj_data *next = player.carrying->next_content;
    free_mail_obj(player.carrying);
    player.carrying = next;
  }
}

/* B2-06: when the message cannot be removed (here the rewrite file cannot
 * be created), has_mail() stays true; the receive loop used to create a new
 * note on every pass, forever. It must stop after reporting. */
static void test_receive_stops_when_message_not_removed(void)
{
  write_mail_file("### 7 1 1000\nhello\n~\n");
  TEST_ASSERT_EQUAL_INT(0, mkdir(MAIL_FILE_TMP, 0700));

  receive_mail();

  TEST_ASSERT_EQUAL_INT(0, rmdir(MAIL_FILE_TMP));
  TEST_ASSERT_EQUAL_INT(0, objects_given);
  TEST_ASSERT_TRUE(no_mail);
  TEST_ASSERT_EQUAL_STRING("read-delete", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 7"));
  TEST_ASSERT_EQUAL_STRING("### 7 1 1000\nhello\n~\n", slurp(MAIL_FILE));
}

/* B2-06: the normal path delivers each message once and removes it. */
static void test_receive_delivers_and_removes(void)
{
  write_mail_file("### 7 1 1000\nhello\n~\n### 9 1 1000\nother\n~\n"
                  "### 7 1 1001\nagain\n~\n");

  receive_mail();

  TEST_ASSERT_EQUAL_INT(2, objects_given);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(slurp(MAIL_FILE), "### 9 1 1000"));
  TEST_ASSERT_NULL(strstr(slurp(MAIL_FILE), "### 7"));
  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_NOT_NULL(strstr(player.carrying->action_description, "again"));
  free_inventory();
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_read_delete_keeps_file_on_damaged_record);
  RUN_TEST(test_scan_file_reports_damage);
  RUN_TEST(test_scan_file_counts_good_file);
  RUN_TEST(test_receive_stops_when_message_not_removed);
  RUN_TEST(test_receive_delivers_and_removes);
  {
    char etc[300];
    if (snprintf(etc, sizeof(etc), "%s/etc", scratch_dir) > 0)
      (void)rmdir(etc);
    (void)rmdir(scratch_dir);
  }
  return UNITY_END();
}
