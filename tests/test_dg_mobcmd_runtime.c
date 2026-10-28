/* Executable tests of the production mobile script commands (dg_mobcmd.c),
 * linked against the real server objects and run in a tiny in-memory world
 * (dg_world_fixture.h). The assertions check that nothing is dereferenced
 * after it is freed or through NULL (ASan/UBSan, Valgrind), that nothing is
 * lost or duplicated (the fixture's index-count checks), and that each
 * rejecting guard emitted its structured diagnostic.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "dg_world_fixture.h"

ACMD(do_mload);
ACMD(do_mpurge);
ACMD(do_mforce);
ACMD(do_mhunt);
ACMD(do_mremember);
ACMD(do_mforget);
ACMD(do_mtransform);
ACMD(do_mjunk);

void setUp(void) { dgw_setup(); }
void tearDown(void) { dgw_teardown(); }

/* ACMDs take char *: pass a heap copy so ASan sees reads past the end. */
static void run(ACMD((*fn)), struct char_data *ch, const char *arg)
{
  char *copy = strdup(arg);
  TEST_ASSERT_NOT_NULL(copy);
  fn(ch, copy, 0, 0);
  free(copy);
}

static int count_carried(struct char_data *ch)
{
  struct obj_data *o;
  int n = 0;
  for (o = ch->carrying; o; o = o->next_content)
    n++;
  return n;
}

/* E1-04: a connected, unswitched player who owns a trigger passes
 * MOB_OR_IMPL. desc->original is NULL, so the old level check dereferenced
 * NULL. Each privileged command must refuse loudly instead. */
static void test_player_with_triggers_is_refused_privileged_commands(void)
{
  struct char_data *p = dgw_player(42, 0);
  struct char_data *guard = dgw_mob(DGW_MOB_GUARD, 0);

  run(do_mload, p, "obj 201");
  TEST_ASSERT_EQUAL_INT(0, count_carried(p));
  run(do_mpurge, p, "zzguard");
  TEST_ASSERT_FALSE(MOB_FLAGGED(guard, MOB_NOTDEADYET));
  run(do_mforce, p, "zzguard say hi");
  run(do_mhunt, p, "zzguard");
  run(do_mremember, p, "zzguard");
  TEST_ASSERT_NULL(SCRIPT_MEM(p));
  run(do_mforget, p, "zzguard");

  TEST_ASSERT_EQUAL_INT(6, dgw_saw("mob-cmd-denied"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("mob-cmd-denied"), "player idnum 42"));
  dgw_free_player(p);
}

/* A mob switched into by an implementor may use them; one switched into by
 * a lower immortal may not. The mob owns a trigger, so MOB_OR_IMPL admits it
 * whatever the controller's level and the privileged check decides. */
static void test_switched_mob_follows_the_controller_level(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct char_data imm;
  struct descriptor_data d;

  memset(&imm, 0, sizeof(imm));
  memset(&d, 0, sizeof(d));
  imm.player.level = LVL_IMPL;
  d.character = m;
  d.original = &imm;
  m->desc = &d;
  SCRIPT(m) = dgw_script();

  run(do_mload, m, "obj 201");
  TEST_ASSERT_EQUAL_INT(1, count_carried(m));
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("mob-cmd-denied"));

  imm.player.level = LVL_IMMORT;
  run(do_mload, m, "obj 201");
  TEST_ASSERT_EQUAL_INT(1, count_carried(m));
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("mob-cmd-denied"));
  m->desc = NULL;
}

/* A script-driven mob (no descriptor) is unaffected. */
static void test_script_driven_mob_may_load(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  run(do_mload, m, "obj 201");
  TEST_ASSERT_EQUAL_INT(1, count_carried(m));
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("mob-cmd-denied"));
}

/* E1-06: a mob sitting on furniture keeps its seat across mtransform. The
 * bench's sitter list points at the mob; losing SITTING() left the bench
 * pointing at the mob after it was freed (teardown extracts the mob, then
 * the bench: heap-use-after-free before the fix). */
