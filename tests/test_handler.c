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
#include <pthread.h>
#include "lists.h"
#include "tb_error_capture.h"

void basic_mud_log(const char *format, ...) { (void)format; }
long obj_script_id(obj_data *obj) { return obj->script_id; }
long char_script_id(char_data *ch) { return ch->script_id; }
void autoquest_trigger_check(struct char_data *ch, struct char_data *vict,
                             struct obj_data *object, int type)
{ (void)ch; (void)vict; (void)object; (void)type; }
void stop_fighting(struct char_data *ch) { (void)ch; }
int invalid_class(struct char_data *ch, struct obj_data *obj)
{ (void)ch; (void)obj; return 0; }
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
const char *skill_name(int num) { (void)num; return "test-spell"; }
void core_dump_real(const char *who, int line) { (void)who; (void)line; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *p;

  (void)file; (void)line; (void)func;
  if (!(p = calloc(count, element_size)))
    abort();
  return p;
}

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

/* H2-11: stacking an accumulating affect past the sbyte range must clamp
 * (and report), never wrap -129 into +127. */
static void test_affect_join_clamps_modifier(void)
{
  struct char_data *ch = calloc(1, sizeof(*ch));
  struct affected_type af;

  TEST_ASSERT_NOT_NULL(ch);
  ch->player_specials = &dummy_mob;
  GET_IDNUM(ch) = 7;
  memset(&af, 0, sizeof(af));
  af.spell = SPELL_CURSE;
  af.location = APPLY_NONE;
  af.duration = 10;
  af.modifier = -100;
  affect_to_char(ch, &af);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  affect_join(ch, &af, TRUE, FALSE, TRUE, FALSE);
  TEST_ASSERT_NOT_NULL(ch->affected);
  TEST_ASSERT_NULL(ch->affected->next);
  TEST_ASSERT_EQUAL_INT(SCHAR_MIN, ch->affected->modifier);
  TEST_ASSERT_EQUAL_INT(20, ch->affected->duration);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("affect-join-clamp", tb_captured_operation);

  free(ch->affected);
  free(ch);
}

/* H2-12: equipping into an occupied slot (e.g. two zone 'E' commands for
 * one slot) reports the rejection with the caller and gives the free
 * object to the character instead of orphaning it. */
static void test_equip_char_occupied_slot_falls_back_to_inventory(void)
{
  struct char_data *ch = calloc(1, sizeof(*ch));
  struct obj_data *worn = calloc(1, sizeof(*worn));
  struct obj_data *extra = calloc(1, sizeof(*extra));

  TEST_ASSERT_NOT_NULL(ch);
  TEST_ASSERT_NOT_NULL(worn);
  TEST_ASSERT_NOT_NULL(extra);
  ch->player_specials = &dummy_mob;
  SET_BIT_AR(MOB_FLAGS(ch), MOB_ISNPC);
  ch->nr = NOBODY;
  extra->item_number = NOTHING;
  IN_ROOM(extra) = NOWHERE;
  GET_EQ(ch, WEAR_HOLD) = worn;

  equip_char(ch, extra, WEAR_HOLD);

  TEST_ASSERT_EQUAL_PTR(worn, GET_EQ(ch, WEAR_HOLD));
  TEST_ASSERT_EQUAL_PTR(extra, ch->carrying);
  TEST_ASSERT_EQUAL_PTR(ch, extra->carried_by);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("equip-reject", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_handler.c"));

  free(extra);
  free(worn);
  free(ch);
}

/* H2-13: an invalid room is reported with the character and the
 * originating call site, not a raw pointer. */
static void test_char_to_room_invalid_room_names_caller(void)
{
  struct char_data *ch = calloc(1, sizeof(*ch));

  TEST_ASSERT_NOT_NULL(ch);
  ch->player_specials = &dummy_mob;
  GET_IDNUM(ch) = 99;
  IN_ROOM(ch) = NOWHERE;
  char_to_room(ch, NOWHERE);
  TEST_ASSERT_EQUAL_INT(NOWHERE, IN_ROOM(ch));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("char-to-room", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 99"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_handler.c"));
  free(ch);
}

/* H2-25: update_object() and object_list_new_owner() recursed once per
 * object in a list, so stack depth grew with the size of an inventory,
 * not with container nesting. Run both on a thread with a 256 KB stack:
 * 50,000 frames do not fit (ASan stack-overflow on the old code), a loop
 * over siblings does. */
#define HOARD_SIZE 50000
static struct obj_data *hoard;
static struct obj_data bag;          /* one container, holding the hoard */
static struct char_data hoarder;

