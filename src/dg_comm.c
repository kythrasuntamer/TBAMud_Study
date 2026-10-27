/**************************************************************************
*  File: dg_comm.c                                         Part of tbaMUD *
*  Usage: Contains routines to handle mud to player communication.        *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Death's Gate MUD is based on CircleMUD, Copyright (C) 1993, 94.        *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
*                                                                         *
*  $Author: Mark A. Heilpern/egreen/Welcor $                              *
*  $Date: 2004/10/11 12:07:00$                                            *
*  $Revision: 1.0.14 $                                                    *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "dg_scripts.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "db.h"
#include "constants.h"

/* local functions */
static void sub_write_to_char(char_data *ch, char *tokens[], void *otokens[], char type[]);

/* same as any_one_arg except that it stops at punctuation.
 * first_arg must hold MAX_INPUT_LENGTH bytes; longer names are truncated
 * but still consumed from argument. */
char *any_one_name(char *argument, char *first_arg)
{
  char *arg = first_arg, *const arg_end = first_arg + MAX_INPUT_LENGTH - 1;

  /* Find first non blank */
  while(isspace(*argument))
    argument++;

  /* Find length of first word */
  for(;
    *argument && !isspace(*argument) && (!ispunct(*argument) || *argument == '#' || *argument == '-') ;
  argument++)
    if (arg < arg_end)
      *arg++ = LOWER(*argument);
  *arg = '\0';

  return argument;
}

static void sub_write_to_char(char_data *ch, char *tokens[], void *otokens[], char type[])
{
  char sb[MAX_STRING_LENGTH];
  int i;

  strlcpy(sb, "", sizeof(sb));

  for (i = 0; tokens[i + 1]; i++) {
    strlcat(sb, tokens[i], sizeof(sb));

    switch (type[i]) {
      case '~':
        if (!otokens[i])
          strlcat(sb, "someone", sizeof(sb));
        else if ((char_data *)otokens[i] == ch)
          strlcat(sb, "you", sizeof(sb));
        else
          strlcat(sb, PERS((char_data *)otokens[i], ch), sizeof(sb));
      break;

      case '|':
        if (!otokens[i])
          strlcat(sb, "someone's", sizeof(sb));
        else if ((char_data *)otokens[i] == ch)
          strlcat(sb, "your", sizeof(sb));
        else {
          strlcat(sb, PERS((char_data *) otokens[i], ch), sizeof(sb));
          strlcat(sb, "'s", sizeof(sb));
        }
      break;

      case '^':
        if (!otokens[i] || !CAN_SEE(ch, (char_data *) otokens[i]))
          strlcat(sb, "its", sizeof(sb));
        else if (otokens[i] == ch)
          strlcat(sb, "your", sizeof(sb));
        else
          strlcat(sb, HSHR((char_data *) otokens[i]), sizeof(sb));
      break;

      case '&':
        if (!otokens[i] || !CAN_SEE(ch, (char_data *) otokens[i]))
          strlcat(sb, "it", sizeof(sb));
        else if (otokens[i] == ch)
          strlcat(sb, "you", sizeof(sb));
        else
          strlcat(sb, HSSH((char_data *) otokens[i]), sizeof(sb));
      break;

      case '*':
        if (!otokens[i] || !CAN_SEE(ch, (char_data *) otokens[i]))
          strlcat(sb, "it", sizeof(sb));
        else if (otokens[i] == ch)
          strlcat(sb, "you", sizeof(sb));
        else
          strlcat(sb, HMHR((char_data *) otokens[i]), sizeof(sb));
      break;

      case '`':
        if (!otokens[i])
          strlcat(sb, "something", sizeof(sb));
        else
          strlcat(sb, OBJS(((obj_data *) otokens[i]), ch), sizeof(sb));
      break;
    }
  }

  strlcat(sb, tokens[i], sizeof(sb));
  strlcat(sb, "\n\r", sizeof(sb));
  send_to_char(ch, "%s", sb);
}

void sub_write(char *arg, char_data *ch, byte find_invis, int targets)
{
  char str[MAX_INPUT_LENGTH * 2];
  char type[MAX_INPUT_LENGTH], name[MAX_INPUT_LENGTH];
  char *tokens[MAX_INPUT_LENGTH], *s, *p;
  void *otokens[MAX_INPUT_LENGTH];
  char_data *to;
  obj_data *obj;
  int i, tmp;
  int to_sleeping = 1; /* mainly for windows compiles */

  if (!arg || !ch || IN_ROOM(ch) == NOWHERE)
    return;

  tokens[0] = str;

  for (i = 0, p = arg, s = str; *p;) {
    /* Each pass writes at most one byte to str and adds at most one token;
     * keep room for the final terminator and the NULL token sentinel. */
    if (i >= MAX_INPUT_LENGTH - 2 || s >= str + sizeof(str) - 2) {
      script_log("sub_write: message truncated at %d tokens, %d bytes.",
                 i, (int)(s - str));
      break;
    }
    switch (*p) {
      case '~':
      case '|':
      case '^':
      case '&':
      case '*':
        /* get char_data, move to next token */
        type[i] = *p;
        *s = '\0';
        p = any_one_name(++p, name);
        otokens[i] =
        find_invis ? (void *)get_char_in_room(&world[IN_ROOM(ch)], name) : (void *)get_char_room_vis(ch, name, NULL);
        tokens[++i] = ++s;
      break;

      case '`':
        /* get obj_data, move to next token */
        type[i] = *p;
        *s = '\0';
        p = any_one_name(++p, name);

        if (find_invis) obj = get_obj_in_room(&world[IN_ROOM(ch)], name);
        else if (!(obj = get_obj_in_list_vis(ch, name, NULL, world[IN_ROOM(ch)].contents))) ;
        else if (!(obj = get_obj_in_equip_vis(ch, name, &tmp, ch->equipment))) ;
        else obj = get_obj_in_list_vis(ch, name, NULL, ch->carrying);

        otokens[i] = (void *)obj;
        tokens[++i] = ++s;
      break;

      case '\\':
        /* A trailing backslash escapes nothing; never step past the NUL. */
        if (*++p)
          *s++ = *p++;
      break;

      default:
        *s++ = *p++;
    }
  }

  *s = '\0';
  tokens[++i] = NULL;

  if (IS_SET(targets, TO_CHAR) && SENDOK(ch))
    sub_write_to_char(ch, tokens, otokens, type);

  if (IS_SET(targets, TO_ROOM))
    for (to = world[IN_ROOM(ch)].people;
      to; to = to->next_in_room)
    if (to != ch && SENDOK(to))
      sub_write_to_char(to, tokens, otokens, type);
}

void send_to_zone(char *messg, zone_rnum zone)
{
  struct descriptor_data *i;

  if (!messg || !*messg)
    return;

  for (i = descriptor_list; i; i = i->next)
    if (!i->connected && i->character && AWAKE(i->character) &&
        (IN_ROOM(i->character) != NOWHERE) &&
        (world[IN_ROOM(i->character)].zone == zone))
      write_to_output(i, "%s", messg);
}

