/**************************************************************************
*  File: genwld.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - Rooms.                                    *
*                                                                         *
*  By Levork. Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer. *
**************************************************************************/

/**
 * @file genwld.c
 * Room table maintenance for OLC (redit, rcopy, zedit new) and the room
 * file writer.
 *
 * Input: room data comes from builders through redit; vnums are checked by
 * the OLC front end against the builder's zone.
 *
 * Pointer stability: add_room() (insert path) and delete_room() reallocate
 * and shift world[], so every struct room_data * held anywhere becomes
 * invalid.  Room mud events and DG wait events are re-pointed here; any
 * other holder of a room pointer must re-resolve it by vnum.  Stored room
 * rnums are renumbered here: characters' and objects' IN_ROOM, GET_WAS_IN,
 * exits, every zone's reset commands and the three start rooms.
 *
 * Ownership: world[r] owns its strings, exits, extra descriptions and
 * proto_script.  people, contents, events and script are runtime lists of
 * the live room and are never taken from an OLC copy.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "comm.h"
#include "genolc.h"
#include "genwld.h"
#include "genzon.h"
#include "shop.h"
#include "dg_olc.h"
#include "mud_event.h"
#include "dg_event.h"

/* Room mud events (e.g. eSPL_DARKNESS from magic.c) store a raw
 * struct room_data * in mud_event_data.pStruct.  add_room() and
 * delete_room() reallocate and shift world[], which moves every room, so
 * after each such change every room's events must point back at the room
 * that owns their list.  The events list itself travels with the room in the
 * struct copy; only the back pointer goes stale. */
static void rebase_room_events(void)
{
  room_rnum r;
  struct item_data *item;

  for (r = 0; r <= top_of_world; r++) {
    if (world[r].events == NULL)
      continue;
    for (item = world[r].events->pFirstItem; item; item = item->pNextItem) {
      struct event *pEvent = (struct event *)item->pContent;

      if (pEvent == NULL || !pEvent->isMudEvent || pEvent->event_obj == NULL) {
        TB_ERROR_MSG("olc", "room-event-rebase", "room",
                     "room vnum %d rnum %d: events list holds a non-mud event; "
                     "its room pointer was not updated", world[r].number, r);
        continue;
      }
      ((struct mud_event_data *)pEvent->event_obj)->pStruct = &world[r];
    }
  }
}


/* Disable a reset command that names a deleted room: '*' is skipped by
 * reset_zone() and dropped by save_zone().  A command list owns its
 * commands' sarg1/sarg2 (only 'V' has them; remove_cmd_from_list() in
 * genzon.c frees them), and destroy_db() frees them for 'V' alone, so a
 * cancelled command gives them up here instead of leaking them. */
static void cancel_zone_command(struct reset_com *cmd)
{
  free(cmd->sarg1);
  free(cmd->sarg2);
  cmd->sarg1 = cmd->sarg2 = NULL;
  cmd->command = '*';
}

/* Containers a 'P' may no longer put into because the command that loaded
 * them was cancelled; more than this many in one zone is reported. */
#define DELETE_ROOM_MAX_GONE_CONTAINERS 64

static int obj_rnum_in(const obj_rnum *set, int n, obj_rnum r)
{
  int k;
  for (k = 0; k < n; k++)
    if (set[k] == r)
      return TRUE;
  return FALSE;
}

/* delete_room()'s pass over one zone's reset commands.  A command that loads
 * into the deleted room (rnum) is cancelled; one above it is renumbered.
 * Commands that act on what an earlier command loaded are chained the way
 * reset_zone() runs them: 'G'/'E' give to the mobile the last 'M' loaded
 * (until the next 'M'); 'P' puts into the latest loaded copy of its
 * container; 'T'/'V' on a mob or object target the last mobile or object
 * loaded.  When that earlier command is cancelled here, the dependent one
 * would act on an earlier mobile or object at the next reset (or fail at
 * every reset), so it is cancelled too, and each one is reported. */
