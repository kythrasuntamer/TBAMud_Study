/**
* @file dg_olc.h
* This source file is used in extending Oasis OLC for trigedit.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
* 
* This source code, which was not part of the CircleMUD legacy code,
* was created by the following people:                                      
* $Author: Mark A. Heilpern/egreen/Welcor $                              
* $Date: 2004/10/11 12:07:00$                                            
* $Revision: 1.0.14 $                                                    
*/
#ifndef TBAMUD_DG_OLC_H_INCLUDED
#define TBAMUD_DG_OLC_H_INCLUDED

#include "dg_scripts.h"

/** Number of names in trig_types[] (the mobile table), and so the most type
 * bits any trigger can use.  otrig_types[] and wtrig_types[] are shorter
 * (20 names); dg_olc.c counts each table up to its "\n" sentinel instead of
 * assuming this length for them. */
#define NUM_TRIG_TYPE_FLAGS		21

/* Submodes of TRIGEDIT connectedness. */
#define TRIGEDIT_MAIN_MENU              0
#define TRIGEDIT_TRIGTYPE               1
#define TRIGEDIT_CONFIRM_SAVESTRING	2
#define TRIGEDIT_NAME			3
#define TRIGEDIT_INTENDED		4
#define TRIGEDIT_TYPES			5
#define TRIGEDIT_COMMANDS		6
#define TRIGEDIT_NARG			7
#define TRIGEDIT_ARGUMENT		8
#define TRIGEDIT_COPY                   9

/** OLC_MODE() value meaning "the trigger-attachment submenu has the input"
 * in medit/oedit/redit.  It is a sentinel, not a real editor mode, so it must
 * differ from every MEDIT_ / OEDIT_ / REDIT_ mode those editors define (all
 * are small integers).  Nothing checks this at compile time; keep new modes
 * far below it.  (The old comment tied it to room numbers, which OLC_MODE
 * never holds.)  The submenu's own state is OLC_SCRIPT_EDIT_MODE(). */
#define OLC_SCRIPT_EDIT		    82766
#define SCRIPT_MAIN_MENU		0
#define SCRIPT_NEW_TRIGGER		1
#define SCRIPT_DEL_TRIGGER		2

#define OLC_SCRIPT_EDIT_MODE(d)	(OLC(d)->script_mode)	/* parse input mode */
#define OLC_SCRIPT(d)           (OLC(d)->script)	/* script editing   */
#define OLC_ITEM_TYPE(d)	(OLC(d)->item_type)	/* mob/obj/room     */

/* prototype exported functions from dg_olc.c */
void script_save_to_disk(FILE *fp, void *item, int type);
void dg_olc_script_copy(struct descriptor_data *d);
void dg_script_menu(struct descriptor_data *d);
int dg_script_edit_parse(struct descriptor_data *d, char *arg);


#endif /* TBAMUD_DG_OLC_H_INCLUDED */
