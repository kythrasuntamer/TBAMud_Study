/**
 * @file test_utils_guards.c
 * Regression tests for the guards in src/utils.c: each test drives the
 * production function with the input that used to overrun a buffer, read
 * freed memory or fail silently, and asserts both that it now stays in
 * bounds (ASan/Valgrind) and that the guard reported itself (fail loudly).
 *
 * The structured reporter tb_error_log_at() lives in utils.c itself, so a
 * link-time --wrap cannot intercept calls made from inside utils.c. Instead
 * logfile points at a temporary file and the tests search the emitted
 * records for "op=<operation>".
 */

#include "unity.h"

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"

extern FILE *logfile;

/* ---------------------------------------------------------------------
 * Diagnostic capture
 * ------------------------------------------------------------------- */

static char captured_log[16384];

/* Reload everything logged since setUp() into captured_log. */
static const char *log_text(void)
{
  size_t n;

  captured_log[0] = '\0';
  if (logfile == NULL || fflush(logfile) != 0 || fseek(logfile, 0L, SEEK_SET) != 0)
    return captured_log;
  n = fread(captured_log, 1, sizeof(captured_log) - 1, logfile);
  captured_log[n] = '\0';
  (void)fseek(logfile, 0L, SEEK_END);
  return captured_log;
}

/* Number of structured records with operation op. */
static int count_op(const char *op)
{
  char needle[96];
  const char *p;
  int count = 0;

  (void)snprintf(needle, sizeof(needle), " op=%s ", op);
  for (p = log_text(); (p = strstr(p, needle)) != NULL; p += strlen(needle))
    count++;
  return count;
}

/* Number of structured records of any kind. */
static int count_records(void)
{
  const char *p;
  int count = 0;

  for (p = log_text(); (p = strstr(p, " op=")) != NULL; p += 4)
    count++;
  return count;
}

/* ---------------------------------------------------------------------
 * Output capture (strong definitions override the weak test stubs)
 * ------------------------------------------------------------------- */

static char paged[MAX_STRING_LENGTH + 1];
static size_t paged_len;

void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  paged_len = strlen(str);
  strlcpy(paged, str, sizeof(paged));
}

void setUp(void)
{
  logfile = tmpfile();
  if (logfile == NULL)
    logfile = stderr;
  captured_log[0] = '\0';
  paged[0] = '\0';
  paged_len = 0;
}

void tearDown(void)
{
  if (logfile != NULL && logfile != stderr) {
    /* Echo the captured records so a failing run shows what was logged. */
    fputs(log_text(), stderr);
    (void)fclose(logfile);
  }
  logfile = NULL;
}

/* A player character with its own player_specials, idnum 'id'. */
static struct char_data *make_player(long id)
{
  struct char_data *ch;

  CREATE(ch, struct char_data, 1);
  CREATE(ch->player_specials, struct player_special_data, 1);
  GET_IDNUM(ch) = id;
  return ch;
}

static void free_player(struct char_data *ch)
{
  free(ch->player_specials);
  free(ch);
}

/* =====================================================================
 * I1-01: column_list must never write past its MAX_STRING_LENGTH buffer.
 * ===================================================================== */

#define CL_ENTRIES 600

void test_column_list_long_list_truncates_and_reports(void)
{
  struct char_data *ch = make_player(42);
  const char **list;
  char *longname;
  int i;

  /* One 60-character social name forces a single 100-column wide column,
   * so 600 rows need ~61 KB, more than the 48 KB buffer. */
  longname = strdup("abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij");
  TEST_ASSERT_NOT_NULL(longname);
  CREATE(list, const char *, CL_ENTRIES);
  list[0] = longname;
  for (i = 1; i < CL_ENTRIES; i++)
    list[i] = "smile";
  ch->player_specials->saved.screen_width = 100;

  column_list(ch, 0, list, CL_ENTRIES, FALSE);
  TEST_ASSERT_TRUE(paged_len < MAX_STRING_LENGTH);
  TEST_ASSERT_NOT_NULL(strstr(paged, "*** OVERFLOW ***"));

  free(list);
  free(longname);
  free_player(ch);
  TEST_ASSERT_EQUAL_INT(1, count_op("column-list-truncated"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "player idnum 42"));
}

void test_column_list_short_list_is_silent(void)
{
  struct char_data *ch = make_player(7);
  const char *list[] = { "smile", "grin", "nod" };

  ch->player_specials->saved.screen_width = 80;
  column_list(ch, 0, list, 3, FALSE);

  free_player(ch);
  TEST_ASSERT_EQUAL_INT(0, count_records());
  TEST_ASSERT_NOT_NULL(strstr(paged, "grin"));
  TEST_ASSERT_NULL(strstr(paged, "OVERFLOW"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_column_list_long_list_truncates_and_reports);
  RUN_TEST(test_column_list_short_list_is_silent);
  return UNITY_END();
}
