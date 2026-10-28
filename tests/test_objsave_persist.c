/* Executable tests of the production rent/crash-save code (objsave.c).
 *
 * Rent files are player-controlled persistent data: their Loc, header and
 * object records come from disk, so every guard here is driven through the
 * real Crash_load()/Crash_listrent()/objsave_parse_objects() entry points
 * against a scratch directory. The test supplies a small, faithful object
 * world (two prototypes) and list-manipulation stand-ins for handler.c.
 * Each test asserts on the structured diagnostic, not merely on survival.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "class.h"
#include "genolc.h"
#include "interpreter.h"
#include "spells.h"
#include "oasis.h"
#include <stdarg.h>
#include <sys/stat.h>
#include "tb_error_capture.h"
#include "tb_persist_support.h"

/* ------------------------------------------------------------------ */
/* Fixture world: vnum 100 is a container that can only be TAKEn,      */
/* vnum 101 is a plain trinket.                                         */
/* ------------------------------------------------------------------ */
struct obj_data *obj_proto;
struct obj_data *object_list;
static struct obj_data protos[2];
static struct index_data indexes[2];
static char scratch_dir[256];
static char paged[MAX_STRING_LENGTH * 2];
static int pages;
static int live_objects;

/* Under ASan, report a read of a dead stack frame (see pager_page_on());
 * ASAN_OPTIONS from the environment still apply on top. Unused without
 * ASan. */
const char *__asan_default_options(void); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
const char *__asan_default_options(void) /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
{
  return "detect_stack_use_after_return=1";
}

/* Diagnostics the production code may log while running. */
void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }

/* Like modify.c's page_string(): with keep_internal the pager owns a copy
 * of str; without it, it keeps str itself and shows the later pages from
 * it after the caller has returned. */
static const char *pager_text;
static char *pager_copy;
static int pager_kept;
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d;
  free(pager_copy);
  pager_copy = NULL;
  pager_kept = keep_internal;
  if (keep_internal) {
    pager_copy = strdup(str);
    TEST_ASSERT_NOT_NULL(pager_copy);
    pager_text = pager_copy;
  } else
    pager_text = str;
  if (snprintf(paged, sizeof(paged), "%s", str) < 0)
    paged[0] = '\0';
  pages++;
}

/* The player pages on: show_string() reads the pager's text again, after
 * the listing function has returned. */
static void pager_page_on(char *out, size_t size)
{
  if (snprintf(out, size, "%s", pager_text ? pager_text : "") < 0)
    out[0] = '\0';
}

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
  return snprintf(filename, fbufsize, "%s/%s.objs", scratch_dir, orig_name) > 0;
}

/* players.c: the fixture's player, also under the unsafe name a damaged
 * index could hold. */
long get_id_by_name(const char *name)
{
  return (!strcmp(name, "Tester") || !strcmp(name, "../evil")) ? 4242 : -1;
}

/* What the code under test told the player (or immortal). */
static char told[MAX_STRING_LENGTH];
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(told);

  (void)ch;
  va_start(args, messg);
  if (vsnprintf(told + used, sizeof(told) - used, messg, args) < 0)
    told[used] = '\0';
  va_end(args);
  return strlen(told);
}

/* Same contract as utils.c get_line(): skip blank and '*' lines, strip EOL,
 * copy at most READ_SIZE bytes. */
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

/* Contract of players.c tag_argument() (tested in test_players_persist):
 * split "Tag : value" into a 4-character tag and the value. */
void tag_argument(char *argument, char *tag)
{
  size_t i, len = strlen(argument);
  char *tmp = argument;

  if (len < 4) {
    tag[0] = '\0';
    argument[0] = '\0';
    return;
  }
  for (i = 0; i < 4; i++)
    tag[i] = argument[i];
  tag[4] = '\0';
  tmp += 4;
  while (*tmp == ':' || *tmp == ' ')
    tmp++;
  memmove(argument, tmp, strlen(tmp) + 1);
}

bitvector_t asciiflag_conv(char *flag)
{
  return (bitvector_t)strtoul(flag, NULL, 10);
}

char *fread_string(FILE *fl, const char *error)
{
  char line[READ_SIZE];
  (void)error;
  if (!fgets(line, sizeof(line), fl))
    return NULL;
  return NULL;
}

void strip_cr(char *buf) { (void)buf; }

/* Contract of players.c fread_string_checked() (tested in
 * test_players_persist): read up to a line ending in '~'; FALSE at EOF. */
int fread_string_checked(FILE *fl, const char *resource, const char *field,
                         char **out)
{
  char line[READ_SIZE], buf[MAX_STRING_LENGTH] = "";
  (void)resource; (void)field;
  *out = NULL;
  while (fgets(line, sizeof(line), fl)) {
    char *tilde = strchr(line, '~');
    if (tilde) {
      *tilde = '\0';
      strlcat(buf, line, sizeof(buf));
      *out = *buf ? strdup(buf) : NULL;
      return 1;
    }
    strlcat(buf, line, sizeof(buf));
  }
  return 0;
}

