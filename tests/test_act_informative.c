/* Executable tests of the production informative commands (act.informative.c).
 * The file is linked with unreferenced sections discarded, so only the
 * fixtures below are needed. db.c is not linked: its globals are defined here.
 * Inputs are heap copies so ASan reports any read past their end.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "act.h"

#include "tb_error_capture.h"

/* --- db.c globals ------------------------------------------------------- */
struct help_index_element *help_table = NULL;
int top_of_helpt = 0;

void basic_mud_log(const char *format, ...) { (void)format; }

void setUp(void)
{
  tb_error_capture_reset();
}
void tearDown(void) {}

/* --- G2-01: search_help bounds ----------------------------------------- */

static struct help_index_element *make_help_table(int count, const char **keys)
{
  /* Exactly 'count' entries, as the boot loader allocates: no slack slot. */
  struct help_index_element *t = calloc((size_t)count, sizeof(*t));
  int i;

  TEST_ASSERT_NOT_NULL(t);
  for (i = 0; i < count; i++) {
    t[i].keywords = strdup(keys[i]);
    t[i].entry = strdup("entry");
  }
  return t;
}

static void free_help_fixture(void)
{
  int i;

  for (i = 0; i < top_of_helpt; i++) {
    free(help_table[i].keywords);
    free(help_table[i].entry);
  }
  free(help_table);
  help_table = NULL;
  top_of_helpt = 0;
}

/* A key that sorts after every keyword used to read help_table[top_of_helpt]. */
static void search_help_key_after_last_entry_misses(void)
{
  const char *keys[] = { "ALPHA", "BETA", "GAMMA" };
  char *key = strdup("ZZZZ");

  help_table = make_help_table(3, keys);
  top_of_helpt = 3;
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help(key, LVL_IMPL));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(key);
  free_help_fixture();
}

static void search_help_finds_first_and_last_entry(void)
{
  const char *keys[] = { "ALPHA", "BETA", "GAMMA" };

  help_table = make_help_table(3, keys);
  top_of_helpt = 3;
  TEST_ASSERT_EQUAL_INT(0, search_help("alp", 0));
  TEST_ASSERT_EQUAL_INT(2, search_help("gamma", 0));
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help("AAA", 0));
  free_help_fixture();
}

static void search_help_empty_table_misses(void)
{
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help("anything", 0));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void search_help_null_table_with_count_is_reported(void)
{
  top_of_helpt = 4;
  TEST_ASSERT_EQUAL_INT(NOWHERE, search_help("x", 0));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("search-index", tb_captured_operation);
  top_of_helpt = 0;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(search_help_key_after_last_entry_misses);
  RUN_TEST(search_help_finds_first_and_last_entry);
  RUN_TEST(search_help_empty_table_misses);
  RUN_TEST(search_help_null_table_with_count_is_reported);
  return UNITY_END();
}
