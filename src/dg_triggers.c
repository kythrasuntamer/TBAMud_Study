/**************************************************************************
*  File: dg_triggers.c                                     part of tbaMUD *
*  Usage: Contains all the trigger functions for scripts.                 *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Death's Gate MUD is based on CircleMUD, Copyright (C) 1993, 94.        *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
*                                                                         *
*  $Author: galion/Mark A. Heilpern/egreen/Welcor $                       *
*  $Date: 2004/10/11 12:07:00$                                            *
*  $Revision: 1.0.14 $                                                    *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "dg_scripts.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "oasis.h"
#include "constants.h"
#include "spells.h"  /* for skill_name() */
#include "act.h"    /* for cmd_door[] */

/* General functions used by several triggers. */

/* Copy first phrase into first_arg, returns rest of string. */
char *one_phrase(char *arg, char *first_arg, size_t first_arg_size)
{
    skip_spaces(&arg);

    if (!*arg)
        *first_arg = '\0';

    else if (*arg == '"')
    {
        char *p, c;

        p = matching_quote(arg);
        c = *p;
        *p = '\0';
        strlcpy(first_arg, arg + 1, first_arg_size);
        if (c == '\0')
            return p;
        else
            return p + 1;
    }

    else
    {
        char *s, *p;

        s = first_arg;
        p = arg;

        while (*p && !isspace(*p) && *p != '"')
            *s++ = *p++;

        *s = '\0';
        return p;
    }

    return arg;
}

int is_substring(char *sub, char *string)
{
    char *s;

    if ((s = str_str(string, sub)))
    {
        int len = strlen(string);
        int sublen = strlen(sub);

        /* check front */
        if ((s == string || isspace(*(s - 1)) || ispunct(*(s - 1))) &&

            /* check end */
            ((s + sublen == string + len) || isspace(s[sublen]) ||
             ispunct(s[sublen])))
            return 1;
    }

    return 0;
}

/* Return 1 if str contains a word or phrase from wordlist. Phrases are in
 * double quotes ("). if wrdlist is NULL, then return 1, if str is NULL,
 * return 0. */
int word_check(char *str, char *wordlist)
{
    char words[MAX_INPUT_LENGTH], phrase[MAX_INPUT_LENGTH], *s;

    if (*wordlist=='*') return 1;

    strlcpy(words, wordlist, sizeof(words));

    for (s = one_phrase(words, phrase, sizeof(phrase)); *phrase; s = one_phrase(s, phrase, sizeof(phrase)))
        if (is_substring(phrase, str))
            return 1;

    return 0;
}

/*Mob triggers. */
void random_mtrigger(char_data *ch)
{
  trig_data *t;

  /* This trigger is only called if a char is in the zone without nohassle. */
  if (!SCRIPT_CHECK(ch, MTRIG_RANDOM) || AFF_FLAGGED(ch, AFF_CHARM))
    return;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_RANDOM) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

void bribe_mtrigger(char_data *ch, char_data *actor, int amount)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(ch, MTRIG_BRIBE) || AFF_FLAGGED(ch, AFF_CHARM))
    return;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_BRIBE) && (amount >= GET_TRIG_NARG(t))) {
      tb_snprintf(buf, sizeof(buf), "%d", amount);
      add_var(&GET_TRIG_VARS(t), "amount", buf, 0);
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

/* Snapshot script IDs rather than holding occupant-list links across callbacks.
 * Callback-driven teleports or extraction may invalidate next_in_room.
 * Each saved ID is re-resolved before it is dereferenced.
 */
struct dg_memory_target {
  long uid;
  struct dg_memory_target *next;
};

static struct dg_memory_target *dg_memory_targets(room_rnum room)
{
  struct dg_memory_target *head = NULL, **tail = &head;
  char_data *person;
  for (person = world[room].people; person; person = person->next_in_room) {
    struct dg_memory_target *node;
    CREATE(node, struct dg_memory_target, 1);
    node->uid = char_script_id(person);
    node->next = NULL;
    *tail = node;
    tail = &node->next;
  }
  return head;
}

static void dg_free_memory_targets(struct dg_memory_target *head)
{
  while (head) {
    struct dg_memory_target *next = head->next;
    free(head);
    head = next;
  }
}

/* A script UID can remain registered until deferred character extraction.
 * Check the pending-extraction flag as well as identity and room. */
static int dg_memory_character_live(char_data *ch, long uid, room_rnum room)
{
  return ch && find_char(uid) == ch &&
         !((IS_NPC(ch) && MOB_FLAGGED(ch, MOB_NOTDEADYET)) ||
           (!IS_NPC(ch) && PLR_FLAGGED(ch, PLR_NOTDEADYET))) &&
         IN_ROOM(ch) == room;
}

/* Mobile trigger handlers run only for a character standing in a valid room.
 * Anything else is a caller bug: report it with the handler name and stop. */
static int dg_trigger_actor_placed(const char_data *ch, const char *handler)
{
  if (ch && world && (long long)IN_ROOM(ch) >= 0 &&
      VALID_ROOM_RNUM(IN_ROOM(ch)))
    return TRUE;
  TB_ERROR_MSG("dg", "trigger-dispatch", handler,
               "invariant violated: character %p uid=%ld is not in a valid "
               "room (rnum %ld, world %s); no triggers dispatched",
               (const void *)ch, ch ? ch->script_id : 0L,
               ch ? (long) IN_ROOM(ch) : -1L, world ? "loaded" : "NULL");
  return FALSE;
}

void greet_memory_mtrigger(char_data *actor)
{
  trig_data *t;
  char_data *ch;
  struct script_memory *mem;
  char buf[MAX_INPUT_LENGTH];
  int command_performed = 0;
  struct dg_memory_target *targets, *target;
  long actor_uid;

  if (!dg_trigger_actor_placed(actor, "greet_memory_mtrigger") ||
      !valid_dg_target(actor, DG_ALLOW_GODS))
    return;

  const room_rnum origin_room = IN_ROOM(actor);
  actor_uid = char_script_id(actor);
  targets = dg_memory_targets(origin_room);

  for (target = targets; target; target = target->next) {
    if (!dg_memory_character_live(actor, actor_uid, origin_room))
      break;
    ch = find_char(target->uid);
    if (!dg_memory_character_live(ch, target->uid, origin_room))
      continue;
    if (!SCRIPT_MEM(ch) || !AWAKE(ch) || FIGHTING(ch) || (ch == actor) ||
        AFF_FLAGGED(ch, AFF_CHARM))
      continue;
    /* Limit this dispatch to the matching memories present on entry.
     * A callback may remember the actor again indefinitely; newly created
     * memories belong to a subsequent dispatch, not this loop.
     */
    size_t remaining = 0;
    for (mem = SCRIPT_MEM(ch); mem; mem = mem->next)
      if (mem->id == actor_uid)
        ++remaining;
    /* Only access live list nodes; callbacks may edit this memory chain. */
    while (remaining > 0 &&
           dg_memory_character_live(actor, actor_uid, origin_room) &&
           dg_memory_character_live(ch, target->uid, origin_room) &&
           SCRIPT_MEM(ch)) {
      struct script_memory **link = &SCRIPT_MEM(ch);
      long actor_id = char_script_id(actor);
      while (*link && (*link)->id != actor_id)
        link = &(*link)->next;
      if (!*link)
        break;
      mem = *link;
      --remaining;
      if (mem->cmd) {
        long owner_uid = char_script_id(ch);
        /* The command interpreter may delete the memory node supplying
         * this string. Own a private copy for the whole invocation. */
        char *command_copy = strdup(mem->cmd);
        if (!command_copy)
          goto cleanup;
        command_interpreter(ch, command_copy);
        free(command_copy); /* command memory itself stays linked */
        /* The callback can remove or relocate the owner. The command node
         * may also have been freed, so do not inspect it afterward. */
        if (!dg_memory_character_live(ch, owner_uid, origin_room))
          goto cleanup;
        command_performed = 1;
        break;
      }
      /* Detach before executing the memory script: no post-callback search
       * for a node that may have been removed by another script. */
      *link = mem->next;
      mem->next = NULL;
      if (!command_performed && SCRIPT(ch)) {
        for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
          if (IS_SET(GET_TRIG_TYPE(t), MTRIG_MEMORY) &&
              CAN_SEE(ch, actor) && !GET_TRIG_DEPTH(t) &&
              rand_number(1, 100) <= GET_TRIG_NARG(t)) {
            ADD_UID_VAR(buf, t, actor_id, "actor", 0);
            long owner_uid = char_script_id(ch);
            script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
            /* The callback can purge or replace the owner. Do not use a
             * stale room-occupant or script-memory pointer afterward. */
            if (!dg_memory_character_live(ch, owner_uid, origin_room)) {
              free(mem);
              goto cleanup;
            }
            break;
          }
        }
      }
      free(mem);
    }
  }
cleanup:
  dg_free_memory_targets(targets);
}

