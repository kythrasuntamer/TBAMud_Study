/**************************************************************************
*  File: act.social.c                                      Part of tbaMUD *
*  Usage: Functions to handle socials.                                    *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/**
 * @file act.social.c
 * Socials (do_action), global emotes (do_gmote) and the social command
 * table (create_command_list(), find_action()).
 *
 * Data contract: soc_mess_list[0 .. top_of_socialt] (top_of_socialt is the
 * inclusive last index, unlike top_of_helpt) is sorted by act_nr and
 * loaded from the socials file or aedit; builders control every message.
 * Any message field may be NULL ('#' in the file, or cleared in aedit).
 * NULL passed to act() means "no message"; a field printed with
 * send_to_char() or formatted with tb_snprintf() is checked first, falls
 * back to do_action's default where one exists, and otherwise is
 * reported as social-field-missing.
 *
 * In a body-part social the player's typed body part is passed as act()'s
 * obj argument, so those messages may name it only with $t; $o/$p/$a would
 * read the string as a struct obj_data and are refused (act-code).
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "screen.h"
#include "spells.h"
#include "act.h"

/** Read an optional social message field. Every field may be NULL ('#' in
 * the socials file, or cleared in aedit); SOC_MSG gives @p dflt instead, so
 * the NULL contract is visible where the field is printed. Evaluates
 * action twice: pass a plain variable. */
#define SOC_MSG(action, field, dflt) \
  ((action)->field ? (action)->field : (dflt))

/* Defaults shared by do_action and do_gmote for optional fields. */
#define SOC_DFLT_NOT_FOUND  "I don't see anything by that name here."
#define SOC_DFLT_CHAR_AUTO  "Erm, no."

/* local defined functions for local use */
/* do_action and do_gmote utility function */
static int find_action(int cmd);
static bool social_body_msg_ok(const struct social_messg *action,
                               const char *msg, const char *field);
static void social_misconfigured(struct char_data *ch,
                                 const struct social_messg *action,
                                 const char *field);

/* Any social message field may be NULL: fread_action() stores NULL for a
 * '#' line and aedit can clear fields. Passing NULL to act() is the
 * documented "no message" contract, but a NULL passed to send_to_char()'s
 * %s or into tb_snprintf() is undefined (glibc prints "(null)"). Optional
 * fields (not_found, char_auto) fall back to fixed defaults as do_action
 * always did; a field the command cannot do without is reported here and
 * the social is not run. */
static void social_misconfigured(struct char_data *ch,
                                 const struct social_messg *action,
                                 const char *field)
{
  TB_WARN_MSG("social", "social-field-missing", "soc_mess_list",
              "social '%s' has no %s message (used by " DIAG_CHAR_FMT ")",
              SOC_MSG(action, command, "(null)"), field,
              DIAG_CHAR(ch));
  send_to_char(ch, "That social is misconfigured; please tell an immortal.\r\n");
}

/* In a body-part social ("lick bob ear") act()'s obj argument is the typed
 * body part, a char[MAX_INPUT_LENGTH] on the stack, so the message may
 * refer to it only as $t. $o/$p/$a would make perform_act() read that
 * string as a struct obj_data (OBJN/OBJS/SANA, CAN_SEE_OBJ). The message
 * text comes from builders (aedit or the socials file); the bytes of the
 * body part come from the player. A message using those codes is reported
 * and the social is refused rather than run. */
static bool social_body_msg_ok(const struct social_messg *action,
                               const char *msg, const char *field)
{
  const char *p;

  for (p = msg; p && (p = strchr(p, '$')) != NULL; p += 2) {
    if (p[1] == '\0')
      break;
    if (p[1] == 'o' || p[1] == 'p' || p[1] == 'a') {
      TB_ERROR_MSG("social", "act-code", "soc_mess_list",
                   "social '%s' field %s uses $%c with a body-part string "
                   "target; only $t may name the body part",
                   SOC_MSG(action, command, "(null)"), field, p[1]);
      return FALSE;
    }
  }
  return TRUE;
}


