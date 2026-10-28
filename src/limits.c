/**************************************************************************
*  File: limits.c                                          Part of tbaMUD *
*  Usage: Limits & gain funcs for HMV, exp, hunger/thirst, idle time.     *
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
#include "spells.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "class.h"
#include "fight.h"
#include "screen.h"
#include "mud_event.h"

/* local file scope function prototypes */
static int graf(int grafage, int p0, int p1, int p2, int p3, int p4, int p5, int p6);
static void check_idling(struct char_data *ch);


/* When age < 15 return the value p0
   When age is 15..29 calculate the line between p1 & p2
   When age is 30..44 calculate the line between p2 & p3
   When age is 45..59 calculate the line between p3 & p4
   When age is 60..79 calculate the line between p4 & p5
   When age >= 80 return the value p6 */
static int graf(int grafage, int p0, int p1, int p2, int p3, int p4, int p5, int p6)
{

  if (grafage < 15)
    return (p0);					/* < 15   */
  else if (grafage <= 29)
    return (p1 + (((grafage - 15) * (p2 - p1)) / 15));	/* 15..29 */
  else if (grafage <= 44)
    return (p2 + (((grafage - 30) * (p3 - p2)) / 15));	/* 30..44 */
  else if (grafage <= 59)
    return (p3 + (((grafage - 45) * (p4 - p3)) / 15));	/* 45..59 */
  else if (grafage <= 79)
    return (p4 + (((grafage - 60) * (p5 - p4)) / 20));	/* 60..79 */
  else
    return (p6);					/* >= 80 */
}

/* The hit_limit, mana_limit, and move_limit functions are gone.  They added an
 * unnecessary level of complexity to the internal structure, weren't
 * particularly useful, and led to some annoying bugs.  From the players' point
 * of view, the only difference the removal of these functions will make is
 * that a character's age will now only affect the HMV gain per tick, and _not_
 * the HMV maximums. */
/* manapoint gain pr. game hour */
int mana_gain(struct char_data *ch)
{
  int gain;

  if (IS_NPC(ch)) {
    /* Neat and fast */
    gain = GET_LEVEL(ch);
  } else {
    gain = graf(age(ch)->year, 4, 8, 12, 16, 12, 10, 8);

    /* Class calculations */

    /* Skill/Spell calculations */

    /* Position calculations    */
    switch (GET_POS(ch)) {
    case POS_SLEEPING:
      gain *= 2;
      break;
    case POS_RESTING:
      gain += (gain / 2);	/* Divide by 2 */
      break;
    case POS_SITTING:
      gain += (gain / 4);	/* Divide by 4 */
      break;
    }

    if (IS_MAGIC_USER(ch) || IS_CLERIC(ch))
      gain *= 2;

    if ((GET_COND(ch, HUNGER) == 0) || (GET_COND(ch, THIRST) == 0))
      gain /= 4;
  }

  if (AFF_FLAGGED(ch, AFF_POISON))
    gain /= 4;

  return (gain);
}

/* Hitpoint gain pr. game hour */
int hit_gain(struct char_data *ch)
{
  int gain;

  if (IS_NPC(ch)) {
    /* Neat and fast */
    gain = GET_LEVEL(ch);
  } else {

    gain = graf(age(ch)->year, 8, 12, 20, 32, 16, 10, 4);

    /* Class/Level calculations */
    /* Skill/Spell calculations */
    /* Position calculations    */

    switch (GET_POS(ch)) {
    case POS_SLEEPING:
      gain += (gain / 2);	/* Divide by 2 */
      break;
    case POS_RESTING:
      gain += (gain / 4);	/* Divide by 4 */
      break;
    case POS_SITTING:
      gain += (gain / 8);	/* Divide by 8 */
      break;
    }

    if (IS_MAGIC_USER(ch) || IS_CLERIC(ch))
      gain /= 2;	/* Ouch. */

    if ((GET_COND(ch, HUNGER) == 0) || (GET_COND(ch, THIRST) == 0))
      gain /= 4;
  }

  if (AFF_FLAGGED(ch, AFF_POISON))
    gain /= 4;

  return (gain);
}

