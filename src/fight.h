/**
* @file fight.h
* Fighting and violence functions and variables.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.
*
*/
#ifndef TBAMUD_FIGHT_H_INCLUDED
#define TBAMUD_FIGHT_H_INCLUDED

/* Structures and defines */
/* Attacktypes with grammar */
struct attack_hit_type {
   const char *singular;
   const char *plural;
};

/** Set ch's position to pos. A character that leaves the seated positions
 * (pos above POS_SITTING: fighting or standing) is first unlinked from any
 * furniture, which keeps SITTING(ch) and the furniture's sitting_here list
 * and seat count (value 1) consistent. Code that stands a character up
 * outside do_stand() must use this instead of assigning GET_POS directly:
 * a stale SITTING(ch) lets do_sit() link ch into a second furniture list,
 * and the first one keeps a pointer to ch after ch is freed.
 * Evaluates each argument once. */
#define POS_CHANGE_UNSEAT(ch, pos) do { \
  struct char_data *pcu_ch_ = (ch); \
  int pcu_pos_ = (pos); \
  if (pcu_pos_ > POS_SITTING && SITTING(pcu_ch_)) \
    char_from_furniture(pcu_ch_); \
  GET_POS(pcu_ch_) = pcu_pos_; \
} while (0)

/** TRUE if ch has been extracted (killed, purged) and awaits
 * extract_pending_chars() at the end of the pulse. The pointer is still
 * allocated, but ch must not act or be acted on. MOB_FLAGS and PLR_FLAGS
 * share storage, so the flag must be chosen by IS_NPC. Evaluates ch more
 * than once: pass a plain variable. */
#define CHAR_PENDING_EXTRACTION(ch) \
  (IS_NPC(ch) ? MOB_FLAGGED((ch), MOB_NOTDEADYET) : PLR_FLAGGED((ch), PLR_NOTDEADYET))

/** TRUE if t (an attack type relative to TYPE_HIT: weapon value 3 or a
 * mob's attack_type) indexes attack_hit_text[], which has NUM_ATTACK_TYPES
 * entries. */
#define VALID_ATTACK_TYPE(t) ((t) >= 0 && (t) < NUM_ATTACK_TYPES)

/** Largest damage dice hit() rolls: weapon values 1/2 and mob
 * damnodice/damsizedice beyond these are clamped (damage() caps a single
 * blow at 100, so nothing larger is meaningful). */
#define FIGHT_MAX_DICE_NUM   100
#define FIGHT_MAX_DICE_SIZE  1000
/** dice(num, size) with both clamped into [0, FIGHT_MAX_DICE_*], so a
 * builder- or script-supplied count can neither stall the game loop nor
 * overflow dice()'s int sum. Evaluates num and size twice: no side
 * effects. Silent by itself; hit() reports before using it. */
#define DICE_BOUNDED(num, size) \
  dice(MIN(MAX((num), 0), FIGHT_MAX_DICE_NUM), \
       MIN(MAX((size), 0), FIGHT_MAX_DICE_SIZE))

/* Functions available in fight.c */
void appear(struct char_data *ch);
void check_killer(struct char_data *ch, struct char_data *vict);
int compute_armor_class(struct char_data *ch);
int damage(struct char_data *ch, struct char_data *victim, int dam, int attacktype);
void death_cry(struct char_data *ch);
void die(struct char_data * ch, struct char_data * killer);
void hit(struct char_data *ch, struct char_data *victim, int type);
void perform_violence(void);
void raw_kill(struct char_data * ch, struct char_data * killer);
void  set_fighting(struct char_data *ch, struct char_data *victim);
int skill_message(int dam, struct char_data *ch, struct char_data *vict,
          int attacktype);
void  stop_fighting(struct char_data *ch);
bool pk_allowed(struct char_data *ch, struct char_data *victim);


/* Global variables */
extern struct attack_hit_type attack_hit_text[];
extern struct char_data *combat_list;

#endif /* TBAMUD_FIGHT_H_INCLUDED*/
