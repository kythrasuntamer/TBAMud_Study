/* Executable tests of the production player-house code (house.c).
 *
 * The house control file (lib/etc/hcontrol) is a format header and a raw
 * binary array of struct house_control_rec (format 1, still read, has no
 * header), so every field in it is untrusted on read.
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
#include <stdint.h>
#include <sys/stat.h>
#include "tb_error_capture.h"
#include "tb_persist_support.h"

/* Rooms 100 (house) and 101 (atrium), joined north/south. */
static struct room_data rooms[2];
static struct room_direction_data north, south;
static char scratch_dir[256];
static char start_dir[1024];
static char sent[MAX_STRING_LENGTH];

/* Under ASan, report a read of a dead stack frame (see pager_page_on());
 * ASAN_OPTIONS from the environment still apply on top. Unused without
 * ASan. */
const char *__asan_default_options(void); /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
const char *__asan_default_options(void) /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
{
  return "detect_stack_use_after_return=1";
}

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
}

/* The player pages on: show_string() reads the pager's text again, after
 * the listing function has returned. */
static void pager_page_on(char *out, size_t size)
{
  if (snprintf(out, size, "%s", pager_text ? pager_text : "") < 0)
    out[0] = '\0';
}

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

/* The object-record format itself is objsave.c's (test_objsave_persist).
 * Here a house object file "parses" as the list a test queued (queue_parsed();
 * empty by default), and each written record's object, Loc and weight is
 * logged, so House_load()/House_save() are tested on their own. */
static obj_save_data *parse_result;
obj_save_data *objsave_parse_objects_from(FILE *fl, const char *resource,
                                          const char *owner)
{
  obj_save_data *result = parse_result;
  (void)fl; (void)resource; (void)owner;
  parse_result = NULL;
  return result;
}
#define MAX_RECORDED 16
static int recorded;
static struct obj_data *recorded_obj[MAX_RECORDED];
static int recorded_loc[MAX_RECORDED], recorded_weight[MAX_RECORDED];
int objsave_save_obj_record(struct obj_data *obj, FILE *fl, int location)
{
  (void)fl;
  if (recorded < MAX_RECORDED) {
    recorded_obj[recorded] = obj;
    recorded_loc[recorded] = location;
    recorded_weight[recorded] = GET_OBJ_WEIGHT(obj);
  }
  recorded++;
  return 1;
}

/* Same contract as objsave.c's staged-file helpers: mkstemp() beside dest,
 * rename() over it on commit. House_save_control() stages hcontrol. */
FILE *objsave_open_staged(const char *dest, char *temp, size_t temp_size)
{
  int fd;
  FILE *fp;
  if (snprintf(temp, temp_size, "%s.tmpXXXXXX", dest) >= (int)temp_size)
    return NULL;
  if ((fd = mkstemp(temp)) < 0)
    return NULL;
  if (!(fp = fdopen(fd, "w"))) {
    close(fd);
    unlink(temp);
  }
  return fp;
}
int objsave_commit_staged(FILE *fp, const char *temp, const char *dest)
{
  int failed = fflush(fp) != 0;
  failed |= fclose(fp) != 0;
  if (failed || rename(temp, dest) != 0) {
    unlink(temp);
    return 0;
  }
  return 1;
}
void objsave_abort_staged(FILE *fp, const char *temp)
{
  if (fp)
    (void) fclose(fp);
  (void) unlink(temp);
}
/* Vnum 300 is the only object prototype (rnum 0); used by asciiconvert. */
static int live_objects;
obj_rnum real_object(obj_vnum vnum) { return vnum == 300 ? 0 : NOTHING; }
struct obj_data *read_object(obj_vnum nr, int type)
{
  struct obj_data *obj;
  if (type != REAL || nr != 0)
    return NULL;
  obj = calloc(1, sizeof(*obj));
  TEST_ASSERT_NOT_NULL(obj);
  live_objects++;
  return obj;
}