/* A pointer captured before a DG callback must be checked against the
 * current live trigger list before dereferencing it. Pointer comparison
 * does not dereference a trigger which has since been extracted.
 */
struct dg_greet_trigger_target {
  trig_data *identity;
  unsigned long serial;  /* distinguishes a new trigger at a reused address */
  struct dg_greet_trigger_target *next;
};

/* current must come from a live trigger list; the snapshot is never
 * dereferenced. Address equality alone is not identity (ABA). */
static int dg_trigger_target_is(const trig_data *current,
                                const struct dg_greet_trigger_target *entry)
{
  if (current != entry->identity)
    return FALSE;
  if (current->serial != entry->serial) {
    /* The snapshotted trigger was freed during a callback and its address
     * reused. Correctly skipped, but reported: it means a script detached
     * and re-attached triggers mid-dispatch. */
    TB_ERROR_LOG("WARNING", "dg", "trigger-identity", "dispatch_snapshot", 0,
                 "trigger address %p reused (serial %lu -> %lu, now vnum %d); "
                 "skipping the replacement", (const void *)current,
                 entry->serial, current->serial,
                 trig_index ? GET_TRIG_VNUM(current) : -1);
    return FALSE;
  }
  return TRUE;
}

static struct dg_greet_trigger_target *dg_greet_trigger_targets(trig_data *first)
{
  struct dg_greet_trigger_target *head = NULL, **tail = &head;
  for (trig_data *t = first; t; t = t->next) {
    struct dg_greet_trigger_target *node;
    CREATE(node, struct dg_greet_trigger_target, 1);
    node->identity = t;
    node->serial = t->serial;
    node->next = NULL;
    *tail = node;
    tail = &node->next;
  }
  return head;
}

static void dg_free_greet_trigger_targets(struct dg_greet_trigger_target *head)
{
  while (head) {
    struct dg_greet_trigger_target *next = head->next;
    free(head);
    head = next;
  }
}

static trig_data *dg_find_live_greet_trigger(char_data *ch,
    const struct dg_greet_trigger_target *entry)
{
  if (!SCRIPT(ch))
    return NULL;
  for (trig_data *t = TRIGGERS(SCRIPT(ch)); t; t = t->next)
    if (dg_trigger_target_is(t, entry))
      return t;
  return NULL;
}

int greet_mtrigger(char_data *actor, int dir)
{
  char_data *ch;
  char buf[MAX_INPUT_LENGTH];
  int intermediate, final = TRUE;
  struct dg_memory_target *targets, *target;
  room_rnum origin_room;
  long actor_uid;

  if (!dg_trigger_actor_placed(actor, "greet_mtrigger") ||
      !valid_dg_target(actor, DG_ALLOW_GODS))
    return TRUE;

  origin_room = IN_ROOM(actor);
  actor_uid = char_script_id(actor);
  targets = dg_memory_targets(origin_room);

  for (target = targets; target; target = target->next) {
    if (!dg_memory_character_live(actor, actor_uid, origin_room))
      break;
    ch = find_char(target->uid);
    if (!dg_memory_character_live(ch, target->uid, origin_room))
      continue;
    if (!SCRIPT_CHECK(ch, MTRIG_GREET | MTRIG_GREET_ALL) ||
        !AWAKE(ch) || FIGHTING(ch) || ch == actor ||
        AFF_FLAGGED(ch, AFF_CHARM))
      continue;

    /* Only dispatch triggers present at entry to this owner's visit.
     * A callback may detach any trigger, including the next one. */
    struct dg_greet_trigger_target *triggers =
      dg_greet_trigger_targets(TRIGGERS(SCRIPT(ch)));
    for (struct dg_greet_trigger_target *entry = triggers; entry;
         entry = entry->next) {
      if (!dg_memory_character_live(actor, actor_uid, origin_room) ||
          !dg_memory_character_live(ch, target->uid, origin_room))
        break;
      trig_data *t = dg_find_live_greet_trigger(ch, entry);
      if (!t)
        continue;
      if (((IS_SET(GET_TRIG_TYPE(t), MTRIG_GREET) && CAN_SEE(ch, actor)) ||
           IS_SET(GET_TRIG_TYPE(t), MTRIG_GREET_ALL)) &&
          !GET_TRIG_DEPTH(t) && rand_number(1, 100) <= GET_TRIG_NARG(t)) {
        if (dir >= 0 && dir < DIR_COUNT)
          add_var(&GET_TRIG_VARS(t), "direction", dirs[rev_dir[dir]], 0);
        else
          add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
        ADD_UID_VAR(buf, t, actor_uid, "actor", 0);
        intermediate = script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
        if (!intermediate)
          final = FALSE;
      }
    }
    dg_free_greet_trigger_targets(triggers);
  }
  dg_free_memory_targets(targets);
  return final;
}

