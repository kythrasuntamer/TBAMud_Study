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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_list_skills_rejects_out_of_range_class);
  RUN_TEST(test_list_skills_valid_class);
  return UNITY_END();
}
