/**************************************************************************
*  File: oasis.c                                           Part of tbaMUD *
*  Usage: Oasis - General.                                                *
*                                                                         *
* By Levork. Copyright 1996 Harvey Gilpin. 1997-2001 George Greer.        *
**************************************************************************/

/**
 * @file oasis.c
 * Shared OLC plumbing: the per-descriptor workspace lifecycle
 * (cleanup_olc, olc_attach_at), zone edit permission (can_edit_zone) and the
 * menu colour globals.
 *
 * Ownership: d->olc and everything it owns is released only here, by
 * cleanup_olc().  The per-field rules are documented on struct
 * oasis_olc_data in oasis.h.  Inputs are builder commands (LVL_BUILDER and
 * up) and the world tables; nothing here parses player text.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "comm.h"
#include "db.h"
#include "shop.h"
#include "genolc.h"
#include "genmob.h"
#include "genshp.h"
#include "genzon.h"
#include "genwld.h"
#include "genobj.h"
#include "oasis.h"
#include "olc_checks.h"
#include "screen.h"
#include "dg_olc.h"
#include "act.h"
#include "handler.h" /* for is_name */
#include "quest.h"
#include "ibt.h"
#include "msgedit.h"

/* Global variables defined here, used elsewhere */
const char *nrm, *grn, *cyn, *yel;

/* Internal Function prototypes  */
static void free_config(struct config_data *data);

/* Only player characters should be using OLC anyway. */
void clear_screen(struct descriptor_data *d)
{
  if (PRF_FLAGGED(d->character, PRF_CLS))
    write_to_output(d, "[H[J");
}

/* Exported utilities */
/** Set the colour string pointers for that which this char will see at
 * colour level NRM.  Changing the entries here will change the colour scheme
 * throughout the OLC.
 *
 * nrm/grn/cyn/yel are process-global and hold the codes of whichever
 * character called this last.  Every menu must call get_char_colors()
 * immediately before formatting with them, and must not keep using them
 * after another descriptor's input has been processed. */
void get_char_colors(struct char_data *ch)
{
  nrm = CCNRM(ch, C_NRM);
  grn = CCGRN(ch, C_NRM);
  cyn = CCCYN(ch, C_NRM);
  yel = CCYEL(ch, C_NRM);
}

/* Release the descriptor's working trigger list (OLC_SCRIPT).
 *
 * Ownership rule (F3-08): dg_olc_script_copy gives the descriptor a private
 * copy of the edited entity's trigger list.  The descriptor owns it until a
 * *_save_internally hands it to the live prototype, which then sets
 * OLC_SCRIPT(d) to NULL.  Every other exit (quit without changes, link loss
 * through close_socket, editor errors) reaches here still owning it.
 *
 * The 'n' (abandon) paths in redit/medit re-attach the list to the scratch
 * entity's proto_script; detach it there so it is freed exactly once.  A
 * list that is also a live prototype's proto_script means a save path forgot
 * to clear OLC_SCRIPT: report it and do not free what the world still uses. */
static void olc_release_script(struct descriptor_data *d)
{
  struct trig_proto_list *script = OLC_SCRIPT(d), *next;
  struct trig_proto_list *live = NULL;
  room_rnum rr;
  obj_rnum ro;
  mob_rnum rm;

  if (script == NULL)
    return;
  OLC_SCRIPT(d) = NULL;

  if (OLC_ROOM(d) && (rr = real_room(OLC_NUM(d))) != NOWHERE)
    live = world[rr].proto_script;
  else if (OLC_OBJ(d) && (ro = real_object(OLC_NUM(d))) != NOTHING)
    live = obj_proto[ro].proto_script;
  else if (OLC_MOB(d) && (rm = real_mobile(OLC_NUM(d))) != NOBODY)
    live = mob_proto[rm].proto_script;

  if (live == script) {
    TB_ERROR_MSG("olc", "script-owner", "trig_proto_list",
                 "OLC_SCRIPT still set after save for vnum %d (state %d) of "
                 DIAG_CHAR_FMT "; left with the prototype",
                 (int)OLC_NUM(d), STATE(d), DIAG_CHAR(d->character));
    return;
  }

  if (OLC_ROOM(d) && OLC_ROOM(d)->proto_script == script)
    OLC_ROOM(d)->proto_script = NULL;
  if (OLC_OBJ(d) && OLC_OBJ(d)->proto_script == script)
    OLC_OBJ(d)->proto_script = NULL;
  if (OLC_MOB(d) && OLC_MOB(d)->proto_script == script)
    OLC_MOB(d)->proto_script = NULL;

  for (; script; script = next) {
    next = script->next;
    free(script);
  }
}