/* move gain pr. game hour */
int move_gain(struct char_data *ch)
{
  int gain;

  if (IS_NPC(ch)) {
    /* Neat and fast */
    gain = GET_LEVEL(ch);
  } else {
    gain = graf(age(ch)->year, 16, 20, 24, 20, 16, 12, 10);

    /* Class/Level calculations */
    /* Skill/Spell calculations */
    /* Position calculations    */
    switch (GET_POS(ch)) {
    case POS_SLEEPING:
      gain += (gain / 2);	/* Divide by 2 */
      break;
    case POS_RESTING:
      gain += (gain / 4);	/* Divide by 4 */
      break;
    case POS_SITTING:
      gain += (gain / 8);	/* Divide by 8 */
      break;
    }

    if ((GET_COND(ch, HUNGER) == 0) || (GET_COND(ch, THIRST) == 0))
      gain /= 4;
  }

  if (AFF_FLAGGED(ch, AFF_POISON))
    gain /= 4;

  return (gain);
}

void set_title(struct char_data *ch, char *title)
{
  if (GET_TITLE(ch) != NULL)
    free(GET_TITLE(ch));

  if (title == NULL) {
    GET_TITLE(ch) = strdup(GET_SEX(ch) == SEX_FEMALE ?
      title_female(GET_CLASS(ch), GET_LEVEL(ch)) :
      title_male(GET_CLASS(ch), GET_LEVEL(ch)));
  } else {
    if (strlen(title) > MAX_TITLE_LENGTH)
      title[MAX_TITLE_LENGTH] = '\0';

    GET_TITLE(ch) = strdup(title);
  }
}