void entry_memory_mtrigger(char_data *ch)
{
  trig_data *t;
  char_data *actor;
  struct script_memory *mem;
  char buf[MAX_INPUT_LENGTH];
  struct dg_memory_target *targets, *target;
  room_rnum origin_room;
  long owner_uid;

  if (!dg_trigger_actor_placed(ch, "entry_memory_mtrigger") ||
      !SCRIPT_MEM(ch) || AFF_FLAGGED(ch, AFF_CHARM))
    return;

  origin_room = IN_ROOM(ch);
  owner_uid = char_script_id(ch);
  targets = dg_memory_targets(origin_room);
  for (target = targets; target; target = target->next) {
    if (!dg_memory_character_live(ch, owner_uid, origin_room))
      break;
    if (!SCRIPT_MEM(ch))
      break;
    actor = find_char(target->uid);
    if (!dg_memory_character_live(actor, target->uid, origin_room))
      continue;
    if (actor!=ch && SCRIPT_MEM(ch)) {
      /* Bound callbacks by the matching memories present initially.
       * Re-created memories remain queued for a later dispatch.
       */
      size_t remaining = 0;
      for (mem = SCRIPT_MEM(ch); mem; mem = mem->next)
        if (mem->id == target->uid)
          ++remaining;
      /* Search the live list again after each callback. A saved next pointer
       * may have been freed or replaced by the callback. */
      while (remaining > 0 &&
             dg_memory_character_live(ch, owner_uid, origin_room) &&
             dg_memory_character_live(actor, target->uid, origin_room) &&
             SCRIPT_MEM(ch)) {
        struct script_memory **link = &SCRIPT_MEM(ch);
        long actor_id = char_script_id(actor);
        while (*link && (*link)->id != actor_id)
          link = &(*link)->next;
        if (!*link)
          break;
        mem = *link;
        --remaining;
        *link = mem->next;
        mem->next = NULL;
        if (mem->cmd)
          command_interpreter(ch, mem->cmd);
        else if (SCRIPT(ch)) {
          /* The remembered NPC may have no script attached; do not
           * dereference SCRIPT(ch) just because SCRIPT_MEM(ch) exists. */
          for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
            if (TRIGGER_CHECK(t, MTRIG_MEMORY) &&
                rand_number(1, 100) <= GET_TRIG_NARG(t)) {
              ADD_UID_VAR(buf, t, actor_id, "actor", 0);
              script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
              break;
            }
          }
        }
        if (mem->cmd)
          free(mem->cmd);
        free(mem);
        /* The callback may have purged the trigger owner. */
        if (!dg_memory_character_live(ch, owner_uid, origin_room))
          goto cleanup;
        /* Retain old multi-memory behavior, but never traverse a cached
         * link after dispatch. Further character-lifetime checks are needed. */
      }

    }
  }
cleanup:
  dg_free_memory_targets(targets);
}

int entry_mtrigger(char_data *ch)
{
  trig_data *t;

  if (!SCRIPT_CHECK(ch, MTRIG_ENTRY) || AFF_FLAGGED(ch, AFF_CHARM))
    return 1;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_ENTRY) && (rand_number(1, 100) <= GET_TRIG_NARG(t))){
      return script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

int command_mtrigger(char_data *actor, char *cmd, char *argument)
{
  char_data *ch;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  struct dg_memory_target *targets, *target;
  room_rnum origin_room;
  long actor_uid;
  int handled = 0;

  /* prevent people we like from becoming trapped :P */
  if (!valid_dg_target(actor, 0))
    return 0;
  if (!dg_trigger_actor_placed(actor, "command_mtrigger"))
    return 0;

  origin_room = IN_ROOM(actor);
  actor_uid = char_script_id(actor);
  targets = dg_memory_targets(origin_room);

  for (target = targets; target && !handled; target = target->next) {
    if (!dg_memory_character_live(actor, actor_uid, origin_room))
      break;
    ch = find_char(target->uid);
    if (!dg_memory_character_live(ch, target->uid, origin_room))
      continue;
    if (!SCRIPT_CHECK(ch, MTRIG_COMMAND) || AFF_FLAGGED(ch, AFF_CHARM) ||
        (actor == ch && !CONFIG_SCRIPT_PLAYERS))
      continue;

    /* A callback returning false may detach any trigger, including the one
     * which just ran; detached triggers are freed once the outermost script
     * returns. Reacquire each trigger from the live list, never via t->next. */
    struct dg_greet_trigger_target *triggers =
      dg_greet_trigger_targets(TRIGGERS(SCRIPT(ch)));
    for (struct dg_greet_trigger_target *entry = triggers; entry;
         entry = entry->next) {
      if (!dg_memory_character_live(actor, actor_uid, origin_room) ||
          !dg_memory_character_live(ch, target->uid, origin_room))
        break;
      t = dg_find_live_greet_trigger(ch, entry);
      if (!t || !TRIGGER_CHECK(t, MTRIG_COMMAND))
        continue;

      if (!GET_TRIG_ARG(t) || !*GET_TRIG_ARG(t)) {
        mudlog(NRM, LVL_BUILDER, TRUE, "SYSERR: Command Trigger #%d has no text argument!",
          GET_TRIG_VNUM(t));
        continue;
      }

      if (*GET_TRIG_ARG(t)=='*' ||
          !strn_cmp(GET_TRIG_ARG(t), cmd, strlen(GET_TRIG_ARG(t)))) {
        ADD_UID_VAR(buf, t, actor_uid, "actor", 0);
        skip_spaces(&argument);
        add_var(&GET_TRIG_VARS(t), "arg", argument, 0);
        skip_spaces(&cmd);
        add_var(&GET_TRIG_VARS(t), "cmd", cmd, 0);

        if (script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW)) {
          handled = 1;
          break;
        }
      }
    }
    dg_free_greet_trigger_targets(triggers);
  }
  dg_free_memory_targets(targets);
  return handled;
}