/** Release d's OLC workspace and return d to normal play.
 *
 * Frees, according to cleanup_type (see CLEANUP_* in oasis.h):
 * - always: OLC_SCRIPT (unless a save handed it to the prototype),
 *   OLC_CONFIG, the zone copy with its command sargs, the shop copy, ibt,
 *   message list, storage, trig and prefs;
 * - room/quest/action/help: struct and strings for CLEANUP_ALL, struct only
 *   for CLEANUP_STRUCTS; obj and mob: struct and strings either way.
 * Never frees OLC_DESC (borrowed) or OLC_IEDIT_OBJ (a live object).
 *
 * Safe on any descriptor, including from close_socket during link loss: it
 * makes no assumption about the character's room.  If STATE(d) is an editor
 * state it clears PLR_WRITING, broadcasts "$n stops using OLC." to the room,
 * logs to mudlog and resets STATE(d) to CON_PLAYING; one-shot internal users
 * (dig, buildwalk, copy) stay in CON_PLAYING and are therefore silent.
 * d->olc is NULL on return. */
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  /* Clean up WHAT? */
  if (d->olc == NULL)
    return;

  /* F3-13: cleanup_type decides whether scratch entities' strings are still
   * owned here (CLEANUP_ALL, CLEANUP_CONFIG) or were handed to the live
   * table (CLEANUP_STRUCTS).  An unknown type is a caller bug: report it and
   * free structs only, since freeing strings that may be shared with the
   * world would be a use-after-free, while a missed string is only a leak. */
  if (cleanup_type != CLEANUP_ALL && cleanup_type != CLEANUP_STRUCTS &&
      cleanup_type != CLEANUP_CONFIG) {
    TB_ERROR_MSG("olc", "cleanup-type", "oasis_olc_data",
                 "type %d for " DIAG_CHAR_FMT " state %d olc_num %d; "
                 "freeing structs only", cleanup_type, DIAG_CHAR(d->character),
                 STATE(d), (int)OLC_NUM(d));
    cleanup_type = CLEANUP_STRUCTS;
  }

  /* Before the scratch entities: the script may be attached to one. */
  olc_release_script(d);

  /* F3-07: the cedit scratch configuration is a deep copy (cedit_setup and
   * cedit_save_internally str_udup/strdup every string), so it is always
   * owned here, whatever cleanup_type the caller passed.  It used to be
   * freed only from inside the OLC_ROOM branch below, which cedit never
   * sets, so every cedit session leaked the struct and its strings. */
  if (OLC_CONFIG(d)) {
    free_config(OLC_CONFIG(d));
    OLC_CONFIG(d) = NULL;
  }

  /* Check for a room. free_room doesn't perform sanity checks, we must be
   * careful here. */
  if (OLC_ROOM(d)) {
    if (cleanup_type == CLEANUP_STRUCTS)
      free(OLC_ROOM(d));
    else {
      free_proto_script(OLC_ROOM(d), WLD_TRIGGER);
      free_room(OLC_ROOM(d));
    }
  }

  /* Check for an existing object in the OLC.  The strings aren't part of the
   * prototype any longer.  They get added with strdup(). */
  if (OLC_OBJ(d)) {
    free_object_strings(OLC_OBJ(d));
    free(OLC_OBJ(d));
  }

  /* Check for a mob.  free_mobile() makes sure strings are not in the
   * prototype. */
  if (OLC_MOB(d))
    free_mobile(OLC_MOB(d));

  /* Check for a zone.  cleanup_type is irrelevant here, free() everything.
   * F3-12: the OLC copy owns every sarg1/sarg2 in its command list
   * (zedit_setup strdup's them, zedit_save_internally gives zone_table its
   * own copies), so they are freed here with the array. */
  if (OLC_ZONE(d)) {
    if (OLC_ZONE(d)->builders)
      free(OLC_ZONE(d)->builders);
    if (OLC_ZONE(d)->name)
      free(OLC_ZONE(d)->name);
    if (OLC_ZONE(d)->cmd) {
      int subcmd;
      /* The scratch list owns its 'V' strings until zedit_save_internally
       * hands them to zone_table (and clears them here). */
      for (subcmd = 0; OLC_ZONE(d)->cmd[subcmd].command != 'S'; subcmd++) {
        free(OLC_ZONE(d)->cmd[subcmd].sarg1);
        free(OLC_ZONE(d)->cmd[subcmd].sarg2);
      }
      free(OLC_ZONE(d)->cmd);
    }
    free(OLC_ZONE(d));
  }

  /* Check for a shop.  free_shop doesn't perform sanity checks, we must be
   * careful here. OLC_SHOP(d) is a _copy_ - no pointers to the original. Just
   * go ahead and free it all. */
  if (OLC_SHOP(d))
      free_shop(OLC_SHOP(d));

  /* Check for a quest. */
  if (OLC_QUEST(d)) {
    if (cleanup_type == CLEANUP_STRUCTS)
      free(OLC_QUEST(d));
    else
      free_quest(OLC_QUEST(d));
  }

  /*. Check for aedit stuff -- M. Scott */
  if (OLC_ACTION(d))  {
    if (cleanup_type == CLEANUP_STRUCTS)
      free(OLC_ACTION(d));
    else
      free_action(OLC_ACTION(d));
  }

  /* Used for cleanup of Hedit */
  if (OLC_HELP(d))  {
    if (cleanup_type == CLEANUP_STRUCTS)
      free(OLC_HELP(d));
    else
      free_help(OLC_HELP(d));
  }

   if (OLC_IBT(d)) {
	   free_olc_ibt(OLC_IBT(d));
	   OLC_IBT(d) = NULL;
   }
   
   if (OLC_MSG_LIST(d)) {
     free_message_list(OLC_MSG_LIST(d));
     OLC_MSG_LIST(d) = NULL;  
     OLC_MSG(d) = NULL;
   }

  /* Free storage if allocated (tedit, aedit, and trigedit). This is the command
   * list - it's been copied to disk already, so just free it -Welcor. */
   if (OLC_STORAGE(d)) {
     free(OLC_STORAGE(d));
     OLC_STORAGE(d) = NULL;
   }
   /* Free this one regardless. If we've left olc, we've either made a fresh
    * copy of it in the trig index, or we lost connection. Either way, we need
    * to get rid of this. */
   if (OLC_TRIG(d)) {
     free_trigger(OLC_TRIG(d));
     OLC_TRIG(d) = NULL;
   }

   /* Free this one regardless. If we've left olc, we've either copied the    *
    * preferences to the player, or we lost connection. Either way, we need   *
    * to get rid of this. */
   if(OLC_PREFS(d)) {
     /*. There is nothing else really to free, except this... .*/
     free(OLC_PREFS(d));
     OLC_PREFS(d) = NULL;
   }

  /* Restore descriptor playing status.  One-shot internal users (dig,
   * buildwalk, the copy commands, editor entry error paths) borrow d->olc
   * without ever leaving CON_PLAYING; they never announced "starts using
   * OLC" or set PLR_WRITING, so do not announce a stop or touch the flag. */
  if (d->character && STATE(d) != CON_PLAYING) {
    REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_WRITING);
    act("$n stops using OLC.", TRUE, d->character, NULL, NULL, TO_ROOM);

    if (cleanup_type == CLEANUP_CONFIG)
      mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), 
        TRUE, "OLC: %s stops editing the game configuration", GET_NAME(d->character));
    else if (STATE(d) == CON_TEDIT)
      mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)),
       TRUE, "OLC: %s stops editing text files.", GET_NAME(d->character));
    else if (STATE(d) == CON_HEDIT)
      mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)),
       TRUE, "OLC: %s stops editing help files.", GET_NAME(d->character));
    else if (OLC_ZNUM(d) >= 0 && OLC_ZNUM(d) <= top_of_zone_table)
      mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)),
        TRUE, "OLC: %s stops editing zone %d allowed zone %d",
        GET_NAME(d->character), zone_table[OLC_ZNUM(d)].number, GET_OLC_ZONE(d->character));
    else
      mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)),
        TRUE, "OLC: %s stops editing an object instance or unzoned data (zone index %d)",
        GET_NAME(d->character), OLC_ZNUM(d));

    STATE(d) = CON_PLAYING;
  }

  free(d->olc);
  d->olc = NULL;
}

