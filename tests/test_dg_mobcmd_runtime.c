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
ACMD(do_mlog);
ACMD(do_mecho);

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

/* E1-12: 'mload obj <container> <its own name>' with no such container
 * around matched the new object itself and left it in no location. */
static void test_mload_container_into_its_own_name(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *chest;

  run(do_mload, m, "obj 205 zzchest");
  chest = world[0].contents;
  TEST_ASSERT_NOT_NULL_MESSAGE(chest, "loaded object was left in no location");
  TEST_ASSERT_EQUAL_INT(DGW_OBJ_CHEST, GET_OBJ_VNUM(chest));
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("mload-place"));

  run(do_mload, m, "obj 201 zzchest");      /* a real container is used */
  TEST_ASSERT_NOT_NULL(chest->contains);
  TEST_ASSERT_EQUAL_INT(DGW_OBJ_STONE, GET_OBJ_VNUM(chest->contains));
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("mload-place"));
}

/* E1-13 / E1-V01: vnums that do not parse or do not fit are refused, not
 * turned into 0 or wrapped onto another prototype (65836 = 65536 + 300). */
static void test_mob_numeric_arguments_are_checked(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct char_data *v = dgw_mob(DGW_MOB_OGRE, 0);
  ACMD(do_mdamage);

  run(do_mload, m, "obj 65737");
  run(do_mload, m, "mob 65836");
  TEST_ASSERT_EQUAL_INT(0, count_carried(m));
  TEST_ASSERT_EQUAL_INT(2, dgw_saw("parse-vnum"));
  run(do_mtransform, m, "65837");
  TEST_ASSERT_EQUAL_STRING("a guard", GET_SHORT(m));
  TEST_ASSERT_EQUAL_INT(3, dgw_saw("parse-vnum"));
  run(do_mdamage, m, "zzogre 99999999999");
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("parse-number"));

  run(do_mload, m, "mob 301 101");     /* room target form works */
  TEST_ASSERT_NOT_NULL(world[1].people);
  (void)v;
}

/* E1-16: a load trigger returning SCRIPT_ERROR_CODE removes the
 * prototype's scripts; that must be reported with the prototype and the
 * trigger, for mobs and objects. */
static void test_load_trigger_error_code_is_reported(void)
{
  static const char *const lines[] = { "return -9999999", NULL };
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *o = dgw_obj(DGW_OBJ_STONE, 0);

  SCRIPT(m) = dgw_script_with(MOB_TRIGGER, MTRIG_LOAD, lines);
  load_mtrigger(m);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("load-proto-disable"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("load-proto-disable"),
                              "mob vnum 300: load trigger 400"));

  SCRIPT(o) = dgw_script_with(OBJ_TRIGGER, OTRIG_LOAD, lines);
  load_otrigger(o);
  TEST_ASSERT_EQUAL_INT(2, dgw_saw("load-proto-disable"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("load-proto-disable"),
                              "obj vnum 201: load trigger 400"));
}

/* E1-22: an m-command rejection reaches the structured error log with the
 * owner's identity, not only the script log. */
static void test_mob_rejection_is_reported_as_cmd_rejected(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);

  run(do_mload, m, "obj 99999");
  TEST_ASSERT_EQUAL_INT(0, count_carried(m));
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("cmd-rejected"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("cmd-rejected"), "mob vnum 300"));
}

/* "<command> <victim uid> 99999" into line; fails the test if it is cut. */
static void damage_line(char *line, size_t size, const char *command,
                        struct char_data *victim)
{
  int n = snprintf(line, size, "%s %c%ld 99999", command, UID_CHAR,
                   char_script_id(victim));

  TEST_ASSERT_TRUE(n > 0 && (size_t)n < size);
}

/* Trigger 1970 (zone 19's sinking-tree death trap) runs
 * "%damage% %actor% 99999": a deliberate kill. It used to log a field-clamp
 * warning on every victim (~46 an hour on a live server). Overkill now kills
 * quietly through mdamage, odamage and wdamage alike; the running trigger
 * register is clear again once each script has returned. */
