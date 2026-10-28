/**************************************************************************
*  File: fight.c                                           Part of tbaMUD *
*  Usage: Combat system.                                                  *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "interpreter.h"
#include "db.h"
#include "spells.h"
#include "screen.h"
#include "constants.h"
#include "dg_scripts.h"
#include "act.h"
#include "class.h"
#include "fight.h"
#include "shop.h"
#include "quest.h"


/* locally defined global variables, used externally */
/* head of l-list of fighting chars */
struct char_data *combat_list = NULL;
/* Weapon attack texts */
struct attack_hit_type attack_hit_text[] =
{
  {"hit", "hits"},    /* 0 */
  {"sting", "stings"},
  {"whip", "whips"},
  {"slash", "slashes"},
  {"bite", "bites"},
  {"bludgeon", "bludgeons"},  /* 5 */
  {"crush", "crushes"},
  {"pound", "pounds"},
  {"claw", "claws"},
  {"maul", "mauls"},
  {"thrash", "thrashes"}, /* 10 */
  {"pierce", "pierces"},
  {"blast", "blasts"},
  {"punch", "punches"},
  {"stab", "stabs"}
};

/* local (file scope only) variables */
static struct char_data *next_combat_list = NULL;

/* local file scope utility functions */
static void perform_group_gain(struct char_data *ch, int base, struct char_data *victim);
static void dam_message(int dam, struct char_data *ch, struct char_data *victim, int w_type);
static void make_corpse(struct char_data *ch);
static struct obj_data *autoloot_corpse(struct char_data *ch, struct char_data *victim);

/* The corpse make_corpse() created most recently, and its script UID.
 * damage()'s autogold/autoloot/autosac act on this object. A lookup by the
 * name "corpse" would find an older corpse first: obj_to_room() appends,
 * and 'get' searches the inventory before the room. */
static struct obj_data *last_corpse;
static long last_corpse_uid;
static void change_alignment(struct char_data *ch, struct char_data *victim);
static void group_gain(struct char_data *ch, struct char_data *victim);
static void solo_gain(struct char_data *ch, struct char_data *victim);
/** @todo refactor this function name */
static char *replace_string(const char *str, const char *weapon_singular, const char *weapon_plural);
static int compute_thaco(struct char_data *ch, struct char_data *vict);


#define IS_WEAPON(type) (((type) >= TYPE_HIT) && ((type) < TYPE_SUFFERING))
/* The Fight related routines */
void appear(struct char_data *ch)
{
  if (affected_by_spell(ch, SPELL_INVISIBLE))
    affect_from_char(ch, SPELL_INVISIBLE);

  REMOVE_BIT_AR(AFF_FLAGS(ch), AFF_INVISIBLE);
  REMOVE_BIT_AR(AFF_FLAGS(ch), AFF_HIDE);

  if (GET_LEVEL(ch) < LVL_IMMORT)
    act("$n slowly fades into existence.", FALSE, ch, 0, 0, TO_ROOM);
  else
    act("You feel a strange presence as $n appears, seemingly from nowhere.",
	FALSE, ch, 0, 0, TO_ROOM);
}

int compute_armor_class(struct char_data *ch)
{
  int armorclass = GET_AC(ch);

  if (AWAKE(ch))
    armorclass += dex_app[GET_DEX(ch)].defensive * 10;

  return (MAX(-100, armorclass));      /* -100 is lowest */
}

void update_pos(struct char_data *victim)
{
  if ((GET_HIT(victim) > 0) && (GET_POS(victim) > POS_STUNNED))
    return;
  else if (GET_HIT(victim) > 0)
    GET_POS(victim) = POS_STANDING;
  else if (GET_HIT(victim) <= -11)
    GET_POS(victim) = POS_DEAD;
  else if (GET_HIT(victim) <= -6)
    GET_POS(victim) = POS_MORTALLYW;
  else if (GET_HIT(victim) <= -3)
    GET_POS(victim) = POS_INCAP;
  else
    GET_POS(victim) = POS_STUNNED;
}

void check_killer(struct char_data *ch, struct char_data *vict)
{
  if (PLR_FLAGGED(vict, PLR_KILLER) || PLR_FLAGGED(vict, PLR_THIEF)) {
    return;
  }

  if (PLR_FLAGGED(ch, PLR_KILLER) || IS_NPC(ch) || IS_NPC(vict) || ch == vict){
      return;
  }

  SET_BIT_AR(PLR_FLAGS(ch), PLR_KILLER);
  send_to_char(ch, "If you want to be a PLAYER KILLER, so be it...\r\n");
  mudlog(BRF, MAX(LVL_IMMORT, MAX(GET_INVIS_LEV(ch), GET_INVIS_LEV(vict))), 
    TRUE, "PC Killer bit set on %s for initiating attack on %s at %s.",
    GET_NAME(ch), GET_NAME(vict), world[IN_ROOM(vict)].name);
}

bool pk_allowed(struct char_data *ch, struct char_data *victim)
{
  /* NPCs are never restricted */
  if (IS_NPC(ch) || IS_NPC(victim))
    return true;

  if (CONFIG_PK_SETTING == CONFIG_PK_OFF)
    return false;

  if (CONFIG_PK_SETTING == CONFIG_PK_LIMITED)
    check_killer(ch, victim);

  return true;
}


/* start one char fighting another (yes, it is horrible, I know... )  */
void set_fighting(struct char_data *ch, struct char_data *vict)
{
  if (ch == vict)
    return;

  if (FIGHTING(ch)) {
    core_dump();
    return;
  }


  if (!pk_allowed(ch, vict)) {
    send_to_char(ch, "Player killing is not permitted.\r\n");
    return;
  }

  ch->next_fighting = combat_list;
  combat_list = ch;

  if (AFF_FLAGGED(ch, AFF_SLEEP))
    affect_from_char(ch, SPELL_SLEEP);

  FIGHTING(ch) = vict;
  /* A seated character that starts fighting leaves its furniture. */
  POS_CHANGE_UNSEAT(ch, POS_FIGHTING);
}