void run_autowiz(void)
{
#if defined(CIRCLE_UNIX)
  if (CONFIG_USE_AUTOWIZ) {
    char min_wiz[32], imm_level[32], parent_pid[32];
    pid_t launcher;
    int status;

    if (snprintf(min_wiz, sizeof(min_wiz), "%d", CONFIG_MIN_WIZLIST_LEV) < 0 ||
        snprintf(imm_level, sizeof(imm_level), "%d", LVL_IMMORT) < 0 ||
        snprintf(parent_pid, sizeof(parent_pid), "%d", (int)getpid()) < 0) {
      log("Cannot run autowiz: failed to format arguments.");
      return;
    }

    mudlog(CMP, LVL_IMMORT, FALSE, "Initiating autowiz.");
    reboot_wizlists();

    launcher = fork();
    if (launcher < 0) {
      { int saved_errno = errno;
        mudlog(BRF, LVL_IMMORT, TRUE,
               "SYSERR: autowiz launcher fork failed: %s (errno=%d) at %s:%d in %s()",
               strerror(saved_errno), saved_errno, __FILE__, __LINE__, __func__);
      }
      return;
    }

    if (launcher == 0) {
      pid_t worker = fork();
      if (worker < 0) {
        TB_REPORT_ERRNO("fork", "autowiz worker");
        _exit(127);
      }
      if (worker > 0)
        _exit(0);

      (void)setsid();
      execlp("nice", "nice", "../bin/autowiz", min_wiz, WIZLIST_FILE,
             imm_level, IMMLIST_FILE, parent_pid, (char *)NULL);
      TB_REPORT_ERRNO("execlp", "../bin/autowiz");
      _exit(127);
    }

    {
      pid_t waited;
      do {
        status = 0;
        waited = waitpid(launcher, &status, 0);
      } while (waited < 0 && errno == EINTR);

      if (waited < 0) {
        int saved_errno = errno;
        mudlog(BRF, LVL_IMMORT, TRUE,
               "SYSERR: waitpid for autowiz launcher failed: %s (errno=%d)",
               strerror(saved_errno), saved_errno);
        return;
      }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      mudlog(BRF, LVL_IMMORT, TRUE,
             "SYSERR: autowiz launcher exited abnormally (status=%d)", status);
  }
#elif defined(CIRCLE_WINDOWS)
  if (CONFIG_USE_AUTOWIZ) {
    char buf[256];
    int res = snprintf(buf, sizeof(buf), "autowiz %d %s %d %s",
                       CONFIG_MIN_WIZLIST_LEV, WIZLIST_FILE, LVL_IMMORT, IMMLIST_FILE);
    if (res >= 0 && (size_t)res < sizeof(buf)) {
      mudlog(CMP, LVL_IMMORT, FALSE, "Initiating autowiz.");
      reboot_wizlists();
      if (system(buf) != 0)
        mudlog(BRF, LVL_IMMORT, TRUE, "Warning: autowiz failed.");
    } else {
      log("Cannot run autowiz: command-line doesn't fit in buffer.");
    }
  }
#endif
}

void gain_exp(struct char_data *ch, int gain)
{
  int is_altered = FALSE;
  int num_levels = 0;

  if (!IS_NPC(ch) && ((GET_LEVEL(ch) < 1 || GET_LEVEL(ch) >= LVL_IMMORT)))
    return;

  if (IS_NPC(ch)) {
    GET_EXP(ch) += gain;
    return;
  }
  if (gain > 0 && GET_LEVEL(ch) == LVL_IMMORT - 1) {
    /* Mortal endgame: earn AP instead of entering immortal ranks. */
    gain = MIN(CONFIG_MAX_EXP_GAIN, gain);
    if (gain > 0) {
      int progress = MIN(99999, GET_ENDGAME_XP(ch));
      int points = (progress + gain) / 100000;
      GET_ENDGAME_XP(ch) = (progress + gain) % 100000;
      GET_ENDGAME_AP(ch) = MIN(1000000, GET_ENDGAME_AP(ch) + points);
      if (points > 0) {
        send_to_char(ch, "You gained %d endgame Advancement Point%s!\r\n", points, points == 1 ? "" : "s");
        save_char(ch);
      }
    }
    return;
  }
  if (gain > 0) {
    if ((IS_HAPPYHOUR) && (IS_HAPPYEXP))
      gain += (int)((float)gain * ((float)HAPPY_EXP / (float)(100)));

    gain = MIN(CONFIG_MAX_EXP_GAIN, gain);	/* put a cap on the max gain per kill */
    GET_EXP(ch) += gain;
    while (GET_LEVEL(ch) < LVL_IMMORT - 1 &&
           GET_LEVEL(ch) < LVL_IMMORT - CONFIG_NO_MORT_TO_IMMORT &&
	GET_EXP(ch) >= level_exp(GET_CLASS(ch), GET_LEVEL(ch) + 1)) {
      GET_LEVEL(ch) += 1;
      num_levels++;
      advance_level(ch);
      is_altered = TRUE;
    }

    if (is_altered) {
      mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "%s advanced %d level%s to level %d.",
		GET_NAME(ch), num_levels, num_levels == 1 ? "" : "s", GET_LEVEL(ch));
      if (num_levels == 1)
        send_to_char(ch, "You rise a level!\r\n");
      else
	send_to_char(ch, "You rise %d levels!\r\n", num_levels);
      set_title(ch, NULL);
      if (GET_LEVEL(ch) >= LVL_IMMORT && !PLR_FLAGGED(ch, PLR_NOWIZLIST))
        run_autowiz();
    }
  } else if (gain < 0) {
    gain = MAX(-CONFIG_MAX_EXP_LOSS, gain);	/* Cap max exp lost per death */
    GET_EXP(ch) += gain;
    if (GET_EXP(ch) < 0)
      GET_EXP(ch) = 0;
  }
  if (GET_LEVEL(ch) >= LVL_IMMORT && !PLR_FLAGGED(ch, PLR_NOWIZLIST))
    run_autowiz();
  }

