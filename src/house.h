/**
* @file house.h
* Player house structures, prototypes and defines.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               
*/
#ifndef TBAMUD_HOUSE_H_INCLUDED
#define TBAMUD_HOUSE_H_INCLUDED

#define MAX_HOUSES	100
#define MAX_GUESTS	10

#define HOUSE_PRIVATE	0

/** One house, as stored in lib/etc/hcontrol.
 * The file is a raw fwrite() of an array of these, so its layout is the
 * in-memory layout: it depends on the compiler ABI (sizes of time_t and
 * long, room_vnum/IDXTYPE, alignment padding) and on field order. Moving
 * the data between platforms or changing this struct makes existing files
 * unreadable. Any record that is written must be zeroed first (memset), so
 * that the spare fields and the padding are defined. On read every field
 * is untrusted: num_of_guests in particular must be checked with
 * VALID_HOUSE_GUESTS() before guests[] is indexed. */
struct house_control_rec {
   room_vnum vnum;		/* vnum of this house		*/
   room_vnum atrium;		/* vnum of atrium		*/
   sh_int exit_num;		/* direction of house's exit	*/
   time_t built_on;		/* date this house was built	*/
   int mode;			/* mode of ownership		*/
   long owner;			/* idnum of house's owner	*/
   int num_of_guests;		/* how many guests for house	*/
   long guests[MAX_GUESTS];	/* idnums of house's guests	*/
   time_t last_payment;		/* date of last house payment   */
   long spare0;
   long spare1;
   long spare2;
   long spare3;
   long spare4;
   long spare5;
   long spare6;
   long spare7;
};

/** TRUE when a house record's guest count fits guests[]. num_of_guests is
 * read from the binary hcontrol file, so check it before any guests[] loop.
 * Evaluates h twice: pass a plain pointer. */
#define VALID_HOUSE_GUESTS(h) \
  ((h)->num_of_guests >= 0 && (h)->num_of_guests <= MAX_GUESTS)

#define TOROOM(room, dir) (world[room].dir_option[dir] ? \
			    world[room].dir_option[dir]->to_room : NOWHERE)

/* Functions in house.c made externally available */
/* Utility Functions */
void	House_boot(void);
void	House_save_all(void);
int	House_can_enter(struct char_data *ch, room_vnum house);
void	House_crashsave(room_vnum vnum);
void	House_list_guests(struct char_data *ch, int i, int quiet);
int House_save(struct obj_data *obj, FILE *fp);
void hcontrol_list_houses(struct char_data *ch, char *arg);
/* In game Commands */
ACMD(do_hcontrol);
ACMD(do_house);


#endif /* TBAMUD_HOUSE_H_INCLUDED */
