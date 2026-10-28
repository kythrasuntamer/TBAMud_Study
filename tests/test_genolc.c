/* Regression tests for the production GenOLC core (genolc.c): the save
 * list driver and the zone export writers.
 *
 * genolc.c is #included so its file-scope export functions can be called
 * directly; it is compiled with -ffunction-sections and linked with
 * --gc-sections, so only what these tests reach is kept. The per-type
 * savers and world lookups are faked here.
 */
#include "unity.h"
#include "genolc.c"  /* NOLINT(bugprone-suspicious-include): tests its file-scope writers */
#include <sys/stat.h>

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* ---- world fakes ------------------------------------------------------ */

static struct zone_data zones[2];
struct zone_data *zone_table = zones;
zone_rnum top_of_zone_table = 1;

zone_rnum real_zone(zone_vnum vnum)
{
  zone_rnum i;
  for (i = 0; i <= top_of_zone_table; i++)
    if (zone_table[i].number == vnum)
      return i;
  return NOWHERE;
}

/* ---- saver fakes: each counts calls and succeeds or fails on demand ---- */

static int saver_calls, saver_fails;

static int fake_save(IDXTYPE rznum, int type)
{
  saver_calls++;
  if (saver_fails)
    return FALSE;                       /* failure: entry left queued */
  remove_from_save_list(zone_table[rznum].number, type);
  return TRUE;
}
int save_mobiles(zone_rnum z) { return fake_save(z, SL_MOB); }
int save_objects(zone_rnum z) { return fake_save(z, SL_OBJ); }
int save_shops(zone_rnum z)   { return fake_save(z, SL_SHP); }
int save_rooms(zone_rnum z)   { return fake_save(z, SL_WLD); }
int save_zone(zone_rnum z)    { return fake_save(z, SL_ZON); }
int save_config(IDXTYPE z)    { return fake_save(z, SL_CFG); }
int save_quests(zone_rnum z)  { return fake_save(z, SL_QST); }

void setUp(void)
{
  memset(zones, 0, sizeof(zones));
  zones[0].number = 0;
  zones[1].number = 30;
  saver_calls = saver_fails = 0;
  tb_error_capture_reset();
}

void tearDown(void)
{
  free_save_list();
}

/* C1-20: a saver that fails returns FALSE and leaves its entry queued;
 * save_all() only advanced on < 0, so it retried the head forever (the
 * shutdown hang). Now each entry is tried once, reported and dropped. */
