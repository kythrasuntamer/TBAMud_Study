/* Executable tests of the production site-ban code (ban.c). BAN_FILE is
 * operator-edited and every record is security-relevant, so load_banned()
 * must report each record it rejects or alters rather than skip it quietly.
 * The test runs in a private temporary directory holding etc/badsites.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "ban.h"
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

/* utils.c owns tb_error_log_at(); the link wraps it (--wrap) so the tests can
 * assert which structured diagnostics ban.c emitted, in order. */
#define MAX_CAPTURED 8
static int captured_errors;
static char captured_ops[MAX_CAPTURED][64];
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func; (void)detail_fmt;
  if (captured_errors < MAX_CAPTURED)
    strlcpy(captured_ops[captured_errors], operation ? operation : "",
            sizeof(captured_ops[0]));
  captured_errors++;
}

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

/* Whitespace-separated splitters, enough for "<flag> <site>" arguments. */
char *one_argument(char *argument, char *first_arg)
{
  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ')
    *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  return one_argument(one_argument(argument, first_arg), second_arg);
}

static char tmpdir[] = "/tmp/tb_ban_test_XXXXXX";
static char olddir[4096];

static void free_ban_list(void)
{
  struct ban_list_element *next;

  for (; ban_list; ban_list = next) {
    next = ban_list->next;
    free(ban_list);
  }
}

static void write_ban_file(const char *contents)
{
  FILE *fp = fopen(BAN_FILE, "w");

  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_TRUE(fputs(contents, fp) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

static int ban_count(void)
{
  int n = 0;
  struct ban_list_element *node;

  for (node = ban_list; node; node = node->next)
    n++;
  return n;
}

void setUp(void)
{
  captured_errors = 0;
  memset(captured_ops, 0, sizeof(captured_ops));
  sent[0] = '\0';
  free_ban_list();
}

void tearDown(void)
{
  free_ban_list();
  unlink(BAN_FILE);
}

/* Finding A2-05: an unknown type used to be inserted as BAN_NOT. */
void test_load_rejects_unknown_ban_type(void)
{
  write_ban_file("everything badhost.example 1700000000 Imm\n"
                 "all goodban.example 1700000000 Imm\n");
  load_banned();

  TEST_ASSERT_EQUAL_INT(1, ban_count());
  TEST_ASSERT_EQUAL_STRING("goodban.example", ban_list->site);
  TEST_ASSERT_EQUAL_INT(BAN_ALL, ban_list->type);
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("load-type", captured_ops[0]);
}

void test_load_reports_malformed_records(void)
{
  write_ban_file("all short.example\n"
                 "\n"
                 "new datebad.example 17x Imm\n"
                 "select ok.example 0 Imm\n");
  load_banned();

  TEST_ASSERT_EQUAL_INT(1, ban_count());
  TEST_ASSERT_EQUAL_INT(BAN_SELECT, ban_list->type);
  TEST_ASSERT_EQUAL_INT(2, captured_errors);
  TEST_ASSERT_EQUAL_STRING("load-parse", captured_ops[0]);
  TEST_ASSERT_EQUAL_STRING("load-parse", captured_ops[1]);
}

void test_load_reports_site_truncation(void)
{
  char line[512];
  char site[BANNED_SITE_LENGTH + 11];

  memset(site, 'a', sizeof(site) - 1);
  site[sizeof(site) - 1] = '\0';
  TEST_ASSERT_TRUE(snprintf(line, sizeof(line), "new %s 1700000000 Imm\n", site) > 0);
  write_ban_file(line);
  load_banned();

  TEST_ASSERT_EQUAL_INT(1, ban_count());
  TEST_ASSERT_EQUAL_size_t(BANNED_SITE_LENGTH, strlen(ban_list->site));
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("load-site-truncate", captured_ops[0]);
}

void test_load_well_formed_file_is_silent(void)
{
  write_ban_file("new a.example 1700000000 Imm\n"
                 "select b.example 1700000001 Imm\n"
                 "all c.example 1700000002 Imm\n");
  load_banned();

  TEST_ASSERT_EQUAL_INT(3, ban_count());
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

/* Finding A2-06: an over-long site was stored truncated without a word. */
void test_do_ban_refuses_overlong_site(void)
{
  struct char_data imm;
  struct player_special_data ps;
  char arg[MAX_INPUT_LENGTH];
  char site[BANNED_SITE_LENGTH + 6];

  memset(&imm, 0, sizeof(imm));
  memset(&ps, 0, sizeof(ps));
  imm.player_specials = &ps;
  imm.player.name = (char *)"Imm";
  GET_IDNUM(&imm) = 5;
  memset(site, 'b', sizeof(site) - 1);
  site[sizeof(site) - 1] = '\0';
  TEST_ASSERT_TRUE(snprintf(arg, sizeof(arg), "all %s", site) > 0);

  do_ban(&imm, arg, 0, 0);

  TEST_ASSERT_NULL(ban_list);
  TEST_ASSERT_NOT_NULL(strstr(sent, "too long"));
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("site-length", captured_ops[0]);
}

void test_do_ban_accepts_site_at_limit(void)
{
  struct char_data imm;
  struct player_special_data ps;
  char arg[MAX_INPUT_LENGTH];
  char site[BANNED_SITE_LENGTH + 1];

  memset(&imm, 0, sizeof(imm));
  memset(&ps, 0, sizeof(ps));
  imm.player_specials = &ps;
  imm.player.name = (char *)"Imm";
  memset(site, 'c', sizeof(site) - 1);
  site[sizeof(site) - 1] = '\0';
  TEST_ASSERT_TRUE(snprintf(arg, sizeof(arg), "new %s", site) > 0);

  do_ban(&imm, arg, 0, 0);

  TEST_ASSERT_NOT_NULL(ban_list);
  TEST_ASSERT_EQUAL_STRING(site, ban_list->site);
  TEST_ASSERT_EQUAL_INT(BAN_NEW, ban_list->type);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

int main(void)
{
  int rc;

  if (!getcwd(olddir, sizeof(olddir)) || !mkdtemp(tmpdir) ||
      chdir(tmpdir) != 0 || mkdir("etc", 0700) != 0) {
    perror("test_ban setup");
    return 1;
  }

  UNITY_BEGIN();
  RUN_TEST(test_load_rejects_unknown_ban_type);
  RUN_TEST(test_load_reports_malformed_records);
  RUN_TEST(test_load_reports_site_truncation);
  RUN_TEST(test_load_well_formed_file_is_silent);
  RUN_TEST(test_do_ban_refuses_overlong_site);
  RUN_TEST(test_do_ban_accepts_site_at_limit);
  rc = UNITY_END();

  rmdir("etc");
  if (chdir(olddir) == 0)
    rmdir(tmpdir);
  return rc;
}
