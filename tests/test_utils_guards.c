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
#include "db.h"
#include "handler.h"

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
    (void)fputs(log_text(), stderr);
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

/* =====================================================================
 * I1-02: stop_follower / die_follower across act() callbacks.
 *
 * act() runs DG ACT triggers synchronously; a trigger can mforce a
 * character to unfollow (re-entering stop_follower) or to follow again.
 * This act_at() replaces the weak stub and plays that trigger.
 * ===================================================================== */

enum act_script { SCRIPT_NONE, SCRIPT_UNFOLLOW_SELF, SCRIPT_UNFOLLOW_OTHER,
                  SCRIPT_REFOLLOW };
static enum act_script act_script;
static struct char_data *act_script_other;
static int act_calls;

char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  struct char_data *master = (struct char_data *)vict_obj;

  (void)src_file; (void)src_line; (void)src_func; (void)hide_invisible;
  (void)obj;
  act_calls++;
  if (type != TO_NOTVICT || str == NULL || strstr(str, "stops following") == NULL)
    return NULL;

  switch (act_script) {
  case SCRIPT_UNFOLLOW_SELF:     /* %force% %actor% unfollow */
    if (ch->master)
      stop_follower(ch);
    break;
  case SCRIPT_UNFOLLOW_OTHER:    /* %force% <other follower> unfollow */
    if (act_script_other && act_script_other->master)
      stop_follower(act_script_other);
    break;
  case SCRIPT_REFOLLOW:          /* %force% %actor% follow <leader> */
    if (ch->master == NULL)
      add_follower(ch, master);
    break;
  default:
    break;
  }
  return NULL;
}

static struct room_data test_room;   /* SECT_INSIDE: always lit */

static void world_setup(void)
{
  memset(&test_room, 0, sizeof(test_room));
  world = &test_room;
  top_of_world = 0;
  act_script = SCRIPT_NONE;
  act_script_other = NULL;
  act_calls = 0;
}

static void world_teardown(void)
{
  world = NULL;
  act_script = SCRIPT_NONE;
}

void test_stop_follower_survives_reentrant_unfollow(void)
{
  struct char_data *a = make_player(1), *b = make_player(2);

  world_setup();
  add_follower(a, b);
  act_script = SCRIPT_UNFOLLOW_SELF;
  stop_follower(a);

  TEST_ASSERT_NULL(a->master);
  TEST_ASSERT_NULL(b->followers);
  world_teardown();
  free_player(a);
  free_player(b);
}

void test_die_follower_survives_callback_unfollowing_next(void)
{
  struct char_data *lead = make_player(10);
  struct char_data *a = make_player(11), *b = make_player(12);

  world_setup();
  add_follower(b, lead);
  add_follower(a, lead);          /* list: a -> b */
  act_script = SCRIPT_UNFOLLOW_OTHER;
  act_script_other = b;           /* a's message makes b unfollow */
  die_follower(lead);

  TEST_ASSERT_NULL(lead->followers);
  TEST_ASSERT_NULL(a->master);
  TEST_ASSERT_NULL(b->master);
  TEST_ASSERT_EQUAL_INT(0, count_records());
  world_teardown();
  free_player(lead);
  free_player(a);
  free_player(b);
}

void test_die_follower_bounds_refollow_loop(void)
{
  struct char_data *lead = make_player(20), *a = make_player(21);

  world_setup();
  add_follower(a, lead);
  act_script = SCRIPT_REFOLLOW;   /* every removal re-adds a */
  die_follower(lead);

  TEST_ASSERT_NULL(lead->followers);
  TEST_ASSERT_NULL(a->master);
  TEST_ASSERT_EQUAL_INT(1, count_op("follower-readded"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "player idnum 20"));
  world_teardown();
  free_player(lead);
  free_player(a);
}

/* =====================================================================
 * I1-11: an inconsistent follower list is reported, not dereferenced.
 * ===================================================================== */

void test_stop_follower_missing_node_reports_and_clears_master(void)
{
  struct char_data *a = make_player(31), *b = make_player(32);
  struct char_data *c = make_player(33);

  world_setup();
  add_follower(c, b);             /* b's list holds c only */
  a->master = b;                  /* corrupt: a claims b, b does not list a */
  stop_follower(a);

  TEST_ASSERT_NULL(a->master);
  TEST_ASSERT_NOT_NULL(b->followers);
  TEST_ASSERT_TRUE(b->followers->follower == c);
  TEST_ASSERT_EQUAL_INT(1, count_op("follower-unlink"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "player idnum 31"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "player idnum 32"));
  stop_follower(c);
  world_teardown();
  free_player(a);
  free_player(b);
  free_player(c);
}