obj_rnum real_object(obj_vnum vnum)
{
  if (vnum == 100)
    return 0;
  if (vnum == 101)
    return 1;
  return NOTHING;
}

struct obj_data *create_obj(void)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));
  TEST_ASSERT_NOT_NULL(obj);
  obj->item_number = NOTHING;
  live_objects++;
  return obj;
}

/* read_object() of this vnum fails, to make objsave_save_obj_record() fail. */
static obj_vnum fail_read_vnum = NOTHING;

struct obj_data *read_object(obj_vnum nr, int type)
{
  obj_rnum r = (type == VIRTUAL) ? real_object(nr) : (obj_rnum)nr;
  struct obj_data *obj;

  if (r == NOTHING || nr == fail_read_vnum)
    return NULL;
  obj = create_obj();
  *obj = protos[r];
  obj->item_number = r;
  return obj;
}

static void free_obj_strings(struct obj_data *obj)
{
  obj_rnum r = obj->item_number;
  struct obj_data *p = (r != NOTHING && r <= 1) ? &protos[r] : NULL;

  if (obj->name && (!p || obj->name != p->name))
    free(obj->name);
  if (obj->short_description && (!p || obj->short_description != p->short_description))
    free(obj->short_description);
  if (obj->description && (!p || obj->description != p->description))
    free(obj->description);
  if (obj->action_description && (!p || obj->action_description != p->action_description))
    free(obj->action_description);
}

void obj_from_char(struct obj_data *object);

/* Like handler.c: take the object off whoever wears or carries it first. */
void extract_obj(struct obj_data *obj)
{
  if (obj->worn_by)
    GET_EQ(obj->worn_by, obj->worn_on) = NULL;
  else if (obj->carried_by)
    obj_from_char(obj);
  free_obj_strings(obj);
  free(obj);
  live_objects--;
}

static void free_tree(struct obj_data *obj)
{
  struct obj_data *next;
  for (; obj; obj = next) {
    next = obj->next_content;
    free_tree(obj->contains);
    extract_obj(obj);
  }
}

void obj_to_char(struct obj_data *object, struct char_data *ch)
{
  object->next_content = ch->carrying;
  ch->carrying = object;
  object->carried_by = ch;
}

void obj_from_char(struct obj_data *object)
{
  struct obj_data **p;
  for (p = &object->carried_by->carrying; *p; p = &(*p)->next_content)
    if (*p == object) {
      *p = object->next_content;
      break;
    }
  object->carried_by = NULL;
  object->next_content = NULL;
}

void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to)
{
  obj->next_content = obj_to->contains;
  obj_to->contains = obj;
  obj->in_obj = obj_to;
}

void equip_char_at(const char *src_file, int src_line, const char *src_func, struct char_data *ch, struct obj_data *obj, int pos)
{ (void)src_file; (void)src_line; (void)src_func; 
  TEST_ASSERT_NULL(GET_EQ(ch, pos));
  GET_EQ(ch, pos) = obj;
  obj->worn_by = ch;
  obj->worn_on = pos;
}

/* handler.c returns NULL (after core_dump()) for an empty or bad slot. */
struct obj_data *unequip_char(struct char_data *ch, int pos)
{
  struct obj_data *obj;
  if (pos < 0 || pos >= NUM_WEARS || GET_EQ(ch, pos) == NULL)
    return NULL;
  obj = GET_EQ(ch, pos);
  GET_EQ(ch, pos) = NULL;
  obj->worn_by = NULL;
  obj->worn_on = -1;
  return obj;
}

int invalid_align(struct char_data *ch, struct obj_data *obj) { (void)ch; (void)obj; return 0; }
int invalid_class(struct char_data *ch, struct obj_data *obj) { (void)ch; (void)obj; return 0; }

/* ------------------------------------------------------------------ */
static struct char_data player;
static struct player_special_data player_specials;

