/* Executable tests of the production magic entry points (spell_parser.c),
 * linked with the production handler.c for the room-occupant snapshot and
 * the room movement primitives. The DG cast triggers and the mag_*()
 * routines are fixtures: the triggers can purge or teleport, as a builder's
 * script would, and the routines record who they were applied to.
 *
 * H2-01: call_magic() must stop when a cast trigger purges a participant
 *        (extract_obj() frees an object at once).
 * H2-10: a staff must not follow next_in_room pointers kept across
 *        call_magic(): a trigger that teleports the next occupant made the
 *        staff carry on through another room's people list.
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
#include "interpreter.h"
#include "dg_event.h"
#include "lists.h"
#include "tb_error_capture.h"

#define TEST_SPELL_OBJ    SPELL_BLESS     /* MAG_ALTER_OBJS in this fixture */
#define TEST_SPELL_CHAR   SPELL_ARMOR     /* MAG_AFFECTS in this fixture */

/* ---------------------------------------------------------------------
 * Externals of the kept handler.c / spell_parser.c sections. None of them
 * is reached by these tests except char_from_room()/char_to_room()'s
 * bookkeeping and one_argument() (empty argument: no explicit target).
 * ------------------------------------------------------------------- */

struct obj_data *object_list = NULL;
struct obj_data *obj_proto = NULL;
void basic_mud_log(const char *format, ...) { (void)format; }
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
char *one_argument(char *argument, char *first_arg)
{ *first_arg = '\0'; return argument; }
int room_is_dark_at(room_rnum room, const char *source_file, int source_line,
                    const char *source_func)
{ (void)room; (void)source_file; (void)source_line; (void)source_func; return 0; }
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
void autoquest_trigger_check(struct char_data *ch, struct char_data *vict,
                             struct obj_data *object, int type)
{ (void)ch; (void)vict; (void)object; (void)type; }
void stop_fighting(struct char_data *ch) { (void)ch; }
void char_from_furniture(struct char_data *ch) { (void)ch; }
int consume_otrigger(obj_data *obj, char_data *actor, int cmd)
{ (void)obj; (void)actor; (void)cmd; return 1; }
void event_cancel(struct event *event) { (void)event; }
void extract_script(void *thing, int type) { (void)thing; (void)type; }
void free_proto_script(void *thing, int type) { (void)thing; (void)type; }
void free_list(struct list_data *pList) { (void)pList; }
void *simple_list(struct list_data *pList) { (void)pList; return NULL; }
void free_obj(struct obj_data *obj) { (void)obj; }

/* ---------------------------------------------------------------------
 * UID registry (dg_scripts.c's lookup table, reduced to what is used)
 * ------------------------------------------------------------------- */

#define MAX_FIXTURE_CHARS 8
static struct char_data *live_chars[MAX_FIXTURE_CHARS];
static struct obj_data *live_obj;
static long live_obj_uid;

long char_script_id(char_data *ch) { return ch->script_id; }
long obj_script_id(obj_data *obj) { return obj->script_id; }
struct char_data *find_char(long n)
{
  int i;

  for (i = 0; i < MAX_FIXTURE_CHARS; i++)
    if (live_chars[i] && live_chars[i]->script_id == n)
      return live_chars[i];
  return NULL;
}
/* Compares the registered address only; never dereferences obj. */
int obj_uid_matches(long uid, const obj_data *obj)
{
  return obj != NULL && live_obj == obj && live_obj_uid == uid;
}

/* ---------------------------------------------------------------------
 * Cast triggers (dg_triggers.c) with the script effects under test
 * ------------------------------------------------------------------- */

static int purge_obj_in_otrigger;       /* "%purge% %self%" on the object */
static struct char_data *purge_owner;   /* this mob's cast trigger ... */
static struct char_data *purge_who;     /* ... mpurges this one */
static struct char_data *teleport_owner;      /* trigger owner ... */
static struct char_data *teleport_who;        /* ... teleports this one */
static room_rnum teleport_to;

int cast_wtrigger(char_data *actor, char_data *vict, obj_data *obj, int spellnum)
{ (void)actor; (void)vict; (void)obj; (void)spellnum; return 1; }

