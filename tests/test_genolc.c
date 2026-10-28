/* Regression tests for the production GenOLC core (genolc.c): the save
 * list driver and the zone export writers.
 *
 * genolc.c is #included so its file-scope export functions can be called
 * directly; it is compiled with -ffunction-sections and linked with
 * --gc-sections, so only what these tests reach is kept. The per-type
 * savers and world lookups are faked here.
 */
#include "unity.h"
#include "genolc.c"
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
  fclose(fl);
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
  RUN_TEST(test_failing_saver_does_not_hang_save_all);
  RUN_TEST(test_successful_saves_empty_the_list);
  RUN_TEST(test_unsavable_types_are_freed);
  return UNITY_END();
}