static void cancel_room_reset_commands(zone_rnum zone, room_rnum rnum,
                                       room_vnum deleted_vnum)
{
  int j, mob_gone = FALSE, tmob_gone = FALSE, tobj_gone = FALSE, ngone = 0;
  obj_rnum gone_containers[DELETE_ROOM_MAX_GONE_CONTAINERS];

  for (j = 0; zone_table[zone].cmd[j].command != 'S'; j++) {
    struct reset_com *c = &zone_table[zone].cmd[j];
    char cmd = c->command;          /* before this pass changes it */
    int gone = FALSE;

    switch (cmd) {
    case 'M':
    case 'O':
    case 'T':
    case 'V':
      if (c->arg3 == rnum)
        gone = TRUE;
      else if (c->arg3 > rnum)
        c->arg3 -= (c->arg3 != NOWHERE); /* with unsigned NOWHERE > any rnum */
      break;
    case 'D':
    case 'R':
      if (c->arg1 == rnum)
        gone = TRUE;
      else if (c->arg1 > rnum)
        c->arg1 -= (c->arg1 != NOWHERE); /* with unsigned NOWHERE > any rnum */
      break;
    case 'G':
    case 'P':
    case 'E':
    case '*':
      break;
    default:
      mudlog(BRF, LVL_GOD, TRUE, "SYSERR: GenOLC: delete_room: Unknown zone entry found!");
    }

    if (!gone &&
        (((cmd == 'G' || cmd == 'E') && mob_gone) ||
         (cmd == 'P' && obj_rnum_in(gone_containers, ngone, c->arg3)) ||
         ((cmd == 'T' || cmd == 'V') &&
          ((c->arg1 == MOB_TRIGGER && tmob_gone) ||
           (c->arg1 == OBJ_TRIGGER && tobj_gone))))) {
      TB_WARN_MSG("olc", "room-delete-orphan", "zone",
                  "zone %d line %d command %d '%c' acted on what a command "
                  "cancelled with deleted room vnum %d loaded; cancelled",
                  zone_table[zone].number, c->line, j, cmd, deleted_vnum);
      gone = TRUE;
    }
    if (gone)
      cancel_zone_command(c);	/* Frees a 'V' command's strings. */

    /* What the following commands act on, as reset_zone() tracks it. */
    switch (cmd) {
    case 'M':
      mob_gone = tmob_gone = gone;
      tobj_gone = FALSE;
      break;
    case 'O':
    case 'P':
    case 'G':
    case 'E':
      tobj_gone = gone;
      tmob_gone = FALSE;
      if (gone && !obj_rnum_in(gone_containers, ngone, c->arg1)) {
        if (ngone < DELETE_ROOM_MAX_GONE_CONTAINERS)
          gone_containers[ngone++] = c->arg1;
        else
          TB_WARN_MSG("olc", "room-delete-orphan", "zone",
                      "zone %d line %d: more than %d cancelled containers; "
                      "a later 'P' into obj rnum %d is not checked",
                      zone_table[zone].number, c->line,
                      DELETE_ROOM_MAX_GONE_CONTAINERS, (int)c->arg1);
      } else if (!gone) {     /* a live copy: later 'P's put into it */
        int k;
        for (k = 0; k < ngone; k++)
          if (gone_containers[k] == c->arg1)
            gone_containers[k] = gone_containers[--ngone];
      }
      break;
    case 'R':
    case 'D':
      tmob_gone = tobj_gone = FALSE;
      break;
    default:      /* 'T', 'V' and '*' leave the targets as they were */
      break;
    }
  }
}

/* Where a character whose GET_WAS_IN room was deleted is put back instead:
 * the start room it would log in to (interpreter.c: frozen, else immortal,
 * else mortal), found by its configured vnum in world[] as delete_room()
 * leaves it; the mortal start room (already renumbered, the Void if it was
 * the deleted room) when that vnum has no room. */
