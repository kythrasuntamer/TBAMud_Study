/* Executable tests of the production OLC editors (hedit.c and friends).
 *
 * Each editor is linked from its real source file and driven through its
 * public entry points (do_oasis_*, *_parse, *_string_cleanup) with a small
 * fixture world.  Everything the editors call outside their own file is
 * supplied here; unrelated code is discarded with --gc-sections (see
 * tests/CMakeLists.txt).  Tables are heap-allocated at their exact size so
 * ASan reports any index past the end, and each guard is checked for the
 * structured diagnostic it must emit (fail loudly), not just for not
 * crashing.
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
#include "hedit.h"
#include "modify.h"
#include <stdarg.h>
#include "tb_error_capture.h"

/* ---------------------------------------------------------------------
 * Output capture
 * ------------------------------------------------------------------- */
static char out[MAX_STRING_LENGTH * 2];

static void out_append(const char *fmt, va_list args)
{
  size_t len = strlen(out);

  if (len < sizeof(out) - 1 &&
      vsnprintf(out + len, sizeof(out) - len, fmt, args) < 0)
    out[len] = '\0';
}

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  (void)d;
  va_start(args, txt);
  out_append(txt, args);
  va_end(args);
  return strlen(out);
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  out_append(messg, args);
  va_end(args);
  return strlen(out);
}

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }

/* ---------------------------------------------------------------------
 * utils.c / genolc.c / interpreter.c helpers (utils.c is not linked: it owns
 * the real tb_error_log_at, which tb_error_capture.h replaces)
 * ------------------------------------------------------------------- */
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  void *p;
  (void)source_file; (void)source_line; (void)source_func;
  p = calloc(count ? count : 1, element_size ? element_size : 1);
  if (!p)
    abort();
  return p;
}
char *str_udup(const char *txt)
{ return strdup((txt && *txt) ? txt : "undefined"); }
void strip_cr(char *buffer)
{
  int rpos, wpos;
  if (buffer == NULL)
    return;
  for (rpos = 0, wpos = 0; buffer[rpos]; rpos++) {
    buffer[wpos] = buffer[rpos];
    wpos += (buffer[rpos] != '\r');
  }
  buffer[wpos] = '\0';
}
char *convert_from_tabs(char *string) { return string; }
char *one_argument(char *argument, char *first_arg)
{
  char *start;
  size_t n = 0;
  while (*argument == ' ')
    argument++;
  start = argument;
  while (*argument && *argument != ' ')
    argument++;
  n = (size_t)(argument - start);
  if (n >= MAX_INPUT_LENGTH)
    n = MAX_INPUT_LENGTH - 1;
  memcpy(first_arg, start, n);
  first_arg[n] = '\0';
  return argument;
}

/* ---------------------------------------------------------------------
 * OLC / editor collaborators
 * ------------------------------------------------------------------- */
const char *nrm = "", *grn = "", *cyn = "", *yel = "";
void get_char_colors(struct char_data *ch) { (void)ch; }
void clear_screen(struct descriptor_data *d) { (void)d; }
int can_edit_zone(struct char_data *ch, zone_rnum rnum)
{ (void)ch; (void)rnum; return TRUE; }
void add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; }
int remove_from_save_list(zone_vnum zone, int type)
{ (void)zone; (void)type; return TRUE; }

static char **string_write_target;
static char *string_write_backstr;
static int string_writes;

void string_write(struct descriptor_data *d, char **writeto, size_t len,
                  long mailto, void *data)
{
  (void)len; (void)mailto;
  string_writes++;
  string_write_target = writeto;
  string_write_backstr = data;
  d->str = writeto;
  d->backstr = data;
}

/* The parts of oasis.c's cleanup_olc() these editors rely on. */
static int cleanups;
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  if (d->olc == NULL)
    return;
  cleanups++;
  if (OLC_HELP(d)) {
    if (cleanup_type == CLEANUP_ALL) {
      free(OLC_HELP(d)->keywords);
      if (!OLC_HELP(d)->duplicate)
        free(OLC_HELP(d)->entry);
    }
    free(OLC_HELP(d));
  }
  free(OLC_STORAGE(d));
  STATE(d) = CON_PLAYING;
  free(d->olc);
  d->olc = NULL;
}

/* ---------------------------------------------------------------------
 * Help table fixture
 * ------------------------------------------------------------------- */
struct help_index_element *help_table = NULL;
int top_of_helpt = 0;
static int help_reboots;

static int search_result;
int search_help(const char *argument, int level)
{ (void)argument; (void)level; return search_result; }