ACMD(do_action)
{
  char arg[MAX_INPUT_LENGTH], part[MAX_INPUT_LENGTH];
  int act_nr;
  struct social_messg *action;
  struct char_data *vict;
  struct obj_data *targ;

  if ((act_nr = find_action(cmd)) < 0) {
    send_to_char(ch, "That action is not supported.\r\n");
    return;
  }

  action = &soc_mess_list[act_nr];

  if (!argument || !*argument) {
    if (!action->char_no_arg) {
      social_misconfigured(ch, action, "char_no_arg");
      return;
    }
    send_to_char(ch, "%s\r\n", action->char_no_arg);
    act(action->others_no_arg, action->hide, ch, 0, 0, TO_ROOM);
    return;
  }

  two_arguments(argument, arg, part);

  if ((!action->char_body_found) && (*part)) {
    send_to_char(ch, "Sorry, this social does not support body parts.\r\n");
    return;
  }

  if (!action->char_found)
    *arg = '\0';

  if (action->char_found)
    one_argument(argument, arg);
  else
    *arg = '\0';

  vict = get_char_vis(ch, arg, NULL, FIND_CHAR_ROOM);
  if (!vict) {
    if (action->char_obj_found) {
      targ = get_obj_in_list_vis(ch, arg, NULL, ch->carrying);
      if (!targ) targ = get_obj_in_list_vis(ch, arg, NULL, world[IN_ROOM(ch)].contents);
      if (targ) {
        act(action->char_obj_found, action->hide, ch, targ, 0, TO_CHAR);
        act(action->others_obj_found, action->hide, ch, targ, 0, TO_ROOM);
        return;
      }
    }
    send_to_char(ch, "%s\r\n", SOC_MSG(action, not_found, SOC_DFLT_NOT_FOUND));
    return;
  }

  if (vict == ch) {
    send_to_char(ch, "%s\r\n", SOC_MSG(action, char_auto, SOC_DFLT_CHAR_AUTO));
    act(action->others_auto, action->hide, ch, 0, 0, TO_ROOM);
    return;
  }

  if (GET_POS(vict) < action->min_victim_position)
    act("$N is not in a proper position for that.", FALSE, ch, 0, vict, TO_CHAR | TO_SLEEP);
  else {
    if (*part) {
      if (!social_body_msg_ok(action, action->char_body_found, "char_body_found") ||
          !social_body_msg_ok(action, action->others_body_found, "others_body_found") ||
          !social_body_msg_ok(action, action->vict_body_found, "vict_body_found")) {
        send_to_char(ch, "That social is misconfigured; please tell an immortal.\r\n");
        return;
      }
      /* part is passed as obj and must be read only through $t (above). */
      act(action->char_body_found, 0, ch, (struct obj_data *)part, vict, TO_CHAR | TO_SLEEP);
      act(action->others_body_found, action->hide, ch, (struct obj_data *)part, vict, TO_NOTVICT);
      act(action->vict_body_found, action->hide, ch, (struct obj_data *)part, vict, TO_VICT);
    } else {
      act(action->char_found, 0, ch, 0, vict, TO_CHAR | TO_SLEEP);
      act(action->others_found, action->hide, ch, 0, vict, TO_NOTVICT);
      act(action->vict_found, action->hide, ch, 0, vict, TO_VICT);
    }
  }
}

/* this function adds in the loaded socials and assigns them a command # */
void create_command_list(void)
{
  int i, j, k;
  struct social_messg temp;

  /* free up old command list */
  if (complete_cmd_info)
    free_command_list();

  /* re check the sort on the socials */
  for (j = 0; j < top_of_socialt; j++) {
    k = j;
    for (i = j + 1; i <= top_of_socialt; i++)
      if (str_cmp(soc_mess_list[i].sort_as, soc_mess_list[k].sort_as) < 0)
        k = i;
    if (j != k) {
      temp = soc_mess_list[j];
      soc_mess_list[j] = soc_mess_list[k];
      soc_mess_list[k] = temp;
    }
  }

  /* count the commands in the command list */
  i = 0;
  while(*cmd_info[i].command != '\n') i++;
  i++;

  CREATE(complete_cmd_info, struct command_info, top_of_socialt + i + 2);

  /* this loop sorts the socials and commands together into one big list */
  i = 0;
  j = 0;
  k = 0;
  while ((*cmd_info[i].command != '\n') || (j <= top_of_socialt))  {
    /* Never consume the cmd_info sentinel while socials remain.  Doing so
     * advances i past the end of cmd_info and the next loop test reads OOB. */
    if (j > top_of_socialt ||
        (*cmd_info[i].command != '\n' &&
         ((i < RESERVE_CMDS) ||
          (str_cmp(cmd_info[i].sort_as, soc_mess_list[j].sort_as) < 1))))
      complete_cmd_info[k++] = cmd_info[i++];
    else {
      soc_mess_list[j].act_nr		= k;
      complete_cmd_info[k].command		= soc_mess_list[j].command;
      complete_cmd_info[k].sort_as		= soc_mess_list[j].sort_as;
      complete_cmd_info[k].minimum_position	= soc_mess_list[j].min_char_position;
      complete_cmd_info[k].command_pointer	= do_action;
      complete_cmd_info[k].minimum_level    	= soc_mess_list[j++].min_level_char;
      complete_cmd_info[k++].subcmd		= 0;
    }
  }
	complete_cmd_info[k] = cmd_info[i];
  log("Command info rebuilt, %d total commands.", k);
}

