/**************************************************************************
*  File: mud_event.c                                       Part of tbaMUD *
*  Usage: Handling of the mud event system                                *
*                                                                         *
*  By Vatiken. Copyright 2012 by Joseph Arnusch                           *
**************************************************************************/

/**
 * @file mud_event.c
 * Named, owner-attached events on top of the dg_event queue: each
 * mud_event_data says what it is (iId, an index into mud_event_index[]) and
 * whom it belongs to, and sits on that owner's events list so the owner can
 * find (char_has_mud_event) and cancel its events.
 *
 * Lifetime: the event queue owns the struct event; the mud_event_data and
 * its sVariables copy are freed by free_mud_event() when the event finishes
 * or is cancelled. Owners must cancel their events before they are freed;
 * rooms are referenced by vnum because world[] is reallocated by OLC.
 * Input: events are created by game code only (spells, skills, protocols).
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_event.h"
#include "constants.h"
#include "comm.h"  /* For access to the game pulse */
#include "mud_event.h"

/* Global List */
struct list_data * world_events = NULL;

/* The mud_event_index[] is merely a tool for organizing events, and giving
 * them a "const char *" name to help in potential debugging */
struct mud_event_list mud_event_index[] = {
  { "Null"         , NULL           , -1          },  /* eNULL */
  { "Protocol"     , get_protocols  , EVENT_DESC  },  /* ePROTOCOLS */
  { "Whirlwind"    , event_whirlwind, EVENT_CHAR  },  /* eWHIRLWIND */
  { "Spell:Darkness",event_countdown, EVENT_ROOM  }   /* eSPL_DARKNESS */
};

/* init_events() is the ideal function for starting global events. This
 * might be the case if you were to move the contents of heartbeat() into
 * the event system */
void init_events(void)
{
  /* Allocate Event List */
  world_events = create_list();
}

/* event_countdown() is used for events which are to be used as a countdown...
 * go figure eh? This could be useful for skills which have an extended cooldown,
 * like "lay on hands" once every 24 hours. Simply add an event to the
 * mud_event_index[] such as:
   * { "Lay on hands"    , event_countdown, EVENT_CHAR  }
 * and then add the event after a successful skill call:
   * NEW_EVENT(eLAYONHANDS, ch, NULL, 24 * SECS_PER_MUD_HOUR)
 * and then add something like this is your skill function:
   * if (char_has_mud_event(ch, eLAYONHANDS)) {
   *   send_to_char(ch, "You must wait a full 24 hours before re-using this skill.\r\n");
   *   return;
   * }
 * The bottom switch() is for any post-event actions, like telling the character they can
 * now access their skill again.
 */
/* Current rnum of an EVENT_ROOM event's room, resolved from its vnum each
 * time (world[] moves when redit adds or deletes rooms). NOWHERE, reported,
 * if the room no longer exists. */
static room_rnum mud_event_room(struct mud_event_data *pMudEvent)
{
  room_rnum rnum = real_room(pMudEvent->iRoomVnum);

  if (VALID_ROOM_RNUM(rnum))
    return rnum;
  TB_WARN_MSG("event", "room-gone", "world",
              "event %s room vnum %d no longer exists",
              mud_event_index[pMudEvent->iId].event_name,
              (int) pMudEvent->iRoomVnum);
  return NOWHERE;
}

EVENTFUNC(event_countdown)
{
  struct mud_event_data * pMudEvent;
  room_rnum rnum = NOWHERE;

  pMudEvent = (struct mud_event_data * ) event_obj;

  switch (mud_event_index[pMudEvent->iId].iEvent_Type) {
    case EVENT_CHAR:
      break;
    case EVENT_ROOM:
      if ((rnum = mud_event_room(pMudEvent)) == NOWHERE)
        return 0;  /* reported; the room is gone, nothing to undo */
      break;
    default:
      break;
  }

  switch (pMudEvent->iId) {
    case eSPL_DARKNESS:
      if (!VALID_ROOM_RNUM(rnum)) {
        TB_ERROR_MSG("event", "room-gone", "world",
                     "event %s fired with no room (rnum %d)",
                     mud_event_index[pMudEvent->iId].event_name, (int) rnum);
        break;
      }
      REMOVE_BIT_AR(ROOM_FLAGS(rnum), ROOM_DARK);
      send_to_room(rnum, "The dark shroud disappates.\r\n");
      break;
    case ePROTOCOLS:
      break;
    case eWHIRLWIND:
      break;
    case eNULL:
      break;
  }

  return 0;
}
/* As of 3.63, there are only global, descriptor, and character events. This
 * is due to the potential scope of the necessary debugging if events were
 * included with rooms, objects, spells or any other structure type. Adding
 * events to these other systems should be just as easy as adding the current
 * library was, and should be available in a future release. - Vat */
/**
 * Queue pMudEvent to fire in `time` pulses and link it on its owner's
 * events list (created on first use).
 *
 * Owner by type (mud_event_index[].iEvent_Type):
 *   EVENT_WORLD  world_events
 *   EVENT_DESC   pStruct = descriptor; its list is made in new_descriptor()
 *   EVENT_CHAR   pStruct = character
 *   EVENT_ROOM   iRoomVnum; pStruct is NULL (world[] moves under redit)
 * pStruct must stay valid until the event fires or is cancelled: every
 * owner cancels its events before it is freed (free_char, extract_obj,
 * delete_room, destroy_db, close_socket).
 */