void free_help_table(void)
{
  int i;

  for (i = 0; i < top_of_helpt; i++) {
    free(help_table[i].keywords);
    if (!help_table[i].duplicate)
      free(help_table[i].entry);
  }
  free(help_table);
  help_table = NULL;
  top_of_helpt = 0;
}

/* hedit_save_to_disk reboots the table after a committed write.  The test
 * keeps the in-memory table instead, so it can inspect what was stored. */
void index_boot(int mode) { (void)mode; help_reboots++; }

/* Rows: "alpha" (owner), "alpha2" (duplicate of row 0), "beta". */
static void help_fixture(void)
{
  free_help_table();
  top_of_helpt = 3;
  help_table = calloc((size_t)top_of_helpt, sizeof(*help_table));
  help_table[0].keywords = strdup("alpha");
  help_table[0].entry = strdup("ALPHA\r\n\r\nAlpha text.\r\n");
  help_table[1].keywords = strdup("alpha2");
  help_table[1].entry = help_table[0].entry;
  help_table[1].duplicate = 1;
  help_table[2].keywords = strdup("beta");
  help_table[2].entry = strdup("BETA\r\n\r\nBeta text.\r\n");
}

/* is_abbrev is real in interpreter.c; the editors only need prefix match. */
int is_abbrev(const char *arg1, const char *arg2)
{
  if (!*arg1)
    return 0;
  for (; *arg1 && *arg2; arg1++, arg2++)
    if (LOWER(*arg1) != LOWER(*arg2))
      return 0;
  return !*arg1;
}

/* ---------------------------------------------------------------------
 * Builder fixture
 * ------------------------------------------------------------------- */
static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;

static void builder_fixture(void)
{
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  memset(&builder_desc, 0, sizeof(builder_desc));
  builder.player_specials = &builder_specials;
  builder.player.name = "Builder";
  builder.player.level = LVL_IMPL;
  builder.char_specials.saved.idnum = 42;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
  builder_desc.connected = CON_PLAYING;
  descriptor_list = &builder_desc;
}

static void type(const char *line)
{
  char buf[MAX_INPUT_LENGTH];

  strlcpy(buf, line, sizeof(buf));
  out[0] = '\0';
  switch (STATE(&builder_desc)) {
  case CON_HEDIT: hedit_parse(&builder_desc, buf); break;
  default: TEST_FAIL_MESSAGE("typed into a descriptor that is not editing");
  }
}

void setUp(void)
{
  tb_error_capture_reset();
  out[0] = '\0';
  cleanups = help_reboots = string_writes = 0;
  string_write_target = NULL;
  string_write_backstr = NULL;
  builder_fixture();
  help_fixture();
}

void tearDown(void)
{
  if (builder_desc.olc)
    cleanup_olc(&builder_desc, CLEANUP_ALL);
  free_help_table();
  descriptor_list = NULL;
}

/* ---------------------------------------------------------------------
 * hedit
 * ------------------------------------------------------------------- */

/* F2-01: answering 'n' when the next row does not match must move to
 * "add" with OLC_ZNUM == NOWHERE; it used to leave top_of_helpt + 2 there and
 * the save wrote past help_table. */
static void test_hedit_no_then_add_appends_instead_of_writing_past_table(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_HEDIT, STATE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_EDIT, OLC_MODE(&builder_desc));

  type("n");
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_ADD, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(NOWHERE, OLC_ZNUM(&builder_desc));

  type("y");
  type("2");
  type("5");
  type("q");
  type("y");
  TEST_ASSERT_EQUAL_INT(4, top_of_helpt);
  TEST_ASSERT_EQUAL_STRING("beta", help_table[3].keywords);
  TEST_ASSERT_EQUAL_INT(5, help_table[3].min_level);
  TEST_ASSERT_NULL(builder_desc.olc);
}

/* F2-01: a matching later row is offered as an edit, not skipped. */
static void test_hedit_no_offers_next_matching_row(void)
{
  char cmd[] = " alpha";

  search_result = 0;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("n");
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_EDIT, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(1, OLC_ZNUM(&builder_desc));
  type("q");
}

/* F2-01: a save whose row went stale is refused with a diagnostic. */
static void test_hedit_save_with_stale_rnum_is_refused_loudly(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  type("2");
  type("5");
  type("q");
  OLC_ZNUM(&builder_desc) = 7;  /* simulate a corrupted index */
  type("y");
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("hedit-rnum-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
  TEST_ASSERT_EQUAL_INT(3, top_of_helpt);
  TEST_ASSERT_NULL(builder_desc.olc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_hedit_no_then_add_appends_instead_of_writing_past_table);
  RUN_TEST(test_hedit_no_offers_next_matching_row);
  RUN_TEST(test_hedit_save_with_stale_rnum_is_refused_loudly);
  return UNITY_END();
}
