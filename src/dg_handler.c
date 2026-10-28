/**************************************************************************
*  File: dg_handler.c                                      Part of tbaMUD *
*  Usage: Contains functions to handle memory for scripts.                *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Death's Gate MUD is based on CircleMUD, Copyright (C) 1993, 94.        *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
*                                                                         *
*  $Author: Mark A. Heilpern/egreen/Welcor $                              *
*  $Date: 2004/10/11 12:07:00$                                            *
*  $Revision: 1.0.14 $                                                    *
***************************************************************************/

/**
 * @file dg_handler.c
 * Allocation and release of DG script structures: trigger instances,
 * per-entity scripts, prototype trigger lists and variable lists.
 *
 * Ownership: a live trig_data belongs to exactly one script_data (its
 * TRIGGERS list) and is also linked on the global trigger_list; a
 * script_data belongs to exactly one mob, obj or room (SCRIPT(thing)); a
 * trig_proto_list belongs to exactly one prototype or instance.  The
 * cmdlist of a trigger is shared with its trig_index prototype and is never
 * freed here.  Freeing while a script is executing is deferred (see
 * dg_script_execution_enter/leave).
 *
 * Input: everything here is driven by engine code and OLC; script content
 * is builder-controlled but only reaches these functions as already-built
 * structures.  delete_variables() takes a validated player name.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "dg_scripts.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "spells.h"
#include "dg_event.h"
#include "constants.h"

/* Trigger callbacks can detach their own trigger (or entire script).
 * Delay destruction until the outermost script_driver frame exits. */
static unsigned int dg_execution_depth;
struct dg_retired_trigger {
  struct trig_data *trig;
  struct dg_retired_trigger *next;
};
struct dg_retired_script {
  struct script_data *script;
  struct dg_retired_script *next;
};
static struct dg_retired_trigger *dg_retired_triggers;
static struct dg_retired_script *dg_retired_scripts;

/** TRUE while any script_driver() frame is active, so frees must be
 * deferred to dg_script_execution_leave(). */
#define DG_IN_SCRIPT_EXECUTION()  (dg_execution_depth != 0)

/** The wait_event_data payload of trigger t's pending 'wait'.  Only valid
 * when GET_TRIG_WAIT(t) != NULL; DG waits are never mud events.  t is
 * evaluated twice: pass a plain variable. */
#define TRIG_WAIT_DATA(t) \
  ((struct wait_event_data *)GET_TRIG_WAIT(t)->event_obj)

/** Mark the start of a script_driver() frame.
 * Every enter must be matched by exactly one dg_script_execution_leave()
 * on every return path of script_driver().  While any frame is active,
 * extract_trigger() and extract_script() unlink immediately but defer the
 * free to the outermost leave.  A retired trigger is already off
 * trigger_list and its owner's TRIGGERS list, so code that runs after a
 * callback must re-validate any trigger/script pointer it kept (address
 * plus serial) before using it. */
void dg_script_execution_enter(void)
{
  dg_execution_depth++;
}

/** Mark the end of a script_driver() frame.  When the outermost frame
 * leaves, retired triggers are freed first, then retired scripts (with
 * their global variables). */
void dg_script_execution_leave(void)
{
  struct dg_retired_trigger *t;
  struct dg_retired_script *s;
  assert(dg_execution_depth != 0);
  if (--dg_execution_depth != 0)
    return;
  while ((t = dg_retired_triggers) != NULL) {
    dg_retired_triggers = t->next;
    free_trigger(t->trig);
    free(t);
  }
  while ((s = dg_retired_scripts) != NULL) {
    dg_retired_scripts = s->next;
    free_varlist(s->script->global_vars);
    free(s->script);
    free(s);
  }
}

/** Report that a script (or proto-script list) about to be freed is still
 * referenced by another entity, then abort.  Continuing would leave that
 * entity with a dangling pointer that is later freed or executed again.
 * @param op Diagnostic operation name.
 * @param list The script_data or trig_proto_list being freed.
 * @param kind "char", "obj" or "room": the other owner's type.
 * @param vnum The other owner's vnum (NOBODY for a player).
 * @param idnum The other owner's player idnum, or -1. */