void test_stop_follower_without_master_reports(void)
{
  struct char_data *a = make_player(41);

  world_setup();
  stop_follower(a);
  TEST_ASSERT_EQUAL_INT(1, count_op("follower-no-master"));
  world_teardown();
  free_player(a);
}

void test_add_follower_twice_reports_and_keeps_one_node(void)
{
  struct char_data *a = make_player(51), *b = make_player(52);
  struct char_data *c = make_player(53);

  world_setup();
  add_follower(a, b);
  add_follower(a, c);

  TEST_ASSERT_TRUE(a->master == b);
  TEST_ASSERT_NULL(c->followers);
  TEST_ASSERT_EQUAL_INT(1, count_op("follower-add"));
  stop_follower(a);
  world_teardown();
  free_player(a);
  free_player(b);
  free_player(c);
}

/* =====================================================================
 * I1-04: get_line consumes whole physical lines and reports overlong ones.
 * ===================================================================== */

static FILE *file_with(const char *text)
{
  FILE *fp = tmpfile();

  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_TRUE(fputs(text, fp) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0L, SEEK_SET));
  return fp;
}

void test_get_line_long_line_tail_is_discarded_and_reported(void)
{
  char line[400], text[512], buf[READ_SIZE];
  FILE *fp;

  /* A zone reset line whose comment carries a 300-byte mob short desc. */
  memset(line, 'a', sizeof(line));
  memcpy(line, "M 0 3001 1 3001 \t(", 18);
  line[320] = '\0';
  (void)snprintf(text, sizeof(text), "%s\nS\n", line);
  fp = file_with(text);
  tb_parse_context_begin(fp, "test.zon", "zone");

  TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
  TEST_ASSERT_EQUAL_INT('M', buf[0]);
  TEST_ASSERT_EQUAL_INT(READ_SIZE - 1, (int)strlen(buf));
  TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
  TEST_ASSERT_EQUAL_STRING("S", buf);   /* not the tail of the M line */
  TEST_ASSERT_EQUAL_INT(1, count_op("line-too-long"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "test.zon"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "65 tail bytes"));

  tb_parse_context_end(fp);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

void test_get_line_exact_fit_line_is_silent(void)
{
  char line[READ_SIZE + 8], buf[READ_SIZE];
  FILE *fp;

  /* 255 bytes plus CRLF: fills the buffer, nothing is lost. */
  memset(line, 'b', READ_SIZE - 1);
  memcpy(line + READ_SIZE - 1, "\r\nS\n", 5);
  fp = file_with(line);

  TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
  TEST_ASSERT_EQUAL_INT(READ_SIZE - 1, (int)strlen(buf));
  TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
  TEST_ASSERT_EQUAL_STRING("S", buf);
  TEST_ASSERT_EQUAL_INT(0, count_records());
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

void test_get_line_long_comment_is_skipped_silently(void)
{
  char text[700], buf[READ_SIZE];
  FILE *fp;

  memset(text, 'c', 600);
  text[0] = '*';
  memcpy(text + 600, "\nS\n", 4);
  fp = file_with(text);

  TEST_ASSERT_EQUAL_INT(2, get_line(fp, buf));
  TEST_ASSERT_EQUAL_STRING("S", buf);
  TEST_ASSERT_EQUAL_INT(0, count_records());
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* =====================================================================
 * I1-05: count_non_protocol_chars stops at the terminator inside an escape.
 * Heap copies sized exactly by strdup() let ASan see any over-read.
 * ===================================================================== */

static int visible_chars(const char *text)
{
  char *copy = strdup(text);
  int n;

  TEST_ASSERT_NOT_NULL(copy);
  n = count_non_protocol_chars(copy);
  free(copy);
  return n;
}

void test_count_non_protocol_trailing_at_sign(void)
{
  TEST_ASSERT_EQUAL_INT(4, visible_chars("Room@"));
  TEST_ASSERT_EQUAL_INT(1, count_op("protocol-escape-truncated"));
}

void test_count_non_protocol_unterminated_bracket(void)
{
  TEST_ASSERT_EQUAL_INT(1, visible_chars("x\t[ab"));
  TEST_ASSERT_EQUAL_INT(1, count_op("protocol-escape-truncated"));
}

void test_count_non_protocol_complete_escapes_are_silent(void)
{
  TEST_ASSERT_EQUAL_INT(5, visible_chars("\tRHall\t[b]x"));
  TEST_ASSERT_EQUAL_INT(0, count_records());
}

/* =====================================================================
 * I1-06: count_color_chars and a lone trailing tab.
 * ===================================================================== */

static int color_chars(const char *text)
{
  char *copy = strdup(text);
  int n;

  TEST_ASSERT_NOT_NULL(copy);
  n = count_color_chars(copy);
  free(copy);
  return n;
}

void test_count_color_chars_trailing_tab(void)
{
  TEST_ASSERT_EQUAL_INT(1, color_chars("Hall\t"));
  TEST_ASSERT_EQUAL_INT(1, count_op("protocol-escape-truncated"));
}

void test_count_color_chars_complete_codes_are_silent(void)
{
  TEST_ASSERT_EQUAL_INT(4, color_chars("\tRHall\tn"));
  TEST_ASSERT_EQUAL_INT(0, count_records());
}

/* =====================================================================
 * I1-07: strfrmt with NULL text, and no stale output from its static buffer.
 * ===================================================================== */

void test_strfrmt_null_text_reports_and_returns_empty(void)
{
  char *out = strfrmt(NULL, 60, 5, 0, 1, 1);

  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_NULL(strstr(out, "Room"));
  TEST_ASSERT_EQUAL_INT(1, count_op("strfrmt-null"));
}

void test_strfrmt_empty_text_does_not_return_previous_output(void)
{
  char first[] = "A previous player's room description.";
  char empty[] = "   ";
  char *out;

  out = strfrmt(first, 60, 1, FALSE, FALSE, FALSE);
  TEST_ASSERT_NOT_NULL(strstr(out, "previous"));
  out = strfrmt(empty, 60, 1, FALSE, FALSE, FALSE);
  TEST_ASSERT_NULL(strstr(out, "previous"));
  TEST_ASSERT_EQUAL_INT(0, count_records());
}

/* =====================================================================
 * I1-08: player_specials on a mob is reported with the caller's location.
 * ===================================================================== */

void test_check_player_special_on_mob_reports_call_site(void)
{
  struct char_data mob;
  char where[64];
  int width;

  memset(&mob, 0, sizeof(mob));
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_ISNPC);
  mob.nr = NOBODY;
  mob.player_specials = &dummy_mob;

  (void)snprintf(where, sizeof(where), "test_utils_guards.c:%d", __LINE__ + 1);
  width = GET_SCREEN_WIDTH(&mob);

  TEST_ASSERT_EQUAL_INT(dummy_mob.saved.screen_width, width);
  TEST_ASSERT_EQUAL_INT(1, count_op("player-special-on-mob"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), where));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "screen_width"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "mob vnum"));
}