static room_rnum was_in_start_room(struct char_data *ch)
{
  room_vnum vnum = CONFIG_MORTAL_START;
  room_rnum start;

  if (PLR_FLAGGED(ch, PLR_FROZEN))
    vnum = CONFIG_FROZEN_START;
  else if (!IS_NPC(ch) && GET_LEVEL(ch) >= LVL_IMMORT)
    vnum = CONFIG_IMMORTAL_START;
  if ((start = real_room(vnum)) == NOWHERE)
    start = r_mortal_start_room;
  return start;
}

/**
 * Add a new room, or update the live room with the same vnum.
 *
 * The strings are copied, so the caller keeps (and must free) its own.
 * The caller's people/contents/events/script are never taken over: a new
 * room starts with empty lists (reported, room-add-shared-list, if the
 * caller passed live ones) and an updated room keeps its own.  proto_script
 * is taken by pointer: afterwards the room owns the list the caller passed.
 * An updated room's previous list is not freed here (see copy_room()): the
 * caller captured it before the call and frees it (redit_save_checked()).
 *
 * Inserting shifts world[] (see the file comment): every room pointer is
 * invalidated and every stored room rnum >= the new index is incremented.
 *
 * @param room Room to add, built by redit or rcopy; room->zone must be a
 *             valid zone rnum.
 * @return The room's rnum, or NOWHERE (reported) when room is NULL or its
 *         zone rnum is out of range; world[] is then unchanged.
 */
room_rnum add_room(struct room_data *room)
{
  struct char_data *tch;
  struct obj_data *tobj;
  int j, found = FALSE;
  room_rnum i;

  if (room == NULL) {
    TB_ERROR_MSG("olc", "room-add-null", "room", "called with no room; nothing added");
    return NOWHERE;
  }

  /* Both paths below index zone_table[room->zone] for the save list.  The
   * caller sets it from its own zone rnum, so a bad one is a broken caller:
   * refuse before world[] changes. */
  if (!VALID_ZONE_RNUM(room->zone)) {
    TB_ERROR_MSG("olc", "room-add-zone", "room",
                 "room vnum %d: zone rnum %d out of range 0..%d; nothing added",
                 room->number, (int)room->zone, (int)top_of_zone_table);
    return NOWHERE;
  }

  if ((i = real_room(room->number)) != NOWHERE) {
    if (SCRIPT(&world[i]))
      extract_script(&world[i], WLD_TRIGGER);
    tch = world[i].people;
    tobj = world[i].contents;
    copy_room(&world[i], room);
    world[i].people = tch;
    world[i].contents = tobj;
    add_to_save_list(zone_table[room->zone].number, SL_WLD);
    log("GenOLC: add_room: Updated existing room #%d.", room->number);
    return i;
  }

  RECREATE(world, struct room_data, top_of_world + 2);
  top_of_world++;

  for (i = top_of_world; i > 0; i--) {
    if (room->number > world[i - 1].number) {
      world[i] = *room;
      copy_room_strings(&world[i], room);
      found = i;
      break;
    } else {
      /* Copy the room over now. */
      world[i] = world[i - 1];
      update_wait_events(&world[i], &world[i-1]);

      /* People in this room must have their in_rooms moved up one. */
      for (tch = world[i].people; tch; tch = tch->next_in_room)
	IN_ROOM(tch) += (IN_ROOM(tch) != NOWHERE);

      /* Move objects too. */
      for (tobj = world[i].contents; tobj; tobj = tobj->next_content)
	IN_ROOM(tobj) += (IN_ROOM(tobj) != NOWHERE);
    }
  }
  if (!found) {
    world[0] = *room;	/* Last place, in front. */
    copy_room_strings(&world[0], room);
  }
  /* A new room starts empty.  The struct copy above also copied the
   * caller's runtime lists; rcopy and redit 'copy' pass a room set up from a
   * live room whose contents are still linked there, so keeping them would
   * make two rooms share one object list whose IN_ROOM names the source. */
  if (room->people || room->contents || room->events || room->script)
    TB_WARN_MSG("olc", "room-add-shared-list", "room",
                "room vnum %d: caller passed live%s%s%s%s; cleared in the new room",
                room->number, room->people ? " people" : "",
                room->contents ? " contents" : "", room->events ? " events" : "",
                room->script ? " script" : "");
  world[found].people = NULL;
  world[found].contents = NULL;
  world[found].events = NULL;
  world[found].script = NULL;
  /* world[] was reallocated and shifted: re-point room event owners. */
  rebase_room_events();

  log("GenOLC: add_room: Added room %d at index #%d.", room->number, found);
  /* found is equal to the array index where we added the room. */

  /* Renumber the room rnums stored in reset commands.  Every zone is
   * scanned: a command in a lower zone may name a room of a later zone
   * (delete_room() scans from 0 for the same reason). */
  for (i = 0; i <= top_of_zone_table; i++)
    for (j = 0; ZCMD(i, j).command != 'S'; j++)
      switch (ZCMD(i, j).command) {
      case 'M':
      case 'O':
      case 'T':
      case 'V':
	ZCMD(i, j).arg3 += (ZCMD(i, j).arg3 != NOWHERE && ZCMD(i, j).arg3 >= found);
	break;
      case 'D':
      case 'R':
	ZCMD(i, j).arg1 += (ZCMD(i, j).arg1 != NOWHERE && ZCMD(i, j).arg1 >= found);
      case 'G':
      case 'P':
      case 'E':
      case '*':
	/* Known zone entries we don't care about. */
        break;
      default:
        mudlog(BRF, LVL_GOD, TRUE, "SYSERR: GenOLC: add_room: Unknown zone entry found!");
      }

  /* Update the loadroom table. Adds 1 or 0; NOWHERE must not wrap to 0. */
  r_mortal_start_room += (r_mortal_start_room != NOWHERE && r_mortal_start_room >= found);
  r_immort_start_room += (r_immort_start_room != NOWHERE && r_immort_start_room >= found);
  r_frozen_start_room += (r_frozen_start_room != NOWHERE && r_frozen_start_room >= found);

  /* GET_WAS_IN (set when a linkless player is moved to the Void, used to
   * put them back on reconnect) is a room rnum too. */
  for (tch = character_list; tch; tch = tch->next)
    GET_WAS_IN(tch) += (GET_WAS_IN(tch) != NOWHERE && GET_WAS_IN(tch) >= found);

  /* Update world exits. */
  i = top_of_world + 1;
  do {
    i--;
    for (j = 0; j < DIR_COUNT; j++)
      if (W_EXIT(i, j) && W_EXIT(i, j)->to_room != NOWHERE)
	W_EXIT(i, j)->to_room += (W_EXIT(i, j)->to_room >= found);
  } while (i > 0);

  add_to_save_list(zone_table[room->zone].number, SL_WLD);

  /* Return what array entry we placed the new room in. */
  return found;
}

