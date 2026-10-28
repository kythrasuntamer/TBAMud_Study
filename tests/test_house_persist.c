/* Executable tests of the production player-house code (house.c).
 *
 * The house control file (lib/etc/hcontrol) is a raw binary array of
 * struct house_control_rec, so every field in it is untrusted on read.
 * The tests write records into a scratch lib tree (the test chdir()s into
 * it, since HCONTROL_FILE is relative), run the real House_boot() and the
 * commands built on it, and assert on the structured diagnostic.
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
#include "house.h"
#include <stdarg.h>
#include <sys/stat.h>
#include "tb_error_capture.h"
#include "tb_persist_support.h"

/* Rooms 100 (house) and 101 (atrium), joined north/south. */
static struct room_data rooms[2];
static struct room_direction_data north, south;
static char scratch_dir[256];
static char start_dir[1024];
static char sent[MAX_STRING_LENGTH];

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(sent);
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(sent + used, sizeof(sent) - used, messg, args) < 0)
    sent[used] = '\0';
  va_end(args);
  return strlen(sent);
}

room_rnum real_room(room_vnum vnum)
{
  if (vnum == 100)
    return 0;
  if (vnum == 101)
    return 1;
  return NOWHERE;
}

char *get_name_by_id(long id) { return id == 4242 ? (char *)"owner" : NULL; }
long get_id_by_name(const char *name)
{
  if (!strcmp(name, "owner"))
    return 4242;
  return !strcmp(name, "guest") ? 5000 : -1;
}

/* House object files are not under test: none exist in the scratch tree. */
obj_save_data *objsave_parse_objects(FILE *fl) { (void)fl; return NULL; }
int objsave_save_obj_record(struct obj_data *obj, FILE *fl, int location)
{ (void)obj; (void)fl; (void)location; return 1; }

/* Reached through do_hcontrol(); no house object files are written here. */
FILE *objsave_open_staged(const char *dest, char *temp, size_t temp_size)
{ (void)dest; (void)temp; (void)temp_size; return NULL; }
int objsave_commit_staged(FILE *fp, const char *temp, const char *dest)
{ (void)fp; (void)temp; (void)dest; return 0; }
void objsave_abort_staged(FILE *fp, const char *temp) { (void)fp; (void)temp; }
obj_rnum real_object(obj_vnum vnum) { (void)vnum; return NOTHING; }
struct obj_data *read_object(obj_vnum nr, int type) { (void)nr; (void)type; return NULL; }

void obj_to_room(struct obj_data *object, room_rnum room) { (void)object; (void)room; }
void extract_obj(struct obj_data *obj) { (void)obj; }
char *CAP(char *txt)
{
  if (txt && *txt)
    *txt = (char)toupper((unsigned char)*txt);
  return txt;
}

/* Command parsing used by do_hcontrol()/do_house(): plain word splitting. */
const char *dirs[] = { "north", "east", "south", "west", "up", "down", "\n" };
int rev_dir[] = { SOUTH, WEST, NORTH, EAST, DOWN, UP };
int is_abbrev(const char *arg1, const char *arg2)
{
  return *arg1 && !strncmp(arg1, arg2, strlen(arg1));
}
char *one_argument(char *argument, char *first_arg)
{
  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ')
    *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}
void half_chop(char *string, char *arg1, char *arg2)
{
  string = one_argument(string, arg1);
  while (*string == ' ')
    string++;
  strlcpy(arg2, string, MAX_INPUT_LENGTH);
}
int search_block(char *arg, const char **list, int exact)
{
  int i;
  (void)exact;
  for (i = 0; *list[i] != '\n'; i++)
    if (!strcmp(arg, list[i]))
      return i;
  return -1;
}

/* ------------------------------------------------------------------ */
static struct house_control_rec good_record(void)
{
  struct house_control_rec rec;
  memset(&rec, 0, sizeof(rec));
  rec.vnum = 100;
  rec.atrium = 101;
  rec.exit_num = NORTH;
  rec.built_on = 1000;
  rec.mode = HOUSE_PRIVATE;
  rec.owner = 4242;
  rec.num_of_guests = 0;
  return rec;
}

static void write_hcontrol(const struct house_control_rec *recs, size_t count)
{
  FILE *fl = fopen(HCONTROL_FILE, "wb");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(count, fwrite(recs, sizeof(*recs), count, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

static long file_size(const char *path)
{
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (long)st.st_size;
}

static struct char_data imm;
static struct player_special_data imm_specials;

void setUp(void)
{
  if (scratch_dir[0] == '\0') {
    const char *base = getenv("TMPDIR");
    char sub[300];
    TEST_ASSERT_NOT_NULL(getcwd(start_dir, sizeof(start_dir)));
    TEST_ASSERT_TRUE(snprintf(scratch_dir, sizeof(scratch_dir), "%s/tb_house_XXXXXX",
                              base && *base ? base : "/tmp") > 0);
    TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
    TEST_ASSERT_TRUE(snprintf(sub, sizeof(sub), "%s/etc", scratch_dir) > 0);
    TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0700));
    TEST_ASSERT_TRUE(snprintf(sub, sizeof(sub), "%s/house", scratch_dir) > 0);
    TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0700));
  }
  TEST_ASSERT_EQUAL_INT(0, chdir(scratch_dir));

  memset(rooms, 0, sizeof(rooms));
  memset(&north, 0, sizeof(north));
  memset(&south, 0, sizeof(south));
  rooms[0].number = 100;
  rooms[1].number = 101;
  north.to_room = 1;
  south.to_room = 0;
  rooms[0].dir_option[NORTH] = &north;
  rooms[1].dir_option[SOUTH] = &south;
  world = rooms;
  top_of_world = 1;

  memset(&imm, 0, sizeof(imm));
  memset(&imm_specials, 0, sizeof(imm_specials));
  imm.player_specials = &imm_specials;
  GET_LEVEL(&imm) = LVL_IMPL;
  sent[0] = '\0';
  tb_error_capture_reset();
}

