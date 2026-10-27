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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum attack { QUIET, FORGET_REMAINING, RELOCATE_OWNER, PURGE_OWNER,
              PURGE_ACTOR, REENTER, RECREATE_MEMORY, PURGE_NEXT_TRIGGER, PURGE_TIMER_OBJECT };
/* Standalone DG test executable: the full game's direction tables are
 * deliberately not linked. These definitions match constants.h's types
 * and support both valid and invalid direction test paths.
 */
const char *dirs[NUM_OF_DIRS] = {
  "north", "east", "south", "west", "up", "down",
  "northwest", "northeast", "southeast", "southwest"
};
int rev_dir[NUM_OF_DIRS] = {
  SOUTH, WEST, NORTH, EAST, DOWN, UP,
  SOUTHEAST, SOUTHWEST, NORTHWEST, NORTHEAST
};

static enum attack attack;
static char_data *owner, *visitor;
static struct script_data *script;
static struct trig_data *trigger, *next_trigger;
static int callbacks, commands, nested_calls;
static int active_owner, active_visitor;
static obj_data *timer_object;
static int active_timer_object;
static int reenter_greeting;

/* CREATE() in the production callback uses the checked allocation API.
 * Provide its test-local implementation; allocator behavior is tested elsewhere.
 */
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  if (element_size == 0 || count == 0 || count > SIZE_MAX / element_size)
    abort();
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}

long char_script_id(char_data *ch) { return ch->script_id; }
long obj_script_id(obj_data *obj) { return obj->script_id; }
int has_obj_by_uid_in_lookup_table(long uid)
{
  return active_timer_object && timer_object && timer_object->script_id == uid;
}
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
    clear_memory(&owner->memory);
    free(owner);
    owner = NULL;
    break;
  case PURGE_ACTOR:
    active_visitor = 0;
    free(visitor);
    visitor = NULL;
    break;
  case PURGE_TIMER_OBJECT:
    if (timer_object) {
      active_timer_object = 0;
      free(timer_object);
      timer_object = NULL;
    }
    break;
  case PURGE_NEXT_TRIGGER:
    if (next_trigger) {
      trigger->next = NULL;
      free(next_trigger);
      next_trigger = NULL;
    }
    break;
  case RECREATE_MEMORY:
    /* A malicious trigger keeps re-remembering the same actor. */
    if (owner && visitor) {
      struct script_memory *fresh = calloc(1, sizeof(*fresh));
      TEST_ASSERT_NOT_NULL(fresh);
      fresh->id = visitor->script_id;
      fresh->next = owner->memory;
      owner->memory = fresh;
    }
    break;
  case REENTER:
    if (!nested_calls++) {
      if (reenter_greeting) greet_memory_mtrigger(visitor);
      else entry_memory_mtrigger(owner);
    }
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
  next_trigger = NULL;
  trigger->trigger_type = MTRIG_MEMORY;
  trigger->narg = 100;
  owner->script = script;
  active_owner = active_visitor = 1;
  timer_object = NULL;
  active_timer_object = 0;
  attack = QUIET;
  callbacks = commands = nested_calls = reenter_greeting = 0;
}
void tearDown(void)
{
  free(timer_object);
  timer_object = NULL;
  if (owner) {
    clear_memory(&owner->memory);
    free(owner);
  }
  if (visitor) free(visitor);
  free(next_trigger);
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

/* Greeting uses visitor as the arriving actor and owner as the remembered NPC.
 * The command path is exercised without depending on room lighting.
 */
/* Greet visibility diagnostics depend on these game-wide services.
 * The fixture supplies deterministic light and a no-op diagnostic sink.
 */
void basic_mud_log(const char *format, ...)
{
  (void)format;
}
int room_is_dark(room_rnum room)
{
  (void)room;
  return 0;
}
int valid_dg_target(char_data *ch, int flags)
{
  (void)flags;
  return ch != NULL;
}
static void greet_command_baseline(void)
{
  owner->memory = remember(1, 1);
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, commands);
  /* Unlike entry triggers, greet commands retain their memory node. */
  TEST_ASSERT_NOT_NULL(owner->memory);
}
static void greet_command_forget(void)
{
  owner->memory = remember(2, 1);
  attack = FORGET_REMAINING;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, commands);
  TEST_ASSERT_NULL(owner->memory);
}
static void greet_command_teleport(void)
{
  owner->memory = remember(1, 1);
  attack = RELOCATE_OWNER;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, commands);
  TEST_ASSERT_EQUAL_INT(1, owner->in_room);
}
static void greet_command_owner_purge(void)
{
  owner->memory = remember(1, 1);
  attack = PURGE_OWNER;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, commands);
  TEST_ASSERT_NULL(owner);
}
static void greet_command_actor_purge(void)
{
  owner->memory = remember(1, 1);
  attack = PURGE_ACTOR;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, commands);
  TEST_ASSERT_NULL(visitor);
}

/* The script-driven greet path uses CAN_SEE, so use two visible NPCs
 * instead of relying on the dummy player-specials state in this fixture.
 */
