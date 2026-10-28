/**************************************************************************
*  File: genwld.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - Rooms.                                    *
*                                                                         *
*  By Levork. Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer. *
**************************************************************************/

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


/* This function will copy the strings so be sure you free your own copies of 
 * the description, title, and such. */
room_rnum add_room(struct room_data *room)
{
  struct char_data *tch;
  struct obj_data *tobj;
  int j, found = FALSE;
  room_rnum i;

  if (room == NULL)
    return NOWHERE;

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

  /* Find what zone that room was in so we can update the loading table. */
  for (i = room->zone; i <= top_of_zone_table; i++)
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

  /* Update the loadroom table. Adds 1 or 0. */
  r_mortal_start_room += (r_mortal_start_room >= found);
  r_immort_start_room += (r_immort_start_room >= found);
  r_frozen_start_room += (r_frozen_start_room >= found);

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

int delete_room(room_rnum rnum)
{
  room_rnum i;
  int j;
  struct char_data *ppl, *next_ppl;
  struct obj_data *obj, *next_obj;
  struct room_data *room;

  if (rnum <= 0 || rnum > top_of_world)	/* Can't delete void yet. */
    return FALSE;

  room = &world[rnum];

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

  /* Find what zone that room was in so we can update the loading table. */
  for (i = 0; i <= top_of_zone_table; i++)
    for (j = 0; ZCMD(i , j).command != 'S'; j++)
      switch (ZCMD(i, j).command) {
      case 'M':
      case 'O':
      case 'T':
      case 'V':
	if (ZCMD(i, j).arg3 == rnum)
	  ZCMD(i, j).command = '*';	/* Cancel command. */
	else if (ZCMD(i, j).arg3 > rnum)
	  ZCMD(i, j).arg3 -= (ZCMD(i, j).arg3 != NOWHERE); /* with unsigned NOWHERE > any rnum */
	break;
      case 'D':
      case 'R':
	if (ZCMD(i, j).arg1 == rnum)
	  ZCMD(i, j).command = '*';	/* Cancel command. */
	else if (ZCMD(i, j).arg1 > rnum)
	  ZCMD(i, j).arg1 -= (ZCMD(i, j).arg1 != NOWHERE); /* with unsigned NOWHERE > any rnum */
      case 'G':
      case 'P':
      case 'E':
      case '*':
        /* Known zone entries we don't care about. */
        break;
      default:
        mudlog(BRF, LVL_GOD, TRUE, "SYSERR: GenOLC: delete_room: Unknown zone entry found!");
      }

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

#if CIRCLE_UNSIGNED_INDEX
  if (rzone == NOWHERE || rzone > top_of_zone_table) {
#else
  if (rzone < 0 || rzone > top_of_zone_table) {
#endif
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
    int flush_ok = tb_fflush_checked(sf, filename);
    int close_ok = tb_fclose_checked(sf, filename);
    if (!flush_ok || !close_ok) {
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

int copy_room(struct room_data *to, struct room_data *from)
{
  free_room_strings(to);
  *to = *from;
  copy_room_strings(to, from);
  to->events = from->events;

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