static void test_script_overkill_damage_is_a_quiet_kill(void)
{
  char line[64];
  const char *const lines[] = { line, NULL };
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct obj_data *o = dgw_obj(DGW_OBJ_STONE, 0);
  struct room_data *room = &world[0];
  struct char_data *v;

  v = dgw_mob(DGW_MOB_OGRE, 0);
  damage_line(line, sizeof(line), "mdamage", v);
  SCRIPT(m) = dgw_script_with(MOB_TRIGGER, MTRIG_COMMAND, lines);
  script_driver(&m, SCRIPT(m)->trig_list, MOB_TRIGGER, TRIG_NEW);
  TEST_ASSERT_TRUE(MOB_FLAGGED(v, MOB_NOTDEADYET));
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("field-clamp"));

  v = dgw_mob(DGW_MOB_OGRE, 0);
  damage_line(line, sizeof(line), "odamage", v);
  SCRIPT(o) = dgw_script_with(OBJ_TRIGGER, OTRIG_COMMAND, lines);
  script_driver(&o, SCRIPT(o)->trig_list, OBJ_TRIGGER, TRIG_NEW);
  TEST_ASSERT_TRUE(MOB_FLAGGED(v, MOB_NOTDEADYET));
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("field-clamp"));

  v = dgw_mob(DGW_MOB_OGRE, 0);
  damage_line(line, sizeof(line), "wdamage", v);
  SCRIPT(room) = dgw_script_with(WLD_TRIGGER, WTRIG_COMMAND, lines);
  script_driver(&room, SCRIPT(room)->trig_list, WLD_TRIGGER, TRIG_NEW);
  TEST_ASSERT_TRUE(MOB_FLAGGED(v, MOB_NOTDEADYET));
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("field-clamp"));

  TEST_ASSERT_NULL(dg_running_trigger());
}

/* E1-22: mlog is the script author's own message, not a rejection. */
static void test_mlog_is_not_a_rejection(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);

  run(do_mlog, m, "hello from the script");
  TEST_ASSERT_EQUAL_INT(0, dgw_saw("cmd-rejected"));
}

/* E1-22: a charmed mob's m-commands are refused loudly, not silently. */
static void test_charmed_mob_refusal_is_reported(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);

  SET_BIT_AR(AFF_FLAGS(m), AFF_CHARM);
  run(do_mload, m, "obj 201");
  run(do_mecho, m, "hi");
  TEST_ASSERT_EQUAL_INT(0, count_carried(m));
  TEST_ASSERT_EQUAL_INT(2, dgw_saw("mob-cmd-charmed"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("mob-cmd-charmed"), "mob vnum 300"));
  REMOVE_BIT_AR(AFF_FLAGS(m), AFF_CHARM);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_script_overkill_damage_is_a_quiet_kill);
  RUN_TEST(test_player_with_triggers_is_refused_privileged_commands);
  RUN_TEST(test_switched_mob_follows_the_controller_level);
  RUN_TEST(test_script_driven_mob_may_load);
  RUN_TEST(test_mtransform_keeps_furniture_link);
  RUN_TEST(test_mtransform_keeps_memory_and_frees_old_strings);
  RUN_TEST(test_mtransform_refuses_players);
  RUN_TEST(test_mjunk_plain_name_junks_one_matching_item);
  RUN_TEST(test_mjunk_all_forms);
  RUN_TEST(test_mload_container_into_its_own_name);
  RUN_TEST(test_mob_numeric_arguments_are_checked);
  RUN_TEST(test_load_trigger_error_code_is_reported);
  RUN_TEST(test_mob_rejection_is_reported_as_cmd_rejected);
  RUN_TEST(test_mlog_is_not_a_rejection);
  RUN_TEST(test_charmed_mob_refusal_is_reported);
  return UNITY_END();
}
