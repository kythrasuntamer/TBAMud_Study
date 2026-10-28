/* Executable tests of production mob script commands (dg_mobcmd.c).
 * Handler functions are minimal stand-ins over zeroed fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "dg_scripts.h"
#include "tb_error_capture.h"

static char_data mob;
static obj_data sword, axe, helm;
static int junked;

int find_all_dots(char *arg)
{
  if (!strcmp(arg, "all"))
    return FIND_ALL;
  if (!strncmp(arg, "all.", 4)) {
    memmove(arg, arg + 4, strlen(arg + 4) + 1);
    return FIND_ALLDOT;
  }
  return FIND_INDIV;
}
/* One word in, one word out: enough for these single-word arguments. */
char *one_argument(char *argument, char *first_arg)
{
  strlcpy(first_arg, argument, MAX_INPUT_LENGTH);
  return argument + strlen(argument);
}
void script_log(const char *format, ...) { (void)format; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}
int isname(const char *str, const char *namelist) { return !strcmp(str, namelist); }
void extract_obj(struct obj_data *obj)
{
  struct obj_data **link;

  for (link = &mob.carrying; *link; link = &(*link)->next_content)
    if (*link == obj) {
      *link = obj->next_content;
      break;
    }
  junked++;
}
struct obj_data *unequip_char(struct char_data *ch, int pos)
{
  struct obj_data *obj = GET_EQ(ch, pos);
  GET_EQ(ch, pos) = NULL;
  return obj;
}
int get_obj_pos_in_equip_vis(struct char_data *ch, char *arg, int *number,
                             struct obj_data *equipment[])
{
  int pos;
  (void)ch; (void)number;
  for (pos = 0; pos < NUM_WEARS; pos++)
    if (equipment[pos] && !strcmp(arg, equipment[pos]->name))
      return pos;
  return -1;
}
struct obj_data *get_obj_in_list_vis(struct char_data *ch, char *name,
                                     int *number, struct obj_data *list)
{
  (void)ch; (void)number;
  for (; list; list = list->next_content)
    if (!strcmp(name, list->name))
      return list;
  return NULL;
}

void setUp(void)
{
  memset(&mob, 0, sizeof(mob));
  memset(&sword, 0, sizeof(sword));
  memset(&axe, 0, sizeof(axe));
  memset(&helm, 0, sizeof(helm));
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_ISNPC);
  GET_MOB_RNUM(&mob) = NOBODY;            /* no prototype */
  sword.name = "sword";
  axe.name = "ax";
  helm.name = "ax";               /* worn, same name as the carried axe */
  sword.next_content = &axe;
  mob.carrying = &sword;
  GET_EQ(&mob, WEAR_HEAD) = &helm;
  junked = 0;
}
void tearDown(void) {}

static void run(const char *what)
{
  char arg[MAX_INPUT_LENGTH];
  strlcpy(arg, what, sizeof(arg));
  do_mjunk(&mob, arg, 0, 0);
}

static void run_follow(const char *who)
{
  char arg[MAX_INPUT_LENGTH];
  strlcpy(arg, who, sizeof(arg));
  do_mfollow(&mob, arg, 0, 0);
}

static void plain_name_junks_one_item(void)
{
  run("ax");
  TEST_ASSERT_EQUAL_INT(1, junked);       /* the worn one is found first */
  TEST_ASSERT_NULL(GET_EQ(&mob, WEAR_HEAD));
  TEST_ASSERT_EQUAL_PTR(&sword, mob.carrying);
  TEST_ASSERT_EQUAL_PTR(&axe, sword.next_content);
}

static void all_dot_name_junks_every_match(void)
{
  run("all.ax");
  TEST_ASSERT_EQUAL_INT(2, junked);
  TEST_ASSERT_NULL(GET_EQ(&mob, WEAR_HEAD));
  TEST_ASSERT_EQUAL_PTR(&sword, mob.carrying);
  TEST_ASSERT_NULL(sword.next_content);
}

static void all_junks_everything(void)
{
  run("all");
  TEST_ASSERT_EQUAL_INT(3, junked);
  TEST_ASSERT_NULL(mob.carrying);
  TEST_ASSERT_NULL(GET_EQ(&mob, WEAR_HEAD));
}

/* mfollow fixtures: names resolve to the two leaders. */
static char_data old_leader, new_leader;
struct char_data *get_char_vis(struct char_data *ch, char *name, int *number, int where)
{
  (void)ch; (void)number; (void)where;
  return !strcmp(name, "new") ? &new_leader : NULL;
}
/* DG_FIND_CHAR_ROOM() looks names up in the mob's room; same fixture. */
struct char_data *get_char_room_vis(struct char_data *ch, char *name, int *number)
{
  return get_char_vis(ch, name, number, FIND_CHAR_ROOM);
}
struct char_data *get_char(char *name) { (void)name; return NULL; }
bool circle_follow(struct char_data *ch, struct char_data *victim)
{ (void)ch; (void)victim; return FALSE; }

/* The old unlink walked k->next->follower until it found the mob, and
 * dereferenced NULL when the master's list did not hold it. */
static void mfollow_tolerates_an_inconsistent_follower_list(void)
{
  memset(&old_leader, 0, sizeof(old_leader));
  memset(&new_leader, 0, sizeof(new_leader));
  tb_error_capture_reset();
  mob.master = &old_leader;          /* but not on old_leader's list */

  run_follow("new");

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("follower-list", tb_captured_operation);
  TEST_ASSERT_EQUAL_PTR(&new_leader, mob.master);
  TEST_ASSERT_EQUAL_PTR(&mob, new_leader.followers->follower);
  free(new_leader.followers);
}

static void mfollow_moves_between_leaders(void)
{
  struct follow_type *entry = calloc(1, sizeof(*entry));

  memset(&old_leader, 0, sizeof(old_leader));
  memset(&new_leader, 0, sizeof(new_leader));
  tb_error_capture_reset();
  entry->follower = &mob;
  old_leader.followers = entry;
  mob.master = &old_leader;

  run_follow("new");

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NULL(old_leader.followers);
  TEST_ASSERT_EQUAL_PTR(&new_leader, mob.master);
  free(new_leader.followers);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(plain_name_junks_one_item);
  RUN_TEST(all_dot_name_junks_every_match);
  RUN_TEST(all_junks_everything);
  RUN_TEST(mfollow_tolerates_an_inconsistent_follower_list);
  RUN_TEST(mfollow_moves_between_leaders);
  return UNITY_END();
}
