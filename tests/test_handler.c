/* Executable tests of production handler.c primitives (fname, furniture,
 * affect_join, obj_from_room). Linked against src/handler.c with unrelated
 * sections discarded; only the externals these paths reach are supplied by
 * test_stubs.c or below. Fixtures are disposable heap/static objects.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "spells.h"
#include "dg_scripts.h"
#include <stdlib.h>
#include <string.h>
#include "tb_error_capture.h"

void basic_mud_log(const char *format, ...) { (void)format; }
long obj_script_id(obj_data *obj) { return obj->script_id; }
long char_script_id(char_data *ch) { return ch->script_id; }

void setUp(void) { tb_error_capture_reset(); }
void tearDown(void) {}

/* H2-04: a first keyword longer than fname()'s static buffer must be
 * truncated (not written past the buffer) and reported. */
static void test_fname_long_keyword_truncated_and_reported(void)
{
  size_t n = READ_SIZE + 44;
  char *kw = malloc(n + 1);
  char *out;

  TEST_ASSERT_NOT_NULL(kw);
  memset(kw, 'a', n);
  kw[n] = '\0';
  out = fname(kw);
  TEST_ASSERT_EQUAL_size_t(READ_SIZE - 1, strlen(out));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("fname-truncate", tb_captured_operation);
  free(kw);
}

static void test_fname_normal_keyword_silent(void)
{
  char *kw = strdup("sword long");

  TEST_ASSERT_NOT_NULL(kw);
  TEST_ASSERT_EQUAL_STRING("sword", fname(kw));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(kw);
}

/* H2-14: obj_from_room's guard must not dereference a NULL object while
 * reporting it, and must name the object when it is not in a room. */
static void test_obj_from_room_null_reported(void)
{
  obj_from_room(NULL);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("obj-from-room", tb_captured_operation);
}

static void test_obj_from_room_nowhere_reported(void)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));

  TEST_ASSERT_NOT_NULL(obj);
  obj->item_number = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  obj_from_room(obj);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("obj-from-room", tb_captured_operation);
  free(obj);
}

/* H2-06: moving occupied furniture out of a room must clear the furniture's
 * back-pointer to its sitters, not only the sitters' forward pointers;
 * otherwise a later extract_obj walks a freed character. */
static void test_obj_from_room_clears_furniture_backpointer(void)
{
  struct room_data rooms[1];
  struct obj_data *chair = calloc(1, sizeof(*chair));
  struct char_data *sitter = calloc(1, sizeof(*sitter));
  struct room_data *saved_world = world;
  room_rnum saved_top = top_of_world;

  TEST_ASSERT_NOT_NULL(chair);
  TEST_ASSERT_NOT_NULL(sitter);
  memset(rooms, 0, sizeof(rooms));
  rooms[0].number = 3001;
  world = rooms;
  top_of_world = 0;

  chair->item_number = NOTHING;
  GET_OBJ_TYPE(chair) = ITEM_FURNITURE;
  IN_ROOM(chair) = NOWHERE;
  obj_to_room(chair, 0);
  GET_IDNUM(sitter) = 42;
  SITTING(sitter) = chair;
  OBJ_SAT_IN_BY(chair) = sitter;
  GET_OBJ_VAL(chair, 1) = 1;

  obj_from_room(chair);

  TEST_ASSERT_NULL(SITTING(sitter));
  TEST_ASSERT_NULL(OBJ_SAT_IN_BY(chair));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(chair, 1));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("furniture-move-occupied", tb_captured_operation);
  TEST_ASSERT_NULL(rooms[0].contents);

  world = saved_world;
  top_of_world = saved_top;
  free(sitter);
  free(chair);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_fname_long_keyword_truncated_and_reported);
  RUN_TEST(test_fname_normal_keyword_silent);
  RUN_TEST(test_obj_from_room_null_reported);
  RUN_TEST(test_obj_from_room_nowhere_reported);
  RUN_TEST(test_obj_from_room_clears_furniture_backpointer);
  return UNITY_END();
}