static void test_failing_saver_does_not_hang_save_all(void)
{
  add_to_save_list(30, SL_MOB);
  add_to_save_list(0, SL_OBJ);
  saver_fails = 1;

  TEST_ASSERT_FALSE(save_all());
  TEST_ASSERT_EQUAL_INT(2, saver_calls);
  TEST_ASSERT_NULL(save_list);
  TEST_ASSERT_EQUAL_STRING("save-all", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
}

static void test_successful_saves_empty_the_list(void)
{
  add_to_save_list(30, SL_MOB);
  add_to_save_list(30, SL_WLD);
  TEST_ASSERT_TRUE(save_all());
  TEST_ASSERT_EQUAL_INT(2, saver_calls);
  TEST_ASSERT_NULL(save_list);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* C1-20: SL_ACT/SL_HLP entries were skipped by moving the head without
 * freeing the node (a leak LeakSanitizer reports). */
/* 'show save' indexed save_types[] with the entry's type unchecked. */
static void test_show_save_list_reports_unknown_type(void)
{
  struct char_data ch;
  struct save_list_data *bad;

  memset(&ch, 0, sizeof(ch));
  CREATE(bad, struct save_list_data, 1);
  bad->zone = 30;
  bad->type = 42;
  save_list = bad;
  do_show_save_list(&ch, (char[]){""}, 0, 0);
  TEST_ASSERT_EQUAL_STRING("save-list", tb_captured_operation);

  tb_error_capture_reset();
  TEST_ASSERT_FALSE(save_all());
  TEST_ASSERT_NULL(save_list);
  TEST_ASSERT_EQUAL_STRING("save-all", tb_captured_operation);
}

static void test_unsavable_types_are_freed(void)
{
  add_to_save_list(AEDIT_PERMISSION, SL_ACT);
  add_to_save_list(HEDIT_PERMISSION, SL_HLP);
  TEST_ASSERT_TRUE(save_all());
  TEST_ASSERT_NULL(save_list);
  TEST_ASSERT_EQUAL_INT(0, saver_calls);
}

/* ---- zone export ------------------------------------------------------ */

struct shop_data *shop_index = NULL;
int top_shop = -1;

shop_rnum real_shop(shop_vnum vnum)
{
  int i;
  for (i = 0; i <= top_shop; i++)
    if (shop_index[i].vnum == vnum)
      return i;
  return NOWHERE;
}

static char *slurp(const char *path)
{
  static char buf[16384];
  FILE *fl = fopen(path, "rb");
  size_t n;

  if (!fl)
    return NULL;
  n = fread(buf, 1, sizeof(buf) - 1, fl);
  buf[n] = '\0';
  (void)fclose(fl);
  return buf;
}

/* C1-24: a shop with no keeper (NOBODY) made the exporter read
 * mob_index[NOBODY], ~2 MB past the table (ASan SEGV before the fix). */
static void test_export_shop_without_keeper(void)
{
  static obj_vnum no_products[] = { NOTHING };
  static struct shop_buy_data no_types[] = { { NOTHING, NULL } };
  static room_vnum no_rooms[] = { NOWHERE };
  struct shop_data shop;

  memset(&shop, 0, sizeof(shop));
  shop.vnum = 3010;
  shop.keeper = NOBODY;
  shop.producing = no_products;
  shop.type = no_types;
  shop.in_room = no_rooms;
  shop_index = &shop;
  top_shop = 0;
  zones[1].bot = 3000;
  zones[1].top = 3099;

  TEST_ASSERT_TRUE(export_save_shops(1));
  TEST_ASSERT_EQUAL_STRING("export-shop", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(slurp("world/export/qq.shp"), "QQ-1\n"));
  shop_index = NULL;
  top_shop = -1;
}

static struct room_data rooms[2];

room_rnum real_room(room_vnum vnum)
{
  room_rnum i;
  for (i = 0; world && i <= top_of_world; i++)
    if (world[i].number == vnum)
      return i;
  return NOWHERE;
}
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; return NOBODY; }
/* Reached only for existing mobs, of which these tests have none. */
struct char_data *mob_proto = NULL;
void check_mobile_strings(struct char_data *mob) { (void)mob; }
int write_mobile_espec(mob_vnum mvnum, struct char_data *mob, FILE *fd)
{ (void)mvnum; (void)mob; (void)fd; return TRUE; }
const char *dirs[] = { "north", "east", "south", "west", "up", "down",
                       "northwest", "northeast", "southeast", "southwest", "\n" };

/* C1-26: the info file is written before the rooms, but read a file-scope
 * exit counter only export_save_rooms() filled, so a first export said the
 * zone had no exits out. */
static void test_export_info_counts_exits_itself(void)
{
  struct room_direction_data out;
  char *info;

  memset(rooms, 0, sizeof(rooms));
  memset(&out, 0, sizeof(out));
  rooms[0].number = 1;    rooms[0].zone = 0;
  rooms[1].number = 3001; rooms[1].zone = 1;
  out.to_room = 0;
  rooms[1].dir_option[NORTH] = &out;
  world = rooms;
  top_of_world = 1;
  zones[1].bot = 3000;
  zones[1].top = 3099;
  zones[1].name = "Zone";
  zones[1].builders = "Builder";

  TEST_ASSERT_TRUE(export_info_file(1));
  info = slurp("world/export/qq.info");
  TEST_ASSERT_NOT_NULL(info);
  TEST_ASSERT_NOT_NULL(strstr(info, "Exits out of this zone have been ZZ'd"));
  TEST_ASSERT_NOT_NULL(strstr(info, "Room QQ01 : Exit to the north"));
  world = NULL;
}

/* C1-26: a writer whose data never reached the disk returned TRUE, so
 * do_export_zone archived the truncated file and reported success. */
static void test_export_write_failure_is_reported(void)
{
  zones[1].bot = 3000;
  zones[1].top = 3099;
  if (symlink("/dev/full", "world/export/qq.mob") != 0)
    TEST_IGNORE_MESSAGE("cannot link world/export/qq.mob to /dev/full");

  TEST_ASSERT_FALSE(export_save_mobiles(1));
  TEST_ASSERT_EQUAL_STRING("export-write", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(0, unlink("world/export/qq.mob"));
}

/* C1-25: a room extra description whose text is NULL (fread_string()
 * returns NULL for an empty one) was passed to strncpy (UBSan null
 * argument / SEGV before the fix). It is now skipped and reported, and a
 * description longer than the export buffer is cut, terminated and
 * reported. */
static void test_export_room_extra_descriptions(void)
{
  struct extra_descr_data empty, big;
  char *huge = malloc(MAX_STRING_LENGTH + 100);
  char *out;

  TEST_ASSERT_NOT_NULL(huge);
  memset(huge, 'x', MAX_STRING_LENGTH + 99);
  huge[MAX_STRING_LENGTH + 99] = '\0';

  memset(rooms, 0, sizeof(rooms));
  rooms[0].number = 3001;
  rooms[0].zone = 1;
  rooms[0].name = "A room";
  rooms[0].description = "Plain.\r\n";
  memset(&empty, 0, sizeof(empty));
  empty.keyword = "sign";
  empty.description = NULL;
  memset(&big, 0, sizeof(big));
  big.keyword = "wall";
  big.description = huge;
  empty.next = &big;
  rooms[0].ex_description = &empty;
  world = rooms;
  top_of_world = 0;
  zones[1].bot = 3000;
  zones[1].top = 3099;

  TEST_ASSERT_TRUE(export_save_rooms(1));
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("export-truncate", tb_captured_operation);
  out = slurp("world/export/qq.wld");
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_NULL(strstr(out, "sign~"));
  TEST_ASSERT_NOT_NULL(strstr(out, "wall~"));
  world = NULL;
  free(huge);
}

/* ---- save list teardown ----------------------------------------------- */

/* Leftovers 4: free_save_list() (comm.c, at shutdown) freed every node but
 * left save_list pointing at the first, so any later add, lookup or save
 * walked freed memory.  The head is cleared now (C1-20). */
static void test_free_save_list_clears_the_head(void)
{
  add_to_save_list(30, SL_MOB);
  add_to_save_list(0, SL_OBJ);
  free_save_list();
  TEST_ASSERT_NULL(save_list);

  TEST_ASSERT_TRUE(add_to_save_list(30, SL_WLD));
  TEST_ASSERT_TRUE(in_save_list(30, SL_WLD));
  TEST_ASSERT_FALSE(in_save_list(30, SL_MOB));
  TEST_ASSERT_NULL(save_list->next);
}

int main(void)
{
  char dir[] = "/tmp/tb_test_genolc.XXXXXX";

  if (!mkdtemp(dir) || chdir(dir) != 0 || mkdir("world", 0700) != 0 ||
      mkdir("world/export", 0700) != 0) {
    perror("test_genolc fixture directory");
    return 1;
  }
  logfile = stderr;

  UNITY_BEGIN();
  RUN_TEST(test_export_shop_without_keeper);
  RUN_TEST(test_export_room_extra_descriptions);
  RUN_TEST(test_export_info_counts_exits_itself);
  RUN_TEST(test_export_write_failure_is_reported);
  RUN_TEST(test_failing_saver_does_not_hang_save_all);
  RUN_TEST(test_successful_saves_empty_the_list);
  RUN_TEST(test_unsavable_types_are_freed);
  RUN_TEST(test_show_save_list_reports_unknown_type);
  RUN_TEST(test_free_save_list_clears_the_head);
  return UNITY_END();
}