void free_command_list(void)
{
  free(complete_cmd_info);
  complete_cmd_info = NULL;
}

void free_social_messages(void)
{
  struct social_messg *mess;
  int i;

  for (i = 0;i <= top_of_socialt;i++)  {
    mess = &soc_mess_list[i];
    free_action(mess);
  }
  free(soc_mess_list);
}

void free_action(struct social_messg *mess)  {
  if (mess->command) free(mess->command);
  if (mess->sort_as) free(mess->sort_as);
  if (mess->char_no_arg) free(mess->char_no_arg);
  if (mess->others_no_arg) free(mess->others_no_arg);
  if (mess->char_found) free(mess->char_found);
  if (mess->others_found) free(mess->others_found);
  if (mess->vict_found) free(mess->vict_found);
  if (mess->char_body_found) free(mess->char_body_found);
  if (mess->others_body_found) free(mess->others_body_found);
  if (mess->vict_body_found) free(mess->vict_body_found);
  if (mess->not_found) free(mess->not_found);
  if (mess->char_auto) free(mess->char_auto);
  if (mess->others_auto) free(mess->others_auto);
  if (mess->char_obj_found) free(mess->char_obj_found);
  if (mess->others_obj_found) free(mess->others_obj_found);
  memset(mess, 0, sizeof(struct social_messg));
}

static int find_action(int cmd)
{
  int bot, top, mid;

  bot = 0;
  top = top_of_socialt;

  if (top < 0)
    return (-1);

  for (;;) {
    mid = (bot + top) / 2;

    if (soc_mess_list[mid].act_nr == cmd)
      return (mid);
    if (bot >= top)
      return (-1);

    if (soc_mess_list[mid].act_nr > cmd)
      top = --mid;
    else
      bot = ++mid;
  }
}

ACMD(do_gmote)
{
  int act_nr, length;
  char arg[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH];
  struct social_messg *action;
  struct char_data *vict = NULL;

  half_chop(argument, buf, arg);

  if(subcmd)
    for (length = strlen(buf), cmd = 0; *complete_cmd_info[cmd].command != '\n'; cmd++)
      if (!strncmp(complete_cmd_info[cmd].command, buf, length))
        break;

  if ((act_nr = find_action(cmd)) < 0) {
    /* Free-form gemote: the player's text goes into the act() format. */
    char text[MAX_STRING_LENGTH / 2], fmt[MAX_STRING_LENGTH];

    ACT_ESCAPE_TEXT(ch, argument, text);
    tb_snprintf(fmt, sizeof(fmt), "Gemote: $n%s", text);
    act(fmt, FALSE, ch, 0, vict, TO_GMOTE);
    return;
  }

  if (ROOM_FLAGGED(IN_ROOM(ch), ROOM_SOUNDPROOF)) {
    send_to_char(ch, "The walls seem to absorb your actions.\r\n");
    return;
  }

action = &soc_mess_list[act_nr];

  if (!action->char_found)
    *arg = '\0';

  if (!*arg) {
    if(!action->others_no_arg || !*action->others_no_arg) {
      send_to_char(ch, "Who are you going to do that to?\r\n");
      return;
    }
    tb_snprintf(buf, sizeof(buf), "Gemote: %s", action->others_no_arg);
  } else if (!(vict = get_char_vis(ch, arg, NULL, FIND_CHAR_WORLD))) {
    send_to_char(ch, "%s\r\n", SOC_MSG(action, not_found, SOC_DFLT_NOT_FOUND));
    return;
  } else if (vict == ch) {
    if(!action->others_auto || !*action->others_auto) {
      send_to_char(ch, "%s\r\n", SOC_MSG(action, char_auto, SOC_DFLT_CHAR_AUTO));
      return;
    }
    tb_snprintf(buf, sizeof(buf), "Gemote: %s", action->others_auto);
  } else {
    if (GET_POS(vict) < action->min_victim_position) {
      act("$N is not in a proper position for that.",
           FALSE, ch, 0, vict, TO_CHAR | TO_SLEEP);
      return;
    }
    if (!action->others_found) {
      social_misconfigured(ch, action, "others_found");
      return;
    }
    tb_snprintf(buf, sizeof(buf), "Gemote: %s", action->others_found);
  }
  act(buf, FALSE, ch, 0, vict, TO_GMOTE);
}
