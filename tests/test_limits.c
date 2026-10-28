/* Executable tests of the production limits.c: the point_update() object
 * pass against scripts that purge other objects, and the gold/bank caps.
 * limits.c is linked as is; the world, the UID table and the game functions
 * it reaches are fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "limits.h"
#include <stdarg.h>
#include "tb_error_capture.h"

/* db.c owns these; this fixture does not link db.c. */
struct obj_data *object_list;
struct time_info_data time_info;

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{ (void)ch; (void)messg; return 0; }
void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void game_info(const char *format, ...) { (void)format; }
void core_dump_real(const char *who, int line) { (void)who; (void)line; }
/* Reached only through the character pass, which these tests leave empty. */
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
int Crash_idlesave(struct char_data *ch) { (void)ch; UNREACHED(); return 0; }
int Crash_rentsave(struct char_data *ch, int cost) { (void)ch; (void)cost; UNREACHED(); return 0; }
/* The regeneration tests call mana_gain()/hit_gain(), which ask age(). */
static int age_allowed;
static struct time_info_data adult_age = { 0, 0, 0, 30 };
struct time_info_data *age(struct char_data *ch)
{ (void)ch; if (!age_allowed) UNREACHED(); return &adult_age; }
int damage(struct char_data *ch, struct char_data *victim, int dam, int attacktype)
{ (void)ch; (void)victim; (void)dam; (void)attacktype; UNREACHED(); return 0; }
void stop_fighting(struct char_data *ch) { (void)ch; UNREACHED(); }
void update_char_objects(struct char_data *ch) { (void)ch; UNREACHED(); }
void update_pos(struct char_data *victim) { (void)victim; UNREACHED(); }
/* Corpse contents: only the orphaned-corpse test has any. */
void obj_from_obj(struct obj_data *obj)
{
  struct obj_data **pp;
  for (pp = &obj->in_obj->contains; *pp; pp = &(*pp)->next_content)
    if (*pp == obj) {
      *pp = obj->next_content;
      break;
    }
  obj->in_obj = NULL;
  obj->next_content = NULL;
}
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to) { (void)obj; (void)obj_to; UNREACHED(); }
void obj_to_room(struct obj_data *object, room_rnum room) { (void)object; (void)room; UNREACHED(); }
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
void *tb_realloc_array_at(void *ptr, size_t count, size_t element_size,
                          const char *source_file, int source_line,
                          const char *source_func)
{
  (void)source_file; (void)source_line; (void)source_func;
  return realloc(ptr, count * element_size);
}

/* --- UID registry: the production contract of obj_script_id() and
 * obj_uid_matches() (dg_scripts.c), reduced to a fixed table. --- */
#define MAX_FIXTURE_OBJS 8
static struct { long uid; struct obj_data *obj; } registry[MAX_FIXTURE_OBJS];
static long next_uid = OBJ_ID_BASE;
long obj_script_id(obj_data *obj)
{
  int k;
  if (obj->script_id)
    return obj->script_id;
  obj->script_id = next_uid++;
  for (k = 0; k < MAX_FIXTURE_OBJS; k++)
    if (!registry[k].obj) {
      registry[k].uid = obj->script_id;
      registry[k].obj = obj;
      break;
    }
  return obj->script_id;
}
int obj_uid_matches(long uid, const obj_data *obj)
{
  int k;
  for (k = 0; k < MAX_FIXTURE_OBJS; k++)
    if (registry[k].obj && registry[k].uid == uid)
      return registry[k].obj == obj;
  return FALSE;
}

/* extract_obj() frees immediately, as handler.c does. */
static int extracted;
void extract_obj(struct obj_data *obj)
{
  struct obj_data **pp;
  int k;
  for (pp = &object_list; *pp; pp = &(*pp)->next)
    if (*pp == obj) {
      *pp = obj->next;
      break;
    }
  for (k = 0; k < MAX_FIXTURE_OBJS; k++)
    if (registry[k].obj == obj)
      registry[k].obj = NULL;
  extracted++;
  free(obj);
}

/* A's timer trigger purges B, as 'opurge <B>' would. */
static struct obj_data *purge_on_timer, *purge_victim;
static int timer_triggers;
void timer_otrigger(struct obj_data *obj)
{
  timer_triggers++;
  if (obj == purge_on_timer && purge_victim)
    extract_obj(purge_victim);
}