static void prepare_greet_script(int count)
{
  SET_BIT_AR(MOB_FLAGS(owner), MOB_ISNPC);
  SET_BIT_AR(MOB_FLAGS(visitor), MOB_ISNPC);
  owner->memory = remember(count, 0);
  reenter_greeting = 1;
}
static void greet_script_baseline(void)
{
  prepare_greet_script(1);
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_EQUAL_INT(0, commands);
  TEST_ASSERT_NULL(owner->memory);
}
static void greet_script_forget(void)
{
  prepare_greet_script(2);
  attack = FORGET_REMAINING;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner->memory);
}
static void greet_script_teleport(void)
{
  prepare_greet_script(2);
  attack = RELOCATE_OWNER;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_EQUAL_INT(1, owner->in_room);
  TEST_ASSERT_NOT_NULL(owner->memory);
}
static void greet_script_owner_purge(void)
{
  prepare_greet_script(2);
  attack = PURGE_OWNER;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner);
}
static void greet_script_actor_purge(void)
{
  prepare_greet_script(2);
  attack = PURGE_ACTOR;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(visitor);
  TEST_ASSERT_NOT_NULL(owner->memory);
}
static void greet_script_reentrant(void)
{
  prepare_greet_script(2);
  attack = REENTER;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(2, callbacks);
  TEST_ASSERT_NULL(owner->memory);
}

/* A callback continually recreating memory must not keep this
 * dispatch running forever; the newly remembered item waits until next time.
 */
static void entry_script_recreate_bounded(void)
{
  owner->memory = remember(1, 0);
  attack = RECREATE_MEMORY;
  entry_memory_mtrigger(owner);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NOT_NULL(owner->memory);
}
static void greet_script_recreate_bounded(void)
{
  prepare_greet_script(1);
  attack = RECREATE_MEMORY;
  greet_memory_mtrigger(visitor);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NOT_NULL(owner->memory);
}

/* Ordinary greeting dispatch must survive callbacks that invalidate the
 * room occupant or the next trigger, without losing the return contract.
 */
static void setup_ordinary_greet(void)
{
  SET_BIT_AR(MOB_FLAGS(owner), MOB_ISNPC);
  SET_BIT_AR(MOB_FLAGS(visitor), MOB_ISNPC);
  script->types = MTRIG_GREET;
  trigger->trigger_type = MTRIG_GREET;
}
static void ordinary_greet_baseline(void)
{
  setup_ordinary_greet();
  TEST_ASSERT_EQUAL_INT(1, greet_mtrigger(visitor, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void ordinary_greet_owner_purge(void)
{
  setup_ordinary_greet();
  attack = PURGE_OWNER;
  TEST_ASSERT_EQUAL_INT(1, greet_mtrigger(visitor, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner);
}
static void ordinary_greet_actor_purge(void)
{
  setup_ordinary_greet();
  attack = PURGE_ACTOR;
  TEST_ASSERT_EQUAL_INT(1, greet_mtrigger(visitor, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(visitor);
}
static void ordinary_greet_owner_teleport(void)
{
  setup_ordinary_greet();
  attack = RELOCATE_OWNER;
  TEST_ASSERT_EQUAL_INT(1, greet_mtrigger(visitor, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_EQUAL_INT(1, owner->in_room);
}
static void ordinary_greet_extracted_next_trigger(void)
{
  setup_ordinary_greet();
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = MTRIG_GREET;
  next_trigger->narg = 100;
  trigger->next = next_trigger;
  attack = PURGE_NEXT_TRIGGER;
  TEST_ASSERT_EQUAL_INT(1, greet_mtrigger(visitor, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(next_trigger);
}

static void setup_timer_object(void)
{
  timer_object = calloc(1, sizeof(*timer_object));
  TEST_ASSERT_NOT_NULL(timer_object);
  timer_object->script_id = 4001;
  timer_object->script = script;
  active_timer_object = 1;
  script->types = OTRIG_TIMER;
  trigger->trigger_type = OTRIG_TIMER;
}
static void timer_object_two_triggers(void)
{
  setup_timer_object();
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_TIMER;
  trigger->next = next_trigger;
  timer_otrigger(timer_object);
  TEST_ASSERT_EQUAL_INT(2, callbacks);
}
static void timer_object_extracted_next_trigger(void)
{
  setup_timer_object();
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_TIMER;
  trigger->next = next_trigger;
  attack = PURGE_NEXT_TRIGGER;
  timer_otrigger(timer_object);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void timer_object_extracted_owner(void)
{
  setup_timer_object();
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_TIMER;
  trigger->next = next_trigger;
  attack = PURGE_TIMER_OBJECT;
  timer_otrigger(timer_object);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(timer_object);
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
  RUN_TEST(greet_command_baseline);
  RUN_TEST(greet_command_forget);
  RUN_TEST(greet_command_teleport);
  RUN_TEST(greet_command_owner_purge);
  RUN_TEST(greet_command_actor_purge);
  RUN_TEST(greet_script_baseline);
  RUN_TEST(greet_script_forget);
  RUN_TEST(greet_script_teleport);
  RUN_TEST(greet_script_owner_purge);
  RUN_TEST(greet_script_actor_purge);
  RUN_TEST(greet_script_reentrant);
  RUN_TEST(entry_script_recreate_bounded);
  RUN_TEST(greet_script_recreate_bounded);
  RUN_TEST(ordinary_greet_baseline);
  RUN_TEST(ordinary_greet_owner_purge);
  RUN_TEST(ordinary_greet_actor_purge);
  RUN_TEST(ordinary_greet_owner_teleport);
  RUN_TEST(ordinary_greet_extracted_next_trigger);
  RUN_TEST(timer_object_two_triggers);
  RUN_TEST(timer_object_extracted_next_trigger);
  RUN_TEST(timer_object_extracted_owner);
  return UNITY_END();
}
