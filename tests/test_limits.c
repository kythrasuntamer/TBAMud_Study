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
void Crash_idlesave(struct char_data *ch) { (void)ch; UNREACHED(); }
void Crash_rentsave(struct char_data *ch, int cost) { (void)ch; (void)cost; UNREACHED(); }
struct time_info_data *age(struct char_data *ch) { (void)ch; UNREACHED(); return &time_info; }
int damage(struct char_data *ch, struct char_data *victim, int dam, int attacktype)
{ (void)ch; (void)victim; (void)dam; (void)attacktype; UNREACHED(); return 0; }
void stop_fighting(struct char_data *ch) { (void)ch; UNREACHED(); }
void update_char_objects(struct char_data *ch) { (void)ch; UNREACHED(); }
void update_pos(struct char_data *victim) { (void)victim; UNREACHED(); }
/* Corpse contents: these tests use empty corpses. */
void obj_from_obj(struct obj_data *obj) { (void)obj; UNREACHED(); }
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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_increase_gold_near_cap_clamps_loudly);
  RUN_TEST(test_increase_bank_near_cap_clamps_loudly);
  RUN_TEST(test_decrease_int_min_does_not_overflow);
  RUN_TEST(test_in_range_changes_are_silent);
  RUN_TEST(test_timer_script_purging_next_object);
  RUN_TEST(test_unexpired_timers_count_down);
  return UNITY_END();
}