void speech_mtrigger(char_data *actor, char *str)
{
  char_data *ch;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  struct dg_memory_target *targets, *target;
  room_rnum origin_room;
  long actor_uid;

  if (!dg_trigger_actor_placed(actor, "speech_mtrigger"))
    return;
  if (!str) {
    TB_ERROR_MSG("dg", "trigger-dispatch", "speech_mtrigger",
                 "invariant violated: NULL speech from uid=%ld; no speech "
                 "triggers dispatched", actor->script_id);
    return;
  }

  /* A callback may teleport or purge the next occupant, so never follow a
   * cached next_in_room link; reacquire each listener by UID instead. */
  origin_room = IN_ROOM(actor);
  actor_uid = char_script_id(actor);
  targets = dg_memory_targets(origin_room);

  for (target = targets; target; target = target->next) {
    if (!dg_memory_character_live(actor, actor_uid, origin_room))
      break;
    ch = find_char(target->uid);
    if (!dg_memory_character_live(ch, target->uid, origin_room))
      continue;
    if (!SCRIPT_CHECK(ch, MTRIG_SPEECH) || !AWAKE(ch) ||
        AFF_FLAGGED(ch, AFF_CHARM) || (actor == ch && !CONFIG_SCRIPT_PLAYERS))
      continue;

    for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
      if (!TRIGGER_CHECK(t, MTRIG_SPEECH))
        continue;

      if (!GET_TRIG_ARG(t) || !*GET_TRIG_ARG(t)) {
        mudlog(NRM, LVL_BUILDER, TRUE, "SYSERR: Speech Trigger #%d has no text argument!",
          GET_TRIG_VNUM(t));
        continue;
      }

      if (((GET_TRIG_NARG(t) && word_check(str, GET_TRIG_ARG(t))) ||
           (!GET_TRIG_NARG(t) && is_substring(GET_TRIG_ARG(t), str)))) {
        ADD_UID_VAR(buf, t, actor_uid, "actor", 0);
        add_var(&GET_TRIG_VARS(t), "speech", str, 0);
        script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
        break; /* t is not used after dispatch */
      }
    }
  }
  dg_free_memory_targets(targets);
}

void act_mtrigger(const char_data *ch, char *str, char_data *actor,
                  char_data *victim, obj_data *object,
                  obj_data *target, char *arg)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (SCRIPT_CHECK(ch, MTRIG_ACT) && !AFF_FLAGGED(ch, AFF_CHARM) &&
      (actor!=ch))
    for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next)  {
      if (!TRIGGER_CHECK(t, MTRIG_ACT))
        continue;

      if (!GET_TRIG_ARG(t) || !*GET_TRIG_ARG(t)) {
        mudlog(NRM, LVL_BUILDER, TRUE, "SYSERR: Act Trigger #%d has no text argument!",
          GET_TRIG_VNUM(t));
        continue;
      }

      if (((GET_TRIG_NARG(t) && word_check(str, GET_TRIG_ARG(t))) ||
           (!GET_TRIG_NARG(t) && is_substring(GET_TRIG_ARG(t), str)))) {
        if (actor)
          ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
        if (victim)
          ADD_UID_VAR(buf, t, char_script_id(victim), "victim", 0);
        if (object)
          ADD_UID_VAR(buf, t, obj_script_id(object), "object", 0);
        if (target)
          ADD_UID_VAR(buf, t, obj_script_id(target), "target", 0);
        if (str) {
          /* act() supplies a string ending with \r\n\0; do not rely on it. */
          char *nstr = strdup(str), *fstr = nstr, *p;
          if (!nstr) {
            TB_ERROR_ERRNO("dg", "allocate", "act_mtrigger",
                           "cannot copy act text for trigger %d; dispatching "
                           "without $arg", GET_TRIG_VNUM(t));
          } else {
            if ((p = strchr(nstr, '\r')) != NULL)
              *p = '\0';
            else
              TB_ERROR_MSG("dg", "act-trigger", "act_mtrigger",
                           "act text for trigger %d lacks the \\r\\n act() "
                           "always appends; using it whole", GET_TRIG_VNUM(t));
            skip_spaces(&nstr);
            add_var(&GET_TRIG_VARS(t), "arg", nstr, 0);
            free(fstr);
          }
        }
        script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
        break;
      }
    }
}

void fight_mtrigger(char_data *ch)
{
  struct char_data *actor;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(ch, MTRIG_FIGHT) || !FIGHTING(ch) ||
      AFF_FLAGGED(ch, AFF_CHARM))
    return;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_FIGHT) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))){
      actor = FIGHTING(ch);
      if (actor)
        ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      else
        add_var(&GET_TRIG_VARS(t), "actor", "nobody", 0);

      script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

void hitprcnt_mtrigger(char_data *ch)
{
  struct char_data *actor;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(ch, MTRIG_HITPRCNT) || !FIGHTING(ch) ||
      AFF_FLAGGED(ch, AFF_CHARM))
    return;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_HITPRCNT) && GET_MAX_HIT(ch) &&
        (((GET_HIT(ch) * 100) / GET_MAX_HIT(ch)) <= GET_TRIG_NARG(t))) {

      actor = FIGHTING(ch);
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

int receive_mtrigger(char_data *ch, char_data *actor, obj_data *obj)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id = obj_script_id(obj);

  if (!SCRIPT_CHECK(ch, MTRIG_RECEIVE) || AFF_FLAGGED(ch, AFF_CHARM))
    return 1;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_RECEIVE) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))){

      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      ADD_UID_VAR(buf, t, object_id, "object", 0);
      ret_val = script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      // check for purged item before dereferencing.
      if (DEAD(actor) || DEAD(ch) || !obj_uid_matches(object_id, obj) || obj->carried_by != actor)
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

int death_mtrigger(char_data *ch, char_data *actor)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(ch, MTRIG_DEATH) || AFF_FLAGGED(ch, AFF_CHARM))
    return 1;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_DEATH) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))){
      if (actor)
        ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      return script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

void load_mtrigger(char_data *ch)
{
  trig_data *t;
  int result = 0;
  mob_rnum rnum;

  if (!SCRIPT_CHECK(ch, MTRIG_LOAD))
    return;

  /* The script may extract its owner; capture the prototype beforehand. */
  rnum = GET_MOB_RNUM(ch);
  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_LOAD) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      result = script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      break;
    }
  }
  if (result == SCRIPT_ERROR_CODE) {
    /* we have recursed beyond reasonable depth */
    /* make sure this mob is the last one in the load chain */
    if (rnum != NOBODY) {
      free_proto_script(&mob_proto[rnum], MOB_TRIGGER);
    }
  }
}