/* Like handler.c: the object goes to the head of the room's contents. */
void obj_to_room(struct obj_data *object, room_rnum room)
{
  object->next_content = world[room].contents;
  world[room].contents = object;
  IN_ROOM(object) = room;
}
/* Like handler.c: to the head of the contents; the weight joins every
 * container up the chain unless the container's capacity is unlimited. */
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to)
{
  struct obj_data *t;

  obj->next_content = obj_to->contains;
  obj_to->contains = obj;
  obj->in_obj = obj_to;
  if (GET_OBJ_VAL(obj_to, 0) > 0)
    for (t = obj_to; t; t = t->in_obj)
      GET_OBJ_WEIGHT(t) += GET_OBJ_WEIGHT(obj);
}
void extract_obj(struct obj_data *obj)
{
  free(obj);
  live_objects--;
}
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

/* The hcontrol format 2 header as house.c writes it (B2-23), spelled out
 * here on purpose: a change to the on-disk format must fail these tests. */
struct test_hcontrol_header {
  unsigned char magic[8];
  uint32_t version, record_size, record_count;
};
static const unsigned char test_hcontrol_magic[8] =
  { 0xFF, 0xFF, 'T', 'B', 'H', 'C', '\r', '\n' };
#define HCONTROL_HEADER_SIZE ((long)sizeof(struct test_hcontrol_header))

/* Write hcontrol with a header claiming version/record_size/record_count,
 * then nbytes of record data. */
