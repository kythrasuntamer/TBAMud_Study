/**
* @file spec_procs.h
* Header file for special procedure modules. This file groups a lot of the
* legacy special procedures found in spec_procs.c and castle.c.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.
* 
*/
#ifndef TBAMUD_SPEC_PROCS_H_INCLUDED
#define TBAMUD_SPEC_PROCS_H_INCLUDED

/*****************************************************************************
 * Begin Functions and defines for castle.c 
 ****************************************************************************/
void assign_kings_castle(void);

/*****************************************************************************
 * Begin Functions and defines for spec_assign.c 
 ****************************************************************************/
void assign_mobiles(void);
void assign_objects(void);
void assign_rooms(void);

#include "structs.h"
/** Display name of a spec proc for 'stat'. Never NULL: a proc missing from
 * spec_func_list is named "Unknown" (and reported once), so the result can
 * be passed to %s directly. */
const char *get_spec_func_name(SPECIAL(*func));

/* King's Castle procs (castle.c), named by get_spec_func_name(). */
SPECIAL(CastleGuard);
SPECIAL(James);
SPECIAL(cleaning);
SPECIAL(DicknDavid);
SPECIAL(tim);
SPECIAL(tom);
SPECIAL(king_welmar);
SPECIAL(training_master);
SPECIAL(peter);
SPECIAL(jerry);

/*****************************************************************************
 * Begin Functions and defines for spec_procs.c 
 ****************************************************************************/
/* Utility functions */
void sort_spells(void);
void list_skills(struct char_data *ch);

/* Special functions */
SPECIAL(guild);
SPECIAL(dump);
SPECIAL(mayor);
SPECIAL(snake);
SPECIAL(thief);
SPECIAL(magic_user);
SPECIAL(guild_guard);
SPECIAL(puff);
SPECIAL(fido);
SPECIAL(janitor);
SPECIAL(cityguard);
SPECIAL(pet_shops);
SPECIAL(bank);

#endif /* TBAMUD_SPEC_PROCS_H_INCLUDED */