static void write_rent_file(const char *name, const char *body)
{
  char path[512];
  FILE *fl;

  TEST_ASSERT_TRUE(get_filename(path, sizeof(path), CRASH_FILE, name));
  fl = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fputs(body, fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

static void remove_rent_file(const char *name)
{
  char path[512];
  if (get_filename(path, sizeof(path), CRASH_FILE, name))
    (void)remove(path);
}

void setUp(void)
{
  memset(&player, 0, sizeof(player));
  memset(&player_specials, 0, sizeof(player_specials));
  player.player_specials = &player_specials;
  player.player.name = (char *)"Tester";
  GET_IDNUM(&player) = 4242;
  GET_CLASS(&player) = CLASS_MAGIC_USER;

  memset(protos, 0, sizeof(protos));
  memset(indexes, 0, sizeof(indexes));
  protos[0].item_number = 0;
  protos[0].name = (char *)"bag";
  protos[0].short_description = (char *)"a bag";
  GET_OBJ_TYPE(&protos[0]) = ITEM_CONTAINER;
  SET_BIT_AR(GET_OBJ_WEAR(&protos[0]), ITEM_WEAR_TAKE);
  protos[1].item_number = 1;
  protos[1].name = (char *)"trinket";
  protos[1].short_description = (char *)"a trinket";
  GET_OBJ_TYPE(&protos[1]) = ITEM_TRASH;
  SET_BIT_AR(GET_OBJ_WEAR(&protos[1]), ITEM_WEAR_TAKE);
  indexes[0].vnum = 100;
  indexes[1].vnum = 101;
  obj_proto = protos;
  obj_index = indexes;
  top_of_objt = 1;

  if (scratch_dir[0] == '\0') {
    const char *base = getenv("TMPDIR");
    TEST_ASSERT_TRUE(snprintf(scratch_dir, sizeof(scratch_dir), "%s/tb_objsave_XXXXXX",
                              base && *base ? base : "/tmp") > 0);
    TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
  }
  paged[0] = '\0';
  told[0] = '\0';
  pages = 0;
  live_objects = 0;
  fail_read_vnum = NOTHING;
  tb_error_capture_reset();
}

void tearDown(void)
{
  int i;
  free_tree(player.carrying);
  player.carrying = NULL;
  for (i = 0; i < NUM_WEARS; i++)
    if (GET_EQ(&player, i)) {
      free_tree(GET_EQ(&player, i)->contains);
      extract_obj(GET_EQ(&player, i));
      GET_EQ(&player, i) = NULL;
    }
  remove_rent_file("Tester");
  obj_index = NULL;
  free(pager_copy);
  pager_copy = NULL;
  pager_text = NULL;
}

/* B2-01: a container recorded on a finger slot it cannot be worn on is put in
 * inventory by auto_equip(); its saved contents must go into it there, not
 * into the NULL that unequip_char() returns for the empty slot. */
static void test_container_refused_by_autoeq_keeps_contents(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nLoc : -1\n\n#100\nLoc : %d\n\n$~\n",
                            RENT_CRASH, (long)time(0), WEAR_FINGER_R + 1) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_NULL(GET_EQ(&player, WEAR_FINGER_R));
  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(100, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_NULL(player.carrying->next_content);
  TEST_ASSERT_NOT_NULL(player.carrying->contains);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying->contains));
  TEST_ASSERT_EQUAL_STRING("autoeq-container", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
}

/* B2-01: a worn slot past NUM_WEARS is rejected at parse time. */
static void test_loc_past_num_wears_is_reported(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nLoc : %d\n\n$~\n",
                            RENT_CRASH, (long)time(0), NUM_WEARS + 40) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));
  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_EQUAL_STRING("rent-loc-range", tb_captured_operation);
}

/* B2-04: a listing longer than the page buffer used to push snprintf()'s
 * running length past the buffer and then write out of bounds. */
static void test_listrent_truncates_loudly(void)
{
  char shrt[241];
  FILE *fl;
  char path[512];
  int i;
  struct descriptor_data desc;

  memset(&desc, 0, sizeof(desc));
  player.desc = &desc;
  memset(shrt, 'x', sizeof(shrt) - 1);
  shrt[sizeof(shrt) - 1] = '\0';
  TEST_ASSERT_TRUE(get_filename(path, sizeof(path), CRASH_FILE, "Tester"));
  fl = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fprintf(fl, "%d %ld 0 0 0 0\n", RENT_CRASH, (long)time(0)) > 0);
  for (i = 0; i < 400; i++)
    TEST_ASSERT_TRUE(fprintf(fl, "#101\nShrt: %s\n\n", shrt) > 0);
  TEST_ASSERT_TRUE(fputs("$~\n", fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));

  Crash_listrent(&player, (char *)"Tester");
  player.desc = NULL;

  TEST_ASSERT_EQUAL_INT(1, pages);
  TEST_ASSERT_EQUAL_STRING("listrent-truncate", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "of 400"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "listing truncated"));
  TEST_ASSERT_EQUAL_INT(0, live_objects);
}

/* Crash_listrent() built its listing in a stack buffer and handed it to
 * page_string() without keep_internal, so the pager kept pointing into
 * that buffer. A listing longer than one page is shown after the call
 * returns, from a dead frame: stack use after return under ASan
 * (detect_stack_use_after_return). The pager must own a copy. */
static void test_listrent_pager_owns_its_copy(void)
{
  char again[256];
  struct descriptor_data desc;

  memset(&desc, 0, sizeof(desc));
  player.desc = &desc;
  write_rent_file("Tester", "1 1000 0 0 0 0\n#101\n\n$~\n");

  Crash_listrent(&player, (char *)"Tester");
  player.desc = NULL;
  pager_page_on(again, sizeof(again));

  TEST_ASSERT_TRUE(pager_kept);
  TEST_ASSERT_NOT_NULL(strstr(again, "a trinket"));
}

static char *slurp_rent(void)
{
  static char buf[4096];
  char path[512];
  size_t n;
  FILE *fl;

  TEST_ASSERT_TRUE(get_filename(path, sizeof(path), CRASH_FILE, "Tester"));
  fl = fopen(path, "r");
  TEST_ASSERT_NOT_NULL(fl);
  n = fread(buf, 1, sizeof(buf) - 1, fl);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  return buf;
}