static void write_hcontrol_as(uint32_t version, uint32_t record_size,
                              uint32_t record_count, const void *data,
                              size_t nbytes)
{
  struct test_hcontrol_header hdr;
  FILE *fl = fopen(HCONTROL_FILE, "wb");

  TEST_ASSERT_NOT_NULL(fl);
  memset(&hdr, 0, sizeof(hdr));
  memcpy(hdr.magic, test_hcontrol_magic, sizeof(hdr.magic));
  hdr.version = version;
  hdr.record_size = record_size;
  hdr.record_count = record_count;
  TEST_ASSERT_EQUAL_size_t(1, fwrite(&hdr, sizeof(hdr), 1, fl));
  TEST_ASSERT_EQUAL_size_t(nbytes, fwrite(data, 1, nbytes, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

/* The current format: header, then count records. */
static void write_hcontrol(const struct house_control_rec *recs, size_t count)
{
  write_hcontrol_as(2, sizeof(*recs), (uint32_t)count, recs,
                    count * sizeof(*recs));
}

/* Format 1, as every build before B2-23 wrote it: the bare record array. */
static void write_legacy_hcontrol(const struct house_control_rec *recs,
                                  size_t count)
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
  recorded = 0;
  tb_error_capture_reset();
}

/* Free a test object tree (new_obj() objects): contents and siblings. */
static void free_objs(struct obj_data *obj)
{
  struct obj_data *next;

  for (; obj; obj = next) {
    next = obj->next_content;
    free_objs(obj->contains);
    free(obj);
  }
}

void tearDown(void)
{
  (void)remove(HCONTROL_FILE);
  /* Whatever a failed test left queued or on the floor. */
  while (parse_result) {
    obj_save_data *next = parse_result->next;
    free_objs(parse_result->obj);
    free(parse_result);
    parse_result = next;
  }
  free_objs(rooms[0].contents);
  free_objs(rooms[1].contents);
  rooms[0].contents = rooms[1].contents = NULL;
  world = NULL;
  free(pager_copy);
  pager_copy = NULL;
  pager_text = NULL;
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

/* B2-09: a trailing partial record is reported instead of ignored. Format
 * 1 has no record count, so only its length can show one. */
static void test_boot_reports_partial_record(void)
{
  struct house_control_rec rec = good_record();
  FILE *fl;

  write_legacy_hcontrol(&rec, 1);
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
  TEST_ASSERT_EQUAL_INT(HCONTROL_HEADER_SIZE + (long)sizeof(rec),
                        file_size(HCONTROL_FILE));
  TEST_ASSERT_TRUE(ROOM_FLAGGED(0, ROOM_HOUSE));
  TEST_ASSERT_TRUE(ROOM_FLAGGED(1, ROOM_ATRIUM));
}

/* Check hcontrol's header byte for byte (format 2, count records) and read
 * its first record into rec. The second read runs only after the first
 * succeeded, and both outputs start zeroed: clang-tidy cannot see that a
 * failed TEST_ASSERT_EQUAL_*() does not return. */
static void read_hcontrol_n(struct house_control_rec *rec, uint32_t count)
{
  struct test_hcontrol_header hdr, expect;
  FILE *fl = fopen(HCONTROL_FILE, "rb");
  int complete;

  memset(rec, 0, sizeof(*rec));
  memset(&hdr, 0, sizeof(hdr));
  TEST_ASSERT_NOT_NULL(fl);
  complete = fread(&hdr, sizeof(hdr), 1, fl) == 1 &&
             fread(rec, sizeof(*rec), 1, fl) == 1;
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  TEST_ASSERT_TRUE_MESSAGE(complete, "hcontrol is shorter than a header and one record");
  memset(&expect, 0, sizeof(expect));
  memcpy(expect.magic, test_hcontrol_magic, sizeof(expect.magic));
  expect.version = 2;
  expect.record_size = sizeof(*rec);
  expect.record_count = count;
  TEST_ASSERT_EQUAL_MEMORY(&expect, &hdr, sizeof(hdr));
}

static void read_hcontrol(struct house_control_rec *rec)
{
  read_hcontrol_n(rec, 1);
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

/* B2-24: deleting the last of MAX_GUESTS guests shifted guests[j + 1] down
 * for j = MAX_GUESTS - 1, reading past guests[] (into last_payment). */
static void test_deleting_last_guest_reads_inside_guests(void)
{
  char arg[] = "guest";
  struct house_control_rec rec = good_record(), on_disk;
  int j;

  for (j = 0; j < MAX_GUESTS; j++)
    rec.guests[j] = 6000 + j;
  rec.guests[MAX_GUESTS - 1] = 5000;
  rec.num_of_guests = MAX_GUESTS;
  rec.last_payment = 1234;
  write_hcontrol(&rec, 1);
  House_boot();
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  GET_IDNUM(&imm) = 4242;
  IN_ROOM(&imm) = 0;
  do_house(&imm, arg, 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Guest deleted"));

  read_hcontrol(&on_disk);
  TEST_ASSERT_EQUAL_INT(MAX_GUESTS - 1, on_disk.num_of_guests);
  TEST_ASSERT_EQUAL_INT(6000, on_disk.guests[0]);
  TEST_ASSERT_EQUAL_INT(0, on_disk.guests[MAX_GUESTS - 1]);
  TEST_ASSERT_EQUAL_INT(1234, on_disk.last_payment);
}

/* B2-27: asciiconvert never released the objects Obj_from_store() built
 * (one leaked live object per record) and reported a record whose
 * prototype is gone as a write error, abandoning the conversion. */
static void test_asciiconvert_releases_objects_and_skips_missing_proto(void)
{
  char cmd[] = "asciiconvert";
  struct house_control_rec rec = good_record();
  struct obj_file_elem elems[3];
  char path[64], ascii[80];
  FILE *fl;

  write_hcontrol(&rec, 1);
  House_boot();
  memset(elems, 0, sizeof(elems));
  elems[0].item_number = 300;
  elems[1].item_number = 999;	/* prototype deleted since */
  elems[2].item_number = 300;
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), LIB_HOUSE "%d.house", 100) > 0);
  TEST_ASSERT_TRUE(snprintf(ascii, sizeof(ascii), "%s.ascii", path) > 0);
  fl = fopen(path, "wb");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(3, fwrite(elems, sizeof(elems[0]), 3, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  live_objects = 0;
  GET_LEVEL(&imm) = LVL_IMPL;

  do_hcontrol(&imm, cmd, 0, 0);

  TEST_ASSERT_EQUAL_INT(0, live_objects);
  TEST_ASSERT_NOT_NULL(strstr(sent, "...2 items"));
  TEST_ASSERT_NOT_NULL(strstr(sent, "All done"));
  TEST_ASSERT_EQUAL_STRING("convert-missing-proto", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 999"));
  TEST_ASSERT_EQUAL_INT(0, remove(path));
  TEST_ASSERT_EQUAL_INT(0, remove(ascii));
}

/* House_save_control() truncated hcontrol in place; a failed write left
 * it empty and every house was lost at the next boot. It must keep the
 * live file and report. */
static void test_hcontrol_save_failure_keeps_live_file(void)
{
  char cmd[] = "build 100 north owner";
  struct house_control_rec none = good_record();
  long before;

  if (geteuid() == 0)
    TEST_IGNORE_MESSAGE("directory permissions do not bind root");
  write_hcontrol(&none, 0);
  House_boot();
  before = file_size(HCONTROL_FILE);
  TEST_ASSERT_EQUAL_INT(0, chmod("etc", 0500));

  do_hcontrol(&imm, cmd, 0, 0);

  TEST_ASSERT_EQUAL_INT(0, chmod("etc", 0700));
  TEST_ASSERT_EQUAL_INT(before, file_size(HCONTROL_FILE));
  TEST_ASSERT_EQUAL_STRING("hcontrol-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "live file kept"));
}

/* Read all of hcontrol into buf (test files are small). */
static size_t slurp_hcontrol(unsigned char *buf, size_t size)
{
  size_t n;
  FILE *fl = fopen(HCONTROL_FILE, "rb");

  TEST_ASSERT_NOT_NULL(fl);
  n = fread(buf, 1, size, fl);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  return n;
}

static void assert_same_house(const struct house_control_rec *expect,
                              const struct house_control_rec *actual)
{
  TEST_ASSERT_EQUAL_INT(expect->vnum, actual->vnum);
  TEST_ASSERT_EQUAL_INT(expect->atrium, actual->atrium);
  TEST_ASSERT_EQUAL_INT(expect->exit_num, actual->exit_num);
  TEST_ASSERT_TRUE_MESSAGE(expect->built_on == actual->built_on, "built_on");
  TEST_ASSERT_EQUAL_INT(expect->mode, actual->mode);
  TEST_ASSERT_TRUE_MESSAGE(expect->owner == actual->owner, "owner");
  TEST_ASSERT_EQUAL_INT(expect->num_of_guests, actual->num_of_guests);
  TEST_ASSERT_TRUE_MESSAGE(expect->last_payment == actual->last_payment,
                           "last_payment");
}

/* B2-23: every existing hcontrol is format 1 (no header). It must still
 * load, be reported once, and come back from the clean boot as format 2
 * holding the same record; the next boot then reads it silently. */
static void test_legacy_hcontrol_loads_and_is_rewritten_with_header(void)
{
  struct house_control_rec rec = good_record(), on_disk;

  rec.last_payment = 1234;
  write_legacy_hcontrol(&rec, 1);

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-legacy\n", tb_captured_op_log);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_TRUE(ROOM_FLAGGED(0, ROOM_HOUSE));
  TEST_ASSERT_EQUAL_INT(HCONTROL_HEADER_SIZE + (long)sizeof(rec),
                        file_size(HCONTROL_FILE));
  read_hcontrol(&on_disk);
  assert_same_house(&rec, &on_disk);

  REMOVE_BIT_AR(ROOM_FLAGS(0), ROOM_HOUSE);
  tb_error_capture_reset();
  House_boot();

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_TRUE(ROOM_FLAGGED(0, ROOM_HOUSE));
}

/* B2-23: records written with another layout (another platform, or a
 * changed struct) used to be misread field by field. The header shows the
 * mismatch: nothing loads, and the file is never replaced by the table in
 * memory, which does not hold its houses. */
static void test_hcontrol_from_other_layout_is_refused_and_kept(void)
{
  char cmd[] = "build 100 north owner", expect[128], path[64];
  unsigned char other[sizeof(struct house_control_rec) + 8];
  unsigned char before[512], after[512];
  size_t n_before;

  memset(other, 0x11, sizeof(other));
  write_hcontrol_as(2, sizeof(other), 1, other, sizeof(other));
  n_before = slurp_hcontrol(before, sizeof(before));

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-format\n", tb_captured_op_log);
  TEST_ASSERT_TRUE(snprintf(expect, sizeof(expect),
                            "format 2 with %zu-byte records, but this build "
                            "reads format 2 with %zu-byte records",
                            sizeof(other), sizeof(struct house_control_rec)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, expect));
  hcontrol_list_houses(&imm, (char *)"");
  TEST_ASSERT_NOT_NULL(strstr(sent, "No houses have been defined"));

  do_hcontrol(&imm, cmd, 0, 0);

  TEST_ASSERT_EQUAL_STRING("hcontrol-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "boot could not read this file"));
  TEST_ASSERT_EQUAL_size_t(n_before, slurp_hcontrol(after, sizeof(after)));
  TEST_ASSERT_EQUAL_MEMORY(before, after, n_before);
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), LIB_HOUSE "%d.house", 100) > 0);
  (void)remove(path);
}

/* B2-23: an unknown format version is refused the same way. */
static void test_hcontrol_unknown_version_is_refused(void)
{
  struct house_control_rec rec = good_record();

  write_hcontrol_as(3, sizeof(rec), 1, &rec, sizeof(rec));

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-format\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format 3 with"));
  TEST_ASSERT_FALSE(ROOM_FLAGGED(0, ROOM_HOUSE));
}

/* B2-23: a header cut short is damage, not a format 1 file. */
static void test_truncated_hcontrol_header_is_refused(void)
{
  char expect[64];
  FILE *fl = fopen(HCONTROL_FILE, "wb");

  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(8, fwrite(test_hcontrol_magic, 1, 8, fl));
  TEST_ASSERT_EQUAL_size_t(2, fwrite("\002", 1, 2, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-format\n", tb_captured_op_log);
  TEST_ASSERT_TRUE(snprintf(expect, sizeof(expect), "header truncated (10 of %ld bytes)",
                            HCONTROL_HEADER_SIZE) > 0);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, expect));
}

/* B2-23: the header's record count must match the file. A file cut off
 * before the last record, and bytes after it, are both reported; what was
 * read still loads, and the file is kept for repair (hcontrol-keep). */
static void test_hcontrol_record_count_mismatch_is_reported(void)
{
  struct house_control_rec rec = good_record();
  unsigned char data[sizeof(struct house_control_rec) + 5];
  long size;

  write_hcontrol_as(2, sizeof(rec), 2, &rec, sizeof(rec));
  size = file_size(HCONTROL_FILE);

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-read\nhcontrol-keep\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "file ends after 1 of the 2 records"));
  TEST_ASSERT_TRUE(ROOM_FLAGGED(0, ROOM_HOUSE));
  TEST_ASSERT_EQUAL_INT(size, file_size(HCONTROL_FILE));

  memcpy(data, &rec, sizeof(rec));
  memset(data + sizeof(rec), 'j', 5);	/* five stray bytes */
  write_hcontrol_as(2, sizeof(rec), 1, data, sizeof(data));
  tb_error_capture_reset();

  House_boot();

  TEST_ASSERT_EQUAL_STRING("hcontrol-read\nhcontrol-keep\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "data after the 1 records the header promises"));
  TEST_ASSERT_EQUAL_INT(HCONTROL_HEADER_SIZE + (long)sizeof(data),
                        file_size(HCONTROL_FILE));
}

/* B2-23: a file boot could not even open may hold every house; a later
 * hcontrol change must not replace it with the empty table in memory. */
static void test_unopenable_hcontrol_is_not_replaced(void)
{
  char cmd[] = "build 100 north owner", path[64];
  struct house_control_rec rec = good_record();
  long before;

  if (geteuid() == 0)
    TEST_IGNORE_MESSAGE("file permissions do not bind root");
  write_hcontrol(&rec, 1);
  before = file_size(HCONTROL_FILE);
  TEST_ASSERT_EQUAL_INT(0, chmod(HCONTROL_FILE, 0));

  House_boot();
  TEST_ASSERT_EQUAL_STRING("hcontrol-open\n", tb_captured_op_log);
  do_hcontrol(&imm, cmd, 0, 0);

  TEST_ASSERT_EQUAL_INT(0, chmod(HCONTROL_FILE, 0600));
  TEST_ASSERT_EQUAL_STRING("hcontrol-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "boot could not read this file"));
  TEST_ASSERT_EQUAL_INT(before, file_size(HCONTROL_FILE));
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), LIB_HOUSE "%d.house", 100) > 0);
  (void)remove(path);
}

/* House_listrent() ('hcontrol show <vnum>') built its listing in a stack
 * buffer and handed it to page_string() without keep_internal, so the
 * pager kept pointing into that buffer: a listing longer than one page is
 * shown after the call returns, from a dead frame. The pager must own a
 * copy. */
static void test_house_listing_pager_owns_its_copy(void)
{
  char show[] = "100", again[256], path[64];
  struct house_control_rec rec = good_record();
  FILE *fl;

  write_hcontrol(&rec, 1);
  House_boot();
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), LIB_HOUSE "%d.house", 100) > 0);
  fl = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fputs("$~\n", fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));

  hcontrol_list_houses(&imm, show);
  pager_page_on(again, sizeof(again));

  TEST_ASSERT_TRUE(pager_kept);
  TEST_ASSERT_NOT_NULL(strstr(again, "filename: " LIB_HOUSE "100.house"));
  (void)remove(path);
}

