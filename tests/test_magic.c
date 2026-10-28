/* Regression tests for magic.c, linked against the production sources.
 * See gameplay_fixture.h for what is real and what is faked. A DG trigger
 * is played by fx_hook, and extract_obj() really frees objects, so a stale
 * pointer in the code under test is a use-after-free that ASan and
 * Valgrind report. */
#include "unity.h"
#include "gameplay_fixture.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "fight.h"
#include "spells.h"
#include "dg_scripts.h"
#include "mud_event.h"

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* db.c's read_mobile(): the summoned mob, created in no room. */
static struct char_data *last_mob;
struct char_data *read_mobile(mob_vnum nr, int type)
{
  (void)nr; (void)type;
  last_mob = fx_new_char("zombie", TRUE, NOWHERE);
  return last_mob;
}

/* lists.c: reached only through join_group(), which needs a group. */
void add_to_list(void *pContent, struct list_data *pList)
{
  (void)pContent; (void)pList;
  TEST_FAIL_MESSAGE("join_group reached without a group");
}

static struct obj_data *corpse_obj;

void setUp(void)
{
  fx_setup(2);
  tb_error_capture_reset();
  fx_rand_value = 50;   /* no spell failure roll */
  last_mob = NULL;
  corpse_obj = NULL;
}

void tearDown(void)
{
  fx_teardown();
}

static struct obj_data *make_corpse_obj(void)
{
  struct obj_data *c = fx_new_obj("corpse", ITEM_CONTAINER);
  GET_OBJ_VAL(c, 3) = 1;   /* IS_CORPSE */
  obj_to_room(c, 0);
  obj_to_obj(fx_new_obj("ring", ITEM_OTHER), c);
  return c;
}

static void free_summoned(struct char_data *mob)
{
  if (mob->master)
    stop_follower(mob);
  fx_free_char(mob);
}

/* A zombie LOAD script running 'mpurge corpse'. */
static int purge_corpse_hook(enum fx_trigger kind, struct char_data *actor,
                             struct obj_data *obj, int argn)
{
  (void)actor; (void)obj; (void)argn;
  if (kind == FX_LOAD_MTRIG && corpse_obj)
    extract_obj(corpse_obj);
  return 1;
}

/* H1-09: animate dead must not touch the corpse after the load trigger
 * freed it (it used to read obj->contains and extract it a second time). */
static void test_animate_dead_survives_load_trigger_purging_corpse(void)
{
  struct char_data *pc = fx_new_char("necro", FALSE, 0);

  corpse_obj = make_corpse_obj();
  fx_hook = purge_corpse_hook;

  mag_summons(10, pc, corpse_obj, SPELL_ANIMATE_DEAD, 0);

  TEST_ASSERT_NOT_NULL(last_mob);
  TEST_ASSERT_NULL(last_mob->carrying);
  TEST_ASSERT_NULL(world[0].contents);   /* corpse and ring both purged */
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("summon-corpse-gone", tb_captured_operation);
  free_summoned(last_mob);
  fx_free_char(pc);
}

/* A zombie LOAD script that purges the zombie itself. */
static int purge_self_hook(enum fx_trigger kind, struct char_data *actor,
                           struct obj_data *obj, int argn)
{
  (void)obj; (void)argn;
  if (kind == FX_LOAD_MTRIG)
    extract_char(actor);
  return 1;
}

/* H1-09: a zombie purged by its own load trigger does not follow, and the
 * corpse keeps its contents. */
static void test_animate_dead_with_purged_zombie(void)
{
  struct char_data *pc = fx_new_char("necro", FALSE, 0);

  corpse_obj = make_corpse_obj();
  fx_hook = purge_self_hook;

  mag_summons(10, pc, corpse_obj, SPELL_ANIMATE_DEAD, 0);

  TEST_ASSERT_NULL(last_mob->master);
  TEST_ASSERT_NULL(pc->followers);
  TEST_ASSERT_NOT_NULL(corpse_obj->contains);
  TEST_ASSERT_EQUAL_STRING("summon-mob-purged", tb_captured_operation);
  free_summoned(last_mob);
  fx_free_char(pc);
}

/* H1-09: the ordinary case moves the corpse contents to the zombie. */
static void test_animate_dead_moves_contents(void)
{
  struct char_data *pc = fx_new_char("necro", FALSE, 0);

  corpse_obj = make_corpse_obj();

  mag_summons(10, pc, corpse_obj, SPELL_ANIMATE_DEAD, 0);

  TEST_ASSERT_NOT_NULL(last_mob->carrying);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_EQUAL_PTR(pc, last_mob->master);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free_summoned(last_mob);
  fx_free_char(pc);
}

/* H1-10: the darkness expiry event carries the room vnum, not a pointer
 * into world[] that OLC can reallocate before it fires. */
static void test_darkness_event_is_keyed_by_room_vnum(void)
{
  struct char_data *pc = fx_new_char("necro", FALSE, 1);

  mag_rooms(10, pc, SPELL_DARKNESS);

  TEST_ASSERT_TRUE(ROOM_FLAGGED(1, ROOM_DARK));
  TEST_ASSERT_EQUAL_INT(eSPL_DARKNESS, fx_last_event_id);
  TEST_ASSERT_NULL(fx_last_event_struct);
  TEST_ASSERT_EQUAL_STRING("3001", fx_last_event_vars);
  fx_free_char(pc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_darkness_event_is_keyed_by_room_vnum);
  RUN_TEST(test_animate_dead_survives_load_trigger_purging_corpse);
  RUN_TEST(test_animate_dead_with_purged_zombie);
  RUN_TEST(test_animate_dead_moves_contents);
  return UNITY_END();
}
