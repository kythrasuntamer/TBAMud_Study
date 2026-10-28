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

/* New classes reuse validated legacy combat/XP/save curves until their
 * independently balanced tables are implemented. Stored class IDs remain
 * distinct; calculations and the ITEM_ANTI_* / NOTRADE_* rules use the
 * established archetype. An unknown class maps to itself, so each caller
 * reports it against its own table. Inline so limits.c, shop.c and objsave.c
 * (and their tests) need not link class.c. */
static inline int class_progression_archetype(int class_num)
{
  switch (class_num) {
    case CLASS_MONK:
    case CLASS_BERSERKER:
    case CLASS_PALADIN: return CLASS_WARRIOR;
    case CLASS_DRUID: return CLASS_CLERIC;
    case CLASS_SORCERER:
    case CLASS_NECROMANCER: return CLASS_MAGIC_USER;
    case CLASS_RANGER:
    case CLASS_BARD: return CLASS_THIEF;
    default: return class_num;
  }
}

/** 1 if ch is a player whose class uses archetype a's progression tables
 * and ITEM_ANTI_* / NOTRADE_* rules (CLASS_MAGIC_USER, _CLERIC, _THIEF or
 * _WARRIOR). Use instead of IS_MAGIC_USER() etc. where every class of that
 * family should behave alike. */
#define IS_ARCHETYPE(ch, a) \
  (!IS_NPC(ch) && class_progression_archetype(GET_CLASS(ch)) == (a))

/* Global variables */

extern const char *class_abbrevs[];
extern const char *pc_class_types[];
extern const char *class_menu;
extern int prac_params[][NUM_CLASSES];
extern struct guild_info_type guild_info[];

#endif /* TBAMUD_CLASS_H_INCLUDED*/