/* A unique (no prototype) test object; containers hold up to 50. */
static struct obj_data *new_obj(int type, int weight)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));

  TEST_ASSERT_NOT_NULL(obj);
  obj->item_number = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  GET_OBJ_TYPE(obj) = type;
  GET_OBJ_WEIGHT(obj) = weight;
  if (type == ITEM_CONTAINER)
    GET_OBJ_VAL(obj, 0) = 50;
  return obj;
}

/* Append obj, read with this Loc, to what the house file "parses" as. */
static void queue_parsed(struct obj_data *obj, int locate)
{
  obj_save_data *node = calloc(1, sizeof(*node)), **tail = &parse_result;

  TEST_ASSERT_NOT_NULL(node);
  node->obj = obj;
  node->locate = locate;
  while (*tail)
    tail = &(*tail)->next;
  *tail = node;
}

/* Boot house 100 with an object file that parses as the queued list. */
static void boot_house_with_queue(void)
{
  struct house_control_rec rec = good_record();
  char path[64];
  FILE *fl;

  write_hcontrol(&rec, 1);
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), LIB_HOUSE "%d.house", 100) > 0);
  fl = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_TRUE(fputs("$~\n", fl) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  House_boot();
  TEST_ASSERT_EQUAL_INT(0, remove(path));
}