/* remove a char from the list of fighting chars */
void stop_fighting(struct char_data *ch)
{
  struct char_data *temp;

  if (ch == next_combat_list)
    next_combat_list = ch->next_fighting;

  REMOVE_FROM_LIST(ch, combat_list, next_fighting);
  ch->next_fighting = NULL;
  FIGHTING(ch) = NULL;
  POS_CHANGE_UNSEAT(ch, POS_STANDING);
  update_pos(ch);
}

static void make_corpse(struct char_data *ch)
{
  char buf2[MAX_NAME_LENGTH + 64];
  struct obj_data *corpse, *o;
  struct obj_data *money;
  int i, x, y;

  corpse = create_obj();

  corpse->item_number = NOTHING;
  IN_ROOM(corpse) = NOWHERE;
  corpse->name = strdup("corpse");

  tb_snprintf(buf2, sizeof(buf2), "The corpse of %s is lying here.", GET_NAME(ch));
  corpse->description = strdup(buf2);

  tb_snprintf(buf2, sizeof(buf2), "the corpse of %s", GET_NAME(ch));
  corpse->short_description = strdup(buf2);

  GET_OBJ_TYPE(corpse) = ITEM_CONTAINER;
  for(x = y = 0; x < EF_ARRAY_MAX || y < TW_ARRAY_MAX; x++, y++) {
    if (x < EF_ARRAY_MAX)
      GET_OBJ_EXTRA_AR(corpse, x) = 0;
    if (y < TW_ARRAY_MAX)
      corpse->obj_flags.wear_flags[y] = 0;
  }
  SET_BIT_AR(GET_OBJ_WEAR(corpse), ITEM_WEAR_TAKE);
  SET_BIT_AR(GET_OBJ_EXTRA(corpse), ITEM_NODONATE);
  GET_OBJ_VAL(corpse, 0) = 0;	/* You can't store stuff in a corpse */
  GET_OBJ_VAL(corpse, 3) = 1;	/* corpse identifier */
  GET_OBJ_WEIGHT(corpse) = GET_WEIGHT(ch) + IS_CARRYING_W(ch);
  GET_OBJ_RENT(corpse) = 100000;
  if (IS_NPC(ch))
    GET_OBJ_TIMER(corpse) = CONFIG_MAX_NPC_CORPSE_TIME;
  else
    GET_OBJ_TIMER(corpse) = CONFIG_MAX_PC_CORPSE_TIME;

  /* Transfer the inventory to the corpse in one step, before any trigger
   * runs: ch and the corpse must never share a half-moved list. A remove
   * trigger below that gives ch a new object ('oload obj N %actor%') then
   * starts a fresh ch->carrying list, swept into the corpse afterwards. */
  corpse->contains = ch->carrying;
  ch->carrying = NULL;
  IS_CARRYING_N(ch) = 0;
  IS_CARRYING_W(ch) = 0;
  for (o = corpse->contains; o != NULL; o = o->next_content)
    o->in_obj = corpse;
  object_list_new_owner(corpse, NULL);

  /* Transfer the equipment. remove_otrigger() runs a script that may
   * purge, swap or remove the item, so the slot is re-read after it. */
  for (i = 0; i < NUM_WEARS; i++)
    if ((o = GET_EQ(ch, i)) != NULL) {
      long uid = obj_script_id(o);

      remove_otrigger(o, ch);
      if (GET_EQ(ch, i) != o || !obj_uid_matches(uid, o)) {
        TB_WARN_MSG("fight", "corpse-eq-changed", "obj",
                    "slot %d (obj uid %ld) changed by remove trigger on "
                    DIAG_CHAR_FMT " during death", i, uid, DIAG_CHAR(ch));
        if (!GET_EQ(ch, i))
          continue;
      }
      obj_to_obj(unequip_char(ch, i), corpse);
    }

  /* Objects a remove trigger handed to ch while it died. */
  while ((o = ch->carrying) != NULL) {
    TB_WARN_MSG("fight", "corpse-inventory-added", "obj",
                "obj vnum %d uid %ld given to " DIAG_CHAR_FMT
                " by a trigger during death; moved to the corpse",
                (int) GET_OBJ_VNUM(o), obj_script_id(o), DIAG_CHAR(ch));
    obj_from_char(o);
    obj_to_obj(o, corpse);
  }

  /* transfer gold */
  if (GET_GOLD(ch) > 0) {
    /* following 'if' clause added to fix gold duplication loophole. The above
     * line apparently refers to the old "partially log in, kill the game
     * character, then finish login sequence" duping bug. The duplication has
     * been fixed (knock on wood) but the test below shall live on, for a
     * while. -gg 3/3/2002 */
    if (IS_NPC(ch) || ch->desc) {
      money = create_money(GET_GOLD(ch));
      obj_to_obj(money, corpse);
    }
    GET_GOLD(ch) = 0;
  }

  obj_to_room(corpse, IN_ROOM(ch));
  last_corpse = corpse;
  last_corpse_uid = obj_script_id(corpse);
}

/* The corpse damage() just produced, if it is still the same object and in
 * ch's room; otherwise NULL. A corpse that vanished (a get trigger during
 * autoloot can purge it) is reported as "autoloot-corpse-missing". A
 * corpse ch cannot see is skipped silently, as 'get' would. */
static struct obj_data *autoloot_corpse(struct char_data *ch,
                                        struct char_data *victim)
{
  if (!last_corpse || !obj_uid_matches(last_corpse_uid, last_corpse) ||
      IN_ROOM(last_corpse) != IN_ROOM(ch)) {
    TB_WARN_MSG("fight", "autoloot-corpse-missing", "obj",
                "corpse uid %ld of " DIAG_CHAR_FMT " gone before auto "
                "loot/sac by " DIAG_CHAR_FMT, last_corpse_uid,
                DIAG_CHAR(victim), DIAG_CHAR(ch));
    last_corpse = NULL;
    return NULL;
  }
  return CAN_SEE_OBJ(ch, last_corpse) ? last_corpse : NULL;
}

