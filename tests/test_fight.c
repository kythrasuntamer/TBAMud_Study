/* Regression tests for fight.c, linked against the production sources.
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

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

static struct char_data *hook_target;

void setUp(void)
{
  fx_setup(2);
  tb_error_capture_reset();
  hook_target = NULL;
}

void tearDown(void)
{
  fx_teardown();
}

static struct obj_data *wield(struct char_data *ch, const char *name)
{
  struct obj_data *w = fx_new_obj(name, ITEM_WEAPON);
  SET_BIT_AR(GET_OBJ_WEAR(w), ITEM_WEAR_WIELD);
  GET_OBJ_VAL(w, 1) = 1;
  GET_OBJ_VAL(w, 2) = 4;
  GET_OBJ_VAL(w, 3) = 0;
  equip_char(ch, w, WEAR_WIELD);
  return w;
}

/* A FIGHT script running 'mjunk dagger': the wielded weapon is unequipped
 * and extracted before hit() looks at it. */
static int junk_weapon_hook(enum fx_trigger kind, struct char_data *actor,
                            struct obj_data *obj, int argn)
{
  (void)obj; (void)argn;
  if (kind == FX_FIGHT_MTRIG && GET_EQ(actor, WEAR_WIELD))
    extract_obj(unequip_char(actor, WEAR_WIELD));
  return 1;
}

/* H1-04: hit() must read the weapon after the fight trigger. */
static void test_hit_reads_weapon_after_fight_trigger(void)
{
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct char_data *pc = fx_new_char("hero", FALSE, 0);

  wield(mob, "dagger");
  fx_hook = junk_weapon_hook;

  hit(mob, pc, TYPE_UNDEFINED);

  TEST_ASSERT_EQUAL_INT(1, fx_trigger_calls[FX_FIGHT_MTRIG]);
  TEST_ASSERT_NULL(GET_EQ(mob, WEAR_WIELD));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  if (FIGHTING(mob)) stop_fighting(mob);
  if (FIGHTING(pc)) stop_fighting(pc);
  fx_free_char(pc);
  fx_free_char(mob);
}

/* A FIGHT script that purges the victim. */
static int purge_victim_hook(enum fx_trigger kind, struct char_data *actor,
                             struct obj_data *obj, int argn)
{
  (void)actor; (void)obj; (void)argn;
  if (kind == FX_FIGHT_MTRIG && hook_target)
    extract_char(hook_target);
  return 1;
}

/* H1-04: a victim purged by the fight trigger is not hit, and it is said. */
static void test_hit_skips_victim_purged_by_trigger(void)
{
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct char_data *victim = fx_new_char("rat", TRUE, 0);

  hook_target = victim;
  fx_hook = purge_victim_hook;

  hit(mob, victim, TYPE_UNDEFINED);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("hit-after-trigger", tb_captured_operation);
  TEST_ASSERT_NULL(FIGHTING(mob));
  fx_free_char(victim);
  fx_free_char(mob);
}

/* A REMOVE script on worn armour running 'oload obj N %actor%' while its
 * wearer dies: a new object lands in the dying character's inventory. */
static int oload_to_actor_hook(enum fx_trigger kind, struct char_data *actor,
                               struct obj_data *obj, int argn)
{
  (void)obj; (void)argn;
  if (kind == FX_REMOVE_OTRIG)
    obj_to_char(fx_new_obj("pastry", ITEM_FOOD), actor);
  return 1;
}

/* H1-05: the object a remove trigger hands to the dying character must end
 * up in the corpse, not orphaned with carried_by pointing at the freed ch. */
static void test_corpse_collects_objects_given_by_remove_trigger(void)
{
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct obj_data *plate = fx_new_obj("plate", ITEM_ARMOR);
  struct obj_data *bread = fx_new_obj("bread", ITEM_FOOD);
  struct obj_data *corpse, *o;
  int n = 0, pastry = 0;

  SET_BIT_AR(GET_OBJ_WEAR(plate), ITEM_WEAR_BODY);
  equip_char(mob, plate, WEAR_BODY);
  obj_to_char(bread, mob);
  fx_hook = oload_to_actor_hook;

  raw_kill(mob, NULL);

  TEST_ASSERT_NULL(mob->carrying);
  TEST_ASSERT_NULL(GET_EQ(mob, WEAR_BODY));
  TEST_ASSERT_EQUAL_INT(0, IS_CARRYING_N(mob));
  corpse = world[0].contents;
  TEST_ASSERT_NOT_NULL(corpse);
  for (o = corpse->contains; o; o = o->next_content) {
    TEST_ASSERT_EQUAL_PTR(corpse, o->in_obj);
    TEST_ASSERT_NULL(o->carried_by);
    if (!strcmp(o->name, "pastry"))
      pastry++;
    n++;
  }
  TEST_ASSERT_EQUAL_INT(3, n);          /* bread, plate, pastry */
  TEST_ASSERT_EQUAL_INT(1, pastry);
  TEST_ASSERT_EQUAL_STRING("corpse-inventory-added", tb_captured_operation);
  fx_free_char(mob);   /* frees mob: nothing may still point at it */
}

/* A REMOVE script that purges the item itself. */
static int purge_self_hook(enum fx_trigger kind, struct char_data *actor,
                           struct obj_data *obj, int argn)
{
  (void)argn;
  if (kind == FX_REMOVE_OTRIG && obj && obj->worn_by == actor)
    extract_obj(obj);
  return 1;
}

/* H1-05: the equipment slot is re-read after the remove trigger. */
static void test_corpse_skips_slot_emptied_by_remove_trigger(void)
{
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct obj_data *plate = fx_new_obj("plate", ITEM_ARMOR);

  SET_BIT_AR(GET_OBJ_WEAR(plate), ITEM_WEAR_BODY);
  equip_char(mob, plate, WEAR_BODY);
  fx_hook = purge_self_hook;

  raw_kill(mob, NULL);

  TEST_ASSERT_EQUAL_INT(0, fx_core_dumps);
  TEST_ASSERT_EQUAL_STRING("corpse-eq-changed", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(world[0].contents);
  TEST_ASSERT_NULL(world[0].contents->contains);
  fx_free_char(mob);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_hit_reads_weapon_after_fight_trigger);
  RUN_TEST(test_hit_skips_victim_purged_by_trigger);
  RUN_TEST(test_corpse_collects_objects_given_by_remove_trigger);
  RUN_TEST(test_corpse_skips_slot_emptied_by_remove_trigger);
  return UNITY_END();
}