/* =====================================================================
 * I1-09: an invalid rnum in IS_DARK is reported with the caller.
 * ===================================================================== */

void test_is_dark_invalid_rnum_reports_caller(void)
{
  char where[64];
  int dark;

  world_setup();
  (void)snprintf(where, sizeof(where), "test_utils_guards.c:%d", __LINE__ + 1);
  dark = IS_DARK(NOWHERE);

  TEST_ASSERT_FALSE(dark);
  TEST_ASSERT_EQUAL_INT(1, count_op("room-rnum-invalid"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), where));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "function=test_is_dark_invalid_rnum_reports_caller"));
  world_teardown();
}

void test_is_dark_valid_room_is_silent(void)
{
  world_setup();
  TEST_ASSERT_FALSE(IS_DARK(0));
  TEST_ASSERT_EQUAL_INT(0, count_records());
  world_teardown();
}

/* =====================================================================
 * I1-10: inverted rand_number bounds are reported with the caller.
 * ===================================================================== */

void test_rand_number_inverted_bounds_reports_caller(void)
{
  char where[64];
  int i, v;

  (void)snprintf(where, sizeof(where), "test_utils_guards.c:%d", __LINE__ + 1);
  v = rand_number(5, 2);
  TEST_ASSERT_TRUE(v >= 2 && v <= 5);
  TEST_ASSERT_EQUAL_INT(1, count_op("rand-range-inverted"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), where));

  for (i = 0; i < 50; i++) {
    v = rand_number(2, 5);
    TEST_ASSERT_TRUE(v >= 2 && v <= 5);
  }
  TEST_ASSERT_EQUAL_INT(1, count_records());
}

