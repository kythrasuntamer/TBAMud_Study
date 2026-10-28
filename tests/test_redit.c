/* Executable tests of the production room editor setup (redit.c): the edit
 * buffer is a copy of the room's data, never of its occupants or objects.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "oasis.h"

/* genolc.c is not linked: same contract as its str_udup(). */
char *str_udup(const char *txt) { return strdup((txt && *txt) ? txt : "undefined"); }

/* Trigger list copying belongs to dg_olc.c; nothing to copy here. */
void dg_olc_script_copy(struct descriptor_data *d) { (void)d; }

static struct room_data rooms[1];
static struct obj_data sword;
static struct char_data visitor;
static struct descriptor_data desc;
static struct oasis_olc_data olc;

void setUp(void)
{
  memset(rooms, 0, sizeof(rooms));
  memset(&desc, 0, sizeof(desc));
  memset(&olc, 0, sizeof(olc));
  rooms[0].name = "Source";
  rooms[0].description = "A room.\r\n";
  rooms[0].contents = &sword;
  rooms[0].people = &visitor;
  world = rooms;
  top_of_world = 0;
  desc.olc = &olc;
}
void tearDown(void)
{
  if (OLC_ROOM(&desc)) {
    free(OLC_ROOM(&desc)->name);
    free(OLC_ROOM(&desc)->description);
    free(OLC_ROOM(&desc));
  }
  world = NULL;
}

/* A copy into a new vnum saved world[i] = *buffer, so the buffer must not
 * carry the source's live lists. */
static void edit_buffer_owns_no_objects_or_people(void)
{
  redit_setup_existing(&desc, 0);
  TEST_ASSERT_NOT_NULL(OLC_ROOM(&desc));
  TEST_ASSERT_NULL(OLC_ROOM(&desc)->contents);
  TEST_ASSERT_NULL(OLC_ROOM(&desc)->people);
  TEST_ASSERT_NULL(OLC_ROOM(&desc)->events);
  TEST_ASSERT_EQUAL_STRING("Source", OLC_ROOM(&desc)->name);
  TEST_ASSERT_TRUE(OLC_ROOM(&desc)->name != rooms[0].name);   /* own copy */
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(edit_buffer_owns_no_objects_or_people);
  return UNITY_END();
}