void tearDown(void)
{
  (void)remove(HCONTROL_FILE);
  world = NULL;
  TEST_ASSERT_EQUAL_INT(0, chdir(start_dir));
}

/* B2-08: num_of_guests from disk bounded every guests[] loop and do_house's
 * append; a record claiming 5000 guests must be rejected at boot. */
static void test_boot_rejects_out_of_range_guest_count(void)
{
  struct house_control_rec rec = good_record();

  rec.num_of_guests = 5000;
  write_hcontrol(&rec, 1);
  House_boot();

  TEST_ASSERT_NOT_NULL(strstr(tb_captured_op_log, "hcontrol-record\n"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "house vnum 100"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "num_of_guests 5000"));
  hcontrol_list_houses(&imm, (char *)"");
  TEST_ASSERT_NOT_NULL(strstr(sent, "No houses have been defined"));
}

/* B2-09: a record whose owner is missing from the player index (deleted, or
 * the index itself was truncated) used to be dropped silently and hcontrol
 * rewritten without it. It is now reported and the file left intact. */
static void test_boot_skip_is_reported_and_file_kept(void)
{
  struct house_control_rec recs[2];
  long before;

  recs[0] = good_record();
  recs[1] = good_record();
  recs[1].vnum = 100;
  recs[1].owner = 999;	/* not in the index */
  recs[0].owner = 999;
  write_hcontrol(recs, 2);
  before = file_size(HCONTROL_FILE);

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-keep", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);	/* two boot-skip + keep */
  TEST_ASSERT_EQUAL_INT(before, file_size(HCONTROL_FILE));
}

/* B2-09: a trailing partial record is reported instead of ignored. */
static void test_boot_reports_partial_record(void)
{
  struct house_control_rec rec = good_record();
  FILE *fl;

  write_hcontrol(&rec, 1);
  fl = fopen(HCONTROL_FILE, "ab");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(5, fwrite("junk!", 1, 5, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-keep", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT((long)sizeof(rec) + 5, file_size(HCONTROL_FILE));
  hcontrol_list_houses(&imm, (char *)"");
  TEST_ASSERT_NOT_NULL(strstr(sent, "Owner"));
}

/* A clean boot loads the house and rewrites nothing it did not read. */
static void test_clean_boot_loads_house(void)
{
  struct house_control_rec rec = good_record();

  write_hcontrol(&rec, 1);
  House_boot();

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT((long)sizeof(rec), file_size(HCONTROL_FILE));
  TEST_ASSERT_TRUE(ROOM_FLAGGED(0, ROOM_HOUSE));
  TEST_ASSERT_TRUE(ROOM_FLAGGED(1, ROOM_ATRIUM));
}

static void read_hcontrol(struct house_control_rec *rec)
{
  FILE *fl = fopen(HCONTROL_FILE, "rb");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(1, fread(rec, sizeof(*rec), 1, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

/* Leave a dirty stack behind for the next call to inherit. */
static void __attribute__((noinline)) dirty_stack(void)
{
  volatile unsigned char junk[8192];
  size_t i;
  for (i = 0; i < sizeof(junk); i++)
    junk[i] = 0xA5;
}

/* B2-23: hcontrol_build_house() filled a stack record field by field and
 * House_save_control() fwrite()s all of it, spare fields and padding
 * included, so stack garbage reached hcontrol. */
static void test_built_house_record_has_no_stack_garbage(void)
{
  char cmd[] = "build 100 north owner";
  struct house_control_rec on_disk, expect, none = good_record();

  write_hcontrol(&none, 0);
  House_boot();
  dirty_stack();
  do_hcontrol(&imm, cmd, 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "House built"));

  read_hcontrol(&on_disk);
  memset(&expect, 0, sizeof(expect));
  expect.vnum = 100;
  expect.atrium = 101;
  expect.exit_num = NORTH;
  expect.built_on = on_disk.built_on;
  expect.mode = HOUSE_PRIVATE;
  expect.owner = 4242;
  TEST_ASSERT_EQUAL_MEMORY(&expect, &on_disk, sizeof(expect));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_boot_rejects_out_of_range_guest_count);
  RUN_TEST(test_boot_skip_is_reported_and_file_kept);
  RUN_TEST(test_boot_reports_partial_record);
  RUN_TEST(test_clean_boot_loads_house);
  RUN_TEST(test_built_house_record_has_no_stack_garbage);
  {
    char sub[300];
    if (snprintf(sub, sizeof(sub), "%s/etc", scratch_dir) > 0)
      (void)rmdir(sub);
    if (snprintf(sub, sizeof(sub), "%s/house", scratch_dir) > 0)
      (void)rmdir(sub);
    (void)rmdir(scratch_dir);
  }
  return UNITY_END();
}
