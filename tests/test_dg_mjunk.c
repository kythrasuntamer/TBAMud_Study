/* Executable test of the production mjunk command (dg_mobcmd.c): plain
 * names, "all.<name>" and "all" each junk what they name.  The branch test
 * was inverted, so a plain name took the "all." path and read past its
 * terminator.  Handler functions are minimal stand-ins over fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "dg_scripts.h"

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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(plain_name_junks_one_item);
  RUN_TEST(all_dot_name_junks_every_match);
  RUN_TEST(all_junks_everything);
  return UNITY_END();
}
