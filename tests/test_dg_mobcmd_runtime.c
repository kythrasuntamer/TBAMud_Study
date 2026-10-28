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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_player_with_triggers_is_refused_privileged_commands);
  RUN_TEST(test_switched_mob_follows_the_controller_level);
  RUN_TEST(test_script_driven_mob_may_load);
  return UNITY_END();
}