/**
 * Delete room rnum.
 *
 * People and objects in it move to the Void (rnum 0); its scripts and
 * events are cancelled; exits into it are removed (or lead to NOWHERE when
 * they carry a keyword or description); reset commands that name it are
 * disabled ('*', a 'V' command's strings freed); start rooms that were
 * this room fall back to the Void, and a GET_WAS_IN that was it to the
 * character's start room.  world[] then shifts down (see the file comment)
 * and every stored room rnum above rnum is decremented.
 *
 * @param rnum Room rnum from redit; 0 (the Void) cannot be deleted.
 * @return TRUE when the room was deleted; FALSE, reported, otherwise.
 */
int delete_room(room_rnum rnum)
{
  room_rnum i;
  room_vnum deleted_vnum;
  int j;
  struct char_data *ppl, *next_ppl;
  struct obj_data *obj, *next_obj;
  struct room_data *room;

  /* Can't delete the Void: it is where everything else is moved. */
  if (rnum <= 0 || rnum > top_of_world) {
    TB_WARN_MSG("olc", "room-delete-range", "room",
                "rnum %d out of range 1..%d; nothing deleted",
                (int)rnum, (int)top_of_world);
    return FALSE;
  }

  room = &world[rnum];
  deleted_vnum = room->number;   /* room is stale once world[] shifts */

  add_to_save_list(zone_table[room->zone].number, SL_WLD);

  /* This is something you might want to read about in the logs. */
  log("GenOLC: delete_room: Deleting room #%d (%s).", room->number, room->name);

  /* The start rooms are rnums.  One equal to the deleted room falls back to
   * the Void; one above it must move down with the world[] shift below
   * (mirroring add_room()), or it names the following room, or
   * top_of_world + 1 when it was the last room. */
  if (r_mortal_start_room == rnum) {
    TB_WARN_MSG("olc", "room-delete-start", "room",
                "deleted room vnum %d was the mortal start room; reset to the Void",
                room->number);
    r_mortal_start_room = 0;	/* The Void */
  } else if (r_mortal_start_room != NOWHERE && r_mortal_start_room > rnum)
    r_mortal_start_room--;
  if (r_immort_start_room == rnum) {
    TB_WARN_MSG("olc", "room-delete-start", "room",
                "deleted room vnum %d was the immortal start room; reset to the Void",
                room->number);
    r_immort_start_room = 0;	/* The Void */
  } else if (r_immort_start_room != NOWHERE && r_immort_start_room > rnum)
    r_immort_start_room--;
  if (r_frozen_start_room == rnum) {
    TB_WARN_MSG("olc", "room-delete-start", "room",
                "deleted room vnum %d was the frozen start room; reset to the Void",
                room->number);
    r_frozen_start_room = 0;	/* The Void */
  } else if (r_frozen_start_room != NOWHERE && r_frozen_start_room > rnum)
    r_frozen_start_room--;

  /* Dump the contents of this room into the Void.  We could also just extract 
   * the people, mobs, and objects here. */
  for (obj = world[rnum].contents; obj; obj = next_obj) {
    next_obj = obj->next_content;
    obj_from_room(obj);
    obj_to_room(obj, 0);
  }
  for (ppl = world[rnum].people; ppl; ppl = next_ppl) {
    next_ppl = ppl->next_in_room;
    char_from_room(ppl);
    char_to_room(ppl, 0);
  }

  free_room_strings(room);
  if (SCRIPT(room))
    extract_script(room, WLD_TRIGGER);
  free_proto_script(room, WLD_TRIGGER);

  if (room->events != NULL) {
	  if (room->events->iSize > 0) {
		struct event * pEvent;

		while ((pEvent = simple_list(room->events)) != NULL)
		  event_cancel(pEvent);
	  }
	  free_list(room->events);
    room->events = NULL;
  }

  /* Change any exit going to this room to go the void. Also fix all the exits 
   * pointing to rooms above this. */
  i = top_of_world + 1;
  do {
    i--;
    for (j = 0; j < DIR_COUNT; j++) {
      if (W_EXIT(i, j) == NULL)
        continue;
      else if (W_EXIT(i, j)->to_room > rnum)
        W_EXIT(i, j)->to_room -= (W_EXIT(i, j)->to_room != NOWHERE); /* with unsigned NOWHERE > any rnum */
      else if (W_EXIT(i, j)->to_room == rnum) {
      	if ((!W_EXIT(i, j)->keyword || !*W_EXIT(i, j)->keyword) &&
      	    (!W_EXIT(i, j)->general_description || !*W_EXIT(i, j)->general_description)) {
          /* no description, remove exit completely */
          if (W_EXIT(i, j)->keyword)
            free(W_EXIT(i, j)->keyword);
          if (W_EXIT(i, j)->general_description)
            free(W_EXIT(i, j)->general_description);
          free(W_EXIT(i, j));
          W_EXIT(i, j) = NULL;
        } else {
          /* description is set, just point to nowhere */
          W_EXIT(i, j)->to_room = NOWHERE;
        }
      }
    }
  } while (i > 0);

  /* Cancel the reset commands that load into this room, renumber the ones
   * that load above it, and cancel what depended on a cancelled command. */
  for (i = 0; i <= top_of_zone_table; i++)
    cancel_room_reset_commands(i, rnum, deleted_vnum);

  /* Remove this room from all shop lists. */
  for (i = 0; i <= top_shop; i++) {
    for (j = 0;SHOP_ROOM(i, j) != NOWHERE;j++) {
      if (SHOP_ROOM(i, j) == world[rnum].number)
        SHOP_ROOM(i, j) = 0; /* set to the void */
    }
  }
  /* Now we actually move the rooms down. */
  for (i = rnum; i < top_of_world; i++) {
    world[i] = world[i + 1];
    update_wait_events(&world[i], &world[i+1]);

    for (ppl = world[i].people; ppl; ppl = ppl->next_in_room)
      IN_ROOM(ppl) -= (IN_ROOM(ppl) != NOWHERE);	/* Redundant check? */

    for (obj = world[i].contents; obj; obj = obj->next_content)
      IN_ROOM(obj) -= (IN_ROOM(obj) != NOWHERE);	/* Redundant check? */
  }

  top_of_world--;
  RECREATE(world, struct room_data, top_of_world + 1);
  /* world[] was shifted and reallocated: re-point room event owners. */
  rebase_room_events();

  /* GET_WAS_IN is where an idle or linkless player is put back on their
   * next command (comm.c); renumber it like the start rooms.  One that was
   * the deleted room gets the character's start room, looked up by vnum in
   * the shifted world[]: the Void (rnum 0) left the player stranded there. */
  for (ppl = character_list; ppl; ppl = ppl->next) {
    if (GET_WAS_IN(ppl) == NOWHERE)
      continue;
    if (GET_WAS_IN(ppl) == rnum) {
      GET_WAS_IN(ppl) = was_in_start_room(ppl);
      TB_WARN_MSG("olc", "room-delete-wasin", "room",
                  DIAG_CHAR_FMT " was_in room vnum %d, which was deleted; "
                  "now start room vnum %d", DIAG_CHAR(ppl), deleted_vnum,
                  world[GET_WAS_IN(ppl)].number);
    } else if (GET_WAS_IN(ppl) > rnum)
      GET_WAS_IN(ppl)--;
  }

  return TRUE;
}

