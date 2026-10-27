/* Executable adversarial tests of the production DG memory callbacks.
 * Linked against dg_triggers.c (not a copied implementation).
 * All characters, scripts and memory chains are disposable test fixtures.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_scripts.h"
#include "interpreter.h"
#include <stdlib.h>
#include <string.h>

enum attack { QUIET, FORGET_REMAINING, RELOCATE_OWNER, PURGE_OWNER,
              PURGE_ACTOR, REENTER };
static enum attack attack;
static char_data *owner, *visitor;
static struct script_data *script;
static struct trig_data *trigger;
static int callbacks, commands, nested_calls;
static int active_owner, active_visitor;

long char_script_id(char_data *ch) { return ch->script_id; }
char_data *find_char(long id)
{
  if (active_owner && owner && owner->script_id == id) return owner;
  if (active_visitor && visitor && visitor->script_id == id) return visitor;
  return NULL;
}
int rand_number(int low, int high) { (void)high; return low; }
void add_var(struct trig_var_data **vars, const char *name,
             const char *value, long context)
{ (void)vars; (void)name; (void)value; (void)context; }

static struct script_memory *remember(int count, int with_command)
{
  struct script_memory *head = NULL;
  while (count--) {
    struct script_memory *m = calloc(1, sizeof(*m));
    TEST_ASSERT_NOT_NULL(m);
    m->id = visitor->script_id;
    if (with_command) m->cmd = strdup("safe-command");
    m->next = head;
    head = m;
  }
  return head;
}

static void clear_memory(struct script_memory **head)
{
  while (*head) {
    struct script_memory *m = *head;
    *head = m->next;
    free(m->cmd);
    free(m);
  }
}

/* Models the externally visible memory-list mutations performed by callbacks. */
static void mutate(void)
{
  switch (attack) {
  case FORGET_REMAINING:
    clear_memory(&owner->memory);
    break;
  case RELOCATE_OWNER:
    owner->in_room = 1;
    break;
  case PURGE_OWNER:
    active_owner = 0;
    free(owner);
    owner = NULL;
    break;
  case PURGE_ACTOR:
    active_visitor = 0;
    free(visitor);
    visitor = NULL;
    break;
  case REENTER:
    if (!nested_calls++) entry_memory_mtrigger(owner);
    break;
  default:
    break;
  }
}
int script_driver(void *go, trig_data *t, int type, int mode)
{
  (void)go; (void)t; (void)type; (void)mode;
  callbacks++;
  mutate();
  return 1;
}
void command_interpreter(char_data *ch, char *command)
{
  (void)ch;
  TEST_ASSERT_EQUAL_STRING("safe-command", command);
  commands++;
  mutate();
}

void setUp(void)
{
  world = calloc(2, sizeof(*world));
  TEST_ASSERT_NOT_NULL(world);
  top_of_world = 1;
  owner = calloc(1, sizeof(*owner));
  visitor = calloc(1, sizeof(*visitor));
  script = calloc(1, sizeof(*script));
  trigger = calloc(1, sizeof(*trigger));
  TEST_ASSERT_NOT_NULL(owner);
  TEST_ASSERT_NOT_NULL(visitor);
  TEST_ASSERT_NOT_NULL(script);
  TEST_ASSERT_NOT_NULL(trigger);
  owner->script_id = 101;
  visitor->script_id = 102;
  owner->in_room = visitor->in_room = 0;
  GET_POS(owner) = POS_STANDING;
  GET_POS(visitor) = POS_STANDING;
  world[0].people = owner;
  owner->next_in_room = visitor;
  script->trig_list = trigger;
  trigger->trigger_type = MTRIG_MEMORY;
  trigger->narg = 100;
  owner->script = script;
  active_owner = active_visitor = 1;
  attack = QUIET;
  callbacks = commands = nested_calls = 0;
}
void tearDown(void)
{
  if (owner) {
    clear_memory(&owner->memory);
    free(owner);
  }
  if (visitor) free(visitor);
  free(trigger);
  free(script);
  free(world);
  world = NULL;
  owner = visitor = NULL;
}
static void basic(void)
{
  owner->memory = remember(1, 0);
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner->memory);
}
static void forget_callback(void)
{
  owner->memory = remember(2, 0);
  attack = FORGET_REMAINING;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner->memory);
}
static void teleport_callback(void)
{
  owner->memory = remember(2, 0);
  attack = RELOCATE_OWNER;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_EQUAL_INT(1, owner->in_room);
  TEST_ASSERT_NOT_NULL(owner->memory);
}
static void purge_owner_callback(void)
{
  owner->memory = remember(1, 0);
  attack = PURGE_OWNER;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner);
}
static void purge_actor_callback(void)
{
  owner->memory = remember(2, 0);
  attack = PURGE_ACTOR;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NOT_NULL(owner->memory);
}
static void nested_callback(void)
{
  owner->memory = remember(2, 0);
  attack = REENTER;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(2, callbacks);
  TEST_ASSERT_NULL(owner->memory);
}
static void command_forget_callback(void)
{
  owner->memory = remember(2, 1);
  attack = FORGET_REMAINING;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, commands);
  TEST_ASSERT_NULL(owner->memory);
}
int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(basic);
  RUN_TEST(forget_callback);
  RUN_TEST(teleport_callback);
  RUN_TEST(purge_owner_callback);
  RUN_TEST(purge_actor_callback);
  RUN_TEST(nested_callback);
  RUN_TEST(command_forget_callback);
  return UNITY_END();
}