/* When ch kills victim */
static void change_alignment(struct char_data *ch, struct char_data *victim)
{
  /* new alignment change algorithm: if you kill a monster with alignment A,
   * you move 1/16th of the way to having alignment -A.  Simple and fast. */
  GET_ALIGNMENT(ch) += (-GET_ALIGNMENT(victim) - GET_ALIGNMENT(ch)) / 16;
}

void death_cry(struct char_data *ch)
{
  int door;

  act("Your blood freezes as you hear $n's death cry.", FALSE, ch, 0, 0, TO_ROOM);

  for (door = 0; door < DIR_COUNT; door++)
    if (CAN_GO(ch, door))
      send_to_room(world[IN_ROOM(ch)].dir_option[door]->to_room, "Your blood freezes as you hear someone's death cry.\r\n");
}

void raw_kill(struct char_data * ch, struct char_data * killer)
{
struct char_data *i;

  if (FIGHTING(ch))
    stop_fighting(ch);

  while (ch->affected)
    affect_remove(ch, ch->affected);

  /* To make ordinary commands work in scripts.  welcor*/
  GET_POS(ch) = POS_STANDING;

  if (killer) {
    if (death_mtrigger(ch, killer))
      death_cry(ch);
  } else
    death_cry(ch);

  if (killer) {
    if (killer->group) {
      while ((i = (struct char_data *) simple_list(killer->group->members)) != NULL)
        if(IN_ROOM(i) == IN_ROOM(ch)  || (world[IN_ROOM(i)].zone == world[IN_ROOM(ch)].zone))
          autoquest_trigger_check(i, ch, NULL, AQ_MOB_KILL);      
    } else
        autoquest_trigger_check(killer, ch, NULL, AQ_MOB_KILL);
  }

  /* Alert Group if Applicable */
  if (GROUP(ch))
    send_to_group(ch, GROUP(ch), "%s has died.\r\n", GET_NAME(ch));

  update_pos(ch);

  make_corpse(ch);
  extract_char(ch);

  if (killer) {
    autoquest_trigger_check(killer, NULL, NULL, AQ_MOB_SAVE);
    autoquest_trigger_check(killer, NULL, NULL, AQ_ROOM_CLEAR);
  }
}

void die(struct char_data * ch, struct char_data * killer)
{
  gain_exp(ch, -(GET_EXP(ch) / 2));
  if (!IS_NPC(ch)) {
    REMOVE_BIT_AR(PLR_FLAGS(ch), PLR_KILLER);
    REMOVE_BIT_AR(PLR_FLAGS(ch), PLR_THIEF);
  }
  raw_kill(ch, killer);
}

static void perform_group_gain(struct char_data *ch, int base,
			     struct char_data *victim)
{
  int share, hap_share;

  share = MIN(CONFIG_MAX_EXP_GAIN, MAX(1, base));

  if ((IS_HAPPYHOUR) && (IS_HAPPYEXP))
  {
    /* This only reports the correct amount - the calc is done in gain_exp */
    hap_share = share + (int)((float)share * ((float)HAPPY_EXP / (float)(100)));
    share = MIN(CONFIG_MAX_EXP_GAIN, MAX(1, hap_share));
  }
  if (share > 1)
    send_to_char(ch, "You receive your share of experience -- %d points.\r\n", share);
  else
    send_to_char(ch, "You receive your share of experience -- one measly little point!\r\n");

  gain_exp(ch, share);
  change_alignment(ch, victim);
}

static void group_gain(struct char_data *ch, struct char_data *victim)
{
  int tot_members = 0, base, tot_gain;
  struct char_data *k;
  
  while ((k = (struct char_data *) simple_list(GROUP(ch)->members)) != NULL)
    if (IN_ROOM(ch) == IN_ROOM(k))
      tot_members++;

  /* round up to the nearest tot_members */
  tot_gain = (GET_EXP(victim) / 3) + tot_members - 1;

  /* prevent illegal xp creation when killing players */
  if (!IS_NPC(victim))
    tot_gain = MIN(CONFIG_MAX_EXP_LOSS * 2 / 3, tot_gain);

  if (tot_members >= 1)
    base = MAX(1, tot_gain / tot_members);
  else
    base = 0;

  while ((k = (struct char_data *) simple_list(GROUP(ch)->members)) != NULL)
    if (IN_ROOM(k) == IN_ROOM(ch))
      perform_group_gain(k, base, victim);
}

static void solo_gain(struct char_data *ch, struct char_data *victim)
{
  int exp, happy_exp;

  exp = MIN(CONFIG_MAX_EXP_GAIN, GET_EXP(victim) / 3);

  /* Calculate level-difference bonus */
  if (IS_NPC(ch))
    exp += MAX(0, (exp * MIN(4, (GET_LEVEL(victim) - GET_LEVEL(ch)))) / 8);
  else
    exp += MAX(0, (exp * MIN(8, (GET_LEVEL(victim) - GET_LEVEL(ch)))) / 8);

  exp = MAX(exp, 1);

  if (IS_HAPPYHOUR && IS_HAPPYEXP) {
    happy_exp = exp + (int)((float)exp * ((float)HAPPY_EXP / (float)(100)));
    exp = MAX(happy_exp, 1);
  }

  if (exp > 1)
    send_to_char(ch, "You receive %d experience points.\r\n", exp);
  else
    send_to_char(ch, "You receive one lousy experience point.\r\n");

  gain_exp(ch, exp);
  change_alignment(ch, victim);
}

/* Expands a dam_weapons template: "#w" becomes weapon_singular and "#W"
 * weapon_plural; any other character, including a lone '#', is copied.
 * Returns a static buffer that the next call overwrites. The output is
 * bounded by the buffer; a longer expansion is truncated and reported. */
