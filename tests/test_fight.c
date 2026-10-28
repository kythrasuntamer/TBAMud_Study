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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_hit_reads_weapon_after_fight_trigger);
  RUN_TEST(test_hit_skips_victim_purged_by_trigger);
  return UNITY_END();
}