static void dg_shared_owner_fatal(const char *op, const void *list,
                                  const char *kind, int vnum, long idnum)
{
  TB_ERROR_LOG("FATAL", "dg", op, "script", 0,
               "%p being freed is still owned by %s vnum=%d idnum=%ld",
               list, kind, vnum, idnum);
  abort();
}

/* frees memory associated with var */
void free_var_el(struct trig_var_data *var)
{
  if (var->name)
    free(var->name);
  if (var->value)
    free(var->value);
  free(var);
}

/* release memory allocated for a variable list */
void free_varlist(struct trig_var_data *vd)
{
    struct trig_var_data *i, *j;

    for (i = vd; i;) {
	j = i;
	i = i->next;
	free_var_el(j);
    }
}

/* Remove var name from var_list. Returns 1 if found, else 0. */
int remove_var(struct trig_var_data **var_list, char *name)
{
  struct trig_var_data *i, *j;

  for (j = NULL, i = *var_list; i && str_cmp(name, i->name);
       j = i, i = i->next);

  if (i) {
    if (j) {
      j->next = i->next;
      free_var_el(i);
    } else {
      *var_list = i->next;
      free_var_el(i);
    }

    return 1;
  }

  return 0;
}

/** Return memory used by a trigger. The command list is free'd when changed and
 * when shutting down.
 * Does not free cmdlist (shared with the trig_index prototype) and does not
 * unlink trig from trigger_list or any script; callers that hold a live
 * trigger use extract_trigger() instead.  Also used on prototypes and OLC
 * copies (db.c destroy_db, oasis.c).  Cancels a pending wait event.
 * @param trig The trigger to free; must not be used afterwards. */
void free_trigger(struct trig_data *trig)
{
    free(trig->name);
    trig->name = NULL;

    if (trig->arglist) {
      free(trig->arglist);
      trig->arglist = NULL;
    }
    if (trig->var_list) {
      free_varlist(trig->var_list);
      trig->var_list = NULL;
    }
    if (GET_TRIG_WAIT(trig))
      event_cancel(GET_TRIG_WAIT(trig));

    free(trig);
}

/** remove a single trigger from a mob/obj/room
 * @pre trig is already unlinked from its owner's TRIGGERS(sc) list
 * (remove_trigger() does this; extract_script() discards the whole list).
 * @pre trig has not been extracted before.
 * @post The pending wait is cancelled, the trig_index use count is
 * decremented, trig is off trigger_list, and trig is freed now or, inside
 * script execution, at the outermost dg_script_execution_leave(). */
void extract_trigger(struct trig_data *trig)
{
  struct trig_data *temp;

  if (GET_TRIG_WAIT(trig)) {
    event_cancel(GET_TRIG_WAIT(trig));
    GET_TRIG_WAIT(trig) = NULL;
  }

  trig_index[trig->nr]->number--;

  /* walk the trigger list and remove this one */
  REMOVE_FROM_LIST(trig, trigger_list, next_in_world);

  if (DG_IN_SCRIPT_EXECUTION()) {
    struct dg_retired_trigger *node;
    CREATE(node, struct dg_retired_trigger, 1);
    node->trig = trig;
    node->next = dg_retired_triggers;
    dg_retired_triggers = node;
  } else {
    free_trigger(trig);
  }
}