static char *replace_string(const char *str, const char *weapon_singular, const char *weapon_plural)
{
  static char buf[256];
  const char *tmpl = str;
  size_t len = 0;

  for (; *str; str++) {
    char single[2] = { *str, '\0' };
    const char *src = single;

    if (*str == '#' && (str[1] == 'W' || str[1] == 'w'))
      src = (*++str == 'W') ? weapon_plural : weapon_singular;

    for (; *src; src++) {
      if (len + 1 >= sizeof(buf)) {
        buf[len] = '\0';
        TB_ERROR_MSG("fight", "damage-message-truncated", "buf",
                     "template of %zu bytes with weapon words of %zu/%zu "
                     "bytes exceeds %zu bytes; truncated", strlen(tmpl),
                     strlen(weapon_singular), strlen(weapon_plural), sizeof(buf));
        return (buf);
      }
      buf[len++] = *src;
    }
  }
  buf[len] = '\0';

  return (buf);
}

/* message for doing damage with a weapon */
static void dam_message(int dam, struct char_data *ch, struct char_data *victim,
		      int w_type)
{
  char *buf;
  int msgnum;

  static struct dam_weapon_type {
    const char *to_room;
    const char *to_char;
    const char *to_victim;
  } dam_weapons[] = {

    /* use #w for singular (i.e. "slash") and #W for plural (i.e. "slashes") */

    {
      "$n tries to #w $N, but misses.",	/* 0: 0     */
      "You try to #w $N, but miss.",
      "$n tries to #w you, but misses."
    },

    {
      "$n tickles $N as $e #W $M.",	/* 1: 1..2  */
      "You tickle $N as you #w $M.",
      "$n tickles you as $e #W you."
    },

    {
      "$n barely #W $N.",		/* 2: 3..4  */
      "You barely #w $N.",
      "$n barely #W you."
    },

    {
      "$n #W $N.",			/* 3: 5..6  */
      "You #w $N.",
      "$n #W you."
    },

    {
      "$n #W $N hard.",			/* 4: 7..10  */
      "You #w $N hard.",
      "$n #W you hard."
    },

    {
      "$n #W $N very hard.",		/* 5: 11..14  */
      "You #w $N very hard.",
      "$n #W you very hard."
    },

    {
      "$n #W $N extremely hard.",	/* 6: 15..19  */
      "You #w $N extremely hard.",
      "$n #W you extremely hard."
    },

    {
      "$n massacres $N to small fragments with $s #w.",	/* 7: 19..23 */
      "You massacre $N to small fragments with your #w.",
      "$n massacres you to small fragments with $s #w."
    },

    {
      "$n OBLITERATES $N with $s deadly #w!!",	/* 8: > 23   */
      "You OBLITERATE $N with your deadly #w!!",
      "$n OBLITERATES you with $s deadly #w!!"
    }
  };

  w_type -= TYPE_HIT;		/* Change to base of table with text */

  /* IS_WEAPON() admits [TYPE_HIT, TYPE_SUFFERING), wider than the table;
   * hit() validates weapon and mob attack types, so this is a backstop
   * for any other caller of damage(). */
  if (!VALID_ATTACK_TYPE(w_type)) {
    TB_ERROR_MSG("fight", "attack-type-range", "attack_hit_text",
                 "attack type %d not in [0,%d) for " DIAG_CHAR_FMT
                 "; using hit", w_type, NUM_ATTACK_TYPES, DIAG_CHAR(ch));
    w_type = 0;
  }

  if (dam == 0)		msgnum = 0;
  else if (dam <= 2)    msgnum = 1;
  else if (dam <= 4)    msgnum = 2;
  else if (dam <= 6)    msgnum = 3;
  else if (dam <= 10)   msgnum = 4;
  else if (dam <= 14)   msgnum = 5;
  else if (dam <= 19)   msgnum = 6;
  else if (dam <= 23)   msgnum = 7;
  else			msgnum = 8;

  /* damage message to onlookers */
  buf = replace_string(dam_weapons[msgnum].to_room,
	  attack_hit_text[w_type].singular, attack_hit_text[w_type].plural);
  act(buf, FALSE, ch, NULL, victim, TO_NOTVICT);

  /* damage message to damager */
  if (GET_LEVEL(ch) >= LVL_IMMORT)
	send_to_char(ch, "(%d) ", dam);
  buf = replace_string(dam_weapons[msgnum].to_char,
	  attack_hit_text[w_type].singular, attack_hit_text[w_type].plural);
  act(buf, FALSE, ch, NULL, victim, TO_CHAR);
  send_to_char(ch, CCNRM(ch, C_CMP));

  /* damage message to damagee */
  if (GET_LEVEL(victim) >= LVL_IMMORT)
    send_to_char(victim, "\tR(%d)", dam);
  buf = replace_string(dam_weapons[msgnum].to_victim,
	  attack_hit_text[w_type].singular, attack_hit_text[w_type].plural);
  act(buf, FALSE, ch, NULL, victim, TO_VICT | TO_SLEEP);
  send_to_char(victim, CCNRM(victim, C_CMP));
}

/*  message for doing damage with a spell or skill. Also used for weapon
 *  damage on miss and death blows. */