static void test_mtransform_keeps_furniture_link(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *bench = dgw_obj(DGW_OBJ_BENCH, 0);

  SITTING(m) = bench;
  NEXT_SITTING(m) = NULL;
  OBJ_SAT_IN_BY(bench) = m;
  GET_OBJ_VAL(bench, 1) = 1;
  GET_POS(m) = POS_SITTING;

  run(do_mtransform, m, "301");

  TEST_ASSERT_EQUAL_INT(DGW_MOB_GUARD, GET_MOB_VNUM(m)); /* keeps its rnum */
  TEST_ASSERT_EQUAL_STRING("an ogre", GET_SHORT(m));
  TEST_ASSERT_EQUAL_PTR(bench, SITTING(m));
  TEST_ASSERT_EQUAL_PTR(m, OBJ_SAT_IN_BY(bench));
}

/* E1-06: mob memory survives (and is freed once, at extraction); strings
 * from an earlier transform are freed, not leaked (LSan). */
static void test_mtransform_keeps_memory_and_frees_old_strings(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  memory_rec *mem;

  CREATE(mem, memory_rec, 1);
  mem->id = 4242;
  MEMORY(m) = mem;

  run(do_mtransform, m, "301");
  run(do_mtransform, m, "300");
  run(do_mtransform, m, "-301");

  TEST_ASSERT_EQUAL_PTR(mem, MEMORY(m));
  TEST_ASSERT_EQUAL_STRING("an ogre", GET_SHORT(m));
}

/* E1-06: a link-dead player who owns triggers passes MOB_OR_IMPL; it must
 * not be overwritten with a mobile. */
static void test_mtransform_refuses_players(void)
{
  struct char_data *p = dgw_player(43, 0);
  struct descriptor_data *d = p->desc;

  p->desc = NULL;   /* link-dead */
  run(do_mtransform, p, "301");
  p->desc = d;

  TEST_ASSERT_EQUAL_INT(1, dgw_saw("mtransform"));
  TEST_ASSERT_FALSE(IS_NPC(p));
  TEST_ASSERT_EQUAL_STRING("Zzplayer", GET_NAME(p));
  dgw_free_player(p);
}

static struct obj_data *give(struct char_data *ch, obj_vnum vnum)
{
  struct obj_data *o = dgw_obj(vnum, NOWHERE);
  obj_to_char(o, ch);
  return o;
}

/* E1-11: a plain name junks only the matching item. 'mjunk axe' (any
 * three-letter name) used to destroy the whole inventory, and shorter names
 * read arg[3] / arg + 4 past what one_argument() wrote. */
static void test_mjunk_plain_name_junks_one_matching_item(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  give(m, DGW_OBJ_STONE);
  give(m, DGW_OBJ_STONE);
  give(m, DGW_OBJ_BAG);

  run(do_mjunk, m, "bag");
  TEST_ASSERT_EQUAL_INT(2, count_carried(m));
  run(do_mjunk, m, "axe");
  TEST_ASSERT_EQUAL_INT(2, count_carried(m));
  run(do_mjunk, m, "ax");
  TEST_ASSERT_EQUAL_INT(2, count_carried(m));
  TEST_ASSERT_EQUAL_INT(2, dgw_saw("mjunk-nomatch"));
  run(do_mjunk, m, "stone");
  TEST_ASSERT_EQUAL_INT(1, count_carried(m));
}

/* E1-11: all.<name> junks every match; all junks the inventory. */
static void test_mjunk_all_forms(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  give(m, DGW_OBJ_STONE);
  give(m, DGW_OBJ_BAG);
  give(m, DGW_OBJ_STONE);

  run(do_mjunk, m, "all.stone");
  TEST_ASSERT_EQUAL_INT(1, count_carried(m));
  TEST_ASSERT_EQUAL_INT(DGW_OBJ_BAG, GET_OBJ_VNUM(m->carrying));
  run(do_mjunk, m, "all");
  TEST_ASSERT_EQUAL_INT(0, count_carried(m));
  run(do_mjunk, m, "all");           /* nothing left: not an error */
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("mjunk-nomatch"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_player_with_triggers_is_refused_privileged_commands);
  RUN_TEST(test_switched_mob_follows_the_controller_level);
  RUN_TEST(test_script_driven_mob_may_load);
  RUN_TEST(test_mtransform_keeps_furniture_link);
  RUN_TEST(test_mtransform_keeps_memory_and_frees_old_strings);
  RUN_TEST(test_mtransform_refuses_players);
  RUN_TEST(test_mjunk_plain_name_junks_one_matching_item);
  RUN_TEST(test_mjunk_all_forms);
  return UNITY_END();
}