/* remove all triggers from a mob/obj/room */
void extract_script(void *thing, int type)
{
  struct script_data *sc = NULL;
  struct trig_data *trig, *next_trig;
  char_data *mob;
  obj_data *obj;
  room_data *room;

  if (!thing) {
    TB_ERROR_MSG("dg", "extract-script-null", "script",
                 "NULL owner passed with type %d; nothing extracted", type);
    return;
  }

  switch (type) {
    case MOB_TRIGGER:
      mob = (struct char_data *)thing;
      sc = SCRIPT(mob);
      SCRIPT(mob) = NULL;
      break;
    case OBJ_TRIGGER:
      obj = (struct obj_data *)thing;
      sc = SCRIPT(obj);
      SCRIPT(obj) = NULL;
      break;
    case WLD_TRIGGER:
      room = (struct room_data *)thing;
      sc = SCRIPT(room);
      SCRIPT(room) = NULL;
      break;
    default:
      /* type is always a *_TRIGGER constant chosen by engine code; any
       * other value means the caller cast the wrong struct, so the
       * owner's script is left attached (and leaks) rather than guessed. */
      TB_ERROR_MSG("dg", "extract-script-type", "script",
                   "invalid owner type %d for owner %p (expected %d/%d/%d); "
                   "script left attached", type, thing,
                   MOB_TRIGGER, OBJ_TRIGGER, WLD_TRIGGER);
      return;
  }

  if (!sc)
    return;

#if 1 /* debugging */
  {
    /* Invariant: SCRIPT(thing) was the only owner of sc.  A second owner
     * would keep a dangling script after sc is freed below, so stop now
     * rather than corrupt the heap later.  These used to be assert()s,
     * compiled out under NDEBUG and silent about which entity was wrong. */
    struct char_data *i = character_list;
    struct obj_data *j = object_list;
    room_rnum k;
    if (sc) {
      for ( ; i ; i = i->next)
        if (sc == SCRIPT(i))
          dg_shared_owner_fatal("script-shared-owner", sc, "char",
                                IS_NPC(i) ? GET_MOB_VNUM(i) : NOBODY,
                                IS_NPC(i) ? -1 : GET_IDNUM(i));

      for ( ; j ; j = j->next)
        if (sc == SCRIPT(j))
          dg_shared_owner_fatal("script-shared-owner", sc, "obj",
                                GET_OBJ_VNUM(j), -1);

      /* world is NULL once destroy_db() has freed it; the objects and
       * mobiles freed after that must not scan the freed array. */
      for (k = 0; world && k <= top_of_world; k++)
        if (sc == SCRIPT(&world[k]))
          dg_shared_owner_fatal("script-shared-owner", sc, "room",
                                world[k].number, -1);
    }
  }
#endif
  for (trig = TRIGGERS(sc); trig; trig = next_trig) {
    next_trig = trig->next;
    extract_trigger(trig);
  }
  TRIGGERS(sc) = NULL;

  /* Thanks to James Long for tracking down this memory leak */
  if (DG_IN_SCRIPT_EXECUTION()) {
    struct dg_retired_script *node;
    CREATE(node, struct dg_retired_script, 1);
    node->script = sc;
    node->next = dg_retired_scripts;
    dg_retired_scripts = node;
  } else {
    free_varlist(sc->global_vars);
    free(sc);
  }
}

/* erase the script memory of a mob */
void extract_script_mem(struct script_memory *sc)
{
  struct script_memory *next;
  while (sc) {
    next = sc->next;
    if (sc->cmd) free(sc->cmd);
    free(sc);
    sc = next;
  }
}

/** Free the prototype trigger list of a mob, obj or room and set it NULL.
 * @param thing The owner; must be non-NULL and of the struct type that
 * matches type.
 * @param type MOB_TRIGGER, OBJ_TRIGGER or WLD_TRIGGER; any other value
 * frees nothing.
 * Aborts with dg/proto-script-shared-owner if another live char, obj or
 * room still points at the same list. */
void free_proto_script(void *thing, int type)
{
  struct trig_proto_list *proto = NULL, *fproto;
  char_data *mob;
  obj_data *obj;
  room_data *room;

  switch (type) {
    case MOB_TRIGGER:
      mob = (struct char_data *)thing;
      proto = mob->proto_script;
      mob->proto_script = NULL;
      break;
    case OBJ_TRIGGER:
      obj = (struct obj_data *)thing;
      proto = obj->proto_script;
      obj->proto_script = NULL;
      break;
    case WLD_TRIGGER:
      room = (struct room_data *)thing;
      proto = room->proto_script;
      room->proto_script = NULL;
      break;
  }
#if 1 /* debugging */
  {
    /* Invariant: thing was the only owner of proto (see the matching
     * check in extract_script()). */
    struct char_data *i = character_list;
    struct obj_data *j = object_list;
    room_rnum k;
    if (proto) {
      for ( ; i ; i = i->next)
        if (proto == i->proto_script)
          dg_shared_owner_fatal("proto-script-shared-owner", proto, "char",
                                IS_NPC(i) ? GET_MOB_VNUM(i) : NOBODY,
                                IS_NPC(i) ? -1 : GET_IDNUM(i));

      for ( ; j ; j = j->next)
        if (proto == j->proto_script)
          dg_shared_owner_fatal("proto-script-shared-owner", proto, "obj",
                                GET_OBJ_VNUM(j), -1);

      /* world is NULL once destroy_db() has freed it; the objects and
       * mobiles freed after that must not scan the freed array. */
      for (k = 0; world && k <= top_of_world; k++)
        if (proto == world[k].proto_script)
          dg_shared_owner_fatal("proto-script-shared-owner", proto, "room",
                                world[k].number, -1);
    }
  }
#endif
  while (proto) {
    fproto = proto;
    proto = proto->next;
    free(fproto);
  }
}