/* B2-12: the savers truncated the live rent file in place and returned
 * silently when it could not be opened; the receptionist then logged a
 * successful rent and extracted the player. A save that cannot be staged
 * must report, keep the live file and tell the caller. */
static void test_crashsave_failure_keeps_live_file(void)
{
  static const char old_file[] = "1 1000 0 0 0 0\n#101\n\n$~\n";

  if (geteuid() == 0)
    TEST_IGNORE_MESSAGE("directory permissions do not bind root");
  write_rent_file("Tester", old_file);
  obj_to_char(read_object(100, VIRTUAL), &player);
  SET_BIT_AR(PLR_FLAGS(&player), PLR_CRASH);
  TEST_ASSERT_EQUAL_INT(0, chmod(scratch_dir, 0500));

  TEST_ASSERT_FALSE(Crash_crashsave(&player));

  TEST_ASSERT_EQUAL_INT(0, chmod(scratch_dir, 0700));
  TEST_ASSERT_EQUAL_STRING(old_file, slurp_rent());
  TEST_ASSERT_TRUE(PLR_FLAGGED(&player, PLR_CRASH));
  TEST_ASSERT_EQUAL_STRING("rent-open", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
}

/* B2-12: a committed crash save replaces the file and clears PLR_CRASH. */
static void test_crashsave_commits(void)
{
  write_rent_file("Tester", "1 1000 0 0 0 0\n$~\n");
  obj_to_char(read_object(101, VIRTUAL), &player);
  SET_BIT_AR(PLR_FLAGS(&player), PLR_CRASH);

  TEST_ASSERT_TRUE(Crash_crashsave(&player));

  TEST_ASSERT_FALSE(PLR_FLAGGED(&player, PLR_CRASH));
  TEST_ASSERT_NOT_NULL(strstr(slurp_rent(), "#101\n"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* B2-13: when a later record failed, earlier equipment slots had already
 * been saved and extracted, so they existed neither in the game nor in a
 * committed file. Nothing may be extracted unless the whole file commits. */
static void test_rentsave_failure_extracts_nothing(void)
{
  struct obj_data *worn = read_object(100, VIRTUAL);
  struct obj_data *carried = read_object(101, VIRTUAL);
  int before;

  equip_char(&player, worn, WEAR_LIGHT);
  obj_to_char(carried, &player);
  before = live_objects;
  fail_read_vnum = 101;	/* the inventory record cannot be written */

  TEST_ASSERT_FALSE(Crash_rentsave(&player, 10));

  TEST_ASSERT_EQUAL_PTR(worn, GET_EQ(&player, WEAR_LIGHT));
  TEST_ASSERT_EQUAL_PTR(carried, player.carrying);
  TEST_ASSERT_EQUAL_INT(before, live_objects);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "rent-write\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "obj vnum 101"));
}

static void test_rentsave_success_extracts_everything(void)
{
  equip_char(&player, read_object(100, VIRTUAL), WEAR_LIGHT);
  obj_to_char(read_object(101, VIRTUAL), &player);

  TEST_ASSERT_TRUE(Crash_rentsave(&player, 10));

  TEST_ASSERT_NULL(GET_EQ(&player, WEAR_LIGHT));
  TEST_ASSERT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(0, live_objects);
  TEST_ASSERT_NOT_NULL(strstr(slurp_rent(), "#100\nLoc : 1\n"));
}

/* B2-14: a failed record inside a container (below the head object) was
 * ignored, so the save "succeeded" without the contents and the live file
 * was replaced by one that had lost them. Weights must also come back. */
static void test_failed_content_record_fails_the_save(void)
{
  static const char old_file[] = "1 1000 0 0 0 0\n$~\n";
  struct obj_data *bag = read_object(100, VIRTUAL);
  struct obj_data *trinket = read_object(101, VIRTUAL);

  write_rent_file("Tester", old_file);
  GET_OBJ_WEIGHT(bag) = 10;
  GET_OBJ_WEIGHT(trinket) = 3;
  obj_to_obj(trinket, bag);
  GET_OBJ_WEIGHT(bag) += GET_OBJ_WEIGHT(trinket);
  equip_char(&player, bag, WEAR_HOLD);
  fail_read_vnum = 101;

  TEST_ASSERT_FALSE(Crash_crashsave(&player));

  TEST_ASSERT_EQUAL_STRING(old_file, slurp_rent());
  TEST_ASSERT_EQUAL_INT(13, GET_OBJ_WEIGHT(bag));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "rent-write\n"));
}

/* B2-15: a record whose prototype is gone (or whose '#' line is malformed)
 * was skipped while temp still pointed at the previous object, so the
 * skipped record's tags overwrote that object. */
static void test_skipped_record_tags_do_not_touch_previous_object(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\n\n#999\nWght: 77\nLoc : %d\n\n"
                            "#x\nWght: 55\n\n$~\n",
                            RENT_CRASH, (long)time(0), WEAR_HOLD + 1) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_WEIGHT(player.carrying));
  TEST_ASSERT_NULL(GET_EQ(&player, WEAR_HOLD));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "missing-proto\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "rent-record\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "vnum 999"));
}