int cast_mtrigger(char_data *actor, char_data *ch, int spellnum)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (ch == NULL)
    return 1;

  if (!SCRIPT_CHECK(ch, MTRIG_CAST) || AFF_FLAGGED(ch, AFF_CHARM))
    return 1;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_CAST) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      tb_snprintf(buf, sizeof(buf), "%d", spellnum);
      add_var(&GET_TRIG_VARS(t), "spell", buf, 0);
      add_var(&GET_TRIG_VARS(t), "spellname", skill_name(spellnum), 0);
      return script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

int damage_mtrigger(char_data *actor, char_data *victim, int dam, int attacktype)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (victim == NULL)
    return dam;

  if (!SCRIPT_CHECK(victim, MTRIG_DAMAGE) || AFF_FLAGGED(victim, AFF_CHARM))
    return dam;

  for (t = TRIGGERS(SCRIPT(victim)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_DAMAGE) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      ADD_UID_VAR(buf, t, char_script_id(victim), "victim", 0);
      tb_snprintf(buf, sizeof(buf), "%d", dam);
      add_var(&GET_TRIG_VARS(t), "damage", buf, 0);
      add_var(&GET_TRIG_VARS(t), "attacktype", skill_name(attacktype), 0);
      return script_driver(&victim, t, MOB_TRIGGER, TRIG_NEW);
    }
  }

  return dam;
}


int leave_mtrigger(char_data *actor, int dir)
{
  trig_data *t;
  char_data *ch;
  char buf[MAX_INPUT_LENGTH];

  if (!valid_dg_target(actor, DG_ALLOW_GODS))
    return 1;
  if (!dg_trigger_actor_placed(actor, "leave_mtrigger"))
    return 1;

  for (ch = world[IN_ROOM(actor)].people; ch; ch = ch->next_in_room) {
    if (!SCRIPT_CHECK(ch, MTRIG_LEAVE) ||
        !AWAKE(ch) || FIGHTING(ch) || (ch == actor) ||
        AFF_FLAGGED(ch, AFF_CHARM))
      continue;

    for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
      if ((IS_SET(GET_TRIG_TYPE(t), MTRIG_LEAVE) && CAN_SEE(ch, actor)) &&
          !GET_TRIG_DEPTH(t) && (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
        if (dir>=0 && dir < DIR_COUNT)
          add_var(&GET_TRIG_VARS(t), "direction", dirs[dir], 0);
        else
          add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
        ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
        return script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      }
    }
  }
  return 1;
}

int door_mtrigger(char_data *actor, int subcmd, int dir)
{
  trig_data *t;
  char_data *ch;
  char buf[MAX_INPUT_LENGTH];

  if (!dg_trigger_actor_placed(actor, "door_mtrigger"))
    return 1;

  for (ch = world[IN_ROOM(actor)].people; ch; ch = ch->next_in_room) {
    if (!SCRIPT_CHECK(ch, MTRIG_DOOR) ||
        !AWAKE(ch) || FIGHTING(ch) || (ch == actor) ||
        AFF_FLAGGED(ch, AFF_CHARM))
      continue;

    for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
      if (IS_SET(GET_TRIG_TYPE(t), MTRIG_DOOR) && CAN_SEE(ch, actor) &&
          !GET_TRIG_DEPTH(t) && (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
        add_var(&GET_TRIG_VARS(t), "cmd", cmd_door[subcmd], 0);
        if (dir>=0 && dir < DIR_COUNT)
          add_var(&GET_TRIG_VARS(t), "direction", dirs[dir], 0);
        else
          add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
        ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
        return script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      }
    }
  }
  return 1;
}

