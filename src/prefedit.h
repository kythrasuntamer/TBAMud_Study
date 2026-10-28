/**************************************************************************
*  File: prefedit.c                                        Part of tbaMUD *
*  Usage: Player-level OLC for setting preferences and toggles            *
*                                                                         *
*  Created by Jamdog for tbaMUD 3.59                                      *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/
#ifndef TBAMUD_PREFEDIT_H_INCLUDED
#define TBAMUD_PREFEDIT_H_INCLUDED

/** @file prefedit.h
 * Player preference editor (prefedit), level 0: any player edits their
 * own toggles, and an IMPL may edit another player's.
 *
 * Include after structs.h (PR_ARRAY_MAX, ACMD) and oasis.h (OLC_PREFS).
 *
 * The PREFEDIT_* accessors expand to code that uses a variable named 'd'
 * (the editor's descriptor) in the caller's scope; PREFEDIT_GET_CHAR is a
 * lookup by idnum that returns NULL once the edited player is gone, so
 * test it before dereferencing.  OLC_PREFS holds copies of the player's
 * values; they are written back to the player only on save. */

/* Toggle structure held for OLC purposes */
struct prefs_data
{
  /* The player being edited, by identity.  An IMPL may edit another
   * player, who can quit, drop link and be extracted (freed) while the
   * editor is open, so no char_data pointer is kept across input lines;
   * prefedit_target() re-resolves this on every use. */
  long  idnum;                     /* GET_IDNUM of the player edited */
  int   pref_flags[PR_ARRAY_MAX];  /* Copy of player's pref flags    */
  int   wimp_level;                /* Copy of player's wimp level    */
  int   page_length;               /* Copy of player's pagelength    */
  int   screen_width;              /* Copy of player's screenwidth   */
};

struct char_data *prefedit_target(struct descriptor_data *d);

/* Prefedit utility macros */

/** Get the character being edited, or NULL once that player is gone.
 * prefedit_parse() checks it before dispatching, so the menus below it may
 * dereference the result. */
#define PREFEDIT_GET_CHAR        prefedit_target(d)

/** Get a pointer to the pref toggles of the character being edited   */
#define PREFEDIT_GET_FLAGS       OLC_PREFS(d)->pref_flags

/** Get the wimpy level of the character being edited                 */
#define PREFEDIT_GET_WIMP_LEV    OLC_PREFS(d)->wimp_level

/** Get the page length of the character being edited                 */
#define PREFEDIT_GET_PAGELENGTH  OLC_PREFS(d)->page_length

/** Get the screen width of the character being edited                */
#define PREFEDIT_GET_SCREENWIDTH OLC_PREFS(d)->screen_width

/** Get one of the flag array elements for the character being edited */
#define PREFEDIT_GET_FLAG(i)     OLC_PREFS(d)->pref_flags[(i)]

/** Get the state of a flag (on/off) for the character being edited   */
#define PREFEDIT_FLAGGED(flag)  IS_SET_AR(PREFEDIT_GET_FLAGS, (flag))

/* Prefedit OLC sub-modes */
#define PREFEDIT_MAIN_MENU                0
#define PREFEDIT_PROMPT                   1
#define PREFEDIT_COLOR                    2
#define PREFEDIT_PAGELENGTH               3
#define PREFEDIT_SCREENWIDTH              4
#define PREFEDIT_WIMPY                    5
#define PREFEDIT_CONFIRM_SAVE             6
#define PREFEDIT_SYSLOG                   7
#define PREFEDIT_TOGGLE_MENU              8

/* External Functions in prefedit.c */
void prefedit_Restore_Defaults(struct descriptor_data *d);
void prefedit_parse(struct descriptor_data * d, char *arg);
void prefedit_forget_char(struct char_data *ch);
ACMD(do_oasis_prefedit);

#endif /* TBAMUD_PREFEDIT_H_INCLUDED */

