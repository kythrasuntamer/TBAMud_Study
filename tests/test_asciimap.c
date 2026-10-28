/* Executable tests of the production ASCII map (asciimap.c): the automap
 * beside a room description must stay inside map[][] whatever minimap size
 * the config holds, and bad sector values must not index the glyph tables.
 * asciimap.c is linked as is; the world and the text helpers are fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "asciimap.h"
#include <stdarg.h>
#include "tb_error_capture.h"

struct zone_data *zone_table;
int rev_dir[] = { SOUTH, WEST, NORTH, EAST, DOWN, UP,
                  SOUTHEAST, SOUTHWEST, NORTHWEST, NORTHEAST };

static char sent[MAX_STRING_LENGTH * 4];
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(sent, sizeof(sent), messg, args) < 0)
    sent[0] = '\0';
  va_end(args);
  return strlen(sent);
}
/* Layout helpers (utils.c): the tests inspect map state, not layout. */
char *strfrmt(char *str, int w, int h, int justify, int hpad, int vpad)
{ (void)w; (void)h; (void)justify; (void)hpad; (void)vpad; return str; }
char *strpaste(char *str1, char *str2, char *joiner)
{ (void)str1; (void)joiner; return str2; }
void basic_mud_log(const char *format, ...) { (void)format; }

#define CORRIDOR 30
static struct room_data rooms[CORRIDOR];
static struct room_direction_data exits[CORRIDOR][2];
static struct zone_data zone;
static struct char_data viewer;
static struct player_special_data viewer_specials;

/* A two-way north/south corridor; the viewer stands in the middle. */
void setUp(void)
{
  int k;

  memset(rooms, 0, sizeof(rooms));
  memset(exits, 0, sizeof(exits));
  memset(&zone, 0, sizeof(zone));
  for (k = 0; k < CORRIDOR; k++) {
    rooms[k].number = 1000 + k;
    rooms[k].zone = 0;
    rooms[k].sector_type = SECT_CITY;
    if (k > 0) {
      exits[k][0].to_room = k - 1;
      rooms[k].dir_option[NORTH] = &exits[k][0];
    }
    if (k + 1 < CORRIDOR) {
      exits[k][1].to_room = k + 1;
      rooms[k].dir_option[SOUTH] = &exits[k][1];
    }
  }
  world = rooms;
  top_of_world = CORRIDOR - 1;
  zone_table = &zone;

  memset(&viewer, 0, sizeof(viewer));
  memset(&viewer_specials, 0, sizeof(viewer_specials));
  viewer.player_specials = &viewer_specials;
  viewer_specials.saved.screen_width = 80;
  GET_IDNUM(&viewer) = 9;
  viewer.in_room = CORRIDOR / 2;
  SET_BIT_AR(PRF_FLAGS(&viewer), PRF_AUTOMAP);
  CONFIG_MAP = MAP_ON;
  sent[0] = '\0';
  tb_error_capture_reset();
}

void tearDown(void) { world = NULL; zone_table = NULL; }

static void look(void)
{
  char desc[] = "A long corridor.";
  str_and_map(desc, &viewer, IN_ROOM(&viewer));
}

/* H3-11: cedit allows minimap size 12. The automap used min/max =
 * centre -/+ 2*size even on the normal map, so the corridor walked MapArea
 * to map[-1][y] and map[51][y] (UBSan/ASan). It must stay in the grid. */
static void test_minimap_max_size_stays_in_grid(void)
{
  CONFIG_MINIMAP_SIZE = 12;
  look();
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_TRUE(strlen(sent) > 0);
}

/* H3-11: the config file accepts any int. An oversized value is clamped and
 * reported. */
static void test_minimap_oversize_is_clamped_loudly(void)
{
  CONFIG_MINIMAP_SIZE = 40;
  look();
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("minimap-size-clamp", tb_captured_operation);
}

/* H3-12: a world-file sector of -20 (or 40) was stored in map[][] and used
 * as door_info[13 + v] / map_info[v]. It must be drawn as unknown and
 * reported. */
static void test_bad_sector_drawn_as_unknown(void)
{
  CONFIG_MINIMAP_SIZE = 4;
  rooms[CORRIDOR / 2 - 1].sector_type = -20;
  rooms[CORRIDOR / 2 + 1].sector_type = 40;
  look();
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("bad-sector", tb_captured_operation);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_bad_sector_drawn_as_unknown);
  RUN_TEST(test_minimap_max_size_stays_in_grid);
  RUN_TEST(test_minimap_oversize_is_clamped_loudly);
  return UNITY_END();
}
