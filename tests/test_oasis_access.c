/* Executable tests of can_edit_zone() and can_use_editor() (oasis.c): which
 * builder may edit which zone, and who may use the help and socials
 * editors. oasis.c is linked with unreferenced sections discarded (see
 * tests/CMakeLists.txt); the zone table is heap-allocated at its exact size
 * so ASan reports any index past it. tb_error_capture.h supplies
 * tb_error_log_at().
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "oasis.h"
#include "tb_error_capture.h"

/* db.c and handler.c collaborators (not linked). */
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
void basic_mud_log(const char *format, ...) { (void)format; }
int is_name(const char *str, const char *namelist)
{ return namelist && !strcmp(str, namelist); }
zone_rnum real_zone(zone_vnum vnum)
{
  zone_rnum i;

  for (i = 0; zone_table && i <= top_of_zone_table; i++)
    if (zone_table[i].number == vnum)
      return i;
  return NOWHERE;
}

/* A world of 'count' zones, vnum 1000 + rnum, all built by "Other". */
static void make_zones(int count)
{
  int i;

  zone_table = calloc((size_t)count, sizeof(*zone_table));
  TEST_ASSERT_NOT_NULL(zone_table);
  top_of_zone_table = (zone_rnum)(count - 1);
  for (i = 0; i < count; i++) {
    zone_table[i].number = (zone_vnum)(1000 + i);
    zone_table[i].builders = (char *)"Other";
  }
}

static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;

/* A connected player builder "Builder" (idnum 42) with 'set olc' olc_zone. */
static void make_builder(int level, int olc_zone)
{
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  memset(&builder_desc, 0, sizeof(builder_desc));
  builder.player_specials = &builder_specials;
  builder.player.name = (char *)"Builder";
  GET_LEVEL(&builder) = level;
  GET_IDNUM(&builder) = 42;
  GET_OLC_ZONE(&builder) = olc_zone;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
}

void setUp(void) { tb_error_capture_reset(); }
void tearDown(void)
{
  free(zone_table);
  zone_table = NULL;
  top_of_zone_table = 0;
}

/* F3-20: can_edit_zone indexed zone_table[rnum] with no bound, so an rnum
 * past the table (a vnum passed by mistake) read past it (ASan heap
 * overflow here before the fix). It is now denied and reported. */
static void edit_zone_rnum_past_table_is_reported(void)
{
  make_zones(3);
  make_builder(LVL_BUILDER, 1001);
  TEST_ASSERT_FALSE(can_edit_zone(&builder, 7));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-index", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "rnum 7 > top_of_zone_table 2"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));

  /* The builder's own zone, in range: allowed, nothing reported. */
  tb_error_capture_reset();
  TEST_ASSERT_TRUE(can_edit_zone(&builder, 1));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* F3-20: with more than 888 zones, rnum 888 is a real zone but was taken
 * as the HEDIT_PERMISSION sentinel: it skipped the NOBUILD and builder
 * checks, and a builder with help-editor rights could edit it. */
static void edit_zone_rnum_equal_to_sentinel_is_a_real_zone(void)
{
  make_zones(HEDIT_PERMISSION + 1);
  SET_BIT_AR(zone_table[HEDIT_PERMISSION].zone_flags, ZONE_NOBUILD);
  make_builder(LVL_BUILDER, HEDIT_PERMISSION);
  TEST_ASSERT_FALSE(can_edit_zone(&builder, HEDIT_PERMISSION));

  REMOVE_BIT_AR(zone_table[HEDIT_PERMISSION].zone_flags, ZONE_NOBUILD);
  TEST_ASSERT_FALSE(can_edit_zone(&builder, HEDIT_PERMISSION));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  /* The help editor's own check no longer shares the zone_rnum space. */
  TEST_ASSERT_TRUE(can_use_editor(&builder, HEDIT_PERMISSION));
}

/* The socials editor still passes AEDIT_PERMISSION to can_edit_zone();
 * with fewer zones than the sentinel it means the editor, as before. */
static void editor_permissions_follow_olc_assignment(void)
{
  make_zones(3);
  make_builder(LVL_BUILDER, AEDIT_PERMISSION);
  TEST_ASSERT_TRUE(can_edit_zone(&builder, AEDIT_PERMISSION));
  TEST_ASSERT_FALSE(can_edit_zone(&builder, HEDIT_PERMISSION));
  TEST_ASSERT_TRUE(can_use_editor(&builder, AEDIT_PERMISSION));
  TEST_ASSERT_FALSE(can_use_editor(&builder, HEDIT_PERMISSION));
  make_builder(LVL_BUILDER, ALL_PERMISSION);
  TEST_ASSERT_TRUE(can_use_editor(&builder, HEDIT_PERMISSION));
  make_builder(LVL_GRGOD, NOWHERE);
  TEST_ASSERT_TRUE(can_use_editor(&builder, HEDIT_PERMISSION));
  builder.desc = NULL;   /* linkless or forced */
  TEST_ASSERT_FALSE(can_use_editor(&builder, HEDIT_PERMISSION));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  /* Anything but the two editor permissions is a caller bug. */
  builder.desc = &builder_desc;
  TEST_ASSERT_FALSE(can_use_editor(&builder, 5));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("editor-permission", tb_captured_operation);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(edit_zone_rnum_past_table_is_reported);
  RUN_TEST(edit_zone_rnum_equal_to_sentinel_is_a_real_zone);
  RUN_TEST(editor_permissions_follow_olc_assignment);
  return UNITY_END();
}