int skill_message(int dam, struct char_data *ch, struct char_data *vict,
		      int attacktype)
{
  int i, j, nr;
  struct message_type *msg;

  struct obj_data *weap = GET_EQ(ch, WEAR_WIELD);

  /* @todo restructure the messages library to a pointer based system as
   * opposed to the current cyclic location system. */
  for (i = 0; i < MAX_MESSAGES; i++) {
    if (fight_messages[i].a_type == attacktype) {
      if (fight_messages[i].number_of_attacks <= 0 || !fight_messages[i].msg) {
        log("SYSERR: Missing fight message data for attack type %d.", attacktype);
        return 0;
      }

      nr = dice(1, fight_messages[i].number_of_attacks);
      for (j = 1, msg = fight_messages[i].msg; (j < nr) && msg; j++)
        msg = msg->next;

      if (!msg) {
        log("SYSERR: Fight message chain too short for attack type %d.", attacktype);
        return 0;
      }

      if (!IS_NPC(vict) && (GET_LEVEL(vict) >= LVL_IMPL)) {
        act(msg->god_msg.attacker_msg, FALSE, ch, weap, vict, TO_CHAR);
        act(msg->god_msg.victim_msg, FALSE, ch, weap, vict, TO_VICT);
        act(msg->god_msg.room_msg, FALSE, ch, weap, vict, TO_NOTVICT);
      } else if (dam != 0) {
        /*
         * Don't send redundant color codes for TYPE_SUFFERING & other types
         * of damage without attacker_msg.
         */
        if (GET_POS(vict) == POS_DEAD) {
          if (msg->die_msg.attacker_msg) {
            send_to_char(ch, CCYEL(ch, C_CMP));
            act(msg->die_msg.attacker_msg, FALSE, ch, weap, vict, TO_CHAR);
            send_to_char(ch, CCNRM(ch, C_CMP));
          }

          send_to_char(vict, CCRED(vict, C_CMP));
          act(msg->die_msg.victim_msg, FALSE, ch, weap, vict, TO_VICT | TO_SLEEP);
          send_to_char(vict, CCNRM(vict, C_CMP));

          act(msg->die_msg.room_msg, FALSE, ch, weap, vict, TO_NOTVICT);
        } else {
          if (msg->hit_msg.attacker_msg) {
            send_to_char(ch, CCYEL(ch, C_CMP));
            act(msg->hit_msg.attacker_msg, FALSE, ch, weap, vict, TO_CHAR);
            send_to_char(ch, CCNRM(ch, C_CMP));
          }

          send_to_char(vict, CCRED(vict, C_CMP));
          act(msg->hit_msg.victim_msg, FALSE, ch, weap, vict, TO_VICT | TO_SLEEP);
          send_to_char(vict, CCNRM(vict, C_CMP));

          act(msg->hit_msg.room_msg, FALSE, ch, weap, vict, TO_NOTVICT);
        }
      } else if (ch != vict) {	/* Dam == 0 */
        if (msg->miss_msg.attacker_msg) {
          send_to_char(ch, CCYEL(ch, C_CMP));
          act(msg->miss_msg.attacker_msg, FALSE, ch, weap, vict, TO_CHAR);
          send_to_char(ch, CCNRM(ch, C_CMP));
        }

        send_to_char(vict, CCRED(vict, C_CMP));
        act(msg->miss_msg.victim_msg, FALSE, ch, weap, vict, TO_VICT | TO_SLEEP);
        send_to_char(vict, CCNRM(vict, C_CMP));

        act(msg->miss_msg.room_msg, FALSE, ch, weap, vict, TO_NOTVICT);
      }
      return (1);
    }
  }
  return (0);
}

/* This function returns the following codes:
 *	< 0	Victim died.
 *	= 0	No damage.
 *	> 0	How much damage done. */