/** Deep-copy source's prototype trigger list (vnums only) to dest.
 * dest->proto_script is overwritten without being freed, so the caller
 * must call free_proto_script(dest) first unless dest is a fresh struct
 * copy of source whose pointer is still shared (read_mobile/read_object).
 * If source has no list, dest is left unchanged.
 * @param source,dest Non-NULL structs of the type named by type.
 * @param type MOB_TRIGGER, OBJ_TRIGGER or WLD_TRIGGER. */
void copy_proto_script(void *source, void *dest, int type)
{
  struct trig_proto_list *tp_src = NULL, *tp_dst = NULL;

  switch (type) {
    case MOB_TRIGGER:
      tp_src = ((char_data *)source)->proto_script;
      break;
    case OBJ_TRIGGER:
      tp_src = ((obj_data *)source)->proto_script;
      break;
    case WLD_TRIGGER:
      tp_src = ((room_data *)source)->proto_script;
      break;
  }

  if (tp_src) {
    CREATE(tp_dst, struct trig_proto_list, 1);
    switch (type) {
      case MOB_TRIGGER:
        ((char_data *)dest)->proto_script = tp_dst;
        break;
      case OBJ_TRIGGER:
        ((obj_data *)dest)->proto_script = tp_dst;
        break;
      case WLD_TRIGGER:
        ((room_data *)dest)->proto_script = tp_dst;
        break;
    }

    while (tp_src) {
      tp_dst->vnum = tp_src->vnum;
      tp_src = tp_src->next;
      if (tp_src)
        CREATE(tp_dst->next, struct trig_proto_list, 1);
      tp_dst = tp_dst->next;
    }
  }
}

/** Delete the saved global script-variable file of a player.
 * @param charname A validated player name (not script-controlled).
 * If removal fails for any reason other than a missing file the failure is
 * logged; the file then survives and a later character created with the
 * same name inherits its variables. */
void delete_variables(const char *charname)
{
  char filename[PATH_MAX];

  if (!get_filename(filename, sizeof(filename), SCRIPT_VARS_FILE, charname))
    return;

  if (remove(filename) < 0 && errno != ENOENT) {
    int saved_errno = errno;
    log("SYSERR: remove script-variable file %s for character %s failed: %s (errno=%d)",
        filename, charname ? charname : "(unknown)",
        strerror(saved_errno), saved_errno);
  }
}

/** Repoint pending wait events of one room's triggers from 'from' to 'to'
 * after the room struct has been copied (genwld.c add/delete shifts).
 * @pre Called before 'from' is overwritten; reads SCRIPT(from).
 * Only covers the one room copied, so rooms that keep their index when
 * RECREATE(world) moves the whole array keep the old address in go.  That
 * is harmless: trig_wait_event() finds a room owner by its vnum
 * (wait_event_data->owner_vnum) and never follows go (D2-03). */
void update_wait_events(struct room_data *to, struct room_data *from)
{
  struct trig_data *trig;

  if (!SCRIPT(from))
    return;

  for (trig = TRIGGERS(SCRIPT(from)); trig; trig = trig->next) {
    if (!GET_TRIG_WAIT(trig))
      continue;

    TRIG_WAIT_DATA(trig)->go = to;
  }
}
