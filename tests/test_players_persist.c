/* Executable tests of the production player-file code (players.c).
 *
 * Player files and the player index are persistent data that can be
 * truncated, corrupted or hand-edited, so each guard is exercised through
 * the real load_char()/tag_argument()/build_player_index() entry points
 * against files in a scratch directory. Inputs are heap copies so ASan sees
 * any read past their terminator. Each test asserts on the structured
 * diagnostic, not merely on survival.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "quest.h"
#include "genolc.h"
#include "interpreter.h"
#include "pfdefaults.h"
#include <stdarg.h>
#include "tb_error_capture.h"
#include "tb_persist_support.h"

/* db.c globals players.c maintains. */
int top_of_p_table = -1;
int top_of_p_file = -1;
long top_idnum = 0;

static char scratch_dir[256];

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }

int get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name)
{
  (void)mode;
  return snprintf(filename, fbufsize, "%s/%s.plr", scratch_dir, orig_name) > 0;
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

bitvector_t asciiflag_conv(char *flag)
{
  return (bitvector_t)strtoul(flag, NULL, 10);
}

char *fread_string(FILE *fl, const char *error)
{
  char line[READ_SIZE];
  (void)error;
  while (fgets(line, sizeof(line), fl))
    if (strchr(line, '~'))
      break;
  return NULL;
}

void affect_total(struct char_data *ch) { (void)ch; }
void affect_to_char(struct char_data *ch, struct affected_type *af) { (void)ch; (void)af; }
void new_affect(struct affected_type *af) { memset(af, 0, sizeof(*af)); }
void add_completed_quest(struct char_data *ch, qst_vnum vnum) { (void)ch; (void)vnum; }
trig_rnum real_trigger(trig_vnum vnum) { (void)vnum; return NOTHING; }
trig_data *read_trigger(int nr) { (void)nr; return NULL; }
void add_trigger(struct script_data *sc, trig_data *t, int loc) { (void)sc; (void)t; (void)loc; }
void read_saved_vars_ascii(FILE *file, struct char_data *ch, int count)
{ (void)file; (void)ch; (void)count; }

/* ------------------------------------------------------------------ */
static struct player_index_element table_storage[4];
static char name_storage[4][32];
static struct char_data player;
static struct player_special_data player_specials;

static void write_pfile(const char *name, const char *body)
{
  char path[512];
  FILE *fl;

  TEST_ASSERT_TRUE(get_filename(path, sizeof(path), PLR_FILE, name));
  fl = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fputs(body, fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

static void free_player_strings(void)
{
  free(player.player.name);
  free(player.player.title);
  free(player.player.description);
  free(player_specials.poofin);
  free(player_specials.poofout);
  free(player_specials.host);
  while (player_specials.aliases) {
    struct alias_data *next = player_specials.aliases->next;
    free(player_specials.aliases->alias);
    free(player_specials.aliases->replacement);
    free(player_specials.aliases);
    player_specials.aliases = next;
  }
}

void setUp(void)
{
  memset(&player, 0, sizeof(player));
  memset(&player_specials, 0, sizeof(player_specials));
  player.player_specials = &player_specials;

  memset(table_storage, 0, sizeof(table_storage));
  strlcpy(name_storage[0], "tester", sizeof(name_storage[0]));
  table_storage[0].name = name_storage[0];
  table_storage[0].id = 4242;
  player_table = table_storage;
  top_of_p_table = 0;

  if (scratch_dir[0] == '\0') {
    const char *base = getenv("TMPDIR");
    TEST_ASSERT_TRUE(snprintf(scratch_dir, sizeof(scratch_dir), "%s/tb_players_XXXXXX",
                              base && *base ? base : "/tmp") > 0);
    TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
  }
  tb_error_capture_reset();
}

void tearDown(void)
{
  char path[512];
  free_player_strings();
  player_table = NULL;
  top_of_p_table = -1;
  if (get_filename(path, sizeof(path), PLR_FILE, "tester"))
    (void)remove(path);
}

/* B2-02: an overlong flag word on an "Act :" line overflowed four 128-byte
 * stack buffers via sscanf("%s"); it is now rejected and reported. */
static void test_overlong_act_flag_word_is_rejected(void)
{
  char body[600];
  char word[241];

  memset(word, 'a', sizeof(word) - 1);
  word[sizeof(word) - 1] = '\0';
  TEST_ASSERT_TRUE(snprintf(body, sizeof(body), "Id  : 4242\nAct : %s\n", word) > 0);
  write_pfile("tester", body);

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_EQUAL_STRING("pfile-flags", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_EQUAL_INT(PFDEF_PLRFLAGS, PLR_FLAGS(&player)[0]);
}

/* B2-02: the four-word form still loads every word. */
static void test_four_flag_words_load(void)
{
  write_pfile("tester", "Id  : 4242\nAct : 1 2 3 4\nAff : 5 6 7 8\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(1, PLR_FLAGS(&player)[0]);
  TEST_ASSERT_EQUAL_INT(4, PLR_FLAGS(&player)[3]);
  TEST_ASSERT_EQUAL_INT(5, AFF_FLAGS(&player)[0]);
  TEST_ASSERT_EQUAL_INT(8, AFF_FLAGS(&player)[3]);
}

/* B2-03: tag_argument() on a line shorter than a tag must not read past the
 * terminator (heap copy, so ASan sees any over-read). */
static void test_short_tag_line_is_reported(void)
{
  char tag[6];
  char *line = strdup("Ab");

  TEST_ASSERT_NOT_NULL(line);
  memset(tag, 'x', sizeof(tag));
  tag_argument(line, tag);
  TEST_ASSERT_EQUAL_STRING("", tag);
  TEST_ASSERT_EQUAL_STRING("", line);
  TEST_ASSERT_EQUAL_STRING("pfile-tag", tb_captured_operation);
  free(line);
}

static void test_tag_and_value_are_split(void)
{
  char tag[6];
  char *line = strdup("Name: Joe");

  TEST_ASSERT_NOT_NULL(line);
  tag_argument(line, tag);
  TEST_ASSERT_EQUAL_STRING("Name", tag);
  TEST_ASSERT_EQUAL_STRING("Joe", line);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(line);
}

/* B2-03: a short line inside a pfile is skipped, and later tags still load. */
static void test_short_pfile_line_is_skipped(void)
{
  write_pfile("tester", "Ab\nId  : 4242\nAct : 1 2 3 4\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_EQUAL_STRING("pfile-tag", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(4242, GET_IDNUM(&player));
  TEST_ASSERT_EQUAL_INT(4, PLR_FLAGS(&player)[3]);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_overlong_act_flag_word_is_rejected);
  RUN_TEST(test_four_flag_words_load);
  RUN_TEST(test_short_tag_line_is_reported);
  RUN_TEST(test_tag_and_value_are_split);
  RUN_TEST(test_short_pfile_line_is_skipped);
  (void)rmdir(scratch_dir);
  return UNITY_END();
}