int damage(struct char_data *ch, struct char_data *victim, int dam, int attacktype)
{
  long local_gold = 0, happy_gold = 0;
  char local_buf[256];
  char loot_arg[16];   /* writable: get_from_container strips "all." */
  struct obj_data *corpse_obj;

  if (GET_POS(victim) <= POS_DEAD) {
    /* This is "normal"-ish now with delayed extraction. -gg 3/15/2001 */
    if (PLR_FLAGGED(victim, PLR_NOTDEADYET) || MOB_FLAGGED(victim, MOB_NOTDEADYET))
      return (-1);

    log("SYSERR: Attempt to damage corpse '%s' in room #%d by '%s'.",
        GET_NAME(victim), GET_ROOM_VNUM(IN_ROOM(victim)), GET_NAME(ch));
    die(victim, ch);
    return (-1);			/* -je, 7/7/92 */
  }

  /* Check for PK if this is not a PK MUD */
  if (!pk_allowed(ch, victim)) {
    send_to_char(ch, "Player killing is not permitted.\r\n");
    return (0);
  }

  /* peaceful rooms */
  if (ch->nr != real_mobile(DG_CASTER_PROXY) &&
      ch != victim && ROOM_FLAGGED(IN_ROOM(ch), ROOM_PEACEFUL)) {
    send_to_char(ch, "This room just has such a peaceful, easy feeling...\r\n");
    return (0);
  }

  /* shopkeeper and MOB_NOKILL protection */
  if (!ok_damage_shopkeeper(ch, victim) || MOB_FLAGGED(victim, MOB_NOKILL)) {
    send_to_char(ch, "This mob is protected.\r\n");
    return (0);
  }

  /* You can't damage an immortal! */
  if (!IS_NPC(victim) && ((GET_LEVEL(victim) >= LVL_IMMORT) && PRF_FLAGGED(victim, PRF_NOHASSLE)))
    dam = 0;

  dam = damage_mtrigger(ch, victim, dam, attacktype);
  if (dam == -1) {
  	return (0);
  }

  if (victim != ch) {
    /* Start the attacker fighting the victim */
    if (GET_POS(ch) > POS_STUNNED && (FIGHTING(ch) == NULL))
      set_fighting(ch, victim);

    /* Start the victim fighting the attacker */
    if (GET_POS(victim) > POS_STUNNED && (FIGHTING(victim) == NULL)) {
      set_fighting(victim, ch);
      if (MOB_FLAGGED(victim, MOB_MEMORY) && !IS_NPC(ch))
	remember(victim, ch);
    }
  }

  /* If you attack a pet, it hates your guts */
  if (victim->master == ch)
    stop_follower(victim);

  /* If the attacker is invisible, he becomes visible */
  if (AFF_FLAGGED(ch, AFF_INVISIBLE) || AFF_FLAGGED(ch, AFF_HIDE))
    appear(ch);

  /* Cut damage in half if victim has sanct, to a minimum 1 */
  if (AFF_FLAGGED(victim, AFF_SANCTUARY) && dam >= 2)
    dam /= 2;

  /* Set the maximum damage per round and subtract the hit points */
  dam = MAX(MIN(dam, 100), 0);
  GET_HIT(victim) -= dam;

  /* Gain exp for the hit */
  if (ch != victim)
    gain_exp(ch, GET_LEVEL(victim) * dam);

  update_pos(victim);

  /* skill_message sends a message from the messages file in lib/misc.
   * dam_message just sends a generic "You hit $n extremely hard.".
   * skill_message is preferable to dam_message because it is more
   * descriptive.
   *
   * If we are _not_ attacking with a weapon (i.e. a spell), always use
   * skill_message. If we are attacking with a weapon: If this is a miss or a
   * death blow, send a skill_message if one exists; if not, default to a
   * dam_message. Otherwise, always send a dam_message. */
  if (!IS_WEAPON(attacktype))
    skill_message(dam, ch, victim, attacktype);
  else {
    if (GET_POS(victim) == POS_DEAD || dam == 0) {
      if (!skill_message(dam, ch, victim, attacktype))
	dam_message(dam, ch, victim, attacktype);
    } else {
      dam_message(dam, ch, victim, attacktype);
    }
  }

  /* Use send_to_char -- act() doesn't send message if you are DEAD. */
  switch (GET_POS(victim)) {
  case POS_MORTALLYW:
    act("$n is mortally wounded, and will die soon, if not aided.", TRUE, victim, 0, 0, TO_ROOM);
    send_to_char(victim, "You are mortally wounded, and will die soon, if not aided.\r\n");
    break;
  case POS_INCAP:
    act("$n is incapacitated and will slowly die, if not aided.", TRUE, victim, 0, 0, TO_ROOM);
    send_to_char(victim, "You are incapacitated and will slowly die, if not aided.\r\n");
    break;
  case POS_STUNNED:
    act("$n is stunned, but will probably regain consciousness again.", TRUE, victim, 0, 0, TO_ROOM);
    send_to_char(victim, "You're stunned, but will probably regain consciousness again.\r\n");
    break;
  case POS_DEAD:
    act("$n is dead!  R.I.P.", FALSE, victim, 0, 0, TO_ROOM);
    send_to_char(victim, "You are dead!  Sorry...\r\n");
    break;

  default:			/* >= POSITION SLEEPING */
    if (dam > (GET_MAX_HIT(victim) / 4))
      send_to_char(victim, "That really did HURT!\r\n");

    if (GET_HIT(victim) < (GET_MAX_HIT(victim) / 4)) {
      send_to_char(victim, "%sYou wish that your wounds would stop BLEEDING so much!%s\r\n",
		CCRED(victim, C_SPR), CCNRM(victim, C_SPR));
      if (ch != victim && MOB_FLAGGED(victim, MOB_WIMPY))
	do_flee(victim, NULL, 0, 0);
    }
    if (!IS_NPC(victim) && GET_WIMP_LEV(victim) && (victim != ch) &&
	GET_HIT(victim) < GET_WIMP_LEV(victim) && GET_HIT(victim) > 0) {
      send_to_char(victim, "You wimp out, and attempt to flee!\r\n");
      do_flee(victim, NULL, 0, 0);
    }
    break;
  }

  /* Help out poor linkless people who are attacked */
  if (!IS_NPC(victim) && !(victim->desc) && GET_POS(victim) > POS_STUNNED) {
    do_flee(victim, NULL, 0, 0);
    if (!FIGHTING(victim)) {
      act("$n is rescued by divine forces.", FALSE, victim, 0, 0, TO_ROOM);
      GET_WAS_IN(victim) = IN_ROOM(victim);
      char_from_room(victim);
      char_to_room(victim, 0);
    }
  }

  /* stop someone from fighting if they're stunned or worse */
  if (GET_POS(victim) <= POS_STUNNED && FIGHTING(victim) != NULL)
    stop_fighting(victim);

  /* Uh oh.  Victim died. */
  if (GET_POS(victim) == POS_DEAD) {
    if (ch != victim && (IS_NPC(victim) || victim->desc)) {
      if (GROUP(ch))
	group_gain(ch, victim);
      else
        solo_gain(ch, victim);
    }

    if (!IS_NPC(victim)) {
      mudlog(BRF, MAX(LVL_IMMORT, MAX(GET_INVIS_LEV(ch), GET_INVIS_LEV(victim))), 
        TRUE, "%s killed by %s at %s", GET_NAME(victim), GET_NAME(ch), world[IN_ROOM(victim)].name);
      if (MOB_FLAGGED(ch, MOB_MEMORY))
	forget(ch, victim);
    }
    /* Cant determine GET_GOLD on corpse, so do now and store */
    if (IS_NPC(victim)) {
      if ((IS_HAPPYHOUR) && (IS_HAPPYGOLD))
      {
        happy_gold = (long)(GET_GOLD(victim) * (((float)(HAPPY_GOLD))/(float)100));
        happy_gold = MAX(0, happy_gold);
        increase_gold(victim, happy_gold);
      }
      local_gold = GET_GOLD(victim);
      tb_snprintf(local_buf, sizeof(local_buf), "%ld", (long)local_gold);
    }

    last_corpse = NULL;
    die(victim, ch);
    /* Each step re-acquires the corpse die() made: the get triggers that
     * looting runs may purge or move it. */
    if (GROUP(ch) && (local_gold > 0) && PRF_FLAGGED(ch, PRF_AUTOSPLIT) ) {
      if ((corpse_obj = autoloot_corpse(ch, victim)) != NULL) {
        strlcpy(loot_arg, "all.coin", sizeof(loot_arg));
        get_from_container(ch, corpse_obj, loot_arg, FIND_OBJ_ROOM, 1);
        do_split(ch, local_buf, 0, 0);
      }
      /* need to remove the gold from the corpse */
    } else if (!IS_NPC(ch) && (ch != victim) && PRF_FLAGGED(ch, PRF_AUTOGOLD)) {
      if ((corpse_obj = autoloot_corpse(ch, victim)) != NULL) {
        strlcpy(loot_arg, "all.coin", sizeof(loot_arg));
        get_from_container(ch, corpse_obj, loot_arg, FIND_OBJ_ROOM, 1);
      }
    }
    if (!IS_NPC(ch) && (ch != victim) && PRF_FLAGGED(ch, PRF_AUTOLOOT)) {
      if ((corpse_obj = autoloot_corpse(ch, victim)) != NULL) {
        strlcpy(loot_arg, "all", sizeof(loot_arg));
        get_from_container(ch, corpse_obj, loot_arg, FIND_OBJ_ROOM, 1);
      }
    }
    if (IS_NPC(victim) && !IS_NPC(ch) && PRF_FLAGGED(ch, PRF_AUTOSAC)) {
      if ((corpse_obj = autoloot_corpse(ch, victim)) != NULL)
        sacrifice_obj(ch, corpse_obj);
    }
    last_corpse = NULL;
    return (-1);
  }
  return (dam);
}