void time_mtrigger(char_data *ch)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  /* This trigger is called if the hour is the same as specified in Narg. */
  if (!SCRIPT_CHECK(ch, MTRIG_TIME) || AFF_FLAGGED(ch, AFF_CHARM))
    return;

  for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next) {
    if (TRIGGER_CHECK(t, MTRIG_TIME) &&
        (time_info.hours == GET_TRIG_NARG(t))) {
      tb_snprintf(buf, sizeof(buf), "%d", time_info.hours);
      add_var(&GET_TRIG_VARS(t), "time", buf, 0);
      script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

/* Object triggers. */
void random_otrigger(obj_data *obj)
{
  trig_data *t;

  if (!SCRIPT_CHECK(obj, OTRIG_RANDOM))
    return;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_RANDOM) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

void timer_otrigger(struct obj_data *obj)
{
  long obj_uid;
  struct dg_greet_trigger_target *targets, *entry;

  if (!obj || !SCRIPT_CHECK(obj, OTRIG_TIMER))
    return;

  /* The object lookup table is the authoritative lifetime check. Never
   * follow an object or trigger pointer saved across a script callback. */
  obj_uid = obj_script_id(obj);
  targets = dg_greet_trigger_targets(TRIGGERS(SCRIPT(obj)));

  for (entry = targets; entry; entry = entry->next) {
    trig_data *t = NULL, *current;
    if (!obj_uid_matches(obj_uid, obj))
      break;
    if (!SCRIPT(obj))
      break;
    for (current = TRIGGERS(SCRIPT(obj)); current; current = current->next) {
      if (dg_trigger_target_is(current, entry)) {
        t = current;
        break;
      }
    }
    if (t && TRIGGER_CHECK(t, OTRIG_TIMER))
      script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
  }

  dg_free_greet_trigger_targets(targets);
}

int get_otrigger(obj_data *obj, char_data *actor)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id;
  if (!SCRIPT_CHECK(obj, OTRIG_GET))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_GET) && (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      object_id = obj_script_id(obj);
      ret_val = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      /* Don't allow a get to take place, if the actor is killed (the mud
       * would choke on obj_to_char) or the object is purged. */
      if (DEAD(actor) || !obj_uid_matches(object_id, obj))
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

/* checks for command trigger on specific object. assumes obj has cmd trig */
/* Object command triggers may extract their owners, triggers, or actors.
 * Snapshot only identities; reacquire targets from live owned lists. */
struct dg_cmd_object_target {
  long uid;
  obj_data *identity;
  int type;
  struct dg_cmd_object_target *next;
};

static void dg_cmd_append(struct dg_cmd_object_target ***tail,
                          obj_data *obj, int type)
{
  struct dg_cmd_object_target *node;
  CREATE(node, struct dg_cmd_object_target, 1);
  node->uid = obj_script_id(obj);
  node->identity = obj;
  node->type = type;
  **tail = node;
  *tail = &node->next;
}

int cmd_otrig(obj_data *obj, char_data *actor, char *cmd,
              char *argument, int type)
{
  struct dg_greet_trigger_target *targets, *entry;
  trig_data *t, *current;
  char buf[MAX_INPUT_LENGTH];
  long uid, actor_uid;
  int handled = 0;

  if (!obj || !SCRIPT_CHECK(obj, OTRIG_COMMAND))
    return 0;
  uid = obj_script_id(obj);
  actor_uid = char_script_id(actor);
  targets = dg_greet_trigger_targets(TRIGGERS(SCRIPT(obj)));
  for (entry = targets; entry; entry = entry->next) {
    if (!obj_uid_matches(uid, obj) || find_char(actor_uid) != actor ||
        !SCRIPT(obj))
      break;
    t = NULL;
    for (current = TRIGGERS(SCRIPT(obj)); current; current = current->next)
      if (dg_trigger_target_is(current, entry)) { t = current; break; }
    if (!t || !TRIGGER_CHECK(t, OTRIG_COMMAND) ||
        !IS_SET(GET_TRIG_NARG(t), type))
      continue;
    if (!GET_TRIG_ARG(t) || !*GET_TRIG_ARG(t)) {
      mudlog(NRM, LVL_BUILDER, TRUE, "SYSERR: O-Command Trigger #%d has no text argument!",
             GET_TRIG_VNUM(t));
      continue;
    }
    if (*GET_TRIG_ARG(t) == '*' ||
        !strn_cmp(GET_TRIG_ARG(t), cmd, strlen(GET_TRIG_ARG(t)))) {
      ADD_UID_VAR(buf, t, actor_uid, "actor", 0);
      skip_spaces(&argument);
      add_var(&GET_TRIG_VARS(t), "arg", argument, 0);
      skip_spaces(&cmd);
      add_var(&GET_TRIG_VARS(t), "cmd", cmd, 0);
      if (script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW)) {
        handled = 1;
        break;
      }
    }
  }
  dg_free_greet_trigger_targets(targets);
  return handled;
}

int command_otrigger(char_data *actor, char *cmd, char *argument)
{
  struct dg_cmd_object_target *targets = NULL, **tail = &targets, *entry;
  obj_data *obj;
  room_rnum origin;
  long actor_uid;
  int i, handled = 0;

  if (!valid_dg_target(actor, 0))
    return 0;
  actor_uid = char_script_id(actor);
  origin = IN_ROOM(actor);
  for (i = 0; i < NUM_WEARS; ++i)
    if ((obj = GET_EQ(actor, i)) != NULL)
      dg_cmd_append(&tail, obj, OCMD_EQUIP);
  for (obj = actor->carrying; obj; obj = obj->next_content)
    dg_cmd_append(&tail, obj, OCMD_INVEN);
  for (obj = world[origin].contents; obj; obj = obj->next_content)
    dg_cmd_append(&tail, obj, OCMD_ROOM);

  for (entry = targets; entry; entry = entry->next) {
    int present = 0;
    if (find_char(actor_uid) != actor)
      break;
    if (IN_ROOM(actor) != origin)
      break;
    if (!obj_uid_matches(entry->uid, entry->identity))
      continue;
    if (entry->type == OCMD_EQUIP) {
      for (i = 0; i < NUM_WEARS; ++i)
        if (GET_EQ(actor, i) == entry->identity) { present = 1; break; }
    } else {
      obj_data *head = (entry->type == OCMD_INVEN) ?
                       actor->carrying : world[origin].contents;
      for (obj = head; obj; obj = obj->next_content)
        if (obj == entry->identity) { present = 1; break; }
    }
    if (present && cmd_otrig(entry->identity, actor, cmd,
                            argument, entry->type)) {
      handled = 1;
      break;
    }
  }
  while (targets) {
    entry = targets;
    targets = targets->next;
    free(entry);
  }
  return handled;
}

int wear_otrigger(obj_data *obj, char_data *actor, int where)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id;

  if (!SCRIPT_CHECK(obj, OTRIG_WEAR))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_WEAR)) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      object_id = obj_script_id(obj);
      ret_val = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      /* Don't allow a wear to take place, if the object is purged. */
      if (!obj_uid_matches(object_id, obj))
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

int remove_otrigger(obj_data *obj, char_data *actor)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id;

  if (!SCRIPT_CHECK(obj, OTRIG_REMOVE))
    return 1;

  if (!valid_dg_target(actor, 0))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_REMOVE)) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      object_id = obj_script_id(obj);
      ret_val = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      /* Don't allow a remove to take place, if the object is purged. */
      if (!obj_uid_matches(object_id, obj))
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