/* B2-22: a rent timestamp in the future made the day count negative; the
 * unsigned cost wrapped to a huge value and the "equipment lost" path ran,
 * rewriting the rent file empty. It must charge nothing and report. */
static void test_future_rent_timestamp_charges_nothing(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body), "%d %ld 10 0 0 1\n#101\n\n$~\n",
                            RENT_RENTED,
                            (long)time(0) + 3L * SECS_PER_REAL_DAY) > 0);
  write_rent_file("Tester", body);
  GET_GOLD(&player) = 100;
  GET_BANK_GOLD(&player) = 50;

  TEST_ASSERT_EQUAL_INT(0, Crash_load(&player));

  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_EQUAL_INT(100, GET_GOLD(&player));
  TEST_ASSERT_EQUAL_INT(50, GET_BANK_GOLD(&player));
  TEST_ASSERT_EQUAL_STRING("rent-clock", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
}

/* B2-22: the old unsigned "cost - gold" wrapped when gold on hand exceeded
 * the rent and credited the difference to the bank. */
static void test_rent_paid_from_hand_leaves_bank_alone(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body), "%d %ld 10 0 0 1\n#101\n\n$~\n",
                            RENT_RENTED,
                            (long)time(0) - 2L * SECS_PER_REAL_DAY - 60) > 0);
  write_rent_file("Tester", body);
  GET_GOLD(&player) = 100;
  GET_BANK_GOLD(&player) = 50;

  TEST_ASSERT_EQUAL_INT(0, Crash_load(&player));

  TEST_ASSERT_EQUAL_INT(80, GET_GOLD(&player));
  TEST_ASSERT_EQUAL_INT(50, GET_BANK_GOLD(&player));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  /* Rent larger than gold on hand comes out of the bank. */
  free_tree(player.carrying);
  player.carrying = NULL;
  write_rent_file("Tester", body);
  GET_GOLD(&player) = 5;
  GET_BANK_GOLD(&player) = 50;
  TEST_ASSERT_EQUAL_INT(0, Crash_load(&player));
  TEST_ASSERT_EQUAL_INT(0, GET_GOLD(&player));
  TEST_ASSERT_EQUAL_INT(35, GET_BANK_GOLD(&player));
}

/* B2-31: record diagnostics named neither the file's owner nor the record
 * ("malformed saved object values"), so an admin could not find it. */
static void test_record_diagnostics_name_owner_line_and_vnum(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nVals: 1 2 x\nZzzz: 1\n\n$~\n",
                            RENT_CRASH, (long)time(0)) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_EQUAL_STRING("parse-values\nrent-tag\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 object line 2 obj vnum 101: malformed"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 object line 3 obj vnum 101: unknown tag Zzzz"));
}

/* B2-07 follow-up: ADes/EDes went through fread_string(), which exit(1)s
 * the server at end of file; a rent file truncated inside an extra
 * description took the game down at its owner's login. */
static void test_truncated_extra_description_is_dropped_not_fatal(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nEDes:\nkey~\nno terminator\n",
                            RENT_CRASH, (long)time(0)) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_NULL(player.carrying->ex_description);
  TEST_ASSERT_EQUAL_STRING("rent-string", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 101"));
}

/* A terminated extra description is kept. */
static void test_extra_description_loads(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nEDes:\nkey~\nA fine thing.\n~\n\n$~\n",
                            RENT_CRASH, (long)time(0)) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_NOT_NULL(player.carrying->ex_description);
  TEST_ASSERT_EQUAL_STRING("key", player.carrying->ex_description->keyword);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(player.carrying->ex_description->keyword);
  free(player.carrying->ex_description->description);
  free(player.carrying->ex_description);
  player.carrying->ex_description = NULL;
}

/* An empty "~" keyword loads as NULL; the next save dereferenced it in
 * objsave_save_obj_record()'s "!*ex_desc->keyword" sanity check. */
static void test_save_skips_extra_description_with_null_keyword(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nEDes:\n~\nA fine thing.\n~\n\n$~\n",
                            RENT_CRASH, (long)time(0)) > 0);
  write_rent_file("Tester", body);
  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));
  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_NOT_NULL(player.carrying->ex_description);
  TEST_ASSERT_NULL(player.carrying->ex_description->keyword);

  TEST_ASSERT_TRUE(Crash_crashsave(&player));

  TEST_ASSERT_EQUAL_STRING("write-edesc", tb_captured_operation);
  TEST_ASSERT_NULL(strstr(slurp_rent(), "EDes"));
  free(player.carrying->ex_description->description);
  free(player.carrying->ex_description);
  player.carrying->ex_description = NULL;
}

/* B2 doc gap (handle_obj): objects nested deeper than MAX_BAG_ROWS were
 * silently left loose in inventory. */
