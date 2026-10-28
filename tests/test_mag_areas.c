/* Executable tests of the production area-spell walk, mag_areas()
 * (magic.c), linked with the production handler.c for the room-occupant
 * snapshot and the room movement primitives. damage() (fight.c) is the
 * fixture: it records each victim and, like a damage or death trigger, can
 * teleport or purge another occupant of the room.
 *
 * H1-19: mag_areas() kept next_in_room across mag_damage(); a trigger that
 *        teleported the next occupant made the spell continue through the
 *        destination room's people list.
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
#include "fight.h"
#include "class.h"
#include "tb_error_capture.h"

/* ---------------------------------------------------------------------
 * Externals of the kept magic.c / handler.c sections
 * ------------------------------------------------------------------- */

struct spell_info_type spell_info[TOP_SPELL_DEFINE + 1];
void basic_mud_log(const char *format, ...) { (void)format; }
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
int dice(int number, int size) { (void)size; return number; }
int rand_number_at(int from, int to, const char *source_file, int source_line,
                   const char *source_func)
{ (void)to; (void)source_file; (void)source_line; (void)source_func; return from; }
int room_is_dark_at(room_rnum room, const char *source_file, int source_line,
                    const char *source_func)
{ (void)room; (void)source_file; (void)source_line; (void)source_func; return 0; }
const char *skill_name(int num) { (void)num; return "earthquake"; }
/* class.c: 100 means no save is ever made (full damage). */
byte saving_throws(int class_num, int type, int level)
{ (void)class_num; (void)type; (void)level; return 100; }
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

/* UID lookup (dg_scripts.c), reduced to the fixture characters. */
#define MAX_FIXTURE_CHARS 8
static struct char_data *live_chars[MAX_FIXTURE_CHARS];
long char_script_id(char_data *ch) { return ch->script_id; }
struct char_data *find_char(long n)
{
  int i;

  for (i = 0; i < MAX_FIXTURE_CHARS; i++)
    if (live_chars[i] && live_chars[i]->script_id == n)
      return live_chars[i];
  return NULL;
}

/* ---------------------------------------------------------------------
 * damage() with the script effects under test
 * ------------------------------------------------------------------- */

static struct char_data *hook_owner;    /* damaging this one ... */
static struct char_data *teleport_who;  /* ... teleports this one */
static struct char_data *purge_who;     /* ... or purges this one */
static int damaged_count;
static struct char_data *damaged[MAX_FIXTURE_CHARS];

int damage(struct char_data *ch, struct char_data *victim, int dam,
           int attacktype)
{
  (void)ch; (void)dam; (void)attacktype;
  if (damaged_count < MAX_FIXTURE_CHARS)
    damaged[damaged_count] = victim;
  damaged_count++;
  if (victim == hook_owner) {
    if (teleport_who) {
      char_from_room(teleport_who);
      char_to_room(teleport_who, 1);
      teleport_who = NULL;
    }
    if (purge_who)
      SET_BIT_AR(MOB_FLAGS(purge_who), MOB_NOTDEADYET);
  }
  return dam;
}

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
  GET_LEVEL(ch) = 10;
  live_chars[slot] = ch;
  char_to_room(ch, room);
}

void setUp(void)
{
  memset(rooms, 0, sizeof(rooms));
  memset(live_chars, 0, sizeof(live_chars));
  world = rooms;
  top_of_world = 1;
  /* Arrivals go to the head of the list: room 0 reads caster, mob_a,
   * mob_b, mob_c. */
  place(&mob_c, 204, 0, 3);
  place(&mob_b, 203, 0, 2);
  place(&mob_a, 202, 0, 1);
  place(&caster, 201, 0, 0);
  place(&bystander, 205, 1, 4);
  hook_owner = teleport_who = purge_who = NULL;
  damaged_count = 0;
  memset(damaged, 0, sizeof(damaged));
  tb_error_capture_reset();
}

void tearDown(void) { world = NULL; top_of_world = 0; }

static void quake(void)
{
  mag_areas(20, &caster, SPELL_EARTHQUAKE, SAVING_SPELL);
}

static void test_area_spell_skips_occupant_teleported_by_a_trigger(void)
{
  /* Damaging mob_a teleports mob_b, the occupant after it, to room 1. */
  hook_owner = &mob_a;
  teleport_who = &mob_b;
  quake();

  TEST_ASSERT_EQUAL_INT(2, damaged_count);
  TEST_ASSERT_EQUAL_PTR(&mob_a, damaged[0]);
  TEST_ASSERT_EQUAL_PTR(&mob_c, damaged[1]);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("area-target-moved", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "target uid 203"));
}

static void test_area_spell_skips_occupant_purged_by_a_trigger(void)
{
  hook_owner = &mob_a;
  purge_who = &mob_b;
  quake();

  TEST_ASSERT_EQUAL_INT(2, damaged_count);
  TEST_ASSERT_EQUAL_PTR(&mob_c, damaged[1]);
  TEST_ASSERT_EQUAL_STRING("area-target-moved", tb_captured_operation);
}

static void test_area_spell_stops_when_the_caster_is_purged(void)
{
  hook_owner = &mob_a;
  purge_who = &caster;
  quake();

  TEST_ASSERT_EQUAL_INT(1, damaged_count);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("area-caster-moved", tb_captured_operation);
}

static void test_area_spell_without_interference_hits_everyone_else(void)
{
  quake();
  TEST_ASSERT_EQUAL_INT(3, damaged_count);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_area_spell_skips_occupant_teleported_by_a_trigger);
  RUN_TEST(test_area_spell_skips_occupant_purged_by_a_trigger);
  RUN_TEST(test_area_spell_stops_when_the_caster_is_purged);
  RUN_TEST(test_area_spell_without_interference_hits_everyone_else);
  return UNITY_END();
}