int drop_otrigger(obj_data *obj, char_data *actor)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id;

  if (!SCRIPT_CHECK(obj, OTRIG_DROP))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_DROP) && (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      object_id = obj_script_id(obj);
      ret_val = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      /* Don't allow a drop to take place, if the object is purged. */
      if (!obj_uid_matches(object_id, obj))
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

int give_otrigger(obj_data *obj, char_data *actor, char_data *victim)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id;

  if (!SCRIPT_CHECK(obj, OTRIG_GIVE))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_GIVE) && (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      ADD_UID_VAR(buf, t, char_script_id(victim), "victim", 0);
      object_id = obj_script_id(obj);
      ret_val = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      /* Don't allow a give to take place, if the object is purged or the
       * object is not carried by the giver. */
      if (!obj_uid_matches(object_id, obj) || obj->carried_by != actor)
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

void load_otrigger(obj_data *obj)
{
  trig_data *t;
  int result = 0;
  obj_rnum rnum;

  if (!SCRIPT_CHECK(obj, OTRIG_LOAD))
    return;

  /* script_driver() sets obj to NULL if the script purges it, and a script
   * may "return" SCRIPT_ERROR_CODE itself. Capture the prototype beforehand. */
  rnum = GET_OBJ_RNUM(obj);
  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_LOAD) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      result = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      break;
    }
  }
  if (result == SCRIPT_ERROR_CODE) {
    /* we have recursed beyond reasonable depth */
    /* make sure this mob is the last one in the load chain */
    if (rnum != NOTHING) {
      free_proto_script(&obj_proto[rnum], OBJ_TRIGGER);
    }
  }
}

int cast_otrigger(char_data *actor, obj_data *obj, int spellnum)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (obj == NULL)
    return 1;

  if (!SCRIPT_CHECK(obj, OTRIG_CAST))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_CAST) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      tb_snprintf(buf, sizeof(buf), "%d", spellnum);
      add_var(&GET_TRIG_VARS(t), "spell", buf, 0);
      add_var(&GET_TRIG_VARS(t), "spellname", skill_name(spellnum), 0);
      return script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

/* A leave callback can remove any room object, including the next one.
 * Snapshot IDs before invoking scripts; reacquire occupants from the live room
 * list before each dispatch instead of preserving next_content pointers. */
struct dg_leave_object_target {
  long uid;
  struct dg_leave_object_target *next;
};

int leave_otrigger(room_data *room, char_data *actor, int dir)
{
  struct dg_leave_object_target *targets = NULL, **tail = &targets, *entry;
  obj_data *obj;
  char buf[MAX_INPUT_LENGTH];
  int final = 1;
  long actor_uid;

  if (!room || !valid_dg_target(actor, DG_ALLOW_GODS))
    return 1;

  actor_uid = char_script_id(actor);
  for (obj = room->contents; obj; obj = obj->next_content) {
    struct dg_leave_object_target *node;
    if (!SCRIPT_CHECK(obj, OTRIG_LEAVE))
      continue;
    CREATE(node, struct dg_leave_object_target, 1);
    node->uid = obj_script_id(obj);
    *tail = node;
    tail = &node->next;
  }

  for (entry = targets; entry; entry = entry->next) {
    if (find_char(actor_uid) != actor)
      break;
    struct dg_greet_trigger_target *triggers, *target;
    trig_data *t;
    obj = NULL;

    if (entry->uid < OBJ_ID_BASE)
      continue;
    for (obj_data *candidate = room->contents; candidate;
         candidate = candidate->next_content) {
      if (candidate->script_id == entry->uid) {
        obj = candidate;
        break;
      }
    }
    if (!obj || !SCRIPT_CHECK(obj, OTRIG_LEAVE))
      continue;

    triggers = dg_greet_trigger_targets(TRIGGERS(SCRIPT(obj)));
    for (target = triggers; target; target = target->next) {
      if (find_char(actor_uid) != actor)
        break;
      trig_data *current;
      if (!obj_uid_matches(entry->uid, obj))
        break;
      obj = NULL;
      for (obj_data *candidate = room->contents; candidate;
           candidate = candidate->next_content) {
        if (candidate->script_id == entry->uid) {
          obj = candidate;
          break;
        }
      }
      if (!obj || !SCRIPT_CHECK(obj, OTRIG_LEAVE))
        break;
      t = NULL;
      for (current = TRIGGERS(SCRIPT(obj)); current; current = current->next) {
        if (dg_trigger_target_is(current, target)) {
          t = current;
          break;
        }
      }
      if (!t || !TRIGGER_CHECK(t, OTRIG_LEAVE) ||
          rand_number(1, 100) > GET_TRIG_NARG(t))
        continue;
      if (dir >= 0 && dir < DIR_COUNT)
        add_var(&GET_TRIG_VARS(t), "direction", dirs[dir], 0);
      else
        add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
      ADD_UID_VAR(buf, t, actor_uid, "actor", 0);
      if (!script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW))
        final = 0;
    }
    dg_free_greet_trigger_targets(triggers);
  }

  while (targets) {
    entry = targets;
    targets = targets->next;
    free(entry);
  }
  return final;
}

int consume_otrigger(obj_data *obj, char_data *actor, int cmd)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id;

  if (!SCRIPT_CHECK(obj, OTRIG_CONSUME))
    return 1;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_CONSUME)) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      switch (cmd) {
              case OCMD_EAT:
                add_var(&GET_TRIG_VARS(t), "command", "eat", 0);
                break;
              case OCMD_DRINK:
                add_var(&GET_TRIG_VARS(t), "command", "drink", 0);
                break;
        case OCMD_QUAFF:
          add_var(&GET_TRIG_VARS(t), "command", "quaff", 0);
          break;
      }
      object_id = obj_script_id(obj);
      ret_val = script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      /* Don't allow a wear to take place, if the object is purged. */
      if (!obj_uid_matches(object_id, obj))
        return 0;
      else
        return ret_val;
    }
  }

  return 1;
}