static void test_too_deep_nesting_is_reported(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nLoc : %d\n\n$~\n",
                            RENT_CRASH, (long)time(0), -6) > 0);	/* MAX_BAG_ROWS is 5 */
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_EQUAL_STRING("rent-nesting", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242 obj vnum 101"));
}

/* Contents whose container never arrived (its record skipped for a missing
 * prototype, or the file cut short) stayed in handle_obj()'s pending row
 * when the file ended: allocated and on object_list, but nowhere in the
 * game (a leak under LeakSanitizer here). They go to inventory, reported. */
static void test_contents_without_container_reach_inventory(void)
{
  char body[256];

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n#101\nLoc : -1\n\n#999\n\n$~\n",
                            RENT_CRASH, (long)time(0)) > 0);	/* no vnum 999 */
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  TEST_ASSERT_NOT_NULL(player.carrying);
  TEST_ASSERT_EQUAL_INT(101, GET_OBJ_VNUM(player.carrying));
  TEST_ASSERT_EQUAL_STRING("missing-proto\nrent-nesting\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "player idnum 4242: 1 object(s) at depth 1 lost their container"));
}

/* Fuzzer PLAYER-6 (restrung-leak): Name, Shrt and Desc (and ADes) were set
 * with a raw strdup() and no free, so a record repeating a tag leaked the
 * earlier copy (LeakSanitizer / Valgrind at exit). A repeat now replaces
 * the object's own copy, while a string still shared with the prototype
 * (the fixture's are literals) is never freed. A unique object owns all of
 * its strings. */
static void test_repeated_string_tags_replace_their_copy(void)
{
  char body[512];
  struct obj_data *unique, *trinket;

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n"
                            "#101\nShrt: first\nShrt: second\nName: n1\nName: n2\n"
                            "Desc: d1\nDesc: d2\nADes:\none\n~\nADes:\ntwo\n~\n"
                            "#%d\nShrt: u1\nShrt: u2\n$~\n",
                            RENT_CRASH, (long)time(0), NOTHING) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  unique = player.carrying;
  TEST_ASSERT_NOT_NULL(unique);
  trinket = unique->next_content;
  TEST_ASSERT_NOT_NULL(trinket);
  TEST_ASSERT_EQUAL_STRING("u2", unique->short_description);
  TEST_ASSERT_EQUAL_STRING("second", trinket->short_description);
  TEST_ASSERT_EQUAL_STRING("n2", trinket->name);
  TEST_ASSERT_EQUAL_STRING("d2", trinket->description);
  TEST_ASSERT_EQUAL_STRING("two\n", trinket->action_description);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Fuzzer PLAYER-3 (name-path): get_filename() now refuses a name that is
 * not a safe path component, such as the "../" a damaged index or a pfile
 * Name tag can supply. Every rent-file path must then refuse, report with
 * the player's idnum and open or remove nothing; most returned silently. */
static void test_rent_paths_refuse_an_unsafe_name(void)
{
  char unsafe[] = "../evil";

  player.player.name = unsafe;

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));
  TEST_ASSERT_FALSE(Crash_delete_crashfile(&player));
  TEST_ASSERT_FALSE(Crash_delete_file(unsafe));
  TEST_ASSERT_FALSE(Crash_clean_file(unsafe));
  Crash_listrent(&player, unsafe);

  TEST_ASSERT_EQUAL_STRING("rent-name\nrent-name\nrent-name\nrent-name\nrent-name\n",
                           tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242: no safe rent file name; rent load refused"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "crash file delete refused"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "rent file delete refused"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "rent file clean refused"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "rent listing refused"));
  TEST_ASSERT_NULL(strstr(tb_captured_detail_log, "../evil"));
  TEST_ASSERT_NOT_NULL(strstr(told, "problem loading your objects"));
  TEST_ASSERT_NOT_NULL(strstr(told, "That is not a valid player name."));
  TEST_ASSERT_NULL(player.carrying);
}

/* --- receptionist fixtures: "offer" is command 1; every act() is kept. --- */
SPECIAL(receptionist);
static struct command_info offer_cmd_table[] = {
  { "RESERVED", "", 0, NULL, 0, 0 },
  { "offer", "offer", 0, NULL, 0, 0 },
  { "\n", "zzzzzzz", 0, NULL, 0, 0 }
};
struct command_info *complete_cmd_info = offer_cmd_table;
#define OFFER_CMD 1

#define ACT_LOG_MAX 8
static int acts;
static char act_formats[ACT_LOG_MAX][256];
static struct obj_data *act_objs[ACT_LOG_MAX];

char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func; (void)hide_invisible;
  (void)ch; (void)vict_obj; (void)type;
  if (acts < ACT_LOG_MAX) {
    if (snprintf(act_formats[acts], sizeof(act_formats[acts]), "%s",
                 str ? str : "") < 0)
      act_formats[acts][0] = '\0';
    act_objs[acts] = obj;
  }
  acts++;
  return NULL;
}
int room_is_dark_at(room_rnum room, const char *source_file, int source_line,
                    const char *source_func)
{ (void)room; (void)source_file; (void)source_line; (void)source_func; return FALSE; }
int rand_number_at(int from, int to, const char *source_file, int source_line,
                   const char *source_func)
{ (void)to; (void)source_file; (void)source_line; (void)source_func; return from; }
int find_command(const char *command) { (void)command; return -1; }

