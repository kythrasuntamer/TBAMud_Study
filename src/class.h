/**
* @file class.h
* Header file for class specific functions and variables.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               
*
*/
#ifndef TBAMUD_CLASS_H_INCLUDED
#define TBAMUD_CLASS_H_INCLUDED

/* Functions available through class.c */
int backstab_mult(int level);
void do_start(struct char_data *ch);
bitvector_t find_class_bitvector(const char *arg);
int invalid_class(struct char_data *ch, struct obj_data *obj);
int level_exp(int chclass, int level);
int parse_class(char arg);
void roll_real_abils(struct char_data *ch);
byte saving_throws(int class_num, int type, int level);
int thaco(int class_num, int level);
/* The lookups above for a character: a table miss also names ch. */
int char_thaco(const struct char_data *ch);
byte char_saving_throw(const struct char_data *ch, int class_num, int type);
int char_level_exp(const struct char_data *ch, int level);
const char *title_female(int chclass, int level);
const char *title_male(int chclass, int level);

/* Global variables */

extern const char *class_abbrevs[];
extern const char *pc_class_types[];
extern const char *class_menu;
extern int prac_params[][NUM_CLASSES];
extern struct guild_info_type guild_info[];

#endif /* TBAMUD_CLASS_H_INCLUDED*/