int cast_otrigger(char_data *actor, obj_data *obj, int spellnum)
{
  (void)actor; (void)spellnum;
  if (obj && purge_obj_in_otrigger) {
    /* extract_obj(): the UID is unregistered and the object freed now. */
    live_obj = NULL;
    free(obj);
  }
  return 1;
}

int cast_mtrigger(char_data *actor, char_data *ch, int spellnum)
{
  (void)actor; (void)spellnum;
  if (ch && ch == purge_owner && purge_who)
    SET_BIT_AR(MOB_FLAGS(purge_who), MOB_NOTDEADYET);  /* deferred extraction */
  if (ch && ch == teleport_owner && teleport_who) {
    char_from_room(teleport_who);
    char_to_room(teleport_who, teleport_to);
    teleport_who = NULL;
  }
  return 1;
}

/* ---------------------------------------------------------------------
 * Spell routines (magic.c, spells.c): record what they were applied to
 * ------------------------------------------------------------------- */

static int alter_obj_calls, affect_calls;
static struct char_data *affected[MAX_FIXTURE_CHARS];

void mag_alter_objs(int level, struct char_data *ch, struct obj_data *obj,
    int spellnum, int savetype)
{
  (void)level; (void)ch; (void)spellnum; (void)savetype;
  /* Touch the object as the real routine does: a freed one is an ASan
   * heap-use-after-free. */
  if (obj->obj_flags.type_flag >= 0)
    alter_obj_calls++;
}
void mag_affects(int level, struct char_data *ch, struct char_data *victim,
    int spellnum, int savetype)
{
  (void)level; (void)ch; (void)spellnum; (void)savetype;
  if (affect_calls < MAX_FIXTURE_CHARS)
    affected[affect_calls] = victim;
  affect_calls++;
}
int mag_damage(int level, struct char_data *ch, struct char_data *victim,
    int spellnum, int savetype)
{ (void)level; (void)ch; (void)victim; (void)spellnum; (void)savetype; return 0; }
void mag_unaffects(int level, struct char_data *ch, struct char_data *victim,
    int spellnum, int type)
{ (void)level; (void)ch; (void)victim; (void)spellnum; (void)type; }
void mag_points(int level, struct char_data *ch, struct char_data *victim,
    int spellnum, int savetype)
{ (void)level; (void)ch; (void)victim; (void)spellnum; (void)savetype; }
void mag_groups(int level, struct char_data *ch, int spellnum, int savetype)
{ (void)level; (void)ch; (void)spellnum; (void)savetype; }
void mag_masses(int level, struct char_data *ch, int spellnum, int savetype)
{ (void)level; (void)ch; (void)spellnum; (void)savetype; }
void mag_areas(int level, struct char_data *ch, int spellnum, int savetype)
{ (void)level; (void)ch; (void)spellnum; (void)savetype; }
void mag_summons(int level, struct char_data *ch, struct obj_data *obj,
    int spellnum, int savetype)
{ (void)level; (void)ch; (void)obj; (void)spellnum; (void)savetype; }
void mag_creations(int level, struct char_data *ch, int spellnum)
{ (void)level; (void)ch; (void)spellnum; }
void mag_rooms(int level, struct char_data *ch, int spellnum)
{ (void)level; (void)ch; (void)spellnum; }
#define NO_SPELL(name) \
  void name(int level, struct char_data *ch, struct char_data *victim, \
            struct obj_data *obj) \
  { (void)level; (void)ch; (void)victim; (void)obj; }
NO_SPELL(spell_charm)
NO_SPELL(spell_create_water)
NO_SPELL(spell_detect_poison)
NO_SPELL(spell_enchant_weapon)
NO_SPELL(spell_identify)
NO_SPELL(spell_locate_object)
NO_SPELL(spell_summon)
NO_SPELL(spell_recall)
NO_SPELL(spell_teleport)

/* ---------------------------------------------------------------------
 * World: room 0 (the caster's) and room 1
 * ------------------------------------------------------------------- */

static struct room_data rooms[2];
static struct char_data caster, mob_a, mob_b, mob_c, bystander;