/* See OLC_ATTACH in oasis.h.  F3-10: do_dig and buildwalk used to free()
 * an existing d->olc (shallowly, leaking the editor's scratch entity) and
 * carry on; do_force can put a builder who is mid-edit into either. */
int olc_attach_at(struct descriptor_data *d, struct char_data *ch,
                  const char *what, const char *source_file, int source_line,
                  const char *source_func)
{
  if (d == NULL) {
    tb_error_log_at("ERROR", "olc", "olc-busy", what, 0,
                    source_file, source_line, source_func,
                    "no descriptor for " DIAG_CHAR_FMT, DIAG_CHAR(ch));
    return FALSE;
  }

  if (STATE(d) != CON_PLAYING || d->olc != NULL) {
    if (ch)
      send_to_char(ch, "You are already editing something.\r\n");
    tb_error_log_at("ERROR", "olc", "olc-busy", what, 0,
                    source_file, source_line, source_func,
                    DIAG_CHAR_FMT " state %d olc_num %d",
                    DIAG_CHAR(ch), STATE(d), d->olc ? (int)OLC_NUM(d) : -1);
    return FALSE;
  }

  CREATE(d->olc, struct oasis_olc_data, 1);
  return TRUE;
}

void split_argument(char *argument, char *tag)
{
  char *tmp = argument, *ttag = tag, *wrt = argument;
  int i;

  for (i = 0; *tmp; tmp++, i++) {
    if (*tmp != ' ' && *tmp != '=')
      *(ttag++) = *tmp;
    else if (*tmp == '=')
      break;
  }

  *ttag = '\0';

  while (*tmp == '=' || *tmp == ' ')
    tmp++;

  while (*tmp)
    *(wrt++) = *(tmp++);

  *wrt = '\0';
}

