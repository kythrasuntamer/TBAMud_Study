/* Executable tests of the production list and mud-event code (lists.c,
 * mud_event.c, dg_event.c): room events across world[] reallocation, owner
 * cancel loops, and list teardown. The world and the outside callbacks are
 * fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "dg_event.h"
#include "mud_event.h"
#include <stdarg.h>
#include "tb_error_capture.h"

unsigned long pulse;

/* Event functions owned by other files; these tests use room events. */
EVENTFUNC(get_protocols) { (void)event_obj; return 0; }
EVENTFUNC(event_whirlwind) { (void)event_obj; return 0; }

static int room_messages;
void send_to_room(room_rnum room, const char *messg, ...)
{ (void)room; (void)messg; room_messages++; }
void basic_mud_log(const char *format, ...) { (void)format; }
static int mudlogs;
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; mudlogs++; }
int rand_number(int from, int to) { (void)to; return from; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  (void)source_file; (void)source_line; (void)source_func;
  return calloc(count ? count : 1, element_size ? element_size : 1);
}
room_rnum real_room(room_vnum vnum)
{
  room_rnum r;
  if (!world)
    return NOWHERE;
  for (r = 0; r <= top_of_world; r++)
    if (world[r].number == vnum)
      return r;
  return NOWHERE;
}

static void make_world(int rooms)
{
  int k;
  world = calloc((size_t)rooms, sizeof(struct room_data));
  TEST_ASSERT_NOT_NULL(world);
  top_of_world = (room_rnum)(rooms - 1);
  for (k = 0; k < rooms; k++)
    world[k].number = (room_vnum)(100 + k);
}

/* What redit's add_room() does to world[]: a new block, rooms shifted up
 * from the insertion point, the old block freed. */
static void add_room_at(room_rnum at, room_vnum vnum)
{
  struct room_data *moved = calloc((size_t)top_of_world + 2, sizeof(*moved));
  room_rnum r;

  TEST_ASSERT_NOT_NULL(moved);
  for (r = 0; r < at; r++)
    moved[r] = world[r];
  moved[at].number = vnum;
  for (r = at; r <= top_of_world; r++)
    moved[r + 1] = world[r];
  free(world);
  world = moved;
  top_of_world++;
}

/* The event queue is bucketed by pulse: advance one pulse at a time, as
 * heartbeat() does. */
static void run_pulses(unsigned long n)
{
  while (n--) {
    pulse++;
    event_process();
  }
}

void setUp(void)
{
  pulse = 0;
  room_messages = mudlogs = 0;
  event_init();
  init_events();
  tb_error_capture_reset();
}

void tearDown(void)
{
  room_rnum r;
  event_free_all();
  if (world)
    for (r = 0; r <= top_of_world; r++)
      if (world[r].events) {
        free_list(world[r].events);
        world[r].events = NULL;
      }
  free(world);
  world = NULL;
  free_list(world_events);
  world_events = NULL;
}

/* H3-13: darkness in room vnum 101, then a room is inserted below it and
 * world[] moves. When the event fires it must find vnum 101 at its new rnum
 * (not read the freed block) and clear ROOM_DARK there. */
static void test_room_event_survives_world_realloc(void)
{
  make_world(3);
  SET_BIT_AR(ROOM_FLAGS(1), ROOM_DARK);
  NEW_EVENT(eSPL_DARKNESS, &world[1], NULL, 5);
  TEST_ASSERT_NOT_NULL(world[1].events);

  add_room_at(0, 99);                 /* vnum 101 is now rnum 2 */
  run_pulses(10);

  TEST_ASSERT_EQUAL_INT(101, world[2].number);
  TEST_ASSERT_FALSE(ROOM_FLAGGED(2, ROOM_DARK));
  TEST_ASSERT_EQUAL_INT(1, room_messages);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H3-13: the room was deleted before the event fired: reported, and no
 * flag is written anywhere. */
static void test_room_event_for_deleted_room_reports(void)
{
  make_world(3);
  NEW_EVENT(eSPL_DARKNESS, &world[1], NULL, 5);
  free_list(world[1].events);         /* the room's list went with it */
  world[1].events = NULL;
  world[1].number = 555;              /* vnum 101 no longer exists */
  run_pulses(10);

  TEST_ASSERT_EQUAL_INT(0, room_messages);
  TEST_ASSERT_TRUE(tb_captured_errors >= 1);
  TEST_ASSERT_EQUAL_STRING("room-gone", tb_captured_operation);
}

int main(void)
{
  int rc;

  /* boot_db() order: the first list created is the registry of lists. */
  global_lists = create_list();
  UNITY_BEGIN();
  RUN_TEST(test_room_event_survives_world_realloc);
  RUN_TEST(test_room_event_for_deleted_room_reports);
  rc = UNITY_END();
  free_list(global_lists);
  return rc;
}
