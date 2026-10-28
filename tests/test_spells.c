/* Executable tests of the production manual spells (spells.c). spells.c is
 * linked as is; the drink-container helpers it calls (act.item.c) and the
 * DG UID accessor are fixtures that record how they were called.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "spells.h"
#include "handler.h"
#include "db.h"
#include "act.h"
#include "dg_scripts.h"
#include "tb_error_capture.h"

int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
long obj_script_id(obj_data *obj) { return obj->script_id; }

/* act.item.c's name helpers index drinknames[] with value 2. */
static int name_from_calls, name_to_calls, last_name_to_type;
void name_from_drinkcon(struct obj_data *obj) { (void)obj; name_from_calls++; }
void name_to_drinkcon(struct obj_data *obj, int type)
{ (void)obj; name_to_calls++; last_name_to_type = type; }
void weight_change_object(struct obj_data *obj, int weight)
{ (void)obj; (void)weight; }

static struct char_data caster;
static struct obj_data skin;

void setUp(void)
{
  memset(&caster, 0, sizeof(caster));
  memset(&skin, 0, sizeof(skin));
  GET_IDNUM(&caster) = 12;
  skin.item_number = NOTHING;
  skin.script_id = 7001;
  GET_OBJ_TYPE(&skin) = ITEM_DRINKCON;
  GET_OBJ_VAL(&skin, 0) = 10;     /* capacity */
  name_from_calls = name_to_calls = 0;
  last_name_to_type = -1;
  tb_error_capture_reset();
}

void tearDown(void) { }

/* H1-07: a waterskin whose liquid was set to 40 ('osetval 2 40' or a world
 * file) reached name_from_drinkcon(), which reads drinknames[40]. Create
 * water must refuse it, name the object and the caster, and leave it. */
static void test_create_water_refuses_bad_liquid(void)
{
  GET_OBJ_VAL(&skin, 1) = 3;
  GET_OBJ_VAL(&skin, 2) = 40;
  spell_create_water(10, &caster, NULL, &skin);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("liquid-type-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "uid 7001 liquid 40"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 12"));
  TEST_ASSERT_EQUAL_INT(0, name_from_calls);
  TEST_ASSERT_EQUAL_INT(0, name_to_calls);
  TEST_ASSERT_EQUAL_INT(40, GET_OBJ_VAL(&skin, 2));
  TEST_ASSERT_EQUAL_INT(3, GET_OBJ_VAL(&skin, 1));

  /* An empty container takes the other branch; negative values too. */
  GET_OBJ_VAL(&skin, 1) = 0;
  GET_OBJ_VAL(&skin, 2) = -1;
  spell_create_water(10, &caster, NULL, &skin);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(0, name_from_calls);
}

/* Control: a valid empty waterskin is filled with water, silently. */
static void test_create_water_fills_valid_container(void)
{
  GET_OBJ_VAL(&skin, 1) = 0;
  GET_OBJ_VAL(&skin, 2) = LIQ_WATER;
  spell_create_water(10, &caster, NULL, &skin);

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(10, GET_OBJ_VAL(&skin, 1));
  TEST_ASSERT_EQUAL_INT(1, name_to_calls);
  TEST_ASSERT_EQUAL_INT(LIQ_WATER, last_name_to_type);
}

/* Coordinator item: summon finds its victim by name across the world, so
 * a character stranded in NOWHERE (left there by an earlier, reported
 * char_to_room() refusal) reached zone_table[NOWHERE] and then
 * char_from_room()'s abort. Recall and teleport move their victim the same
 * way. Each must refuse and report. */
static int moves;
void char_from_room_at(const char *src_file, int src_line,
                       const char *src_func, struct char_data *ch)
{ (void)src_file; (void)src_line; (void)src_func; (void)ch; moves++; }
void char_to_room_at(const char *src_file, int src_line, const char *src_func,
                     struct char_data *ch, room_rnum room)
{ (void)src_file; (void)src_line; (void)src_func; (void)ch; (void)room; moves++; }

struct zone_data *zone_table = NULL;
void entry_memory_mtrigger(char_data *ch) { (void)ch; }
int greet_mtrigger(char_data *actor, int dir) { (void)actor; (void)dir; return 1; }
void greet_memory_mtrigger(char_data *actor) { (void)actor; }
int mag_savingthrow(struct char_data *ch, int type, int modifier)
{ (void)ch; (void)type; (void)modifier; return 0; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
int rand_number_at(int from, int to, const char *source_file, int source_line,
                   const char *source_func)
{ (void)to; (void)source_file; (void)source_line; (void)source_func;
  return from; }

static void test_moving_spells_refuse_a_stranded_victim(void)
{
  struct char_data victim;

  memset(&victim, 0, sizeof(victim));
  victim.player_specials = &dummy_mob;
  GET_IDNUM(&victim) = 77;
  IN_ROOM(&victim) = NOWHERE;
  moves = 0;

  spell_summon(30, &caster, &victim, NULL);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("victim-nowhere", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 77"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "summon by player idnum 12"));

  spell_recall(30, &caster, &victim, NULL);
  spell_teleport(30, &caster, &victim, NULL);
  TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(0, moves);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_create_water_refuses_bad_liquid);
  RUN_TEST(test_create_water_fills_valid_container);
  RUN_TEST(test_moving_spells_refuse_a_stranded_victim);
  return UNITY_END();
}