static void free_config(struct config_data *data)
{
  /* Free strings. */
  free_strings(data, OASIS_CFG);

  /* Free the data structure. */
  free(data);
}

/** Checks to see if a builder can modify the specified zone.
 *
 * @param ch   The immortal requesting access.  A character with no
 *             descriptor (linkless, or an NPC) is always denied, which is
 *             also what keeps buildwalk inert for linkless characters.
 * @param rnum Either a real zone number (0..top_of_zone_table) or one of
 *             the permission sentinels HEDIT_PERMISSION, AEDIT_PERMISSION or
 *             ALL_PERMISSION.  The sentinels share the zone_rnum space: in a
 *             world with more than 666 zones a real rnum can equal one, and
 *             would then be treated as the sentinel.  NOWHERE is denied.
 * @return TRUE if the builder has access, otherwise FALSE. */
int can_edit_zone(struct char_data *ch, zone_rnum rnum)
{
  /* no access if called with bad arguments */
  if (!ch->desc || IS_NPC(ch) || rnum == NOWHERE)
    return FALSE;

  /* If zone is flagged NOBUILD, then No-one can edit it (use zunlock to open it) */
  if (rnum != HEDIT_PERMISSION && rnum != AEDIT_PERMISSION && ZONE_FLAGGED(rnum, ZONE_NOBUILD) )
    return FALSE;

  if (GET_OLC_ZONE(ch) == ALL_PERMISSION)
    return TRUE;

  if (GET_OLC_ZONE(ch) == HEDIT_PERMISSION && rnum == HEDIT_PERMISSION)
    return TRUE;

  if (GET_OLC_ZONE(ch) == AEDIT_PERMISSION && rnum == AEDIT_PERMISSION)
    return TRUE;

  /* always access if ch is high enough level */
  if (GET_LEVEL(ch) >= LVL_GRGOD)
    return (TRUE);

  /* always access if a player helped build the zone in the first place */
  if (rnum != HEDIT_PERMISSION && rnum != AEDIT_PERMISSION)
    if (is_name(GET_NAME(ch), zone_table[rnum].builders))
      return (TRUE);

  /* no access if you haven't been assigned a zone */
  if (GET_OLC_ZONE(ch) == NOWHERE) {
    return FALSE;
  }

  /* no access if you're not at least LVL_BUILDER */
  if (GET_LEVEL(ch) < LVL_BUILDER)
    return FALSE;

  /* always access if you're assigned to this zone */
  if (real_zone(GET_OLC_ZONE(ch)) == rnum)
    return TRUE;

  return (FALSE);
}

void send_cannot_edit(struct char_data *ch, zone_vnum zone)
{
  char buf[MAX_STRING_LENGTH];

  if (GET_OLC_ZONE(ch) != NOWHERE) {
    send_to_char(ch, "You do not have permission to edit zone %d.  Try zone %d.\r\n", zone, GET_OLC_ZONE(ch));
    tb_snprintf(buf, sizeof(buf), "OLC: %s tried to edit zone %d (allowed zone %d).", GET_NAME(ch), zone, GET_OLC_ZONE(ch));
  } else {
    send_to_char(ch, "You do not have permission to edit zone %d.\r\n", zone);
    tb_snprintf(buf, sizeof(buf), "OLC: %s tried to edit zone %d.", GET_NAME(ch), zone);
  }
  mudlog(BRF, LVL_IMPL, TRUE, "%s", buf);
}

