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

/* Diagnostics the production code may log while running. */
void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  if (snprintf(paged, sizeof(paged), "%s", str) < 0)
    paged[0] = '\0';
  pages++;
}

int get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name)
{
  (void)mode;
  return snprintf(filename, fbufsize, "%s/%s.objs", scratch_dir, orig_name) > 0;
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

void equip_char(struct char_data *ch, struct obj_data *obj, int pos)
{
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
  (void)rmdir(scratch_dir);
  return UNITY_END();
}