/* B2-32: the receptionist formatted each object's short description into
 * an act() format, so a '$' in it (builder or iedit text) was read as an
 * act() code: "a $N doll" printed the player's name. The object must reach
 * act() as its obj argument ($p), never as format text. */
static void test_offer_passes_objects_to_act_as_p(void)
{
  char arg[] = "";
  struct descriptor_data desc;
  struct char_data recep;
  struct obj_data *doll = read_object(101, VIRTUAL);
  int i;

  memset(&desc, 0, sizeof(desc));
  memset(&recep, 0, sizeof(recep));
  player.desc = &desc;
  GET_POS(&recep) = POS_STANDING;
  doll->short_description = strdup("a $N doll");
  TEST_ASSERT_NOT_NULL(doll->short_description);
  obj_to_char(doll, &player);

  /* Unrentable: Crash_report_unrentables(). */
  SET_BIT_AR(GET_OBJ_EXTRA(doll), ITEM_NORENT);
  acts = 0;
  TEST_ASSERT_TRUE(receptionist(&player, &recep, OFFER_CMD, arg));
  TEST_ASSERT_EQUAL_STRING("$n tells you, 'You cannot store $p.'", act_formats[0]);
  TEST_ASSERT_EQUAL_PTR(doll, act_objs[0]);

  /* Rentable: Crash_report_rent(). */
  REMOVE_BIT_AR(GET_OBJ_EXTRA(doll), ITEM_NORENT);
  GET_OBJ_RENT(doll) = 10;
  CONFIG_MAX_OBJ_SAVE = 30;
  acts = 0;
  TEST_ASSERT_TRUE(receptionist(&player, &recep, OFFER_CMD, arg));
  CONFIG_MAX_OBJ_SAVE = 0;
  TEST_ASSERT_EQUAL_STRING("$n tells you, '   10 coins for $p..'", act_formats[0]);
  TEST_ASSERT_EQUAL_PTR(doll, act_objs[0]);
  for (i = 0; i < acts && i < ACT_LOG_MAX; i++)
    TEST_ASSERT_NULL(strstr(act_formats[i], "doll"));
  player.desc = NULL;
}

/* The carried object of the given type (each test loads one of each). */
static struct obj_data *carried_of_type(int type)
{
  struct obj_data *obj;

  for (obj = player.carrying; obj; obj = obj->next_content)
    if (GET_OBJ_TYPE(obj) == type)
      return obj;
  TEST_FAIL_MESSAGE("no carried object of that type");
  return NULL;
}

/* A rent record may set an object's Type and Vals, but only world
 * prototypes went through db.c's check_object(). A hand-edited rent file
 * could plant attack type 40 (attack_hit_text[] has 15 entries), a liquid
 * outside drinks[], or 2000000000d6 dice that dice() loops over on every
 * blow. They are repaired to check_object()'s defaults and reported. */
static void test_rent_values_are_repaired_like_world_objects(void)
{
  char body[512];
  struct obj_data *weapon, *drink, *fountain;

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n"
                            "#101\nType: %d\nVals: 0 2000000000 6 40\n"
                            "#101\nType: %d\nVals: 10 5 99 0\n"
                            "#101\nType: %d\nVals: 10 5 -1 0\n$~\n",
                            RENT_CRASH, (long)time(0), ITEM_WEAPON,
                            ITEM_DRINKCON, ITEM_FOUNTAIN) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  weapon = carried_of_type(ITEM_WEAPON);
  drink = carried_of_type(ITEM_DRINKCON);
  fountain = carried_of_type(ITEM_FOUNTAIN);
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(weapon, 3));
  TEST_ASSERT_EQUAL_INT(MAX_WEAPON_NDICE, GET_OBJ_VAL(weapon, 1));
  TEST_ASSERT_EQUAL_INT(6, GET_OBJ_VAL(weapon, 2));
  TEST_ASSERT_EQUAL_INT(LIQ_WATER, GET_OBJ_VAL(drink, 2));
  TEST_ASSERT_EQUAL_INT(LIQ_WATER, GET_OBJ_VAL(fountain, 2));
  TEST_ASSERT_EQUAL_STRING("rent-object-check\nrent-object-check\n"
                           "rent-object-check\nrent-object-check\n",
                           tb_captured_op_log);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 object line 1 obj vnum 101: "
                              "attack type 40 out of range 0..14; using 0 (hit)"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "object line 1 obj vnum 101: damage dice "
                              "2000000000d6 outside 0..50d0..50; using 50d6"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "player idnum 4242 object line 4 obj vnum 101: "
                              "liquid 99 out of range"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "object line 7 obj vnum 101: liquid -1 out of range"));
}