void attach_mud_event(struct mud_event_data *pMudEvent, long time)
{
  struct event * pEvent;
  struct descriptor_data * d;
  struct char_data * ch;
  struct room_data * room;
  room_rnum rnum;

  pEvent = event_create(mud_event_index[pMudEvent->iId].func, pMudEvent, time);
  pEvent->isMudEvent = TRUE;
  pMudEvent->pEvent = pEvent;        

  switch (mud_event_index[pMudEvent->iId].iEvent_Type) {
    case EVENT_WORLD:
      add_to_list(pEvent, world_events);
    break;
    case EVENT_DESC:
      d = (struct descriptor_data *) pMudEvent->pStruct;
      add_to_list(pEvent, d->events);
    break;
    case EVENT_CHAR:
      ch = (struct char_data *) pMudEvent->pStruct;
      
      if (ch->events == NULL)
        ch->events = create_list();
              
      add_to_list(pEvent, ch->events);
    break;
    case EVENT_ROOM:
      /* The event is already queued; if its room cannot be found it still
       * fires (and frees) normally, it just has no room list to sit on. */
      if ((rnum = mud_event_room(pMudEvent)) == NOWHERE)
        break;
      room = &world[rnum];

      if (room->events == NULL)
        room->events = create_list();

      add_to_list(pEvent, room->events);
    break;
  }
}

struct mud_event_data *new_mud_event(event_id iId, void *pStruct, char *sVariables)
{
  struct mud_event_data *pMudEvent;
  char *varString;

  CREATE(pMudEvent, struct mud_event_data, 1);
  varString = (sVariables != NULL) ? strdup(sVariables) : NULL;

  pMudEvent->iId         = iId;
  pMudEvent->pStruct     = pStruct;
  pMudEvent->sVariables  = varString;
  pMudEvent->pEvent      = NULL;
  pMudEvent->iRoomVnum   = NOWHERE;

  /* Rooms are kept by vnum, never by address: see iRoomVnum. */
  if (mud_event_index[iId].iEvent_Type == EVENT_ROOM) {
    if (pStruct)
      pMudEvent->iRoomVnum = ((struct room_data *) pStruct)->number;
    pMudEvent->pStruct = NULL;
  }

  return (pMudEvent);
}

/**
 * Unlink pMudEvent from its owner's events list and free it (called by
 * dg_event.c when the event finishes or is cancelled; never call it
 * directly on a queued event). The owner's list is left allocated, possibly
 * empty: the owner frees it. The struct event itself is freed by the
 * caller; its event_obj is set to NULL here.
 */
void free_mud_event(struct mud_event_data *pMudEvent)
{
  struct descriptor_data * d;
  struct char_data * ch;
  struct room_data * room;
  room_rnum rnum;

  switch (mud_event_index[pMudEvent->iId].iEvent_Type) {
    case EVENT_WORLD:
      remove_from_list(pMudEvent->pEvent, world_events);
    break;
    case EVENT_DESC:
      d = (struct descriptor_data *) pMudEvent->pStruct;
      remove_from_list(pMudEvent->pEvent, d->events);
    break;
    /* The owner's events list is left in place, even when it becomes empty:
     * the owner frees it on teardown (free_char, extract_obj, delete_room,
     * destroy_db, close_socket), right after a cancel loop that calls this.
     * Freeing it here used to hand those loops a NULL list for free_list(). */
    case EVENT_CHAR:
      ch = (struct char_data *) pMudEvent->pStruct;
      if (ch->events)
        remove_from_list(pMudEvent->pEvent, ch->events);
    break;
    case EVENT_ROOM:
      if ((rnum = mud_event_room(pMudEvent)) == NOWHERE)
        break;
      room = &world[rnum];
      if (room->events)
        remove_from_list(pMudEvent->pEvent, room->events);
    break;
  }

  if (pMudEvent->sVariables != NULL)
    free(pMudEvent->sVariables);

  pMudEvent->pEvent->event_obj = NULL;
  free(pMudEvent);
}

struct mud_event_data * char_has_mud_event(struct char_data * ch, event_id iId)
{
  struct event * pEvent;
  struct mud_event_data * pMudEvent = NULL;
  struct iterator_data it;
  bool found = FALSE;

  if (ch->events == NULL)
    return NULL;

  if (ch->events->iSize == 0)
    return NULL;

  /* A private iterator: breaking out of a simple_list() loop would leave
   * the global cursor parked mid-list for the next caller. */
  for (pEvent = (struct event *) merge_iterator(&it, ch->events); pEvent;
       pEvent = (struct event *) next_in_list(&it)) {
    if (!pEvent->isMudEvent)
      continue;
     pMudEvent = (struct mud_event_data * ) pEvent->event_obj;
    if (pMudEvent->iId == iId) {
     found = TRUE;
     break;
    }
  }
  remove_iterator(&it);

  if (found)
    return (pMudEvent);
  
  return NULL;
} 

void clear_char_event_list(struct char_data * ch)
{
  struct event * pEvent;
    
  if (ch->events == NULL)
    return;
    
  if (ch->events->iSize == 0)
    return;
    
  clear_simple_list();  

  while ((pEvent = (struct event *) simple_list(ch->events)) != NULL) {
    event_cancel(pEvent);
  } 
}