static void place(struct char_data *ch, long uid, room_rnum room, int slot)
{
  memset(ch, 0, sizeof(*ch));
  SET_BIT_AR(MOB_FLAGS(ch), MOB_ISNPC);
  ch->nr = NOBODY;
  ch->player_specials = &dummy_mob;
  ch->script_id = uid;
  ch->in_room = NOWHERE;
  GET_POS(ch) = POS_STANDING;
  live_chars[slot] = ch;
  char_to_room(ch, room);
}

void setUp(void)
{
  memset(rooms, 0, sizeof(rooms));
  memset(live_chars, 0, sizeof(live_chars));
  world = rooms;
  top_of_world = 1;
  /* char_to_room() puts each arrival at the head of the list, so room 0
   * reads caster, mob_a, mob_b, mob_c. */
  place(&mob_c, 104, 0, 3);
  place(&mob_b, 103, 0, 2);
  place(&mob_a, 102, 0, 1);
  place(&caster, 101, 0, 0);
  place(&bystander, 105, 1, 4);

  memset(spell_info, 0, sizeof(spell_info[0]) * (TOP_SPELL_DEFINE + 1));
  spell_info[TEST_SPELL_OBJ].routines = MAG_ALTER_OBJS;
  spell_info[TEST_SPELL_OBJ].name = "bless";
  spell_info[TEST_SPELL_CHAR].routines = MAG_AFFECTS;
  spell_info[TEST_SPELL_CHAR].name = "armor";

  purge_obj_in_otrigger = 0;
  purge_owner = purge_who = NULL;
  teleport_owner = teleport_who = NULL;
  teleport_to = 1;
  alter_obj_calls = affect_calls = 0;
  memset(affected, 0, sizeof(affected));
  live_obj = NULL;
  tb_error_capture_reset();
}

void tearDown(void) { world = NULL; top_of_world = 0; }

static struct obj_data *make_live_obj(long uid, int type)
{
  struct obj_data *obj;

  CREATE(obj, struct obj_data, 1);
  obj->item_number = NOTHING;
  obj->in_room = NOWHERE;
  obj->script_id = uid;
  GET_OBJ_TYPE(obj) = type;
  live_obj = obj;
  live_obj_uid = uid;
  return obj;
}

/* ---------------------------------------------------------------------
 * H2-01: call_magic() re-validates its participants after each trigger
 * ------------------------------------------------------------------- */

static void test_call_magic_stops_when_object_trigger_purges_target(void)
{
  struct obj_data *target = make_live_obj(5001, ITEM_WEAPON);

  purge_obj_in_otrigger = 1;     /* the trigger frees target */
  TEST_ASSERT_EQUAL_INT(0, call_magic(&caster, NULL, target, TEST_SPELL_OBJ,
                                      10, CAST_SPELL));
  TEST_ASSERT_EQUAL_INT(0, alter_obj_calls);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("cast-target-purged", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj uid 5001"));
}

static void test_call_magic_stops_when_mob_trigger_purges_victim(void)
{
  purge_owner = purge_who = &mob_a;    /* "mpurge %self%" */
  TEST_ASSERT_EQUAL_INT(0, call_magic(&caster, &mob_a, NULL, TEST_SPELL_CHAR,
                                      10, CAST_SPELL));
  TEST_ASSERT_EQUAL_INT(0, affect_calls);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("cast-target-purged", tb_captured_operation);
}

static void test_call_magic_casts_when_nothing_is_purged(void)
{
  struct obj_data *target = make_live_obj(5002, ITEM_WEAPON);

  TEST_ASSERT_EQUAL_INT(1, call_magic(&caster, NULL, target, TEST_SPELL_OBJ,
                                      10, CAST_SPELL));
  TEST_ASSERT_EQUAL_INT(1, alter_obj_calls);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(target);
}

/* ---------------------------------------------------------------------
 * H2-10: the staff loop re-resolves each occupant
 * ------------------------------------------------------------------- */

static struct obj_data *make_staff(void)
{
  struct obj_data *staff = make_live_obj(6001, ITEM_STAFF);

  GET_OBJ_VAL(staff, 0) = 10;                 /* level */
  GET_OBJ_VAL(staff, 2) = 3;                  /* charges */
  GET_OBJ_VAL(staff, 3) = TEST_SPELL_CHAR;    /* spell */
  return staff;
}

static void test_staff_skips_occupant_teleported_by_a_trigger(void)
{
  struct obj_data *staff = make_staff();
  char arg[] = "";

  /* mob_a's cast trigger teleports mob_b, the occupant after it, into
   * room 1, where the bystander stands. */
  teleport_owner = &mob_a;
  teleport_who = &mob_b;
  mag_objectmagic(&caster, staff, arg);

  /* mob_a and mob_c are affected; mob_b (gone) and the bystander in
   * room 1 are not. */
  TEST_ASSERT_EQUAL_INT(2, affect_calls);
  TEST_ASSERT_EQUAL_PTR(&mob_a, affected[0]);
  TEST_ASSERT_EQUAL_PTR(&mob_c, affected[1]);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("staff-target-moved", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "target uid 103"));
  TEST_ASSERT_EQUAL_INT(2, GET_OBJ_VAL(staff, 2));
  free(staff);
}

