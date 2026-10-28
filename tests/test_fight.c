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
#include "act.h"

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

/* H1-08: weapon value 3 past attack_hit_text[] ('osetval 3 40') falls back
 * to a plain hit and names the weapon and wielder. */
static void test_hit_rejects_weapon_attack_type_out_of_range(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct obj_data *dagger = wield(pc, "dagger");

  GET_OBJ_VAL(dagger, 3) = 40;
  GET_POS(mob) = POS_SLEEPING;   /* a sleeping victim is always hit */

  hit(pc, mob, TYPE_UNDEFINED);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("attack-type-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "val3 40"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_TRUE(GET_HIT(mob) < 100);
  if (FIGHTING(mob)) stop_fighting(mob);
  if (FIGHTING(pc)) stop_fighting(pc);
  fx_free_char(mob);
  fx_free_char(pc);
}

/* H1-08: a mob attack type out of range is reported too. */
static void test_hit_rejects_mob_attack_type_out_of_range(void)
{
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct char_data *rat = fx_new_char("rat", TRUE, 0);

  mob->mob_specials.attack_type = -5;
  GET_POS(rat) = POS_SLEEPING;

  hit(mob, rat, TYPE_UNDEFINED);

  TEST_ASSERT_EQUAL_STRING("attack-type-range", tb_captured_operation);
  if (FIGHTING(mob)) stop_fighting(mob);
  if (FIGHTING(rat)) stop_fighting(rat);
  fx_free_char(rat);
  fx_free_char(mob);
}

/* H1-15: with an older corpse already lying in the room, autoloot and
 * autosac must act on the corpse of the mob just killed. obj_to_room()
 * appends, so a lookup of "corpse" found the old one first. */
static void test_autoloot_takes_the_new_corpse(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *mob = fx_new_char("rat", TRUE, 0);
  struct obj_data *old = fx_new_obj("corpse", ITEM_CONTAINER);
  struct obj_data *treasure = fx_new_obj("treasure", ITEM_OTHER);
  struct obj_data *bread = fx_new_obj("bread", ITEM_FOOD);

  GET_OBJ_VAL(old, 3) = 1;
  obj_to_room(old, 0);
  obj_to_obj(treasure, old);
  obj_to_char(bread, mob);
  SET_BIT_AR(PRF_FLAGS(pc), PRF_AUTOLOOT);
  SET_BIT_AR(PRF_FLAGS(pc), PRF_AUTOSAC);
  GET_HIT(mob) = 5;   /* damage() caps a blow at 100 */

  TEST_ASSERT_TRUE(damage(pc, mob, 100, TYPE_HIT) < 0);

  TEST_ASSERT_EQUAL_PTR(pc, bread->carried_by);
  TEST_ASSERT_EQUAL_PTR(old, treasure->in_obj);
  /* The new corpse was sacrificed; the old one is the only object left. */
  TEST_ASSERT_EQUAL_PTR(old, world[0].contents);
  TEST_ASSERT_NULL(old->next_content);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(mob);
  fx_free_char(pc);
}

/* H1-18: a weapon with 2e9 damage dice ('osetval 1 2000000000') used to
 * make dice() loop 2e9 times; it is clamped and reported. */
static void test_hit_clamps_weapon_dice(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *mob = fx_new_char("guard", TRUE, 0);
  struct obj_data *club = wield(pc, "club");

  GET_OBJ_VAL(club, 1) = 2000000000;
  GET_MAX_HIT(mob) = GET_HIT(mob) = 1000;
  GET_POS(mob) = POS_SLEEPING;

  hit(pc, mob, TYPE_UNDEFINED);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("weapon-dice-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "dice 2000000000d4"));
  TEST_ASSERT_TRUE(GET_HIT(mob) < 1000);
  if (FIGHTING(mob)) stop_fighting(mob);
  if (FIGHTING(pc)) stop_fighting(pc);
  fx_free_char(mob);
  fx_free_char(pc);
}

/* H1-16: on a world without the DG caster proxy mob, real_mobile() returns
 * NOBODY, the same as a player's nr; players must still be stopped by a
 * peaceful room. (The fixture's real_mobile() always returns NOBODY.) */
static void test_peaceful_room_stops_player_without_proxy_mob(void)
{
  struct char_data *pc = fx_new_char("hero", FALSE, 0);
  struct char_data *mob = fx_new_char("guard", TRUE, 0);

  SET_BIT_AR(ROOM_FLAGS(0), ROOM_PEACEFUL);

  TEST_ASSERT_EQUAL_INT(0, damage(pc, mob, 10, TYPE_HIT));
  TEST_ASSERT_EQUAL_INT(100, GET_HIT(mob));
  TEST_ASSERT_NOT_NULL(strstr(fx_last_send, "peaceful"));
  fx_free_char(mob);
  fx_free_char(pc);
}

/* H1-23 (act.offensive.c): a skilled bandager rolling under the skill
 * succeeds; the test used to be inverted and hurt the patient instead. */
static void test_bandage_succeeds_when_roll_is_under_skill(void)
{
  struct char_data *pc = fx_new_char("healer", FALSE, 0);
  struct char_data *mob = fx_new_char("rat", TRUE, 0);
  char buf[MAX_INPUT_LENGTH];

  SET_SKILL(pc, SKILL_BANDAGE, 95);
  GET_HIT(mob) = -5;
  GET_POS(mob) = POS_MORTALLYW;
  fx_rand_value = 50;               /* rand_number(1, 101) rolls 50 */
  strlcpy(buf, "rat", sizeof(buf));

  do_bandage(pc, buf, 0, 0);

  TEST_ASSERT_EQUAL_INT(0, GET_HIT(mob));
  fx_free_char(mob);
  fx_free_char(pc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_bandage_succeeds_when_roll_is_under_skill);
  RUN_TEST(test_peaceful_room_stops_player_without_proxy_mob);
  RUN_TEST(test_hit_clamps_weapon_dice);
  RUN_TEST(test_autoloot_takes_the_new_corpse);
  RUN_TEST(test_hit_rejects_weapon_attack_type_out_of_range);
  RUN_TEST(test_hit_rejects_mob_attack_type_out_of_range);
  RUN_TEST(test_hit_reads_weapon_after_fight_trigger);
  RUN_TEST(test_hit_skips_victim_purged_by_trigger);
  RUN_TEST(test_corpse_collects_objects_given_by_remove_trigger);
  RUN_TEST(test_corpse_skips_slot_emptied_by_remove_trigger);
  return UNITY_END();
}