int save_rooms(zone_rnum rzone)
{
  int i;
  struct room_data *room;
  FILE *sf;
  char filename[128];
  char buf[MAX_STRING_LENGTH];
  char buf1[MAX_STRING_LENGTH];
  char buf2[MAX_STRING_LENGTH];

  if (!VALID_ZONE_RNUM(rzone)) {
    log("SYSERR: GenOLC: save_rooms: Invalid zone number %d passed! (0-%d)", rzone, top_of_zone_table);
    return FALSE;
  }

  log("GenOLC: save_rooms: Saving rooms in zone #%d (%d-%d).",
	zone_table[rzone].number, genolc_zone_bottom(rzone), zone_table[rzone].top);

  tb_snprintf(filename, sizeof(filename), "%s/%d.new", WLD_PREFIX, zone_table[rzone].number);
  if (!(sf = fopen(filename, "w"))) {
    TB_REPORT_ERRNO("fopen", filename);
    return FALSE;
  }

  for (i = genolc_zone_bottom(rzone); i <= zone_table[rzone].top; i++) {
    room_rnum rnum;

    if ((rnum = real_room(i)) != NOWHERE) {
      int j;

      room = (world + rnum);

      /* Copy the description and strip off trailing newlines. */
      strlcpy(buf, room->description ? room->description : "Empty room.", sizeof(buf));
      strip_cr(buf);

      /* Save the numeric and string section of the file. */
      int n = snprintf(buf2, MAX_STRING_LENGTH, "#%d\n"
			"%s%c\n"
			"%s%c\n"
			"%d %d %d %d %d %d\n",
	room->number,
	room->name ? room->name : "Untitled", STRING_TERMINATOR,
	buf, STRING_TERMINATOR,
	zone_table[room->zone].number, room->room_flags[0], room->room_flags[1], room->room_flags[2], 
	  room->room_flags[3], room->sector_type 
      );
      
      if (n < 0) {
        mudlog(BRF, LVL_BUILDER, TRUE,
               "SYSERR: Could not format room #%d for saving; zone %d not saved.",
               room->number, zone_table[rzone].number);
        tb_fclose(sf);
        if (remove(filename) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", filename);
        return FALSE;
      }

      if (n >= MAX_STRING_LENGTH) {
        mudlog(BRF, LVL_BUILDER, TRUE,
               "SYSERR: Could not save room #%d due to size (%d >= maximum of %d); "
               "zone %d not saved.",
               room->number, n, MAX_STRING_LENGTH, zone_table[rzone].number);
        tb_fclose(sf);
        if (remove(filename) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", filename);
        return FALSE;
      }

  tb_fprintf(sf, "%s", convert_from_tabs(buf2));
 
      /* Now you write out the exits for the room. */
      for (j = 0; j < DIR_COUNT; j++) {
	if (R_EXIT(room, j)) {
	  int dflag;
	  if (R_EXIT(room, j)->general_description) {
	    strlcpy(buf, R_EXIT(room, j)->general_description, sizeof(buf));
	    strip_cr(buf);
	  } else
	    *buf = '\0';

	  /* Figure out door flag. */
	  if (IS_SET(R_EXIT(room, j)->exit_info, EX_ISDOOR)) {
	    if (IS_SET(R_EXIT(room, j)->exit_info, EX_PICKPROOF))
	      dflag = 2;
	    else
	      dflag = 1;
	      
	   if (IS_SET(R_EXIT(room, j)->exit_info, EX_HIDDEN))
          dflag += 2;   
          
	  } else
	    dflag = 0;

	  if (R_EXIT(room, j)->keyword)
	    strlcpy(buf1, R_EXIT(room, j)->keyword, sizeof(buf1));
	  else
	    *buf1 = '\0';

	  /* Now write the exit to the file. */
	  tb_fprintf(sf,	"D%d\n"
			"%s~\n"
			"%s~\n"
			"%d %d %d\n", j, buf, buf1, dflag,
		R_EXIT(room, j)->key != NOTHING ? R_EXIT(room, j)->key : -1,
		R_EXIT(room, j)->to_room != NOWHERE ? world[R_EXIT(room, j)->to_room].number : -1);

	}
      }

      if (room->ex_description) {
        struct extra_descr_data *xdesc;

	for (xdesc = room->ex_description; xdesc; xdesc = xdesc->next) {
	  /* parse_room() stores fread_string() results, which are NULL for an
	   * empty '~' body.  Writing an empty field round-trips to the same
	   * NULL on the next boot, so the entry is kept, but it is reported
	   * because nobody can look at an extra description without both. */
	  if (!xdesc->keyword || !xdesc->description)
	    TB_WARN_MSG("olc", "room-save-exdesc", filename,
	                "room vnum %d zone %d: extra desc with NULL %s saved empty",
	                room->number, zone_table[rzone].number,
	                !xdesc->keyword ? "keyword" : "description");
	  strlcpy(buf, xdesc->description ? xdesc->description : "", sizeof(buf));
	  strip_cr(buf);
	  tb_fprintf(sf,	"E\n"
			"%s~\n"
			"%s~\n", xdesc->keyword ? xdesc->keyword : "", buf);
	}
      }
      tb_fprintf(sf, "S\n");
      script_save_to_disk(sf, room, WLD_TRIGGER);
    }
  }

  /* Write the final line and commit only a fully flushed temporary file. */
  tb_fprintf(sf, "$~\n");
  {
    /* ferror() stays set after any failed write.  tb_fprintf() reported
     * each one, but a later flush that succeeds would hide it and commit a
     * file with a hole in it (C2-16). */
    int stream_ok = !ferror(sf);
    int flush_ok = tb_fflush_checked(sf, filename);
    int close_ok = tb_fclose_checked(sf, filename);
    if (!stream_ok)
      TB_ERROR_MSG("olc", "save-stream-error", filename,
                   TB_ERRID_PERSIST_WRITE " zone %d rooms: a write failed; previous file kept",
                   zone_table[rzone].number);
    if (!stream_ok || !flush_ok || !close_ok) {
      if (remove(filename) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", filename);
      return FALSE;
    }
  }

  /* Old file we're replacing. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.wld", WLD_PREFIX, zone_table[rzone].number);
  if (!tb_replace_file_checked(filename, buf))
    return FALSE;

  if (in_save_list(zone_table[rzone].number, SL_WLD))
    remove_from_save_list(zone_table[rzone].number, SL_WLD);
  return TRUE;
}

/**
 * Replace live room `to` with the OLC copy `from`.
 *
 * `to`'s strings are freed; the copy's strings are deep-copied and its
 * proto_script pointer is taken over.  `to`'s previous proto_script is NOT
 * freed: it belongs to the caller, which captured it before add_room() and
 * frees it after (redit_save_checked(), audit F1-08).  Freeing it here as
 * well (C2-11) freed the list twice on every redit save of a room with
 * triggers.  `to` keeps its own events.  `from`'s people, contents and
 * events are cleared so nothing is linked from two rooms; the caller
 * restores `to`'s people and contents (add_room() does).
 *
 * @return TRUE.
 */
int copy_room(struct room_data *to, struct room_data *from)
{
  struct list_data *to_events = to->events;

  /* `to` is a live room (add_room() update path) and `from` the OLC copy.
   * Pending events point at `to`, so `to` keeps its own list: taking the
   * copy's (always NULL from redit) would orphan them, and delete_room()
   * could then never cancel them. */
  if (from->events && from->events != to_events)
    TB_WARN_MSG("olc", "room-copy-events", "room",
                "room vnum %d: source copy carried its own events list; "
                "the live room's list was kept", from->number);
  /* The live room's trigger list is overwritten below but not freed: the
   * caller holds it and frees it (see the function comment). */
  free_room_strings(to);
  *to = *from;
  copy_room_strings(to, from);
  to->events = to_events;

  /* Don't put people and objects in two locations. Should this be done here? */
  from->people = NULL;
  from->contents = NULL;
  from->events = NULL;

  return TRUE;
}

/* Copy strings over so bad things don't happen.  We do not free the existing 
 * strings here because copy_room() did a shallow copy previously and we'd be 
 * freeing the very strings we're copying.  If this function is used elsewhere,
 * be sure to free_room_strings() the 'dest' room first. */
int copy_room_strings(struct room_data *dest, struct room_data *source)
{
  int i;

  if (dest == NULL || source == NULL) {
    log("SYSERR: GenOLC: copy_room_strings: NULL values passed.");
    return FALSE;
  }

  dest->description = str_udup(source->description);
  dest->name = str_udup(source->name);

  for (i = 0; i < DIR_COUNT; i++) {
    if (!R_EXIT(source, i))
      continue;

    CREATE(R_EXIT(dest, i), struct room_direction_data, 1);
    *R_EXIT(dest, i) = *R_EXIT(source, i);
    if (R_EXIT(source, i)->general_description)
      R_EXIT(dest, i)->general_description = strdup(R_EXIT(source, i)->general_description);
    if (R_EXIT(source, i)->keyword)
      R_EXIT(dest, i)->keyword = strdup(R_EXIT(source, i)->keyword);
  }

  if (source->ex_description)
    copy_ex_descriptions(&dest->ex_description, source->ex_description);

  return TRUE;
}

int free_room_strings(struct room_data *room)
{
  int i;

  /* Free descriptions. */
  if (room->name)
    free(room->name);
  if (room->description)
    free(room->description);
  if (room->ex_description)
    free_ex_descriptions(room->ex_description);

  /* Free exits. */
  for (i = 0; i < DIR_COUNT; i++) {
    if (room->dir_option[i]) {
      if (room->dir_option[i]->general_description)
        free(room->dir_option[i]->general_description);

      if (room->dir_option[i]->keyword)
        free(room->dir_option[i]->keyword);

      free(room->dir_option[i]);
      room->dir_option[i] = NULL;
    }
  }

  return TRUE;
}