/* Rolls ch's damage dice: weapon values 1 and 2 (weapon != NULL) or the
 * mob's bare-hand dice. Both come from world files, OLC and scripts
 * ('osetval 1 2000000000'), and dice() loops num times summing into an
 * int: a huge count stalls the single-threaded game loop and num*size can
 * overflow. damage() caps a blow at 100 anyway, so dice beyond
 * FIGHT_MAX_DICE_NUM d FIGHT_MAX_DICE_SIZE are clamped and reported. */
static int hit_dice(struct char_data *ch, struct obj_data *weapon, int num, int size)
{
  if (num >= 0 && num <= FIGHT_MAX_DICE_NUM && size >= 0 && size <= FIGHT_MAX_DICE_SIZE)
    return dice(num, size);

  if (weapon)
    TB_ERROR_MSG("fight", "weapon-dice-range", "obj",
                 "weapon vnum %d uid %ld dice %dd%d outside %dd%d, wielder "
                 DIAG_CHAR_FMT "; clamped", (int) GET_OBJ_VNUM(weapon),
                 obj_script_id(weapon), num, size, FIGHT_MAX_DICE_NUM,
                 FIGHT_MAX_DICE_SIZE, DIAG_CHAR(ch));
  else
    TB_ERROR_MSG("fight", "weapon-dice-range", "char",
                 DIAG_CHAR_FMT " bare-hand dice %dd%d outside %dd%d; clamped",
                 DIAG_CHAR(ch), num, size, FIGHT_MAX_DICE_NUM,
                 FIGHT_MAX_DICE_SIZE);
  return DICE_BOUNDED(num, size);
}

/* Calculate the THAC0 of the attacker. 'victim' currently isn't used but you
 * could use it for special cases like weapons that hit evil creatures easier
 * or a weapon that always misses attacking an animal. */
static int compute_thaco(struct char_data *ch, struct char_data *victim)
{
  int calc_thaco;

  if (!IS_NPC(ch))
    calc_thaco = thaco(GET_CLASS(ch), GET_LEVEL(ch));
  else		/* THAC0 for monsters is set in the HitRoll */
    calc_thaco = 20;
  calc_thaco -= str_app[STRENGTH_APPLY_INDEX(ch)].tohit;
  calc_thaco -= GET_HITROLL(ch);
  calc_thaco -= (int) ((GET_INT(ch) - 13) / 1.5);	/* Intelligence helps! */
  calc_thaco -= (int) ((GET_WIS(ch) - 13) / 1.5);	/* So does wisdom */

  return calc_thaco;
}

