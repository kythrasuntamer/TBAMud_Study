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
#include <sys/stat.h>
#include "tb_error_capture.h"
#include "tb_persist_support.h"
#ifdef TB_SAVE_FAULT_INJECTION
#include <dirent.h>
#include <errno.h>
#endif

/* db.c globals players.c maintains. */
int top_of_p_table = -1;
int top_of_p_file = -1;
long top_idnum = 0;
struct index_data **trig_index = NULL;

static char scratch_dir[256];

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }

/* utils.c's get_filename() accepts only a legal player name (1 to
 * MAX_NAME_LENGTH ASCII letters), reports anything else as
 * persist/filename-unsafe, leaves filename "" and returns 0. This stand-in
 * is looser (any [A-Za-z0-9_-] name) and quiet, so tests see only the
 * caller's report; what matters here is that a path-like name is refused. */
int get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name)
{
  const char *p;

  (void)mode;
  if (orig_name == NULL || *orig_name == '\0')
    return 0;
  for (p = orig_name; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
      return 0;
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

void parse_at(char *str) { (void)str; }

void affect_total(struct char_data *ch) { (void)ch; }
/* Like handler.c: the character gets its own copy, at the head of the list. */
void affect_to_char(struct char_data *ch, struct affected_type *af)
{
  struct affected_type *copy = malloc(sizeof(*copy));
  TEST_ASSERT_NOT_NULL(copy);
  *copy = *af;
  copy->next = ch->affected;
  ch->affected = copy;
}
void new_affect(struct affected_type *af) { memset(af, 0, sizeof(*af)); }
void add_completed_quest(struct char_data *ch, qst_vnum vnum) { (void)ch; (void)vnum; }
/* quest.c: vnum 300 is the only loaded quest; clear_quest() as in quest.c. */
qst_rnum real_quest(qst_vnum vnum) { return vnum == 300 ? 0 : NOTHING; }
void clear_quest(struct char_data *ch)
{
  GET_QUEST(ch) = NOTHING;
  GET_QUEST_TIME(ch) = -1;
  GET_QUEST_COUNTER(ch) = 0;
  REMOVE_BIT_AR(PRF_FLAGS(ch), PRF_QUEST);
}
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

/* B2-07: an unterminated Desc used to reach fread_string(), which exit(1)s
 * the whole server (and printed an uninitialised context buffer). */
static void test_unterminated_desc_is_dropped_not_fatal(void)
{
  write_pfile("tester", "Id  : 4242\nDesc:\nA tall stranger\nwith no end");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_NULL(player.player.description);
  TEST_ASSERT_EQUAL_STRING("pfile-desc", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
}

/* B2-07: an oversize Desc is consumed up to its '~' and later tags load. */
static void test_oversize_desc_is_skipped(void)
{
  static char body[MAX_STRING_LENGTH + 4096];
  char line[201];
  size_t used = 0;
  int i;

  memset(line, 'd', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  used += (size_t)snprintf(body + used, sizeof(body) - used, "Id  : 4242\nDesc:\n");
  for (i = 0; i < (MAX_STRING_LENGTH / 200) + 4; i++)
    used += (size_t)snprintf(body + used, sizeof(body) - used, "%s\n", line);
  TEST_ASSERT_TRUE(snprintf(body + used, sizeof(body) - used, "~\nAct : 1 2 3 4\n") > 0);
  write_pfile("tester", body);

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_NULL(player.player.description);
  TEST_ASSERT_EQUAL_STRING("pfile-desc", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(4, PLR_FLAGS(&player)[3]);
}

/* Write a player file that may hold NUL bytes. */
static void write_pfile_bytes(const char *name, const char *body, size_t len)
{
  char path[512];
  FILE *fl;

  TEST_ASSERT_TRUE(get_filename(path, sizeof(path), PLR_FILE, name));
  fl = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(len, fwrite(body, 1, len, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

/* Fuzzer PLAYER-4 (fread-string-nul): a '~'-string line whose first byte is
 * NUL reads as empty, and fread_string_checked() scanned back from tmp[-1]
 * (stack-buffer-underflow under ASan). That chunk is dropped and reported;
 * the rest of the description still loads. */
static void test_desc_line_starting_with_nul_is_dropped(void)
{
  static const char body[] = "Id  : 4242\nDesc:\n\0hidden\nA tall stranger.\n~\n";

  write_pfile_bytes("tester", body, sizeof(body) - 1);

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_STRING("A tall stranger.\r\n", player.player.description);
  TEST_ASSERT_EQUAL_STRING("fread-string\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "Desc: NUL byte in string data"));
}

static void test_terminated_desc_loads(void)
{
  write_pfile("tester", "Id  : 4242\nDesc:\nA tall stranger.\n~\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_EQUAL_STRING("A tall stranger.\r\n", player.player.description);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

int sprintascii(char *out, bitvector_t bits)
{
  return sprintf(out, "%lu", (unsigned long)bits);
}

/* The player index lives at the relative path LIB_PLRFILES INDEX_FILE, so
 * the index tests run inside the scratch directory. */
static char start_dir[1024];

static void enter_scratch(void)
{
  char sub[300];
  TEST_ASSERT_NOT_NULL(getcwd(start_dir, sizeof(start_dir)));
  TEST_ASSERT_EQUAL_INT(0, chdir(scratch_dir));
  TEST_ASSERT_TRUE(snprintf(sub, sizeof(sub), "%s/plrfiles", scratch_dir) > 0);
  (void)mkdir(sub, 0700);
  player_table = NULL;
  top_of_p_table = -1;
}

static void leave_scratch(void)
{
  free_player_index();
  (void)remove(LIB_PLRFILES INDEX_FILE);
  (void)remove(LIB_PLRFILES);
  TEST_ASSERT_EQUAL_INT(0, chdir(start_dir));
}

static void write_index(const char *body)
{
  FILE *fl = fopen(LIB_PLRFILES INDEX_FILE, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fputs(body, fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

static char *read_index(void)
{
  static char buf[2048];
  size_t n;
  FILE *fl = fopen(LIB_PLRFILES INDEX_FILE, "r");
  TEST_ASSERT_NOT_NULL(fl);
  n = fread(buf, 1, sizeof(buf) - 1, fl);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  return buf;
}

/* B2-10: a malformed first record used to stop the load (top_of_p_table
 * -1, so the next new character became implementor) and every later
 * player vanished from the next index save. */
static void test_malformed_index_record_is_skipped_and_kept(void)
{
  enter_scratch();
  write_index("not a record\n4242 tester 10 0 1000\n4243 other 5 0 1000\n~\n");

  build_player_index();

  TEST_ASSERT_EQUAL_INT(1, top_of_p_table);
  TEST_ASSERT_EQUAL_STRING("tester", player_table[0].name);
  TEST_ASSERT_EQUAL_STRING("other", player_table[1].name);
  TEST_ASSERT_EQUAL_STRING("index-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "record 1 of 3"));
  TEST_ASSERT_NULL(strstr(tb_captured_detail, "not a record"));

  save_player_index();
  TEST_ASSERT_NOT_NULL(strstr(read_index(), "4243 other 5 0 1000\n"));
  TEST_ASSERT_NOT_NULL(strstr(read_index(), "not a record\n~\n"));
  leave_scratch();
}

/* B2-11: the index was truncated in place with fopen("w") and its write
 * errors ignored; a failed save left an empty index (next new character
 * became implementor). A save that cannot be staged must keep the live
 * index and report. */
static void test_index_save_failure_keeps_live_index(void)
{
  static const char body[] = "4242 tester 10 0 1000\n~\n";

  if (geteuid() == 0)
    TEST_IGNORE_MESSAGE("directory permissions do not bind root");
  enter_scratch();
  write_index(body);
  build_player_index();
  player_table[0].level = 11;	/* a change worth saving */
  TEST_ASSERT_EQUAL_INT(0, chmod(LIB_PLRFILES, 0500));

  save_player_index();

  TEST_ASSERT_EQUAL_INT(0, chmod(LIB_PLRFILES, 0700));
  TEST_ASSERT_EQUAL_STRING(body, read_index());
  TEST_ASSERT_EQUAL_STRING("index-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "live index kept"));

  save_player_index();
  TEST_ASSERT_EQUAL_STRING("4242 tester 11 0 1000\n~\n", read_index());
  leave_scratch();
}

/* B2-26: localtime() fails for a last-login value it cannot represent and
 * tb_strftime() then left the PCLEAN log's time string unwritten (an
 * uninitialised read under Valgrind). It is now reported and replaced. */
static void test_remove_player_with_unrepresentable_last_login(void)
{
  enter_scratch();
  write_index("4242 tester 10 0 100000000000000000\n4243 other 5 0 1000\n~\n");
  build_player_index();
  TEST_ASSERT_EQUAL_INT(1, top_of_p_table);

  remove_player(0);

  TEST_ASSERT_EQUAL_INT(0, top_of_p_table);
  TEST_ASSERT_EQUAL_STRING("other", player_table[0].name);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "pclean-time\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "player idnum 4242"));
  leave_scratch();
}

/* B2-28: remove_player() compacts the table, so clean_pfiles()'s i++
 * skipped the player that shifted into the removed slot: of two adjacent
 * deleted players only the first was removed. */
struct pclean_criteria_data pclean_criteria[] = { { -1, 0 } };

static void test_clean_pfiles_removes_adjacent_deleted_players(void)
{
  enter_scratch();
  write_index("4242 tester 10 1 1000\n4243 other 5 1 1000\n4244 keeper 5 0 1000\n~\n");
  build_player_index();
  TEST_ASSERT_EQUAL_INT(2, top_of_p_table);

  clean_pfiles();

  TEST_ASSERT_EQUAL_INT(0, top_of_p_table);
  TEST_ASSERT_EQUAL_STRING("keeper", player_table[0].name);
  leave_scratch();
}

/* B2-29: free_player_index() set top_of_p_table to 0 with player_table
 * NULL, so every later lookup read player_table[0] through NULL. */
static void test_lookups_after_free_player_index_find_nothing(void)
{
  char newbie[] = "Newbie";

  enter_scratch();
  write_index("4242 tester 10 0 1000\n~\n");
  build_player_index();
  TEST_ASSERT_EQUAL_INT(0, top_of_p_table);

  free_player_index();

  TEST_ASSERT_NULL(player_table);
  TEST_ASSERT_EQUAL_INT(-1, top_of_p_table);
  TEST_ASSERT_EQUAL_INT(-1, top_of_p_file);
  TEST_ASSERT_EQUAL_INT(-1, get_ptable_by_name("tester"));
  TEST_ASSERT_EQUAL_INT(-1, get_id_by_name("tester"));
  TEST_ASSERT_NULL(get_name_by_id(4242));
  TEST_ASSERT_EQUAL_INT(0, create_entry(newbie));
  TEST_ASSERT_EQUAL_STRING("newbie", player_table[0].name);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  leave_scratch();
}

/* B2-29: remove_player() indexed player_table[pfilepos] with no bounds
 * check (heap overflow under ASan for one past the end). */
static void test_remove_player_rejects_bad_position(void)
{
  enter_scratch();
  write_index("4242 tester 10 0 1000\n~\n");
  build_player_index();

  remove_player(1);
  TEST_ASSERT_EQUAL_STRING("index-pos", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "position 1 outside 0..0"));
  remove_player(-1);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "position -1 outside 0..0"));

  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(0, top_of_p_table);
  TEST_ASSERT_EQUAL_STRING("tester", player_table[0].name);
  leave_scratch();
}

/* Fuzzer PLAYER-3 (name-path): a damaged index can hold a name such as
 * "../evil", which get_filename() now refuses. remove_player() skipped
 * each refused file silently and still dropped the entry; it must delete
 * nothing, report the idnum and keep the entry, and clean_pfiles() (which
 * re-examines a slot after a removal) must move on rather than spin on
 * it. The deleted player after it is still removed. */
static void test_clean_pfiles_refuses_unsafe_name_and_moves_on(void)
{
  enter_scratch();
  write_index("4242 ../evil 10 1 1000\n4243 other 5 1 1000\n~\n");
  build_player_index();
  TEST_ASSERT_EQUAL_INT(1, top_of_p_table);

  clean_pfiles();

  TEST_ASSERT_EQUAL_INT(0, top_of_p_table);
  TEST_ASSERT_EQUAL_STRING("../evil", player_table[0].name);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "pclean-name\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242: no safe name for file type 0"));
  TEST_ASSERT_NULL(strstr(tb_captured_detail_log, "../evil"));
  leave_scratch();
}

/* Fuzzer PLAYER-3: load_char() returned -1 silently when get_filename()
 * refused the index name; the refusal is now reported with the idnum. */
static void test_load_char_refuses_unsafe_index_name(void)
{
  enter_scratch();
  write_index("4242 ../evil 10 0 1000\n~\n");
  build_player_index();

  TEST_ASSERT_EQUAL_INT(-1, load_char("../evil", &player));

  TEST_ASSERT_EQUAL_STRING("pfile-name", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242: no safe player file name"));
  leave_scratch();
}

/* B2-16: Clas/Levl/Sex index tables (class_abbrevs[], titles, exp) and
 * skills[] is unsigned char; out-of-range file values were stored as-is. */
static void test_out_of_range_class_level_sex_skill(void)
{
  write_pfile("tester", "Id  : 4242\nClas: 99\nLevl: 1000\nSex : -3\n"
                        "Skil:\n5 300\n6 40\n0 0\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(PFDEF_CLASS, GET_CLASS(&player));
  TEST_ASSERT_EQUAL_INT(PFDEF_LEVEL, GET_LEVEL(&player));
  TEST_ASSERT_EQUAL_INT(PFDEF_SEX, GET_SEX(&player));
  TEST_ASSERT_EQUAL_INT(0, GET_SKILL(&player, 5));
  TEST_ASSERT_EQUAL_INT(40, GET_SKILL(&player, 6));
  TEST_ASSERT_EQUAL_INT(4, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("pfile-range\npfile-range\npfile-range\npfile-range\n",
                           tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "tag Clas value 99"));
}

/* G2-12, H2-09: the class indexes class_abbrevs[] (who, users), the shift
 * 1 << GET_CLASS (who -c) and spell_info[].min_level[] (cast). Pin the
 * lower bound too: -1 is CLASS_UNDEFINED, and min_level[-1] reads the
 * field before the array. Every class is validated where it is loaded. */
static void test_negative_class_is_rejected_at_load(void)
{
  write_pfile("tester", "Id  : 4242\nClas: -1\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(PFDEF_CLASS, GET_CLASS(&player));
  TEST_ASSERT_EQUAL_STRING("pfile-range\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242 tag Clas value -1"));
}

/* Errloc finding: the endgame ranks EgHP/EgMP (0..100; do_endgame()
 * refuses a purchase at 100) were clamped below 0 silently and taken as-is
 * above 100, and EgAP/EgXP were clamped below 0 silently. Each now goes to
 * its nearest bound and is reported. */
static void test_endgame_values_are_clamped_and_reported(void)
{
  write_pfile("tester", "Id  : 4242\nEgHP: -3\nEgMP: 150\nEgAP: -7\nEgXP: 12\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(0, GET_ENDGAME_HP_RANK(&player));
  TEST_ASSERT_EQUAL_INT(100, GET_ENDGAME_MP_RANK(&player));
  TEST_ASSERT_EQUAL_INT(0, GET_ENDGAME_AP(&player));
  TEST_ASSERT_EQUAL_INT(12, GET_ENDGAME_XP(&player));
  TEST_ASSERT_EQUAL_STRING("pfile-range\npfile-range\npfile-range\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 tag EgHP value -3 outside 0..100; using 0"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 tag EgMP value 150 outside 0..100; using 100"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 tag EgAP value -7 outside 0.."));
}

/* The bounds themselves are valid ranks and load silently. */
static void test_endgame_ranks_at_their_bounds_load(void)
{
  write_pfile("tester", "Id  : 4242\nEgHP: 100\nEgMP: 0\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(100, GET_ENDGAME_HP_RANK(&player));
  TEST_ASSERT_EQUAL_INT(0, GET_ENDGAME_MP_RANK(&player));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* G2-02: Qcur was loaded unchecked. A quest deleted since the save made
 * real_quest() return NOTHING, and do_score() then read
 * aquest_table[NOTHING]. A stale quest must be reported and cleared. */
static void test_stale_current_quest_is_cleared(void)
{
  write_pfile("tester", "Id  : 4242\nQcnt: 3\nQcur: 9999\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(0, GET_QUEST_COUNTER(&player));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("pfile-quest", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "quest vnum 9999"));
}

static void test_loaded_current_quest_is_kept(void)
{
  write_pfile("tester", "Id  : 4242\nQcnt: 3\nQcur: 300\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(300, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_INT(3, GET_QUEST_COUNTER(&player));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* G2-02: current_quest is an int but real_quest() takes a 16-bit
 * qst_vnum, so Qcur 70300 was looked up as quest 4764 (a different,
 * possibly loaded, quest). It is out of range instead. */
static void test_out_of_range_current_quest_is_rejected(void)
{
  write_pfile("tester", "Id  : 4242\nQcur: 70300\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_STRING("pfile-range\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "tag Qcur value 70300"));
}

/* G2-02: Qcur -1 was kept as -1, which is not NOTHING (65535), so the
 * player counted as on a quest that real_quest() cannot find. */
static void test_negative_current_quest_is_rejected(void)
{
  write_pfile("tester", "Id  : 4242\nQcur: -1\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(NOTHING, GET_QUEST(&player));
  TEST_ASSERT_EQUAL_STRING("pfile-range\n", tb_captured_op_log);
}

/* B2-30: integer and HMVS parse failures went to plain log() with no
 * idnum (and echoed the raw value), and an unknown tag whose first letter
 * matched a case was dropped with no report at all. */
static void test_bad_int_and_unknown_tags_are_reported(void)
{
  write_pfile("tester", "Id  : 4242\nGold: 12abcSECRET\nHit : 5\n"
                        "Axyz: 1\nZzzz: 2\nTrig: 7\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(PFDEF_GOLD, GET_GOLD(&player));
  TEST_ASSERT_EQUAL_INT(PFDEF_HIT, GET_HIT(&player));
  TEST_ASSERT_EQUAL_STRING("pfile-int\npfile-int\npfile-tag\npfile-tag\n",
                           tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "player idnum 4242 tag Gold"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "player idnum 4242 tag Hit "));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "unknown tag Axyz"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "unknown tag Zzzz"));
  TEST_ASSERT_NULL(strstr(tb_captured_detail_log, "SECRET"));
}

/* PFILE_SET_STRING: a repeated string tag used to leak the earlier copy
 * (LeakSanitizer / Valgrind report the first "Titl"). */
static void test_repeated_string_tag_replaces_value(void)
{
  write_pfile("tester", "Id  : 4242\nTitl: the first\nTitl: the second\n"
                        "PfIn: in1\nPfIn: in2\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_STRING("the second", player.player.title);
  TEST_ASSERT_EQUAL_STRING("in2", player_specials.poofin);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* B2-17: the legacy one-word "Pref" form converted f1, which was never
 * written on that path (uninitialised, or a stale Act/Aff word). */
static void test_legacy_pref_uses_its_own_value(void)
{
  write_pfile("tester", "Id  : 4242\nAct : 9 9 9 9\nPref: 7\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  TEST_ASSERT_EQUAL_INT(7, PRF_FLAGS(&player)[0]);
  TEST_ASSERT_EQUAL_INT(PFDEF_PREFFLAGS, PRF_FLAGS(&player)[1]);
  TEST_ASSERT_EQUAL_STRING("pfile-flags-legacy", tb_captured_operation);
}

static void test_wrong_flag_word_count_is_reported(void)
{
  write_pfile("tester", "Id  : 4242\nAff : 1 2\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_EQUAL_INT(PFDEF_AFFFLAGS, AFF_FLAGS(&player)[0]);
  TEST_ASSERT_EQUAL_STRING("pfile-flags", tb_captured_operation);
}

/* B2-18: get_line() results were ignored, so a short alias list reused
 * the previous record's buffers (three copies of "foo") or read
 * uninitialised ones. */
static void test_truncated_alias_list_is_reported(void)
{
  int n = 0;
  struct alias_data *a;

  write_pfile("tester", "Id  : 4242\nAlis: 3\n foo\n bar\n0\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));

  for (a = player_specials.aliases; a; a = a->next)
    n++;
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_STRING("foo", player_specials.aliases->alias);
  TEST_ASSERT_EQUAL_STRING("pfile-alias", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "alias 2 of 3"));
}

static void test_huge_alias_count_is_rejected(void)
{
  write_pfile("tester", "Id  : 4242\nAlis: 2000000000\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_NULL(player_specials.aliases);
  TEST_ASSERT_EQUAL_STRING("pfile-alias", tb_captured_operation);
}

/* Same contract as interpreter.c free_alias() (not linked here). */
void free_alias(struct alias_data *a)
{
  free(a->alias);
  free(a->replacement);
  free(a);
}

/* P2 fuzz_player leak: a second "Alis: 0" set the list to NULL without
 * freeing the aliases the first block loaded (LeakSanitizer). Policy: the
 * later block replaces the earlier one, and the duplicate is reported. */
static void test_duplicate_alias_tag_count0_frees_earlier_list(void)
{
  write_pfile("tester", "Id  : 4242\nName: tester\nAlis: 2\n foo\n bar\n0\n"
                        " baz\n qux\n0\nAlis: 0\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_NULL(player_specials.aliases);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("pfile-alias-dup", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242 name tester"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "2 alias(es) dropped"));
}

/* A second non-empty "Alis:" block replaces the first instead of silently
 * merging into it. */
static void test_duplicate_alias_tag_later_block_wins(void)
{
  write_pfile("tester", "Id  : 4242\nAlis: 1\n foo\n bar\n0\n"
                        "Alis: 1\n baz\n qux\n1\n");

  TEST_ASSERT_EQUAL_INT(0, load_char("tester", &player));
  TEST_ASSERT_NOT_NULL(player_specials.aliases);
  TEST_ASSERT_NULL(player_specials.aliases->next);
  TEST_ASSERT_EQUAL_STRING("baz", player_specials.aliases->alias);
  TEST_ASSERT_EQUAL_INT(1, player_specials.aliases->type);
  TEST_ASSERT_EQUAL_STRING("pfile-alias-dup", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "name (no name yet)"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "1 alias(es) dropped"));
}

/* --- save_char() fixtures: a heap affect list handled like handler.c. --- */
void affect_remove(struct char_data *ch, struct affected_type *af)
{
  struct affected_type **p;
  for (p = &ch->affected; *p; p = &(*p)->next)
    if (*p == af) {
      *p = af->next;
      free(af);
      return;
    }
}
static void add_affect(struct char_data *ch, int spell)
{
  struct affected_type *af = calloc(1, sizeof(*af));
  TEST_ASSERT_NOT_NULL(af);
  af->spell = spell;
  af->duration = 5;
  af->next = ch->affected;
  ch->affected = af;
}
static void free_affects(struct char_data *ch)
{
  while (ch->affected)
    affect_remove(ch, ch->affected);
}
/* Like handler.c, minus the affects: an item moves between slot and hand. */
static int equips;
struct obj_data *unequip_char(struct char_data *ch, int pos)
{
  struct obj_data *obj = GET_EQ(ch, pos);
  GET_EQ(ch, pos) = NULL;
  return obj;
}
void equip_char_at(const char *src_file, int src_line, const char *src_func,
                   struct char_data *ch, struct obj_data *obj, int pos)
{
  (void)src_file; (void)src_line; (void)src_func;
  TEST_ASSERT_NULL(GET_EQ(ch, pos));
  GET_EQ(ch, pos) = obj;
  equips++;
}
/* B2-21: save_char() must run no script. These fail any test that does. */
int remove_otrigger(obj_data *obj, char_data *actor)
{ (void)obj; (void)actor; TEST_FAIL_MESSAGE("save_char fired a remove trigger"); return 1; }
int wear_otrigger(obj_data *obj, char_data *actor, int where)
{ (void)obj; (void)actor; (void)where; TEST_FAIL_MESSAGE("save_char fired a wear trigger"); return 1; }
void strip_cr(char *buf) { (void)buf; }
void save_char_vars_ascii(FILE *file, struct char_data *ch) { (void)file; (void)ch; }

/* B2-19: affects past MAX_AFFECT were removed from the live character and
 * never re-applied (silently, for exactly one extra). */
static void test_save_keeps_affects_past_max(void)
{
  int i, n = 0;
  struct affected_type *a;

  player.player.name = strdup("Tester");
  GET_IDNUM(&player) = 4242;
  GET_PFILEPOS(&player) = 0;
  for (i = 1; i <= MAX_AFFECT + 1; i++)
    add_affect(&player, i);

  save_char(&player);

  for (a = player.affected; a; a = a->next)
    n++;
  TEST_ASSERT_EQUAL_INT(MAX_AFFECT + 1, n);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "save-affects\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "has 33 affects"));
  TEST_ASSERT_NULL(strstr(tb_captured_op_log, "index-save"));
  free_affects(&player);
  {
    char path[512];
    if (get_filename(path, sizeof(path), PLR_FILE, "Tester"))
      (void)remove(path);
  }
}

/* B2-20: nanny's deleted-character path kept a stale GET_PFILEPOS, so the
 * re-created character was never indexed and save_char() returned silently
 * after writing a pfile nobody could log into. It must now report. */
static void test_save_of_unindexed_player_is_reported(void)
{
  char path[512];

  player.player.name = strdup("Nobody");
  GET_IDNUM(&player) = 777;
  GET_PFILEPOS(&player) = 0;

  save_char(&player);

  TEST_ASSERT_EQUAL_STRING("save-index-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 777"));
  if (get_filename(path, sizeof(path), PLR_FILE, "Nobody"))
    (void)remove(path);
}

/* B2-21: save_char() takes every worn item off and puts it back in its own
 * slot with no trigger in between (a remove or wear trigger it used to
 * fire could purge an item it was still holding). */
static void test_save_restores_worn_items_without_triggers(void)
{
  struct obj_data ring, cloak;
  char path[512];

  memset(&ring, 0, sizeof(ring));
  memset(&cloak, 0, sizeof(cloak));
  player.player.name = strdup("Tester");
  GET_IDNUM(&player) = 4242;
  GET_PFILEPOS(&player) = 0;
  GET_EQ(&player, WEAR_FINGER_R) = &ring;
  GET_EQ(&player, WEAR_ABOUT) = &cloak;
  equips = 0;

  save_char(&player);

  TEST_ASSERT_EQUAL_PTR(&ring, GET_EQ(&player, WEAR_FINGER_R));
  TEST_ASSERT_EQUAL_PTR(&cloak, GET_EQ(&player, WEAR_ABOUT));
  TEST_ASSERT_EQUAL_INT(2, equips);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  if (get_filename(path, sizeof(path), PLR_FILE, "Tester"))
    (void)remove(path);
}

#ifdef TB_SAVE_FAULT_INJECTION
/* save_char()'s commit as players.c sees it (this executable is linked with
 * --wrap=rename and --wrap=fclose): fail on demand, to prove each failure is
 * reported where it happens and never replaces the existing pfile. */
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __real_rename(const char *from, const char *to);
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __real_fclose(FILE *fl);
static int fail_rename, fail_fclose, fail_fflush, fail_unlink, fail_ferror;
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __real_fflush(FILE *fl);
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __real_unlink(const char *path);
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __real_ferror(FILE *fl);

/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_fflush(FILE *fl);
int __wrap_fflush(FILE *fl)
{
  if (fail_fflush && fl != NULL) {
    errno = ENOSPC;
    return EOF;
  }
  return __real_fflush(fl);
}

/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_unlink(const char *path);
int __wrap_unlink(const char *path)
{
  if (fail_unlink) {
    errno = EACCES;
    return -1;
  }
  return __real_unlink(path);
}

/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_ferror(FILE *fl);
int __wrap_ferror(FILE *fl)
{
  return fail_ferror ? 1 : __real_ferror(fl);
}

/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
  if (fail_rename) {
    errno = EXDEV;
    return -1;
  }
  return __real_rename(from, to);
}

/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_fclose(FILE *fl);
int __wrap_fclose(FILE *fl)
{
  int rc = __real_fclose(fl);   /* the stream is closed either way */

  if (fail_fclose && rc == 0) {
    errno = EIO;
    return EOF;
  }
  return rc;
}

static void read_whole_file(const char *path, char *buf, size_t size)
{
  FILE *fl = fopen(path, "r");
  size_t n;

  TEST_ASSERT_NOT_NULL(fl);
  n = fread(buf, 1, size - 1, fl);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

/* Staged "<pfile>.tmpXXXXXX" files left beside the pfile at path. */
static int staged_files_left(const char *path)
{
  char dir[512], prefix[256];
  const char *slash = strrchr(path, '/');
  struct dirent *entry;
  DIR *dp;
  int left = 0;

  TEST_ASSERT_NOT_NULL(slash);
  TEST_ASSERT_TRUE(snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path) < (int)sizeof(dir));
  TEST_ASSERT_TRUE(snprintf(prefix, sizeof(prefix), "%s.tmp", slash + 1) < (int)sizeof(prefix));
  dp = opendir(dir);
  TEST_ASSERT_NOT_NULL(dp);
  while ((entry = readdir(dp)) != NULL)
    if (!strncmp(entry->d_name, prefix, strlen(prefix)))
      left++;
  TEST_ASSERT_EQUAL_INT(0, closedir(dp));
  return left;
}

/* Remove the staged files a test deliberately left behind. */
static void remove_staged_files(const char *path)
{
  char dir[512], prefix[256], victim[1024];
  const char *slash = strrchr(path, '/');
  struct dirent *entry;
  DIR *dp;

  TEST_ASSERT_NOT_NULL(slash);
  TEST_ASSERT_TRUE(snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path) < (int)sizeof(dir));
  TEST_ASSERT_TRUE(snprintf(prefix, sizeof(prefix), "%s.tmp", slash + 1) < (int)sizeof(prefix));
  dp = opendir(dir);
  TEST_ASSERT_NOT_NULL(dp);
  while ((entry = readdir(dp)) != NULL)
    if (!strncmp(entry->d_name, prefix, strlen(prefix))) {
      TEST_ASSERT_TRUE(snprintf(victim, sizeof(victim), "%s/%s", dir, entry->d_name)
                       < (int)sizeof(victim));
      TEST_ASSERT_EQUAL_INT(0, remove(victim));
    }
  TEST_ASSERT_EQUAL_INT(0, closedir(dp));
}

#define ORIGINAL_PFILE "Id  : 4242\nName: tester\nLevl: 7\n"

static void save_with_existing_pfile(char *path, size_t size)
{
  write_pfile("tester", ORIGINAL_PFILE);
  TEST_ASSERT_TRUE(get_filename(path, size, PLR_FILE, "tester"));
  player.player.name = strdup("tester");
  GET_IDNUM(&player) = 4242;
  GET_PFILEPOS(&player) = 0;
}

/* A refused rename() keeps the previous pfile and removes the staged copy;
 * the report names rename and the player.  Red before: one generic "save"
 * record after the fact. */
static void test_save_rename_failure_keeps_pfile_and_reports_rename(void)
{
  char path[512], body[256];

  save_with_existing_pfile(path, sizeof(path));
  fail_rename = 1;
  save_char(&player);
  fail_rename = 0;

  read_whole_file(path, body, sizeof(body));
  TEST_ASSERT_EQUAL_STRING(ORIGINAL_PFILE, body);
  TEST_ASSERT_EQUAL_INT(0, staged_files_left(path));
  TEST_ASSERT_EQUAL_STRING("rename", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "previous file kept"));
}

/* A failing fclose() of the staged file is reported as fclose, the commit is
 * never attempted, and the staged copy is removed.  Red before: reported as
 * a generic "save" failure. */
static void test_save_close_failure_reports_fclose_and_skips_commit(void)
{
  char path[512], body[256];

  save_with_existing_pfile(path, sizeof(path));
  fail_fclose = 1;
  save_char(&player);
  fail_fclose = 0;

  read_whole_file(path, body, sizeof(body));
  TEST_ASSERT_EQUAL_STRING(ORIGINAL_PFILE, body);
  TEST_ASSERT_EQUAL_INT(0, staged_files_left(path));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "fclose\n"));
  TEST_ASSERT_NULL(strstr(tb_captured_op_log, "rename\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "player idnum 4242"));
}

/* A failing fflush() of the staged file is reported as fflush (with errno),
 * the commit is never attempted and the previous pfile is kept.  Red before:
 * folded into one generic "save" record with ferror(). */
static void test_save_flush_failure_reports_fflush(void)
{
  char path[512], body[256];

  save_with_existing_pfile(path, sizeof(path));
  fail_fflush = 1;
  save_char(&player);
  fail_fflush = 0;

  read_whole_file(path, body, sizeof(body));
  TEST_ASSERT_EQUAL_STRING(ORIGINAL_PFILE, body);
  TEST_ASSERT_EQUAL_INT(0, staged_files_left(path));
  TEST_ASSERT_TRUE(tb_captured_has_op("fflush"));
  TEST_ASSERT_FALSE(tb_captured_has_op("rename"));
  TEST_ASSERT_FALSE(tb_captured_has_op("save"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "player idnum 4242"));
}

/* A stream error latched by an earlier write is reported as write. */
static void test_save_stream_error_reports_write(void)
{
  char path[512], body[256];

  save_with_existing_pfile(path, sizeof(path));
  fail_ferror = 1;
  save_char(&player);
  fail_ferror = 0;

  read_whole_file(path, body, sizeof(body));
  TEST_ASSERT_EQUAL_STRING(ORIGINAL_PFILE, body);
  TEST_ASSERT_EQUAL_INT(0, staged_files_left(path));
  TEST_ASSERT_EQUAL_STRING("write", tb_captured_ops[0]);
  TEST_ASSERT_FALSE(tb_captured_has_op("rename"));
}

/* rename() and then the cleanup unlink() both fail: each is reported at its
 * own call, the previous pfile is kept, and the staged copy survives as
 * evidence.  Red before: one "save" record, the unlink result discarded. */
static void test_save_cleanup_failure_after_rename_failure_reports_unlink(void)
{
  char path[512], body[256];

  save_with_existing_pfile(path, sizeof(path));
  fail_rename = fail_unlink = 1;
  save_char(&player);
  fail_rename = fail_unlink = 0;

  read_whole_file(path, body, sizeof(body));
  TEST_ASSERT_EQUAL_STRING(ORIGINAL_PFILE, body);
  TEST_ASSERT_EQUAL_INT(1, staged_files_left(path));
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("rename", tb_captured_ops[0]);
  TEST_ASSERT_EQUAL_STRING("unlink", tb_captured_ops[1]);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  remove_staged_files(path);
}
#endif /* TB_SAVE_FAULT_INJECTION */

int main(void)
{
  UNITY_BEGIN();
#ifdef TB_SAVE_FAULT_INJECTION
  RUN_TEST(test_save_rename_failure_keeps_pfile_and_reports_rename);
  RUN_TEST(test_save_close_failure_reports_fclose_and_skips_commit);
  RUN_TEST(test_save_flush_failure_reports_fflush);
  RUN_TEST(test_save_stream_error_reports_write);
  RUN_TEST(test_save_cleanup_failure_after_rename_failure_reports_unlink);
#endif
  RUN_TEST(test_overlong_act_flag_word_is_rejected);
  RUN_TEST(test_four_flag_words_load);
  RUN_TEST(test_short_tag_line_is_reported);
  RUN_TEST(test_tag_and_value_are_split);
  RUN_TEST(test_short_pfile_line_is_skipped);
  RUN_TEST(test_unterminated_desc_is_dropped_not_fatal);
  RUN_TEST(test_oversize_desc_is_skipped);
  RUN_TEST(test_terminated_desc_loads);
  RUN_TEST(test_desc_line_starting_with_nul_is_dropped);
  RUN_TEST(test_malformed_index_record_is_skipped_and_kept);
  RUN_TEST(test_index_save_failure_keeps_live_index);
  RUN_TEST(test_remove_player_with_unrepresentable_last_login);
  RUN_TEST(test_clean_pfiles_removes_adjacent_deleted_players);
  RUN_TEST(test_out_of_range_class_level_sex_skill);
  RUN_TEST(test_legacy_pref_uses_its_own_value);
  RUN_TEST(test_bad_int_and_unknown_tags_are_reported);
  RUN_TEST(test_repeated_string_tag_replaces_value);
  RUN_TEST(test_wrong_flag_word_count_is_reported);
  RUN_TEST(test_truncated_alias_list_is_reported);
  RUN_TEST(test_huge_alias_count_is_rejected);
  RUN_TEST(test_duplicate_alias_tag_count0_frees_earlier_list);
  RUN_TEST(test_duplicate_alias_tag_later_block_wins);
  RUN_TEST(test_save_keeps_affects_past_max);
  RUN_TEST(test_save_of_unindexed_player_is_reported);
  RUN_TEST(test_stale_current_quest_is_cleared);
  RUN_TEST(test_loaded_current_quest_is_kept);
  RUN_TEST(test_out_of_range_current_quest_is_rejected);
  RUN_TEST(test_negative_current_quest_is_rejected);
  RUN_TEST(test_negative_class_is_rejected_at_load);
  RUN_TEST(test_endgame_values_are_clamped_and_reported);
  RUN_TEST(test_endgame_ranks_at_their_bounds_load);
  RUN_TEST(test_lookups_after_free_player_index_find_nothing);
  RUN_TEST(test_remove_player_rejects_bad_position);
  RUN_TEST(test_clean_pfiles_refuses_unsafe_name_and_moves_on);
  RUN_TEST(test_load_char_refuses_unsafe_index_name);
  RUN_TEST(test_save_restores_worn_items_without_triggers);
  (void)rmdir(scratch_dir);
  return UNITY_END();
}