static void *update_hoard(void *arg)
{
  (void)arg;
  update_char_objects(&hoarder);
  object_list_new_owner(hoarder.carrying, NULL);
  return NULL;
}

static void test_long_inventory_is_updated_without_deep_recursion(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int i;

  hoard = calloc(HOARD_SIZE, sizeof(*hoard));
  TEST_ASSERT_NOT_NULL(hoard);
  memset(&bag, 0, sizeof(bag));
  memset(&hoarder, 0, sizeof(hoarder));
  hoarder.player_specials = &dummy_mob;
  for (i = 0; i < HOARD_SIZE; i++) {
    hoard[i].item_number = NOTHING;
    hoard[i].in_room = NOWHERE;
    hoard[i].carried_by = &hoarder;
    GET_OBJ_TIMER(&hoard[i]) = 5;
    hoard[i].next_content = (i + 1 < HOARD_SIZE) ? &hoard[i + 1] : NULL;
  }
  /* The last object contains a bag: contents are still visited. */
  bag.item_number = NOTHING;
  bag.in_room = NOWHERE;
  bag.carried_by = &hoarder;
  GET_OBJ_TIMER(&bag) = 5;
  hoard[HOARD_SIZE - 1].contains = &bag;
  hoarder.carrying = &hoard[0];

  TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
  TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstacksize(&attr, 256 * 1024));
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, &attr, update_hoard, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));

  TEST_ASSERT_EQUAL_INT(4, GET_OBJ_TIMER(&hoard[0]));
  TEST_ASSERT_EQUAL_INT(4, GET_OBJ_TIMER(&hoard[HOARD_SIZE - 1]));
  TEST_ASSERT_EQUAL_INT(4, GET_OBJ_TIMER(&bag));
  TEST_ASSERT_NULL(hoard[0].carried_by);
  TEST_ASSERT_NULL(hoard[HOARD_SIZE - 1].carried_by);
  TEST_ASSERT_NULL(bag.carried_by);
  free(hoard);
  hoard = NULL;
}

/* H2-26: free_group() on a group that still has members drained it with
 * leave_group(), whose last call re-entered free_group(): the list and the
 * group were freed twice. A non-empty group is now refused and reported,
 * and the supported path (the last member leaves) frees it once. */
void send_to_group(struct char_data *ch, struct group_data *group,
                   const char *msg, ...)
{ (void)ch; (void)group; (void)msg; }
/* lists.c's error reporting and random_from_list(). */
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
int rand_number_at(int from, int to, const char *source_file, int source_line,
                   const char *source_func)
{ (void)to; (void)source_file; (void)source_line; (void)source_func;
  return from; }

static void test_free_group_with_members_is_refused(void)
{
  struct char_data a, b;
  struct group_data *group;

  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  a.player_specials = b.player_specials = &dummy_mob;
  GET_IDNUM(&a) = 31;
  GET_IDNUM(&b) = 32;
  /* Boot order (db.c): the first list created is global_lists, which every
   * later list is added to. */
  global_lists = create_list();
  group_list = create_list();
  group = create_group(&a);
  join_group(&b, group);

  free_group(group);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("free-nonempty", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 31"));
  TEST_ASSERT_EQUAL_PTR(group, a.group);
  TEST_ASSERT_EQUAL_PTR(group, b.group);
  TEST_ASSERT_EQUAL_INT(2, group->members->iSize);

  leave_group(&b);
  leave_group(&a);               /* last member: frees the group once */
  TEST_ASSERT_NULL(a.group);
  TEST_ASSERT_NULL(b.group);
  TEST_ASSERT_EQUAL_INT(0, group_list->iSize);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  free_list(group_list);
  group_list = NULL;
  TEST_ASSERT_EQUAL_INT(0, global_lists->iSize);
  free(global_lists);
  global_lists = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_fname_long_keyword_truncated_and_reported);
  RUN_TEST(test_fname_normal_keyword_silent);
  RUN_TEST(test_obj_from_room_null_reported);
  RUN_TEST(test_obj_from_room_nowhere_reported);
  RUN_TEST(test_obj_from_room_clears_furniture_backpointer);
  RUN_TEST(test_affect_join_clamps_modifier);
  RUN_TEST(test_equip_char_occupied_slot_falls_back_to_inventory);
  RUN_TEST(test_char_to_room_invalid_room_names_caller);
  RUN_TEST(test_long_inventory_is_updated_without_deep_recursion);
  RUN_TEST(test_free_group_with_members_is_refused);
  return UNITY_END();
}