static int floor_count(void)
{
  struct obj_data *obj;
  int n = 0;

  for (obj = rooms[0].contents; obj; obj = obj->next_content)
    n++;
  return n;
}

/* House_save() wrote every record at Loc 0, so after a reboot a house's
 * containers came back empty with their contents on the floor. Records now
 * carry their depth, contents before their container (Crash_save()'s
 * order), and a container is saved without its contents' weight. */
static void test_house_save_writes_container_depth(void)
{
  struct obj_data *bag = new_obj(ITEM_CONTAINER, 7);
  struct obj_data *gem = new_obj(ITEM_OTHER, 3), *coin = new_obj(ITEM_OTHER, 1);
  char path[64];

  obj_to_obj(gem, bag);
  obj_to_room(coin, 0);
  obj_to_room(bag, 0);	/* floor: bag, coin; bag weighs 10 */

  House_crashsave(100);

  TEST_ASSERT_EQUAL_INT(3, recorded);
  TEST_ASSERT_EQUAL_PTR(coin, recorded_obj[0]);
  TEST_ASSERT_EQUAL_INT(0, recorded_loc[0]);
  TEST_ASSERT_EQUAL_PTR(gem, recorded_obj[1]);
  TEST_ASSERT_EQUAL_INT(-1, recorded_loc[1]);
  TEST_ASSERT_EQUAL_PTR(bag, recorded_obj[2]);
  TEST_ASSERT_EQUAL_INT(0, recorded_loc[2]);
  TEST_ASSERT_EQUAL_INT(7, recorded_weight[2]);
  TEST_ASSERT_EQUAL_INT(10, GET_OBJ_WEIGHT(bag));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), LIB_HOUSE "%d.house", 100) > 0);
  TEST_ASSERT_EQUAL_INT(0, remove(path));
}