/* =====================================================================
 * I1-12: mudlog(NULL) and an unknown get_filename mode are reported.
 * ===================================================================== */

void test_mudlog_null_format_reports(void)
{
  mudlog(NRM, LVL_IMMORT, TRUE, NULL);
  TEST_ASSERT_EQUAL_INT(1, count_op("mudlog-null-format"));
}

void test_get_filename_unknown_mode_reports(void)
{
  char path[PATH_MAX];

  TEST_ASSERT_EQUAL_INT(0, get_filename(path, sizeof(path), 9999, "bob"));
  TEST_ASSERT_EQUAL_INT(1, count_op("filename-mode"));
}

void test_get_filename_null_name_reports(void)
{
  char path[PATH_MAX];

  TEST_ASSERT_EQUAL_INT(0, get_filename(path, sizeof(path), PLR_FILE, NULL));
  TEST_ASSERT_EQUAL_INT(1, count_op("filename-args"));
}

/* =====================================================================
 * I1-13: get_filename refuses names that would leave the data directory.
 * ===================================================================== */

void test_get_filename_rejects_path_traversal(void)
{
  char path[PATH_MAX];

  path[0] = '\0';
  TEST_ASSERT_EQUAL_INT(0, get_filename(path, sizeof(path), CRASH_FILE,
                                        "../../etc/x"));
  TEST_ASSERT_NULL(strstr(path, ".."));
  TEST_ASSERT_EQUAL_INT(1, count_op("filename-name-invalid"));
  TEST_ASSERT_NULL(strstr(log_text(), "etc"));   /* raw text not logged */
}

void test_get_filename_accepts_player_name(void)
{
  char path[PATH_MAX];

  TEST_ASSERT_EQUAL_INT(1, get_filename(path, sizeof(path), CRASH_FILE, "Bob"));
  TEST_ASSERT_NOT_NULL(strstr(path, "A-E" SLASH "bob."));
  TEST_ASSERT_EQUAL_INT(0, count_records());
}

/* =====================================================================
 * STARTS_WITH_VOWEL / AN: empty and NULL strings take "a".
 * ===================================================================== */

void test_an_handles_empty_and_null(void)
{
  const char *none = NULL;

  TEST_ASSERT_EQUAL_STRING("an", AN("apple"));
  TEST_ASSERT_EQUAL_STRING("a", AN("pear"));
  TEST_ASSERT_EQUAL_STRING("a", AN(""));
  TEST_ASSERT_EQUAL_STRING("a", TANA(none));
}

void test_nullstr_maps_null_to_empty(void)
{
  const char *none = NULL;
  int calls = 0;

  TEST_ASSERT_EQUAL_STRING("", NULLSTR(none));
  TEST_ASSERT_EQUAL_STRING("x", NULLSTR("x"));
  /* The argument is evaluated once. */
  TEST_ASSERT_EQUAL_STRING("", NULLSTR((calls++, none)));
  TEST_ASSERT_EQUAL_INT(1, calls);
}

/* =====================================================================
 * G2-12: CLASS_ABBR range-checks a pfile class before indexing
 * class_abbrevs[], and reports the player and the caller.
 * ===================================================================== */

/* class.c's table (not linked here). */
const char *class_abbrevs[] = { "Wi", "Cl", "Th", "Wa", "Mo", "\n" };

void test_class_abbr_out_of_range_class_reports_caller(void)
{
  struct char_data *c = make_player(4242);
  char where[64];
  const char *abbr;

  GET_CLASS(c) = 40;              /* 'Clas: 40' from a hand-edited pfile */
  (void)snprintf(where, sizeof(where), "test_utils_guards.c:%d", __LINE__ + 1);
  abbr = CLASS_ABBR(c);
  TEST_ASSERT_EQUAL_STRING("??", abbr);
  TEST_ASSERT_EQUAL_INT(1, count_op("class-range"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), where));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "player idnum 4242 class 40"));

  GET_CLASS(c) = -3;
  TEST_ASSERT_EQUAL_STRING("??", CLASS_ABBR(c));
  TEST_ASSERT_EQUAL_INT(2, count_op("class-range"));
  free_player(c);
}

