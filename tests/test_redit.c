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
#include "olc_checks.h"

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

/* zedit.c's olc_renumber_sessions() moves other builders' rnums (its own
 * cases, door directions and NOWHERE included, are in test_olc_editors.c);
 * here only the call is recorded. */
static int renumber_calls, renumber_kind, renumber_refpt, renumber_delta;
static struct descriptor_data *renumber_skip;
void olc_renumber_sessions(int kind, int refpt, int delta,
                           struct descriptor_data *skip)
{
  renumber_calls++;
  renumber_kind = kind;
  renumber_refpt = refpt;
  renumber_delta = delta;
  renumber_skip = skip;
}

/* Saving a new room moves every other open session's room rnums at or
 * above the inserted one up by one; the saving session itself is skipped.
 * The old code renumbered by hand, bumping a door's direction and wrapping
 * NOWHERE rooms to 0. */
static void new_room_renumbers_other_sessions(void)
{
  static struct room_data saved, rooms3[3];

  memset(rooms3, 0, sizeof(rooms3));
  world = rooms3;
  top_of_world = 2;
  memset(&saved, 0, sizeof(saved));
  saved.number = NOWHERE;                 /* a new room */
  OLC_ROOM(&desc) = NULL;
  desc.olc = &olc;
  olc.room = &saved;
  OLC_NUM(&desc) = 150;
  renumber_calls = 0;

  redit_save_internally(&desc);
  /* Detach the static fixtures first so a failed assertion cannot make
   * tearDown free them. */
  olc.room = NULL;

  TEST_ASSERT_EQUAL_INT(1, renumber_calls);
  TEST_ASSERT_EQUAL_INT(OLC_RNUM_ROOM, renumber_kind);
  TEST_ASSERT_EQUAL_INT(2, renumber_refpt);
  TEST_ASSERT_EQUAL_INT(+1, renumber_delta);
  TEST_ASSERT_EQUAL_PTR(&desc, renumber_skip);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(edit_buffer_owns_no_objects_or_people);
  RUN_TEST(new_room_renumbers_other_sessions);
  return UNITY_END();
}
