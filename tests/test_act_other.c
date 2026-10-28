/* Executable tests of the production miscellaneous player commands
 * (act.other.c). act.other.c and lists.c (group iteration) are linked with
 * unreferenced sections discarded; the fixtures below cover the rest.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "act.h"

#include "tb_error_capture.h"

/* --- Globals and functions owned by files that are not linked ---------- */
struct room_data *world = NULL;
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL;
mob_rnum top_of_mobt = 0;

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
int is_number(const char *str)
{
  if (!*str)
    return 0;
  for (; *str; str++)
    if (*str < '0' || *str > '9')
      return 0;
  return 1;
}
char *one_argument(char *argument, char *first_arg)
{
  size_t n = 0;

  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ' && n + 1 < MAX_INPUT_LENGTH)
    first_arg[n++] = *argument++;
  first_arg[n] = '\0';
  return argument;
}
int str_cmp(const char *a, const char *b) { return strcasecmp(a, b); }
void save_char(struct char_data *ch) { (void)ch; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *allocation = calloc(count, element_size);

  (void)file; (void)line; (void)func;
  if (!allocation)
    abort();
  return allocation;
}
/* Gold changes with limits.c's clamp at MAX_GOLD. */
int increase_gold(struct char_data *ch, int amount)
{
  long long g = (long long)GET_GOLD(ch) + amount;
  GET_GOLD(ch) = (int)(g > MAX_GOLD ? MAX_GOLD : g);
  return GET_GOLD(ch);
}
int decrease_gold(struct char_data *ch, int amount)
{ GET_GOLD(ch) -= amount; return GET_GOLD(ch); }
static char sent[MAX_STRING_LENGTH];
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(sent);
  int n;

  (void)ch;
  va_start(args, messg);
  n = vsnprintf(sent + used, sizeof(sent) - used, messg, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}

static struct room_data room;

void setUp(void)
{
  memset(&room, 0, sizeof(room));
  world = &room;
  sent[0] = '\0';
  tb_error_capture_reset();
}
void tearDown(void) {}

static struct char_data *make_pc(const char *name, long idnum, int gold)
{
  struct char_data *ch = calloc(1, sizeof(*ch));

  TEST_ASSERT_NOT_NULL(ch);
  ch->player_specials = calloc(1, sizeof(*ch->player_specials));
  TEST_ASSERT_NOT_NULL(ch->player_specials);
  ch->player.name = (char *)name;
  GET_IDNUM(ch) = idnum;
  GET_GOLD(ch) = gold;
  GET_POS(ch) = POS_STANDING;
  IN_ROOM(ch) = 0;
  return ch;
}

static void free_pc(struct char_data *ch)
{
  free(ch->player_specials);
  free(ch);
}

/* --- G2-11: split must not lose coins at MAX_GOLD ----------------------- */

static void split_refuses_share_past_max_gold(void)
{
  struct char_data *a = make_pc("Alice", 1, 1000);
  struct char_data *b = make_pc("Bob", 2, MAX_GOLD - 10);
  struct group_data group;
  char *arg = strdup("100");

  memset(&group, 0, sizeof(group));
  group.members = create_list();
  add_to_list(a, group.members);
  add_to_list(b, group.members);
  a->group = b->group = &group;

  do_split(a, arg, 0, 0);
  TEST_ASSERT_EQUAL_INT(1000, GET_GOLD(a));            /* nothing debited */
  TEST_ASSERT_EQUAL_INT(MAX_GOLD - 10, GET_GOLD(b));
  TEST_ASSERT_NOT_NULL(strstr(sent, "Bob cannot carry that much more gold"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("split-gold-cap", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 1 split of 100"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "recipient player idnum 2"));

  /* A share that fits is split as before. */
  sent[0] = '\0';
  tb_error_capture_reset();
  GET_GOLD(b) = 0;
  do_split(a, arg, 0, 0);
  TEST_ASSERT_EQUAL_INT(950, GET_GOLD(a));
  TEST_ASSERT_EQUAL_INT(50, GET_GOLD(b));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  free(arg);
  remove_from_list(a, group.members);
  remove_from_list(b, group.members);
  free_list(group.members);
  free_pc(a);
  free_pc(b);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(split_refuses_share_past_max_gold);
  return UNITY_END();
}