void gain_exp_regardless(struct char_data *ch, int gain)
{
  int is_altered = FALSE;
  int num_levels = 0;

  if ((IS_HAPPYHOUR) && (IS_HAPPYEXP))
    gain += (int)((float)gain * ((float)HAPPY_EXP / (float)(100)));

  GET_EXP(ch) += gain;
  if (GET_EXP(ch) < 0)
    GET_EXP(ch) = 0;

  if (!IS_NPC(ch)) {
    while (GET_LEVEL(ch) < LVL_IMPL &&
	GET_EXP(ch) >= level_exp(GET_CLASS(ch), GET_LEVEL(ch) + 1)) {
      GET_LEVEL(ch) += 1;
      num_levels++;
      advance_level(ch);
      is_altered = TRUE;
    }

    if (is_altered) {
      mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "%s advanced %d level%s to level %d.",
		GET_NAME(ch), num_levels, num_levels == 1 ? "" : "s", GET_LEVEL(ch));
      if (num_levels == 1)
        send_to_char(ch, "You rise a level!\r\n");
      else
	send_to_char(ch, "You rise %d levels!\r\n", num_levels);
      set_title(ch, NULL);
    }
  }
  if (GET_LEVEL(ch) >= LVL_IMMORT && !PLR_FLAGGED(ch, PLR_NOWIZLIST))
    run_autowiz();
}

void gain_condition(struct char_data *ch, int condition, int value)
{
  bool intoxicated;

  if (IS_NPC(ch) || GET_COND(ch, condition) == -1)	/* No change */
    return;

  intoxicated = (GET_COND(ch, DRUNK) > 0);

  GET_COND(ch, condition) += value;

  GET_COND(ch, condition) = MAX(0, GET_COND(ch, condition));
  GET_COND(ch, condition) = MIN(24, GET_COND(ch, condition));

  if (GET_COND(ch, condition) || PLR_FLAGGED(ch, PLR_WRITING))
    return;

  switch (condition) {
  case HUNGER:
    send_to_char(ch, "You are hungry.\r\n");
    break;
  case THIRST:
    send_to_char(ch, "You are thirsty.\r\n");
    break;
  case DRUNK:
    if (intoxicated)
      send_to_char(ch, "You are now sober.\r\n");
    break;
  default:
    break;
  }

}

static void check_idling(struct char_data *ch)
{
  if (ch->char_specials.timer > CONFIG_IDLE_VOID) {
    if (GET_WAS_IN(ch) == NOWHERE && IN_ROOM(ch) != NOWHERE) {
      GET_WAS_IN(ch) = IN_ROOM(ch);
      if (FIGHTING(ch)) {
	stop_fighting(FIGHTING(ch));
	stop_fighting(ch);
      }
      act("$n disappears into the void.", TRUE, ch, 0, 0, TO_ROOM);
      send_to_char(ch, "You have been idle, and are pulled into a void.\r\n");
      save_char(ch);
      Crash_crashsave(ch);
      char_from_room(ch);
      char_to_room(ch, 1);
    } else if (ch->char_specials.timer > CONFIG_IDLE_RENT_TIME) {
      if (IN_ROOM(ch) != NOWHERE)
	char_from_room(ch);
      char_to_room(ch, 3);
      if (ch->desc) {
	STATE(ch->desc) = CON_DISCONNECT;
	/*
	 * For the 'if (d->character)' test in close_socket().
	 * -gg 3/1/98 (Happy anniversary.)
	 */
	ch->desc->character = NULL;
	ch->desc = NULL;
      }
      if (CONFIG_FREE_RENT)
	Crash_rentsave(ch, 0);
      else
	Crash_idlesave(ch);
      mudlog(CMP, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "%s force-rented and extracted (idle).", GET_NAME(ch));
      add_llog_entry(ch, LAST_IDLEOUT);
      extract_char(ch);
    }
  }
}

/* An object whose timer expired this tick, captured before any callback.
 * obj is compared by address only (obj_uid_matches) and dereferenced only
 * after the UID still maps to it. */
struct timed_obj_target {
  struct obj_data *obj;
  long uid;
};

/* Corpse j (UID uid) has rotted: tell whoever can see it, spill its
 * contents where the corpse is, and extract it. act() can run act triggers
 * that purge the corpse, so it is re-validated after each act(). */
