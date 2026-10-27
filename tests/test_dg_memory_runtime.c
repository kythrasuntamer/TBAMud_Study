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
              PURGE_ACTOR, REENTER, RECREATE_MEMORY, PURGE_NEXT_TRIGGER, PURGE_TIMER_OBJECT, PURGE_TIMER_OBJECT_FALSE,
              PURGE_NEXT_TRIGGER_FALSE, PURGE_CURRENT_TRIGGER_FALSE,
              PURGE_LOAD_OBJECT_ERROR };
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

/* Standalone fixture dependencies for command-trigger diagnostics.
 * Invalid trigger-argument paths must never dereference a missing index.
 * Production engine supplies these definitions in its normal link. */
struct index_data **trig_index;
void mudlog(int type, int level, int file, const char *str, ...)
{
  (void)type; (void)level; (void)file; (void)str;
}
void skip_spaces(char **string)
{
  if (!string || !*string)
    return;
  while (**string == ' ' || **string == '\t')
    ++*string;
}

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
int obj_uid_matches(long uid, const obj_data *obj)
{
  return uid >= OBJ_ID_BASE && active_timer_object &&
         timer_object == obj && timer_object->script_id == uid;
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
  case PURGE_TIMER_OBJECT_FALSE:
  case PURGE_TIMER_OBJECT:
    if (timer_object) {
      active_timer_object = 0;
      if (world && world[0].contents == timer_object)
        world[0].contents = timer_object->next_content;
      free(timer_object);
      timer_object = NULL;
    }
    break;
  case PURGE_NEXT_TRIGGER_FALSE:
  case PURGE_NEXT_TRIGGER:
    if (next_trigger) {
      trigger->next = NULL;
      free(next_trigger->arglist);
      free(next_trigger);
      next_trigger = NULL;
    }
    break;
  case PURGE_CURRENT_TRIGGER_FALSE:
    /* Models a script which detaches the trigger that is running it. */
    if (trigger) {
      script->trig_list = trigger->next;
      free(trigger->arglist);
      free(trigger);
      trigger = NULL;
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
  (void)t; (void)type; (void)mode;
  callbacks++;
  if (attack == PURGE_LOAD_OBJECT_ERROR) {
    /* The production driver clears the caller's pointer on self-purge. */
    attack = PURGE_TIMER_OBJECT;
    mutate();
    attack = PURGE_LOAD_OBJECT_ERROR;
    *(obj_data **)go = NULL;
    return SCRIPT_ERROR_CODE;
  }
  mutate();
  return attack != PURGE_TIMER_OBJECT_FALSE &&
         attack != PURGE_NEXT_TRIGGER_FALSE &&
         attack != PURGE_CURRENT_TRIGGER_FALSE;
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
  if (next_trigger) free(next_trigger->arglist);
  free(next_trigger);
  if (trigger) free(trigger->arglist);
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
  timer_object->script_id = OBJ_ID_BASE + 4001;
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

/* Each action callback must reject an extracted object rather than trusting
 * a non-NULL local pointer following script_driver(). */
static void object_action_extraction_denied(void)
{
  setup_timer_object();
  timer_object->carried_by = owner;
  script->types = OTRIG_GIVE;
  trigger->trigger_type = OTRIG_GIVE;
  trigger->narg = 100;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(0, give_otrigger(timer_object, owner, visitor));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(timer_object);
}
static void object_drop_extraction_denied(void)
{
  setup_timer_object();
  script->types = OTRIG_DROP;
  trigger->trigger_type = OTRIG_DROP;
  trigger->narg = 100;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(0, drop_otrigger(timer_object, owner));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void object_get_extraction_denied(void)
{
  setup_timer_object();
  script->types = OTRIG_GET;
  trigger->trigger_type = OTRIG_GET;
  trigger->narg = 100;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(0, get_otrigger(timer_object, owner));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void object_wear_extraction_denied(void)
{
  setup_timer_object();
  script->types = OTRIG_WEAR;
  trigger->trigger_type = OTRIG_WEAR;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(0, wear_otrigger(timer_object, owner, 0));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void object_remove_extraction_denied(void)
{
  setup_timer_object();
  script->types = OTRIG_REMOVE;
  trigger->trigger_type = OTRIG_REMOVE;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(0, remove_otrigger(timer_object, owner));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void object_consume_extraction_denied(void)
{
  setup_timer_object();
  script->types = OTRIG_CONSUME;
  trigger->trigger_type = OTRIG_CONSUME;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(0, consume_otrigger(timer_object, owner, OCMD_EAT));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}

static void leave_object_two_triggers(void)
{
  setup_timer_object();
  world[0].contents = timer_object;
  script->types = OTRIG_LEAVE;
  trigger->trigger_type = OTRIG_LEAVE;
  trigger->narg = 100;
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_LEAVE;
  next_trigger->narg = 100;
  trigger->next = next_trigger;
  TEST_ASSERT_EQUAL_INT(1, leave_otrigger(&world[0], owner, -1));
  TEST_ASSERT_EQUAL_INT(2, callbacks);
}
static void leave_object_extracted_owner(void)
{
  setup_timer_object();
  world[0].contents = timer_object;
  script->types = OTRIG_LEAVE;
  trigger->trigger_type = OTRIG_LEAVE;
  trigger->narg = 100;
  attack = PURGE_TIMER_OBJECT;
  TEST_ASSERT_EQUAL_INT(1, leave_otrigger(&world[0], owner, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(timer_object);
}
static void leave_object_extracted_next_trigger(void)
{
  setup_timer_object();
  world[0].contents = timer_object;
  script->types = OTRIG_LEAVE;
  trigger->trigger_type = OTRIG_LEAVE;
  trigger->narg = 100;
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_LEAVE;
  next_trigger->narg = 100;
  trigger->next = next_trigger;
  attack = PURGE_NEXT_TRIGGER;
  TEST_ASSERT_EQUAL_INT(1, leave_otrigger(&world[0], owner, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}

static void setup_command_object(void)
{
  setup_timer_object();
  SET_BIT_AR(MOB_FLAGS(owner), MOB_ISNPC);
  script->types = OTRIG_COMMAND;
  trigger->trigger_type = OTRIG_COMMAND;
  trigger->narg = OCMD_ROOM;
  trigger->arglist = strdup("*");
  TEST_ASSERT_NOT_NULL(trigger->arglist);
  world[0].contents = timer_object;
}
static void command_object_baseline(void)
{
  setup_command_object();
  char cmd[] = "look", arg[] = "";
  TEST_ASSERT_EQUAL_INT(1, command_otrigger(owner, cmd, arg));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void command_object_extracted_on_false(void)
{
  setup_command_object();
  char cmd[] = "look", arg[] = "";
  attack = PURGE_TIMER_OBJECT_FALSE;
  TEST_ASSERT_EQUAL_INT(0, command_otrigger(owner, cmd, arg));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(timer_object);
}
static void command_object_removed_next_trigger(void)
{
  setup_command_object();
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_COMMAND;
  next_trigger->narg = OCMD_ROOM;
  next_trigger->arglist = strdup("*");
  TEST_ASSERT_NOT_NULL(next_trigger->arglist);
  trigger->next = next_trigger;
  char cmd[] = "look", arg[] = "";
  attack = PURGE_NEXT_TRIGGER_FALSE;
  TEST_ASSERT_EQUAL_INT(0, command_otrigger(owner, cmd, arg));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}

static void leave_actor_extracted_mid_dispatch(void)
{
  setup_timer_object();
  world[0].contents = timer_object;
  script->types = OTRIG_LEAVE;
  trigger->trigger_type = OTRIG_LEAVE;
  trigger->narg = 100;
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = OTRIG_LEAVE;
  next_trigger->narg = 100;
  trigger->next = next_trigger;
  attack = PURGE_OWNER;
  TEST_ASSERT_EQUAL_INT(1, leave_otrigger(&world[0], owner, -1));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner);
}

/* Mobile command triggers: a false-returning script may detach the trigger
 * which ran it. The handler must not follow that trigger's next link. */
static void setup_command_mob(void)
{
  SET_BIT_AR(MOB_FLAGS(owner), MOB_ISNPC);
  script->types = MTRIG_COMMAND;
  trigger->trigger_type = MTRIG_COMMAND;
  trigger->arglist = strdup("*");
  TEST_ASSERT_NOT_NULL(trigger->arglist);
}
static void command_mob_baseline(void)
{
  setup_command_mob();
  char cmd[] = "look", arg[] = "";
  TEST_ASSERT_EQUAL_INT(1, command_mtrigger(visitor, cmd, arg));
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void command_mob_detached_current_trigger(void)
{
  setup_command_mob();
  next_trigger = calloc(1, sizeof(*next_trigger));
  TEST_ASSERT_NOT_NULL(next_trigger);
  next_trigger->trigger_type = MTRIG_COMMAND;
  next_trigger->arglist = strdup("*");
  TEST_ASSERT_NOT_NULL(next_trigger->arglist);
  trigger->next = next_trigger;
  char cmd[] = "look", arg[] = "";
  attack = PURGE_CURRENT_TRIGGER_FALSE;
  TEST_ASSERT_EQUAL_INT(0, command_mtrigger(visitor, cmd, arg));
  /* The surviving trigger still receives the command, as before. */
  TEST_ASSERT_EQUAL_INT(2, callbacks);
  TEST_ASSERT_NULL(trigger);
}
/* Speech matching links is_substring()/word_check(); these fixtures follow
 * the dg_scripts.c contracts (case-insensitive search, quoted phrases). */
char *str_str(char *cs, char *ct)
{
  if (!cs || !ct || !*ct)
    return NULL;
  for (; *cs; cs++) {
    size_t i = 0;
    while (ct[i] && cs[i] && LOWER(cs[i]) == LOWER(ct[i]))
      i++;
    if (!ct[i])
      return cs;
  }
  return NULL;
}
char *matching_quote(char *p)
{
  char *start = p;
  for (p++; *p && *p != '"'; p++)
    if (*p == '\\' && p[1] != '\0')
      p++;
  if (!*p && p > start)
    p--;
  return p;
}
/* Speech triggers reacquire listeners by UID rather than next_in_room. */
static void setup_speech_mob(void)
{
  SET_BIT_AR(MOB_FLAGS(owner), MOB_ISNPC);
  script->types = MTRIG_SPEECH;
  trigger->trigger_type = MTRIG_SPEECH;
  trigger->narg = 0;
  trigger->arglist = strdup("hello");
  TEST_ASSERT_NOT_NULL(trigger->arglist);
}
static void speech_mob_baseline(void)
{
  setup_speech_mob();
  char said[] = "hello there";
  speech_mtrigger(visitor, said);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
}
static void speech_mob_listener_purged(void)
{
  setup_speech_mob();
  char said[] = "hello there";
  attack = PURGE_OWNER;
  speech_mtrigger(visitor, said);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(owner);
}

/* Load triggers: a script may "return" SCRIPT_ERROR_CODE and purge itself,
 * leaving the handler's object pointer NULL. */
struct obj_data *obj_proto;
static int proto_script_frees;
static void *proto_script_freed;
void free_proto_script(void *thing, int type)
{
  (void)type;
  proto_script_frees++;
  proto_script_freed = thing;
}
static void load_object_purged_on_error(void)
{
  struct obj_data prototype[1];
  memset(prototype, 0, sizeof(prototype));
  obj_proto = prototype;
  setup_timer_object();
  script->types = OTRIG_LOAD;
  trigger->trigger_type = OTRIG_LOAD;
  trigger->narg = 100;
  timer_object->item_number = 0;
  proto_script_frees = 0;
  proto_script_freed = NULL;
  attack = PURGE_LOAD_OBJECT_ERROR;
  load_otrigger(timer_object);
  TEST_ASSERT_EQUAL_INT(1, callbacks);
  TEST_ASSERT_NULL(timer_object);
  /* Recursion-error semantics are preserved for the original prototype. */
  TEST_ASSERT_EQUAL_INT(1, proto_script_frees);
  TEST_ASSERT_EQUAL_PTR(&prototype[0], proto_script_freed);
  obj_proto = NULL;
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
  RUN_TEST(object_action_extraction_denied);
  RUN_TEST(object_drop_extraction_denied);
  RUN_TEST(object_get_extraction_denied);
  RUN_TEST(object_wear_extraction_denied);
  RUN_TEST(object_remove_extraction_denied);
  RUN_TEST(object_consume_extraction_denied);
  RUN_TEST(leave_object_two_triggers);
  RUN_TEST(leave_object_extracted_owner);
  RUN_TEST(leave_object_extracted_next_trigger);
  RUN_TEST(command_object_baseline);
  RUN_TEST(command_object_extracted_on_false);
  RUN_TEST(command_object_removed_next_trigger);
  RUN_TEST(leave_actor_extracted_mid_dispatch);
  RUN_TEST(command_mob_baseline);
  RUN_TEST(command_mob_detached_current_trigger);
  RUN_TEST(speech_mob_baseline);
  RUN_TEST(speech_mob_listener_purged);
  RUN_TEST(load_object_purged_on_error);
  return UNITY_END();
}
