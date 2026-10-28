/**
* @file hedit.h
* Oasis OLC Help Editor.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* Author: Steve Wolfe, Scott Meisenholder, Rhade
* All rights reserved.  See license.doc for complete information.
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.                             
*
*/
#ifndef TBAMUD_HEDIT_H_INCLUDED
#define TBAMUD_HEDIT_H_INCLUDED

#include "utils.h" /* for ACMD definition */

/** TRUE when r indexes a live help_table row.  top_of_helpt is a COUNT
 * (rows 0..top_of_helpt-1), unlike top_of_socialt or top_of_world, which are
 * inclusive last indices.  In hedit, OLC_ZNUM holds either such a row or
 * NOWHERE ("add a new entry"); nothing else may be stored there.
 * r is evaluated more than once: pass a plain variable. */
#define VALID_HELP_RNUM(r) \
  ((r) != NOWHERE && (long)(r) >= 0 && (long)(r) < (long)top_of_helpt)

/* Functions made externally available */
/* Utility functions */
/*
 * All the following functions are declared in oasis.h
  void hedit_parse(struct descriptor_data *, char *);
  void hedit_string_cleanup(struct descriptor_data *, int);
  ACMD(do_oasis_hedit);
*/
/* Action fuctions */
ACMD(do_helpcheck);
ACMD(do_hindex);


#endif /* TBAMUD_HEDIT_H_INCLUDED*/