void time_otrigger(obj_data *obj)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(obj, OTRIG_TIME))
    return;

  for (t = TRIGGERS(SCRIPT(obj)); t; t = t->next) {
    if (TRIGGER_CHECK(t, OTRIG_TIME) &&
        (time_info.hours == GET_TRIG_NARG(t))) {
      tb_snprintf(buf, sizeof(buf), "%d", time_info.hours);
      add_var(&GET_TRIG_VARS(t), "time", buf, 0);
      script_driver(&obj, t, OBJ_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

/* World triggers. */
void reset_wtrigger(struct room_data *room)
{
  trig_data *t;

  if (!SCRIPT_CHECK(room, WTRIG_RESET))
    return;

  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_RESET) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

void random_wtrigger(struct room_data *room)
{
  trig_data *t;

  if (!SCRIPT_CHECK(room, WTRIG_RANDOM))
    return;

  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_RANDOM) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

int enter_wtrigger(struct room_data *room, char_data *actor, int dir)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(room, WTRIG_ENTER))
    return 1;

  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_ENTER) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      if (dir>=0 && dir < DIR_COUNT)
        add_var(&GET_TRIG_VARS(t), "direction", dirs[rev_dir[dir]], 0);
      else
        add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      return script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

int command_wtrigger(char_data *actor, char *cmd, char *argument)
{
  struct room_data *room;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!dg_trigger_actor_placed(actor, "command_wtrigger"))
    return 0;
  if (!SCRIPT_CHECK(&world[IN_ROOM(actor)], WTRIG_COMMAND))
    return 0;

  /* prevent people we like from becoming trapped :P */
  if (!valid_dg_target(actor, 0))
    return 0;

  room = &world[IN_ROOM(actor)];
  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (!TRIGGER_CHECK(t, WTRIG_COMMAND))
      continue;

    if (!GET_TRIG_ARG(t) || !*GET_TRIG_ARG(t)) {
      mudlog(NRM, LVL_BUILDER, TRUE, "SYSERR: W-Command Trigger #%d has no text argument!",
        GET_TRIG_VNUM(t));
      continue;
    }

    if (*GET_TRIG_ARG(t)=='*' ||
        !strn_cmp(GET_TRIG_ARG(t), cmd, strlen(GET_TRIG_ARG(t)))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      skip_spaces(&argument);
      add_var(&GET_TRIG_VARS(t), "arg", argument, 0);
      skip_spaces(&cmd);
      add_var(&GET_TRIG_VARS(t), "cmd", cmd, 0);

      return script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
    }
  }

  return 0;
}

void speech_wtrigger(char_data *actor, char *str)
{
  struct room_data *room;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!dg_trigger_actor_placed(actor, "speech_wtrigger"))
    return;
  if (!SCRIPT_CHECK(&world[IN_ROOM(actor)], WTRIG_SPEECH))
    return;

  room = &world[IN_ROOM(actor)];
  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (!TRIGGER_CHECK(t, WTRIG_SPEECH))
      continue;

    if (!GET_TRIG_ARG(t) || !*GET_TRIG_ARG(t)) {
      mudlog(NRM, LVL_BUILDER, TRUE, "SYSERR: W-Speech Trigger #%d has no text argument!",
        GET_TRIG_VNUM(t));
      continue;
    }

    if (*GET_TRIG_ARG(t)=='*' ||
       (GET_TRIG_NARG(t) && word_check(str, GET_TRIG_ARG(t))) ||
       (!GET_TRIG_NARG(t) && is_substring(GET_TRIG_ARG(t), str))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      add_var(&GET_TRIG_VARS(t), "speech", str, 0);
      script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

int drop_wtrigger(obj_data *obj, char_data *actor)
{
  struct room_data *room;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];
  int ret_val;
  long object_id = obj_script_id(obj);

  if (!dg_trigger_actor_placed(actor, "drop_wtrigger"))
    return 1;
  if (!SCRIPT_CHECK(&world[IN_ROOM(actor)], WTRIG_DROP))
    return 1;

  room = &world[IN_ROOM(actor)];
  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next)
    if (TRIGGER_CHECK(t, WTRIG_DROP) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      ADD_UID_VAR(buf, t, object_id, "object", 0);
      ret_val = script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
      // check for purged object before dereferencing.
      if (!obj_uid_matches(object_id, obj) || obj->carried_by != actor)
        return 0;
      else
        return ret_val;
    }

  return 1;
}

int cast_wtrigger(char_data *actor, char_data *vict, obj_data *obj, int spellnum)
{
  room_data *room;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!dg_trigger_actor_placed(actor, "cast_wtrigger"))
    return 1;
  if (!SCRIPT_CHECK(&world[IN_ROOM(actor)], WTRIG_CAST))
    return 1;

  room = &world[IN_ROOM(actor)];
  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_CAST) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {

      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      if (vict)
        ADD_UID_VAR(buf, t, char_script_id(vict), "victim", 0);
      if (obj)
        ADD_UID_VAR(buf, t, obj_script_id(obj), "object", 0);
      tb_snprintf(buf, sizeof(buf), "%d", spellnum);
      add_var(&GET_TRIG_VARS(t), "spell", buf, 0);
      add_var(&GET_TRIG_VARS(t), "spellname", skill_name(spellnum), 0);
      return script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

int leave_wtrigger(struct room_data *room, char_data *actor, int dir)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!valid_dg_target(actor, DG_ALLOW_GODS))
    return 1;

  if (!SCRIPT_CHECK(room, WTRIG_LEAVE))
    return 1;

  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_LEAVE) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      if (dir>=0 && dir < DIR_COUNT)
        add_var(&GET_TRIG_VARS(t), "direction", dirs[dir], 0);
      else
        add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      return script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

int door_wtrigger(char_data *actor, int subcmd, int dir)
{
  room_data *room;
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!dg_trigger_actor_placed(actor, "door_wtrigger"))
    return 1;
  if (!SCRIPT_CHECK(&world[IN_ROOM(actor)], WTRIG_DOOR))
    return 1;

  room = &world[IN_ROOM(actor)];
  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_DOOR) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      add_var(&GET_TRIG_VARS(t), "cmd", cmd_door[subcmd], 0);
      if (dir>=0 && dir < DIR_COUNT)
        add_var(&GET_TRIG_VARS(t), "direction", dirs[dir], 0);
      else
        add_var(&GET_TRIG_VARS(t), "direction", "none", 0);
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      return script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}

void time_wtrigger(struct room_data *room)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(room, WTRIG_TIME))
    return;

  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_TIME) &&
        (time_info.hours == GET_TRIG_NARG(t))) {
      tb_snprintf(buf, sizeof(buf), "%d", time_info.hours);
      add_var(&GET_TRIG_VARS(t), "time", buf, 0);
      script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
      break;
    }
  }
}

int login_wtrigger(struct room_data *room, char_data *actor)
{
  trig_data *t;
  char buf[MAX_INPUT_LENGTH];

  if (!SCRIPT_CHECK(room, WTRIG_LOGIN))
    return 1;

  for (t = TRIGGERS(SCRIPT(room)); t; t = t->next) {
    if (TRIGGER_CHECK(t, WTRIG_LOGIN) &&
        (rand_number(1, 100) <= GET_TRIG_NARG(t))) {
      ADD_UID_VAR(buf, t, char_script_id(actor), "actor", 0);
      return script_driver(&room, t, WLD_TRIGGER, TRIG_NEW);
    }
  }

  return 1;
}