/* Values inside every range load untouched and unreported. */
static void test_rent_values_in_range_are_kept(void)
{
  char body[512];
  struct obj_data *weapon, *drink;

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n"
                            "#101\nType: %d\nVals: 0 50 50 %d\n"
                            "#101\nType: %d\nVals: 10 5 %d 0\n$~\n",
                            RENT_CRASH, (long)time(0), ITEM_WEAPON,
                            NUM_ATTACK_TYPES - 1, ITEM_DRINKCON,
                            NUM_LIQ_TYPES - 1) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  weapon = carried_of_type(ITEM_WEAPON);
  drink = carried_of_type(ITEM_DRINKCON);
  TEST_ASSERT_EQUAL_INT(NUM_ATTACK_TYPES - 1, GET_OBJ_VAL(weapon, 3));
  TEST_ASSERT_EQUAL_INT(50, GET_OBJ_VAL(weapon, 1));
  TEST_ASSERT_EQUAL_INT(50, GET_OBJ_VAL(weapon, 2));
  TEST_ASSERT_EQUAL_INT(NUM_LIQ_TYPES - 1, GET_OBJ_VAL(drink, 2));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* check_object() only reports a bad spell level, spell number, charge
 * count or seat count on a world object; a rent object is reported the
 * same way (warnings) and keeps its values. */
static void test_rent_values_check_object_reports_are_kept(void)
{
  char body[512];
  struct obj_data *scroll, *wand, *chair;

  TEST_ASSERT_TRUE(snprintf(body, sizeof(body),
                            "%d %ld 0 0 0 0\n"
                            "#101\nType: %d\nVals: -5 99999 0 -1\n"
                            "#101\nType: %d\nVals: 10 3 9 %d\n"
                            "#101\nType: %d\nVals: 2 4 0 0\n$~\n",
                            RENT_CRASH, (long)time(0), ITEM_SCROLL,
                            ITEM_WAND, MAX_SPELLS + 1, ITEM_FURNITURE) > 0);
  write_rent_file("Tester", body);

  TEST_ASSERT_EQUAL_INT(1, Crash_load(&player));

  scroll = carried_of_type(ITEM_SCROLL);
  wand = carried_of_type(ITEM_WAND);
  chair = carried_of_type(ITEM_FURNITURE);
  TEST_ASSERT_EQUAL_INT(-5, GET_OBJ_VAL(scroll, 0));
  TEST_ASSERT_EQUAL_INT(99999, GET_OBJ_VAL(scroll, 1));
  TEST_ASSERT_EQUAL_INT(9, GET_OBJ_VAL(wand, 2));
  TEST_ASSERT_EQUAL_INT(MAX_SPELLS + 1, GET_OBJ_VAL(wand, 3));
  TEST_ASSERT_EQUAL_INT(4, GET_OBJ_VAL(chair, 1));
  TEST_ASSERT_EQUAL_INT(5, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "object line 1 obj vnum 101: spell level -5 out of range"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "object line 1 obj vnum 101: value 1 spell 99999 out of range"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "object line 4 obj vnum 101: has 9 charges, more than its maximum 3"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "object line 4 obj vnum 101: value 3 spell"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "object line 7 obj vnum 101: seats 4, more than its capacity 2"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_container_refused_by_autoeq_keeps_contents);
  RUN_TEST(test_loc_past_num_wears_is_reported);
  RUN_TEST(test_listrent_truncates_loudly);
  RUN_TEST(test_crashsave_failure_keeps_live_file);
  RUN_TEST(test_crashsave_commits);
  RUN_TEST(test_rentsave_failure_extracts_nothing);
  RUN_TEST(test_rentsave_success_extracts_everything);
  RUN_TEST(test_failed_content_record_fails_the_save);
  RUN_TEST(test_skipped_record_tags_do_not_touch_previous_object);
  RUN_TEST(test_future_rent_timestamp_charges_nothing);
  RUN_TEST(test_rent_paid_from_hand_leaves_bank_alone);
  RUN_TEST(test_record_diagnostics_name_owner_line_and_vnum);
  RUN_TEST(test_truncated_extra_description_is_dropped_not_fatal);
  RUN_TEST(test_extra_description_loads);
  RUN_TEST(test_save_skips_extra_description_with_null_keyword);
  RUN_TEST(test_too_deep_nesting_is_reported);
  RUN_TEST(test_offer_passes_objects_to_act_as_p);
  RUN_TEST(test_rent_values_are_repaired_like_world_objects);
  RUN_TEST(test_rent_values_in_range_are_kept);
  RUN_TEST(test_rent_values_check_object_reports_are_kept);
  RUN_TEST(test_listrent_pager_owns_its_copy);
  RUN_TEST(test_contents_without_container_reach_inventory);
  RUN_TEST(test_repeated_string_tags_replace_their_copy);
  RUN_TEST(test_rent_paths_refuse_an_unsafe_name);
  (void)rmdir(scratch_dir);
  return UNITY_END();
}