void test_class_abbr_valid_class_and_npc_are_silent(void)
{
  struct char_data *c = make_player(7);

  GET_CLASS(c) = CLASS_WARRIOR;
  TEST_ASSERT_EQUAL_STRING("Wa", CLASS_ABBR(c));
  GET_CLASS(c) = NUM_CLASSES - 1;
  TEST_ASSERT_EQUAL_STRING("Mo", CLASS_ABBR(c));
  SET_BIT_AR(MOB_FLAGS(c), MOB_ISNPC);
  GET_CLASS(c) = 40;              /* NPC classes never index the table */
  TEST_ASSERT_EQUAL_STRING("--", CLASS_ABBR(c));
  TEST_ASSERT_EQUAL_INT(0, count_records());
  free_player(c);
}

/* =====================================================================
 * TB_TABLE_NAME: a name-table lookup that reports a bad index at its
 * caller instead of reading past the table.
 * ===================================================================== */

void test_table_name_out_of_range_reports_caller(void)
{
  static const char *colours[] = { "red", "green", "\n" };
  char where[64];

  (void)snprintf(where, sizeof(where), "test_utils_guards.c:%d", __LINE__ + 1);
  TEST_ASSERT_EQUAL_STRING("<invalid>", TB_TABLE_NAME(colours, 2, 7));
  TEST_ASSERT_EQUAL_INT(1, count_op("name-lookup-range"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), where));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "resource=colours"));
  TEST_ASSERT_NOT_NULL(strstr(log_text(), "index 7 not in [0,2)"));

  TEST_ASSERT_EQUAL_STRING("<invalid>", TB_TABLE_NAME(colours, 2, -1));
  TEST_ASSERT_EQUAL_INT(2, count_op("name-lookup-range"));
}

void test_table_name_valid_index_is_silent(void)
{
  static const char *colours[] = { "red", "green", "\n" };

  TEST_ASSERT_EQUAL_STRING("red", TB_TABLE_NAME(colours, 2, 0));
  TEST_ASSERT_EQUAL_STRING("green", TB_TABLE_NAME(colours, 2, 1));
  TEST_ASSERT_EQUAL_INT(0, count_records());
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_column_list_long_list_truncates_and_reports);
  RUN_TEST(test_column_list_short_list_is_silent);
  RUN_TEST(test_stop_follower_survives_reentrant_unfollow);
  RUN_TEST(test_die_follower_survives_callback_unfollowing_next);
  RUN_TEST(test_die_follower_bounds_refollow_loop);
  RUN_TEST(test_stop_follower_missing_node_reports_and_clears_master);
  RUN_TEST(test_stop_follower_without_master_reports);
  RUN_TEST(test_add_follower_twice_reports_and_keeps_one_node);
  RUN_TEST(test_get_line_long_line_tail_is_discarded_and_reported);
  RUN_TEST(test_get_line_exact_fit_line_is_silent);
  RUN_TEST(test_get_line_long_comment_is_skipped_silently);
  RUN_TEST(test_count_non_protocol_trailing_at_sign);
  RUN_TEST(test_count_non_protocol_unterminated_bracket);
  RUN_TEST(test_count_non_protocol_complete_escapes_are_silent);
  RUN_TEST(test_count_color_chars_trailing_tab);
  RUN_TEST(test_count_color_chars_complete_codes_are_silent);
  RUN_TEST(test_strfrmt_null_text_reports_and_returns_empty);
  RUN_TEST(test_strfrmt_empty_text_does_not_return_previous_output);
  RUN_TEST(test_check_player_special_on_mob_reports_call_site);
  RUN_TEST(test_is_dark_invalid_rnum_reports_caller);
  RUN_TEST(test_is_dark_valid_room_is_silent);
  RUN_TEST(test_rand_number_inverted_bounds_reports_caller);
  RUN_TEST(test_mudlog_null_format_reports);
  RUN_TEST(test_get_filename_unknown_mode_reports);
  RUN_TEST(test_get_filename_null_name_reports);
  RUN_TEST(test_get_filename_rejects_path_traversal);
  RUN_TEST(test_get_filename_accepts_player_name);
  RUN_TEST(test_an_handles_empty_and_null);
  RUN_TEST(test_nullstr_maps_null_to_empty);
  RUN_TEST(test_class_abbr_out_of_range_class_reports_caller);
  RUN_TEST(test_class_abbr_valid_class_and_npc_are_silent);
  RUN_TEST(test_table_name_out_of_range_reports_caller);
  RUN_TEST(test_table_name_valid_index_is_silent);
  return UNITY_END();
}