static void decay_corpse(struct obj_data *j, long uid)
{
  struct obj_data *jj, *next_thing2;

  if (j->carried_by)
    act("$p decays in your hands.", FALSE, j->carried_by, j, 0, TO_CHAR);
  else if ((IN_ROOM(j) != NOWHERE) && (world[IN_ROOM(j)].people)) {
    act("A quivering horde of maggots consumes $p.",
        TRUE, world[IN_ROOM(j)].people, j, 0, TO_ROOM);
    if (obj_uid_matches(uid, j) && IN_ROOM(j) != NOWHERE &&
        world[IN_ROOM(j)].people)
      act("A quivering horde of maggots consumes $p.",
          TRUE, world[IN_ROOM(j)].people, j, 0, TO_CHAR);
  }
  if (!obj_uid_matches(uid, j)) {
    TB_WARN_MSG("limits", "timer-target-purged", "object_list",
                "decaying corpse uid %ld was purged by a script during its decay message",
                uid);
    return;
  }
  /* Moving contents runs no scripts, so next_content stays valid here. */
  for (jj = j->contains; jj; jj = next_thing2) {
    next_thing2 = jj->next_content;	/* Next in inventory */
    obj_from_obj(jj);

    if (j->in_obj)
      obj_to_obj(jj, j->in_obj);
    else if (j->carried_by)
      obj_to_room(jj, IN_ROOM(j->carried_by));
    else if (IN_ROOM(j) != NOWHERE)
      obj_to_room(jj, IN_ROOM(j));
    else
      core_dump();
  }
  extract_obj(j);
}

/* Update PCs, NPCs, and objects */
void point_update(void)
{
  struct char_data *i, *next_char;
  struct obj_data *j;
  /* Reused across ticks; holds no pointer beyond the tick that filled it. */
  static struct timed_obj_target *timed = NULL;
  static size_t timed_cap = 0;
  size_t ntimed, k;

  /* characters */
  for (i = character_list; i; i = next_char) {
    next_char = i->next;

    gain_condition(i, HUNGER, -1);
    gain_condition(i, DRUNK, -1);
    gain_condition(i, THIRST, -1);

    if (GET_POS(i) >= POS_STUNNED) {
      GET_HIT(i) = MIN(GET_HIT(i) + hit_gain(i), GET_MAX_HIT(i));
      GET_MANA(i) = MIN(GET_MANA(i) + mana_gain(i), GET_MAX_MANA(i));
      GET_MOVE(i) = MIN(GET_MOVE(i) + move_gain(i), GET_MAX_MOVE(i));
      if (AFF_FLAGGED(i, AFF_POISON))
	if (damage(i, i, 2, SPELL_POISON) == -1)
	  continue;	/* Oops, they died. -gg 6/24/98 */
      if (GET_POS(i) <= POS_STUNNED)
	update_pos(i);
    } else if (GET_POS(i) == POS_INCAP) {
      if (damage(i, i, 1, TYPE_SUFFERING) == -1)
	continue;
    } else if (GET_POS(i) == POS_MORTALLYW) {
      if (damage(i, i, 2, TYPE_SUFFERING) == -1)
	continue;
    }
    if (!IS_NPC(i)) {
      update_char_objects(i);
      (i->char_specials.timer)++;
      if (GET_LEVEL(i) < CONFIG_IDLE_MAX_LEVEL)
	check_idling(i);
    }
  }

  /* objects. Pass 1 runs no callbacks: it counts every timer down and
   * snapshots the objects that expired as (address, UID) pairs. Pass 2 then
   * decays corpses and fires timer triggers. Both can run scripts (act
   * triggers, the timer trigger itself) that purge ANY object, and
   * extract_obj() frees immediately, so no saved list pointer (j->next) is
   * followed across a callback: each target is re-validated by UID first. */
  ntimed = 0;
  for (j = object_list; j; j = j->next) {
    if (IS_CORPSE(j)) {
      if (GET_OBJ_TIMER(j) > 0)
	GET_OBJ_TIMER(j)--;
      if (GET_OBJ_TIMER(j))
        continue;
    } else if (GET_OBJ_TIMER(j) <= 0 || --GET_OBJ_TIMER(j) > 0)
      continue;
    /* note to .rej hand-patchers: timer triggers fire last in point_update() */
    if (ntimed == timed_cap) {
      timed_cap = timed_cap ? timed_cap * 2 : 64;
      RECREATE(timed, struct timed_obj_target, timed_cap);
    }
    timed[ntimed].obj = j;
    timed[ntimed].uid = obj_script_id(j);
    ntimed++;
  }

  for (k = 0; k < ntimed; k++) {
    j = timed[k].obj;
    if (!obj_uid_matches(timed[k].uid, j)) {
      TB_WARN_MSG("limits", "timer-target-purged", "object_list",
                  "expired object uid %ld was purged by an earlier script this tick",
                  timed[k].uid);
      continue;
    }
    if (IS_CORPSE(j))
      decay_corpse(j, timed[k].uid);
    else
      timer_otrigger(j);
  }

  /* Take 1 from the happy-hour tick counter, and end happy-hour if zero */
       if (HAPPY_TIME > 1)  HAPPY_TIME--;
  else if (HAPPY_TIME == 1)   /* Last tick - set everything back to zero */
  {
    HAPPY_QP = 0;
    HAPPY_EXP = 0;
    HAPPY_GOLD = 0;
    HAPPY_TIME = 0;
   game_info("Happy hour has ended!");
  }
}

