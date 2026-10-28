/* Regression tests for the production event_countdown() (mud_event.c):
 * room-keyed events resolve their room by vnum when they fire. Only
 * mud_event.c is linked; the room table and send_to_room are local. */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "comm.h"
#include "dg_event.h"
#include "mud_event.h"
#include "tb_error_capture.h"

/* Other events in mud_event_index[], not exercised here. */
EVENTFUNC(get_protocols) { (void)event_obj; return 0; }
EVENTFUNC(event_whirlwind) { (void)event_obj; return 0; }

static struct room_data rooms[2];
static int room_messages;

/* db.c's real_room() over the two local rooms (vnums 3000, 3001). */
room_rnum real_room(room_vnum vnum)
{
  return (vnum == 3000 || vnum == 3001) ? (room_rnum) (vnum - 3000) : NOWHERE;
}

void send_to_room(room_rnum room, const char *messg, ...)
{
  (void)room; (void)messg;
  room_messages++;
}

void setUp(void)
{
  memset(rooms, 0, sizeof(rooms));
  rooms[0].number = 3000;
  rooms[1].number = 3001;
  world = rooms;
  top_of_world = 1;
  room_messages = 0;
  tb_error_capture_reset();
}

void tearDown(void)
{
  world = NULL;
  top_of_world = 0;
}

static long fire_darkness(const char *vnum_text)
{
  struct mud_event_data ev;
  char buf[32];

  memset(&ev, 0, sizeof(ev));
  ev.iId = eSPL_DARKNESS;
  if (vnum_text) {
    strlcpy(buf, vnum_text, sizeof(buf));
    ev.sVariables = buf;
  }
  return event_countdown(&ev);
}

/* H1-10: darkness expiring in an existing room clears ROOM_DARK there. */
static void test_darkness_expiry_clears_flag_by_vnum(void)
{
  SET_BIT_AR(ROOM_FLAGS(1), ROOM_DARK);
  TEST_ASSERT_EQUAL_INT(0, fire_darkness("3001"));
  TEST_ASSERT_FALSE(ROOM_FLAGGED(1, ROOM_DARK));
  TEST_ASSERT_EQUAL_INT(1, room_messages);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H1-10: a room deleted before expiry is reported, nothing is indexed. */
static void test_darkness_expiry_in_deleted_room_is_reported(void)
{
  TEST_ASSERT_EQUAL_INT(0, fire_darkness("4321"));
  TEST_ASSERT_EQUAL_INT(0, room_messages);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("room-event-stale", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 4321"));
}

/* H1-10: an event without a usable vnum is an error, not a crash. */
static void test_darkness_expiry_without_vnum_is_reported(void)
{
  TEST_ASSERT_EQUAL_INT(0, fire_darkness(NULL));
  TEST_ASSERT_EQUAL_STRING("room-event-stale", tb_captured_operation);
  tb_error_capture_reset();
  TEST_ASSERT_EQUAL_INT(0, fire_darkness("30x1"));
  TEST_ASSERT_EQUAL_STRING("room-event-stale", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(0, room_messages);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_darkness_expiry_clears_flag_by_vnum);
  RUN_TEST(test_darkness_expiry_in_deleted_room_is_reported);
  RUN_TEST(test_darkness_expiry_without_vnum_is_reported);
  return UNITY_END();
}