static struct obj_data *new_timed_obj(int timer)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));
  TEST_ASSERT_NOT_NULL(obj);
  GET_OBJ_TYPE(obj) = ITEM_TRASH;
  GET_OBJ_TIMER(obj) = timer;
  GET_OBJ_RNUM(obj) = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  return obj;
}

void setUp(void)
{
  memset(registry, 0, sizeof(registry));
  object_list = NULL;
  character_list = NULL;
  purge_on_timer = purge_victim = NULL;
  timer_triggers = extracted = 0;
  tb_error_capture_reset();
}

void tearDown(void)
{
  while (object_list)
    extract_obj(object_list);
}

/* H3-08: A and B expire on the same tick; A's timer script purges B, which
 * is A->next. B must be skipped (and reported), never read after free. */
static void test_timer_script_purging_next_object(void)
{
  struct obj_data *a = new_timed_obj(1), *b = new_timed_obj(1);

  a->next = b;
  object_list = a;
  purge_on_timer = a;
  purge_victim = b;

  point_update();

  TEST_ASSERT_EQUAL_INT(1, timer_triggers);
  TEST_ASSERT_EQUAL_INT(1, extracted);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("timer-target-purged", tb_captured_operation);
}

/* Timers that do not expire this tick only count down. */
static void test_unexpired_timers_count_down(void)
{
  struct obj_data *a = new_timed_obj(3);

  object_list = a;
  point_update();

  TEST_ASSERT_EQUAL_INT(2, GET_OBJ_TIMER(a));
  TEST_ASSERT_EQUAL_INT(0, timer_triggers);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static struct char_data payer;
static struct player_special_data payer_specials;

static void reset_payer(void)
{
  memset(&payer, 0, sizeof(payer));
  memset(&payer_specials, 0, sizeof(payer_specials));
  payer.player_specials = &payer_specials;
  GET_IDNUM(&payer) = 77;
}

/* H3-09: 2,139,000,000 + 10,000,000 overflowed int (UBSan) before the cap
 * test. It must clamp at MAX_GOLD and report the coins not applied. */
static void test_increase_gold_near_cap_clamps_loudly(void)
{
  reset_payer();
  GET_GOLD(&payer) = 2139000000;

  TEST_ASSERT_EQUAL_INT(MAX_GOLD, increase_gold(&payer, 10000000));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("gold-clamp", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 77"));
}

static void test_increase_bank_near_cap_clamps_loudly(void)
{
  reset_payer();
  GET_BANK_GOLD(&payer) = 2100000000;

  TEST_ASSERT_EQUAL_INT(MAX_BANK, increase_bank(&payer, 100000000));
  TEST_ASSERT_EQUAL_STRING("bank-clamp", tb_captured_operation);
}

/* decrease_*(INT_MIN) used to negate INT_MIN in int. */
static void test_decrease_int_min_does_not_overflow(void)
{
  reset_payer();
  GET_GOLD(&payer) = 5;

  TEST_ASSERT_EQUAL_INT(MAX_GOLD, decrease_gold(&payer, INT_MIN));
  TEST_ASSERT_EQUAL_STRING("gold-clamp", tb_captured_operation);
}

/* In-range changes are exact and silent. */
static void test_in_range_changes_are_silent(void)
{
  reset_payer();
  GET_GOLD(&payer) = 100;
  GET_BANK_GOLD(&payer) = 100;

  TEST_ASSERT_EQUAL_INT(150, increase_gold(&payer, 50));
  TEST_ASSERT_EQUAL_INT(70, decrease_gold(&payer, 80));
  TEST_ASSERT_EQUAL_INT(160, increase_bank(&payer, 60));
  TEST_ASSERT_EQUAL_INT(0, decrease_bank(&payer, 160));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H3-10: a rotting corpse that is in no room, object or inventory used to
 * core_dump() and leave its contents unreachable. They are now reported and
 * extracted. */
static void test_orphaned_corpse_contents_reported(void)
{
  struct obj_data *corpse = new_timed_obj(1), *content = new_timed_obj(0);

  GET_OBJ_TYPE(corpse) = ITEM_CONTAINER;
  GET_OBJ_VAL(corpse, 3) = 1;
  corpse->contains = content;
  content->in_obj = corpse;
  corpse->next = content;
  object_list = corpse;

  point_update();

  TEST_ASSERT_EQUAL_INT(2, extracted);
  TEST_ASSERT_NULL(object_list);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("corpse-orphan", tb_captured_operation);
}

/* C1-19 adoption: gain_exp() asks for the next level's experience through
 * char_level_exp(), so a class-table miss names the player. */
static const struct char_data *exp_subject;
static int exp_level;
int char_level_exp(const struct char_data *ch, int level)
{ exp_subject = ch; exp_level = level; return 1000000; }

/* gain_exp()'s level-up path (not reached: the player gains too little). */
void advance_level(struct char_data *ch) { (void)ch; UNREACHED(); }
void reboot_wizlists(void) { UNREACHED(); }
const char *title_male(int chclass, int level)
{ (void)chclass; (void)level; UNREACHED(); return ""; }
const char *title_female(int chclass, int level)
{ (void)chclass; (void)level; UNREACHED(); return ""; }

static void test_gain_exp_names_the_player(void)
{
  struct char_data pc;
  struct player_special_data pc_specials;

  memset(&pc, 0, sizeof(pc));
  memset(&pc_specials, 0, sizeof(pc_specials));
  pc.player_specials = &pc_specials;
  GET_LEVEL(&pc) = 1;
  config_info.play.max_exp_gain = 1000;
  exp_subject = NULL;
  gain_exp(&pc, 100);
  TEST_ASSERT_EQUAL_PTR(&pc, exp_subject);
  TEST_ASSERT_EQUAL_INT(2, exp_level);
  TEST_ASSERT_EQUAL_INT(1, GET_LEVEL(&pc));    /* not enough to level */
  config_info.play.max_exp_gain = 0;
}

/* Regeneration follows the class archetype: Sorcerer/Necromancer regain
 * mana like a Wizard and Druid like a Cleric (double), and give up half
 * their hit point regeneration like them.  Red before: the new casters
 * regenerated like warriors. */
static int regen(int chclass, int (*fn)(struct char_data *))
{
  static struct char_data ch;
  static struct player_special_data sp;

  memset(&ch, 0, sizeof(ch));
  memset(&sp, 0, sizeof(sp));
  ch.player_specials = &sp;
  GET_CLASS(&ch) = chclass;
  GET_POS(&ch) = POS_STANDING;
  GET_COND(&ch, HUNGER) = GET_COND(&ch, THIRST) = 10;
  return fn(&ch);
}

static void test_regeneration_follows_class_archetype(void)
{
  int wizard_mana, warrior_mana, wizard_hit, warrior_hit;

  age_allowed = 1;
  wizard_mana = regen(CLASS_MAGIC_USER, mana_gain);
  warrior_mana = regen(CLASS_WARRIOR, mana_gain);
  wizard_hit = regen(CLASS_MAGIC_USER, hit_gain);
  warrior_hit = regen(CLASS_WARRIOR, hit_gain);
  TEST_ASSERT_EQUAL_INT(2 * warrior_mana, wizard_mana);
  TEST_ASSERT_EQUAL_INT(wizard_mana, regen(CLASS_SORCERER, mana_gain));
  TEST_ASSERT_EQUAL_INT(wizard_mana, regen(CLASS_NECROMANCER, mana_gain));
  TEST_ASSERT_EQUAL_INT(regen(CLASS_CLERIC, mana_gain), regen(CLASS_DRUID, mana_gain));
  TEST_ASSERT_EQUAL_INT(warrior_mana, regen(CLASS_PALADIN, mana_gain));
  TEST_ASSERT_EQUAL_INT(wizard_hit, regen(CLASS_SORCERER, hit_gain));
  TEST_ASSERT_EQUAL_INT(regen(CLASS_CLERIC, hit_gain), regen(CLASS_DRUID, hit_gain));
  TEST_ASSERT_EQUAL_INT(warrior_hit, regen(CLASS_BERSERKER, hit_gain));
  age_allowed = 0;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_orphaned_corpse_contents_reported);
  RUN_TEST(test_increase_gold_near_cap_clamps_loudly);
  RUN_TEST(test_increase_bank_near_cap_clamps_loudly);
  RUN_TEST(test_decrease_int_min_does_not_overflow);
  RUN_TEST(test_in_range_changes_are_silent);
  RUN_TEST(test_timer_script_purging_next_object);
  RUN_TEST(test_unexpired_timers_count_down);
  RUN_TEST(test_gain_exp_names_the_player);
  RUN_TEST(test_regeneration_follows_class_archetype);
  return UNITY_END();
}