/* Coins: purse (GET_GOLD) and bank (GET_BANK_GOLD) both live in [0, cap].
 *
 * The caps (MAX_GOLD, MAX_BANK) sit only ~7.5M below INT_MAX, so the sum is
 * computed in long long: an int sum would be signed overflow before any
 * comparison could catch it. A change that would leave the range is applied
 * up to the boundary and the coins that could not be applied are reported
 * (op "gold-clamp" / "bank-clamp"): a clamp destroys or invents money, so it
 * is never silent. Callers that move coins between purse and bank (the bank
 * spec proc) must refuse a transfer that would clamp BEFORE debiting the
 * other side. */
static int add_coins_clamped(struct char_data *ch, int *field, long long amt,
                             int cap, const char *op, const char *resource)
{
  long long want = (long long) *field + amt;
  long long got = want < 0 ? 0 : (want > cap ? cap : want);

  if (got != want)
    TB_WARN_MSG("economy", op, resource,
                DIAG_CHAR_FMT " curr %d amt %lld range [0,%d]: %lld coins not applied",
                DIAG_CHAR(ch), *field, amt, cap, want - got);
  *field = (int) got;
  return *field;
}

/* Note: amt may be negative */
int increase_gold(struct char_data *ch, int amt)
{
  add_coins_clamped(ch, &GET_GOLD(ch), amt, MAX_GOLD, "gold-clamp", "points.gold");
  if (GET_GOLD(ch) == MAX_GOLD)
    send_to_char(ch, "%sYou have reached the maximum gold!\r\n%sYou must spend it or bank it before you can gain any more.\r\n", QBRED, QNRM);

  return (GET_GOLD(ch));
}

int decrease_gold(struct char_data *ch, int deduction)
{
  /* Negated in long long: -INT_MIN does not fit in an int. */
  add_coins_clamped(ch, &GET_GOLD(ch), -(long long) deduction, MAX_GOLD,
                    "gold-clamp", "points.gold");
  return (GET_GOLD(ch));
}

int increase_bank(struct char_data *ch, int amt)
{
  if (IS_NPC(ch)) return 0;

  add_coins_clamped(ch, &GET_BANK_GOLD(ch), amt, MAX_BANK, "bank-clamp",
                    "player_specials.bank_gold");
  if (GET_BANK_GOLD(ch) == MAX_BANK)
    send_to_char(ch, "%sYou have reached the maximum bank balance!\r\n%sYou cannot put more into your account unless you withdraw some first.\r\n", QBRED, QNRM);
  return (GET_BANK_GOLD(ch));
}

int decrease_bank(struct char_data *ch, int deduction)
{
  if (IS_NPC(ch)) return 0;

  add_coins_clamped(ch, &GET_BANK_GOLD(ch), -(long long) deduction, MAX_BANK,
                    "bank-clamp", "player_specials.bank_gold");
  return (GET_BANK_GOLD(ch));
}