static void test_staff_skips_occupant_purged_by_a_trigger(void)
{
  struct obj_data *staff = make_staff();
  char arg[] = "";

  /* mob_a's cast trigger purges mob_b, which is still in the room but
   * pending extraction when the staff reaches it. */
  purge_owner = &mob_a;
  purge_who = &mob_b;
  mag_objectmagic(&caster, staff, arg);

  TEST_ASSERT_EQUAL_INT(2, affect_calls);
  TEST_ASSERT_EQUAL_PTR(&mob_a, affected[0]);
  TEST_ASSERT_EQUAL_PTR(&mob_c, affected[1]);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("staff-target-moved", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "target uid 103"));
  free(staff);
}

static void test_staff_without_interference_hits_everyone_else(void)
{
  struct obj_data *staff = make_staff();
  char arg[] = "";

  mag_objectmagic(&caster, staff, arg);
  TEST_ASSERT_EQUAL_INT(3, affect_calls);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(staff);
}

/* The snapshot helper reports an invalid room at its caller and yields
 * nothing to iterate. */
static void test_room_people_uids_invalid_room_reports_caller(void)
{
  long *uids = (long *) &uids;    /* must be reset to NULL */

  TEST_ASSERT_EQUAL_INT(0, room_people_uids(NOWHERE, &uids));
  TEST_ASSERT_NULL(uids);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("room-people-snapshot", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_spell_parser.c"));

  TEST_ASSERT_EQUAL_INT(4, room_people_uids(0, &uids));
  TEST_ASSERT_EQUAL_INT(101, (int) uids[0]);
  TEST_ASSERT_EQUAL_PTR(&mob_c, room_person_live(uids[3], 0));
  TEST_ASSERT_NULL(room_person_live(uids[3], 1));
  free(uids);
}

/* Coordinator item: a caster stranded in NOWHERE (by an earlier, reported
 * char_to_room() refusal) can still type 'cast'. call_magic() must refuse
 * before the triggers and room-flag checks index world[] with NOWHERE. */
static void test_call_magic_refuses_a_stranded_caster(void)
{
  char_from_room(&caster);             /* stranded in NOWHERE */
  TEST_ASSERT_EQUAL_INT(0, call_magic(&caster, &mob_a, NULL, TEST_SPELL_CHAR,
                                      10, CAST_SPELL));
  TEST_ASSERT_EQUAL_INT(0, affect_calls);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("caster-nowhere", tb_captured_operation);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_call_magic_stops_when_object_trigger_purges_target);
  RUN_TEST(test_call_magic_stops_when_mob_trigger_purges_victim);
  RUN_TEST(test_call_magic_casts_when_nothing_is_purged);
  RUN_TEST(test_staff_skips_occupant_teleported_by_a_trigger);
  RUN_TEST(test_staff_skips_occupant_purged_by_a_trigger);
  RUN_TEST(test_staff_without_interference_hits_everyone_else);
  RUN_TEST(test_room_people_uids_invalid_room_reports_caller);
  RUN_TEST(test_call_magic_refuses_a_stranded_caster);
  return UNITY_END();
}
