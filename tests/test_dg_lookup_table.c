/* Executable tests of the production DG UID lookup table (dg_scripts.c):
 * ownership-checked removal across UID rebinding and in-range ID allocation.
 * Entities are zeroed fixtures; only their addresses and script_id are used.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_scripts.h"
#include "tb_error_capture.h"

/* ID counters live in db.c, which this fixture does not link. */
long max_mob_id = MOB_ID_BASE;
long max_obj_id = OBJ_ID_BASE;

static int mudlogs;
void mudlog(int type, int level, int file, const char *str, ...)
{
  (void)type; (void)level; (void)file; (void)str;
  mudlogs++;
}
void basic_mud_log(const char *format, ...) { (void)format; }

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}

static char_data old_ch, new_ch, mob_a, mob_b;
static obj_data obj_a, obj_b;

void setUp(void)
{
  init_lookup_table();
  memset(&old_ch, 0, sizeof(old_ch));
  memset(&new_ch, 0, sizeof(new_ch));
  memset(&mob_a, 0, sizeof(mob_a));
  memset(&mob_b, 0, sizeof(mob_b));
  memset(&obj_a, 0, sizeof(obj_a));
  memset(&obj_b, 0, sizeof(obj_b));
  max_mob_id = MOB_ID_BASE;
  max_obj_id = OBJ_ID_BASE;
  mudlogs = 0;
  tb_error_capture_reset();
}
void tearDown(void)
{
  /* Remove whatever each fixture still owns; mismatches are no-ops. */
  char_data *chars[] = { &old_ch, &new_ch, &mob_a, &mob_b };
  obj_data *objs[] = { &obj_a, &obj_b };
  for (size_t i = 0; i < sizeof(chars) / sizeof(*chars); i++)
    if (chars[i]->script_id)
      remove_from_lookup_table(chars[i]->script_id, chars[i]);
  for (size_t i = 0; i < sizeof(objs) / sizeof(*objs); i++)
    if (objs[i]->script_id)
      remove_from_lookup_table(objs[i]->script_id, objs[i]);
}

/* A player re-logs while the old character awaits extraction: freeing the
 * old character must not unregister the new one. */
static void stale_holder_removal_keeps_rebound_uid(void)
{
  old_ch.script_id = new_ch.script_id = 42;
  add_to_lookup_table(42, &old_ch);
  add_to_lookup_table(42, &new_ch);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("uid-rebind", tb_captured_operation);
  remove_from_lookup_table(42, &old_ch);
  TEST_ASSERT_EQUAL_PTR(&new_ch, find_char(42));
  /* The refused removal is reported with both identities. */
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("uid-remove", tb_captured_operation);
  old_ch.script_id = 0;
}
static void owner_removal_unregisters_uid(void)
{
  new_ch.script_id = 42;
  add_to_lookup_table(42, &new_ch);
  remove_from_lookup_table(42, &new_ch);
  TEST_ASSERT_NULL(find_char(42));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  new_ch.script_id = 0;
}
static void mob_ids_skip_registered_uid(void)
{
  /* Simulate a stale counter pointing at an ID that is still held. */
  mob_a.script_id = MOB_ID_BASE;
  add_to_lookup_table(MOB_ID_BASE, &mob_a);
  TEST_ASSERT_EQUAL_INT64(MOB_ID_BASE + 1, char_script_id(&mob_b));
  TEST_ASSERT_EQUAL_PTR(&mob_a, find_char(MOB_ID_BASE));
  TEST_ASSERT_EQUAL_PTR(&mob_b, find_char(MOB_ID_BASE + 1));
}
static void mob_ids_wrap_within_mobile_range(void)
{
  max_mob_id = ROOM_ID_BASE;
  long id = char_script_id(&mob_a);
  TEST_ASSERT_TRUE(id >= MOB_ID_BASE && id < ROOM_ID_BASE);
  TEST_ASSERT_EQUAL_PTR(&mob_a, find_char(id));
  TEST_ASSERT_EQUAL_INT(1, mudlogs);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("uid-allocate", tb_captured_operation);
}
static void obj_ids_skip_registered_uid(void)
{
  obj_a.script_id = OBJ_ID_BASE;
  add_to_lookup_table(OBJ_ID_BASE, &obj_a);
  TEST_ASSERT_EQUAL_INT64(OBJ_ID_BASE + 1, obj_script_id(&obj_b));
  TEST_ASSERT_TRUE(obj_uid_matches(OBJ_ID_BASE, &obj_a));
  TEST_ASSERT_TRUE(obj_uid_matches(OBJ_ID_BASE + 1, &obj_b));
}
static void obj_ids_wrap_within_object_range(void)
{
  max_obj_id = LONG_MAX;
  long id = obj_script_id(&obj_a);
  TEST_ASSERT_EQUAL_INT64(OBJ_ID_BASE, id);
  TEST_ASSERT_TRUE(obj_uid_matches(id, &obj_a));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("uid-allocate", tb_captured_operation);
}
static void removing_unregistered_uid_is_reported(void)
{
  remove_from_lookup_table(4242, &new_ch);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("uid-remove", tb_captured_operation);
}
static void object_existence_requires_object_range(void)
{
  new_ch.script_id = 42;
  add_to_lookup_table(42, &new_ch);
  TEST_ASSERT_FALSE(has_obj_by_uid_in_lookup_table(42));
}
/* DGDOC-9: the object UID 1000000000 equals the bucket heads' sentinel; it
 * was stored in the head, where no lookup looks, so the object could never
 * be found or removed. */
static void object_uid_equal_to_the_sentinel_is_found(void)
{
  obj_a.script_id = 1000000000L;
  add_to_lookup_table(obj_a.script_id, &obj_a);
  TEST_ASSERT_TRUE(obj_uid_matches(obj_a.script_id, &obj_a));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  remove_from_lookup_table(obj_a.script_id, &obj_a);
  TEST_ASSERT_FALSE(obj_uid_matches(obj_a.script_id, &obj_a));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  obj_a.script_id = 0;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(stale_holder_removal_keeps_rebound_uid);
  RUN_TEST(owner_removal_unregisters_uid);
  RUN_TEST(mob_ids_skip_registered_uid);
  RUN_TEST(mob_ids_wrap_within_mobile_range);
  RUN_TEST(obj_ids_skip_registered_uid);
  RUN_TEST(obj_ids_wrap_within_object_range);
  RUN_TEST(removing_unregistered_uid_is_reported);
  RUN_TEST(object_existence_requires_object_range);
  RUN_TEST(object_uid_equal_to_the_sentinel_is_found);
  return UNITY_END();
}
