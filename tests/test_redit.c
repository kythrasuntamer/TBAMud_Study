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

/* redit_save_internally() fixtures: the new room lands at rnum 2. */
room_rnum add_room(struct room_data *room) { (void)room; return 2; }
void free_proto_script(void *thing, int type) { (void)thing; (void)type; }
void assign_triggers(void *i, int type) { (void)i; (void)type; }
size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{ (void)d; (void)txt; return 0; }

/* Other builders' buffers follow the shifted room rnums -- room arguments
 * only, never NOWHERE.  The old code bumped a door's direction and
 * wrapped NOWHERE rooms to 0. */
static void new_room_renumbers_only_room_arguments(void)
{
  static struct room_data saved, rooms3[3];
  static struct descriptor_data zdesc, rdesc;
  static struct oasis_olc_data zolc, rolc;
  static struct zone_data zone;
  static struct reset_com cmds[3];
  static struct room_data other;
  static struct room_direction_data exit_nowhere, exit_up;

  memset(rooms3, 0, sizeof(rooms3));
  world = rooms3;
  top_of_world = 2;
  memset(&saved, 0, sizeof(saved));
  saved.number = NOWHERE;                 /* a new room */
  OLC_ROOM(&desc) = NULL;
  desc.olc = &olc;
  olc.room = &saved;
  OLC_NUM(&desc) = 150;

  memset(cmds, 0, sizeof(cmds));
  cmds[0].command = 'D'; cmds[0].arg1 = 3; cmds[0].arg2 = 2;
  cmds[1].command = 'O'; cmds[1].arg3 = NOWHERE;
  cmds[2].command = 'S';
  memset(&zone, 0, sizeof(zone));
  zone.cmd = cmds;
  memset(&zdesc, 0, sizeof(zdesc));
  memset(&zolc, 0, sizeof(zolc));
  zdesc.olc = &zolc;
  zolc.zone = &zone;
  STATE(&zdesc) = CON_ZEDIT;

  memset(&other, 0, sizeof(other));
  exit_nowhere.to_room = NOWHERE;
  exit_up.to_room = 2;
  other.dir_option[0] = &exit_nowhere;
  other.dir_option[1] = &exit_up;
  memset(&rdesc, 0, sizeof(rdesc));
  memset(&rolc, 0, sizeof(rolc));
  rdesc.olc = &rolc;
  rolc.room = &other;
  STATE(&rdesc) = CON_REDIT;

  zdesc.next = &rdesc;
  descriptor_list = &zdesc;

  redit_save_internally(&desc);
  /* Detach the static fixtures first so a failed assertion cannot make
   * tearDown free them. */
  descriptor_list = NULL;
  olc.room = NULL;

  TEST_ASSERT_EQUAL_INT(4, cmds[0].arg1);           /* room moved up */
  TEST_ASSERT_EQUAL_INT(2, cmds[0].arg2);           /* direction kept */
  TEST_ASSERT_EQUAL_INT(NOWHERE, cmds[1].arg3);
  TEST_ASSERT_EQUAL_INT(NOWHERE, exit_nowhere.to_room);
  TEST_ASSERT_EQUAL_INT(3, exit_up.to_room);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(edit_buffer_owns_no_objects_or_people);
  RUN_TEST(new_room_renumbers_only_room_arguments);
  return UNITY_END();
}
