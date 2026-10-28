/* Executable tests of production spec procs (spec_procs.c, spec_assign.c):
 * practice tables against pfile class values, the skill-list truncation
 * marker, the pet shop's storage room, and spec-proc names for 'stat'.
 * The world and the game functions they reach are fixtures.
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
#include "spells.h"
#include "class.h"
#include "spec_procs.h"
#include <stdarg.h>
#include "tb_error_capture.h"

/* class.c / spell_parser.c tables, sized as in production. */
int prac_params[4][NUM_CLASSES];
struct spell_info_type spell_info[TOP_SPELL_DEFINE + 1];
int spell_sort_info[MAX_SKILLS + 1];
struct command_info *complete_cmd_info;

static char sent[MAX_STRING_LENGTH * 2];
static int pages;
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(sent, sizeof(sent), messg, args) < 0)
    sent[0] = '\0';
  va_end(args);
  return strlen(sent);
}
static char paged[MAX_STRING_LENGTH + 1];
void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  pages++;
  strlcpy(paged, str, sizeof(paged));
}
void basic_mud_log(const char *format, ...) { (void)format; }

static struct char_data student;
static struct player_special_data student_specials;

void setUp(void)
{
  int k;

  for (k = 0; k <= TOP_SPELL_DEFINE; k++)
    spell_info[k].name = "a skill";
  for (k = 0; k <= MAX_SKILLS; k++)
    spell_sort_info[k] = k;
  memset(&student, 0, sizeof(student));
  memset(&student_specials, 0, sizeof(student_specials));
  student.player_specials = &student_specials;
  GET_IDNUM(&student) = 12;
  GET_LEVEL(&student) = 10;
  sent[0] = paged[0] = '\0';
  pages = 0;
  tb_error_capture_reset();
}
void tearDown(void) {}

/* H3-18: a pfile with 'Clas: 42' must not index prac_params[][42] or
 * spell_info[].min_level[42]; 'practice' reports and refuses. */
static void test_list_skills_rejects_out_of_range_class(void)
{
  GET_CLASS(&student) = 42;
  list_skills(&student);
  TEST_ASSERT_EQUAL_INT(0, pages);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("class-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 12"));
}

static void test_list_skills_valid_class(void)
{
  GET_CLASS(&student) = CLASS_WARRIOR;
  list_skills(&student);
  TEST_ASSERT_EQUAL_INT(1, pages);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H3-20: a list longer than buf2 used to be cut silently (the overflow
 * marker branch was unreachable). It must carry the marker and report. */
static void test_list_skills_truncation_is_marked(void)
{
  static char long_name[301];
  int k;

  memset(long_name, 'x', sizeof(long_name) - 1);
  for (k = 0; k <= TOP_SPELL_DEFINE; k++)
    spell_info[k].name = long_name;
  GET_CLASS(&student) = CLASS_WARRIOR;

  list_skills(&student);

  TEST_ASSERT_EQUAL_INT(1, pages);
  TEST_ASSERT_NOT_NULL(strstr(paged, "**OVERFLOW**"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("skill-list-truncated", tb_captured_operation);
}

/* 'buy' path: not exercised (the test only lists). */
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{ (void)first_arg; (void)second_arg; UNREACHED(); return argument; }
struct char_data *get_char_room(char *name, int *num, room_rnum room)
{ (void)name; (void)num; (void)room; UNREACHED(); return NULL; }
int decrease_gold(struct char_data *ch, int amt) { (void)ch; (void)amt; UNREACHED(); return 0; }
struct char_data *read_mobile(mob_vnum nr, int type) { (void)nr; (void)type; UNREACHED(); return NULL; }
void add_follower(struct char_data *ch, struct char_data *leader) { (void)ch; (void)leader; UNREACHED(); }

room_rnum real_room(room_vnum vnum)
{
  room_rnum r;
  for (r = 0; world && r <= top_of_world; r++)
    if (world[r].number == vnum)
      return r;
  return NOWHERE;
}

/* H3-21: pet shop vnum 3031 whose storage room 3032 was deleted, so rnum + 1
 * is room 3033 with an unrelated NPC in it. 'list' must not offer it. */
static void test_pet_shop_without_storage_room_reports(void)
{
  static struct room_data rooms[2];
  static struct command_info cmds[] = { { "list" }, { "buy" }, { "\n" } };
  static struct index_data mobidx[1];
  struct char_data shopper, npc;
  char arg[MAX_INPUT_LENGTH] = "";

  memset(rooms, 0, sizeof(rooms));
  memset(&npc, 0, sizeof(npc));
  rooms[0].number = 3031;
  rooms[1].number = 3033;
  world = rooms;
  top_of_world = 1;
  complete_cmd_info = cmds;
  mob_index = mobidx;
  top_of_mobt = 0;
  SET_BIT_AR(MOB_FLAGS(&npc), MOB_ISNPC);
  npc.player.short_descr = (char *)"the cityguard";
  rooms[1].people = &npc;
  shopper = student;
  shopper.in_room = 0;

  TEST_ASSERT_TRUE(pet_shops(&shopper, NULL, 0, arg));
  TEST_ASSERT_NULL(strstr(sent, "cityguard"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("pet-room-missing", tb_captured_operation);
  world = NULL;
  mob_index = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_pet_shop_without_storage_room_reports);
  RUN_TEST(test_list_skills_truncation_is_marked);
  RUN_TEST(test_list_skills_rejects_out_of_range_class);
  RUN_TEST(test_list_skills_valid_class);
  return UNITY_END();
}