/* The records House_save() writes load back nested, in their saved order,
 * with the container's weight restored. */
static void test_house_load_rebuilds_containers(void)
{
  struct obj_data *bag = new_obj(ITEM_CONTAINER, 7);
  struct obj_data *gem = new_obj(ITEM_OTHER, 3), *coin = new_obj(ITEM_OTHER, 1);

  queue_parsed(coin, 0);
  queue_parsed(gem, -1);
  queue_parsed(bag, 0);

  boot_house_with_queue();

  TEST_ASSERT_EQUAL_PTR(gem, bag->contains);
  TEST_ASSERT_EQUAL_PTR(bag, gem->in_obj);
  TEST_ASSERT_EQUAL_INT(10, GET_OBJ_WEIGHT(bag));
  TEST_ASSERT_EQUAL_PTR(bag, rooms[0].contents);
  TEST_ASSERT_EQUAL_PTR(coin, bag->next_content);
  TEST_ASSERT_NULL(coin->next_content);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Every house file written before this change holds only Loc 0: it still
 * loads, flat, as it always did, and silently. */
static void test_house_load_flat_file_stays_flat(void)
{
  struct obj_data *bag = new_obj(ITEM_CONTAINER, 7);

  queue_parsed(new_obj(ITEM_OTHER, 1), 0);
  queue_parsed(new_obj(ITEM_OTHER, 3), 0);
  queue_parsed(bag, 0);

  boot_house_with_queue();

  TEST_ASSERT_EQUAL_INT(3, floor_count());
  TEST_ASSERT_NULL(bag->contains);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Contents whose container never arrives (its record was skipped, or the
 * file was cut short) go on the floor, reported, never into limbo. */
static void test_house_load_orphaned_contents_go_on_the_floor(void)
{
  struct obj_data *ring = new_obj(ITEM_OTHER, 1), *gem = new_obj(ITEM_OTHER, 3);

  queue_parsed(ring, -3);			/* its containers never come */
  queue_parsed(new_obj(ITEM_OTHER, 1), 0);
  queue_parsed(gem, -1);			/* the file ends here */

  boot_house_with_queue();

  TEST_ASSERT_EQUAL_INT(3, floor_count());
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(ring));
  TEST_ASSERT_EQUAL_INT(0, IN_ROOM(gem));
  TEST_ASSERT_EQUAL_STRING("house-nesting\nhouse-nesting\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "house vnum 100: 1 object(s) at depth 3 lost their container"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "house vnum 100: 1 object(s) at depth 1 lost their container"));
}

/* Contents followed by an object that is not a container, an object nested
 * deeper than HOUSE_NEST_ROWS and a wear-slot Loc are each put on the floor
 * and reported. */
static void test_house_load_odd_locs_are_reported(void)
{
  queue_parsed(new_obj(ITEM_OTHER, 3), -1);
  queue_parsed(new_obj(ITEM_OTHER, 1), 0);	/* not a container */
  queue_parsed(new_obj(ITEM_OTHER, 1), -6);	/* deeper than 5 */
  queue_parsed(new_obj(ITEM_OTHER, 1), 3);	/* a wear slot */

  boot_house_with_queue();

  TEST_ASSERT_EQUAL_INT(4, floor_count());
  TEST_ASSERT_EQUAL_STRING("house-nesting\nhouse-nesting\nhouse-nesting\n",
                           tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "precede an object that is not a container"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "nested 6 deep (max 5)"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log, "wear-slot Loc 3"));
}

/* Fuzzer PLAYER-2 (hcontrol-time): built_on and last_payment come from the
 * binary hcontrol file and can be any time_t. localtime() returns NULL for
 * one it cannot represent, and tb_strftime() then left the listing's
 * buffers unwritten, so 'hcontrol show' printed (and over-read) stack
 * bytes. They show as Unknown and are reported. */
static void test_hcontrol_show_reports_unrepresentable_times(void)
{
  struct house_control_rec rec = good_record();
  char row[160];

  if (sizeof(time_t) < 8)
    TEST_IGNORE_MESSAGE("needs a 64-bit time_t");
  rec.built_on = (time_t)100000000000000000LL;
  rec.last_payment = (time_t)100000000000000000LL;
  write_hcontrol(&rec, 1);
  House_boot();
  tb_error_capture_reset();
  dirty_stack();

  hcontrol_list_houses(&imm, (char *)"");

  TEST_ASSERT_TRUE(snprintf(row, sizeof(row), "%7d %7d  %-15s    %2d    %-12s %s\r\n",
                            100, 101, "Unknown", 0, "Owner", "Unknown") > 0);
  TEST_ASSERT_NOT_NULL(strstr(sent, row));
  TEST_ASSERT_EQUAL_STRING("hcontrol-time\nhcontrol-time\n", tb_captured_op_log);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "house vnum 100 built_on 100000000000000000 is not a valid time"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail_log,
                              "house vnum 100 last_payment 100000000000000000"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_boot_rejects_out_of_range_guest_count);
  RUN_TEST(test_boot_skip_is_reported_and_file_kept);
  RUN_TEST(test_boot_reports_partial_record);
  RUN_TEST(test_clean_boot_loads_house);
  RUN_TEST(test_built_house_record_has_no_stack_garbage);
  RUN_TEST(test_deleting_last_guest_reads_inside_guests);
  RUN_TEST(test_asciiconvert_releases_objects_and_skips_missing_proto);
  RUN_TEST(test_hcontrol_save_failure_keeps_live_file);
  RUN_TEST(test_legacy_hcontrol_loads_and_is_rewritten_with_header);
  RUN_TEST(test_hcontrol_from_other_layout_is_refused_and_kept);
  RUN_TEST(test_hcontrol_unknown_version_is_refused);
  RUN_TEST(test_truncated_hcontrol_header_is_refused);
  RUN_TEST(test_hcontrol_record_count_mismatch_is_reported);
  RUN_TEST(test_unopenable_hcontrol_is_not_replaced);
  RUN_TEST(test_house_listing_pager_owns_its_copy);
  RUN_TEST(test_house_save_writes_container_depth);
  RUN_TEST(test_house_load_rebuilds_containers);
  RUN_TEST(test_house_load_flat_file_stays_flat);
  RUN_TEST(test_house_load_orphaned_contents_go_on_the_floor);
  RUN_TEST(test_house_load_odd_locs_are_reported);
  RUN_TEST(test_hcontrol_show_reports_unrepresentable_times);
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