void hit(struct char_data *ch, struct char_data *victim, int type)
{
  struct obj_data *wielded;
  int w_type, victim_ac, calc_thaco, dam, diceroll, was_pending;

  /* Check that the attacker and victim exist */
  if (!ch || !victim) return;

  /* A victim killed earlier this round is "normal" with delayed extraction
   * (damage() ignores it); a fight already pending is noted here so only a
   * change made by the trigger below is reported. */
  was_pending = CHAR_PENDING_EXTRACTION(ch) || CHAR_PENDING_EXTRACTION(victim);

  /* check if the character has a fight trigger */
  fight_mtrigger(ch);

  /* Do some sanity checking, in case someone flees, etc. */
  if (IN_ROOM(ch) != IN_ROOM(victim)) {
    if (FIGHTING(ch) && FIGHTING(ch) == victim)
      stop_fighting(ch);
    return;
  }

  /* The fight script may have purged ch or victim (extraction is deferred,
   * so the pointers stay allocated but must not fight on). */
  if (CHAR_PENDING_EXTRACTION(ch) || CHAR_PENDING_EXTRACTION(victim)) {
    if (!was_pending)
      TB_WARN_MSG("fight", "hit-after-trigger", "char",
                  DIAG_CHAR_FMT " vs " DIAG_CHAR_FMT ": one was purged by the "
                  "fight trigger; hit skipped", DIAG_CHAR(ch), DIAG_CHAR(victim));
    return;
  }

  /* Only now look at the weapon: the fight script can junk, purge or swap
   * it ('mjunk dagger'), so a pointer read before the trigger may be freed. */
  wielded = GET_EQ(ch, WEAR_WIELD);

  /* Find the weapon type (for display purposes only). It indexes
   * attack_hit_text[]: weapon value 3 comes from world files and scripts
   * ('osetval 3 N') unvalidated, so an out-of-range value falls back to a
   * plain hit and is reported with the weapon and wielder. */
  if (wielded && GET_OBJ_TYPE(wielded) == ITEM_WEAPON) {
    if (VALID_ATTACK_TYPE(GET_OBJ_VAL(wielded, 3)))
      w_type = GET_OBJ_VAL(wielded, 3) + TYPE_HIT;
    else {
      TB_ERROR_MSG("fight", "attack-type-range", "obj",
                   "weapon vnum %d uid %ld val3 %d not in [0,%d), wielder "
                   DIAG_CHAR_FMT, (int) GET_OBJ_VNUM(wielded),
                   obj_script_id(wielded), GET_OBJ_VAL(wielded, 3),
                   NUM_ATTACK_TYPES, DIAG_CHAR(ch));
      w_type = TYPE_HIT;
    }
  } else {
    if (IS_NPC(ch) && ch->mob_specials.attack_type != 0) {
      if (VALID_ATTACK_TYPE(ch->mob_specials.attack_type))
        w_type = ch->mob_specials.attack_type + TYPE_HIT;
      else {
        TB_ERROR_MSG("fight", "attack-type-range", "char",
                     DIAG_CHAR_FMT " attack type %d not in [0,%d)",
                     DIAG_CHAR(ch), ch->mob_specials.attack_type,
                     NUM_ATTACK_TYPES);
        w_type = TYPE_HIT;
      }
    } else
      w_type = TYPE_HIT;
  }

  /* Calculate chance of hit. Lower THAC0 is better for attacker. */
  calc_thaco = compute_thaco(ch, victim);

  /* Calculate the raw armor including magic armor.  Lower AC is better for defender. */
  victim_ac = compute_armor_class(victim) / 10;

  /* roll the die and take your chances... */
  diceroll = rand_number(1, 20);

  /* report for debugging if necessary */
  if (CONFIG_DEBUG_MODE >= NRM)
    send_to_char(ch, "\t1Debug:\r\n   \t2Thaco: \t3%d\r\n   \t2AC: \t3%d\r\n   \t2Diceroll: \t3%d\tn\r\n", 
      calc_thaco, victim_ac, diceroll);

  /* Decide whether this is a hit or a miss.
   *  Victim asleep = hit, otherwise:
   *     1   = Automatic miss.
   *   2..19 = Checked vs. AC.
   *    20   = Automatic hit. */
  if (diceroll == 20 || !AWAKE(victim))
    dam = TRUE;
  else if (diceroll == 1)
    dam = FALSE;
  else
    dam = (calc_thaco - diceroll <= victim_ac);

  if (!dam)
    /* the attacker missed the victim */
    damage(ch, victim, 0, type == SKILL_BACKSTAB ? SKILL_BACKSTAB : w_type);
  else {
    /* okay, we know the guy has been hit.  now calculate damage.
     * Start with the damage bonuses: the damroll and strength apply */
    dam = str_app[STRENGTH_APPLY_INDEX(ch)].todam;
    dam += GET_DAMROLL(ch);

    /* Maybe holding arrow? */
    if (wielded && GET_OBJ_TYPE(wielded) == ITEM_WEAPON) {
      /* Add weapon-based damage if a weapon is being wielded */
      dam += hit_dice(ch, wielded, GET_OBJ_VAL(wielded, 1), GET_OBJ_VAL(wielded, 2));
    } else {
      /* If no weapon, add bare hand damage instead */
        if (IS_NPC(ch))
          dam += hit_dice(ch, NULL, ch->mob_specials.damnodice, ch->mob_specials.damsizedice);
        else
          dam += rand_number(0, 2);	/* Max 2 bare hand damage for players */
    }

    /* Include a damage multiplier if victim isn't ready to fight:
     * Position sitting  1.33 x normal
     * Position resting  1.66 x normal
     * Position sleeping 2.00 x normal
     * Position stunned  2.33 x normal
     * Position incap    2.66 x normal
     * Position mortally 3.00 x normal
     * Note, this is a hack because it depends on the particular
     * values of the POSITION_XXX constants. */
    if (GET_POS(victim) < POS_FIGHTING)
      dam *= 1 + (POS_FIGHTING - GET_POS(victim)) / 3;

    /* at least 1 hp damage min per hit */
    dam = MAX(1, dam);

    if (type == SKILL_BACKSTAB)
      damage(ch, victim, dam * backstab_mult(GET_LEVEL(ch)), SKILL_BACKSTAB);
    else
      damage(ch, victim, dam, w_type);
  }

  /* check if the victim has a hitprcnt trigger */
  hitprcnt_mtrigger(victim);
}

/* control the fights going on.  Called every 2 seconds from comm.c. */
void perform_violence(void)
{
  struct char_data *ch, *tch;

  for (ch = combat_list; ch; ch = next_combat_list) {
    next_combat_list = ch->next_fighting;

    if (FIGHTING(ch) == NULL || IN_ROOM(ch) != IN_ROOM(FIGHTING(ch))) {
      stop_fighting(ch);
      continue;
    }

    if (IS_NPC(ch)) {
      if (GET_MOB_WAIT(ch) > 0) {
        GET_MOB_WAIT(ch) -= PULSE_VIOLENCE;
        continue;
      }
      GET_MOB_WAIT(ch) = 0;
      if (GET_POS(ch) < POS_FIGHTING) {
        POS_CHANGE_UNSEAT(ch, POS_FIGHTING);
        act("$n scrambles to $s feet!", TRUE, ch, 0, 0, TO_ROOM);
      }
    }

    if (GET_POS(ch) < POS_FIGHTING) {
      send_to_char(ch, "You can't fight while sitting!!\r\n");
      continue;
    }

 if (GROUP(ch) && GROUP(ch)->members && GROUP(ch)->members->iSize) {
      struct iterator_data Iterator;

      tch = (struct char_data *) merge_iterator(&Iterator, GROUP(ch)->members);
    for (; tch ; tch = next_in_list(&Iterator)) {
        if (tch == ch)
          continue;
        if (!IS_NPC(tch) && !PRF_FLAGGED(tch, PRF_AUTOASSIST))
          continue;
        if (IN_ROOM(ch) != IN_ROOM(tch))
          continue;
        if (FIGHTING(tch))
          continue;
        if (GET_POS(tch) != POS_STANDING)
          continue;
        if (!CAN_SEE(tch, ch))
          continue;
      
        do_assist(tch, GET_NAME(ch), 0, 0);				  
      }
    }

    hit(ch, FIGHTING(ch), TYPE_UNDEFINED);
    if (MOB_FLAGGED(ch, MOB_SPEC) && GET_MOB_SPEC(ch) && !MOB_FLAGGED(ch, MOB_NOTDEADYET)) {
      char actbuf[MAX_INPUT_LENGTH] = "";
      (GET_MOB_SPEC(ch)) (ch, ch, 0, actbuf);
    }
  }
}
