/**************************************************************************
*  File: act.wizard.c                                      Part of tbaMUD *
*  Usage: Player-level god commands and other goodies.                    *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/** @file act.wizard.c
 * Immortal commands (goto, stat, set, show, zcheck, copyover, ...) and the
 * last-login (LAST_FILE) and recent-player bookkeeping.
 *
 * Who controls the input:
 * - Command arguments come from immortals, and also from mortals and DG
 *   scripts through 'force', mforce and the level 0 commands (return).
 *   ch->desc may therefore be NULL in any ACMD; commands that need it
 *   check it first (command-no-desc).
 * - Values shown by stat/show/zcheck come from world files, player files,
 *   OLC and DG scripts, none of which range-check them. Every table lookup
 *   goes through TABLE_NAME / VALID_ATTACK_TYPE, every vnum argument
 *   through parse_vnum_arg(), and every time display through TIME_STR.
 *
 * Temporary file copies of players (stat file, show player, last <name>,
 * set file): the command owns the char_data it CREATEs and load_char()s,
 * frees it with free_char() before returning, and never links it into
 * world[].people or character_list. extract_char_final() must not be used
 * on it: for a character without a descriptor it save_char()s the stale
 * copy over the pfile and Crash_delete_crashfile()s the player's objects.
 * If the player is in the world (even linkdead) the in-world character is
 * the current one; 'stat file' refuses such a player (stat-file-online).
 *
 * Diagnostics use subsystem "act"; the stable operation names are listed
 * at each TB_* call and asserted in tests/test_act_wizard.c. */

#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "spells.h"
#include "house.h"
#include "screen.h"
#include "constants.h"
#include "oasis.h"
#include "dg_scripts.h"
#include "shop.h"
#include "act.h"
#include "genzon.h" /* for real_zone_by_thing */
#include "class.h"
#include "genolc.h"
#include "genobj.h"
#include "fight.h"
#include "house.h"
#include "modify.h"
#include "quest.h"
#include "ban.h"
#include "screen.h"

/* Range checks for indexing the display tables (attack_hit_text[],
 * apply_types[], genders[], ...). The indexes come from world files, player
 * files, OLC or DG scripts, and none of those bound them, so every table
 * lookup in this file goes through one of these. The unsigned cast makes a
 * negative index fail the test. Each evaluates its argument once. */
#define TABLE_INDEX_OK(i, n)     ((unsigned int) (i) < (unsigned int) (n))
#define VALID_ATTACK_TYPE(t)     TABLE_INDEX_OK((t), NUM_ATTACK_TYPES)

/* TRUE when ch asked not to have its own communication echoed back. NPCs
 * (and switched immortals' bodies) have no preferences of their own: their
 * PRF_FLAGS are the shared dummy_mob's. ch is evaluated twice. */
#define NOREPEAT(ch)             (!IS_NPC(ch) && PRF_FLAGGED((ch), PRF_NOREPEAT))

/* First statement of a command that needs the caller's descriptor. Any
 * command can arrive without one: 'force' and DG mforce reach linkdead
 * players and NPCs. Reports command-no-desc with the caller and returns
 * from the ACMD. ch is evaluated several times: pass the ACMD's ch. */
#define ACMD_REQUIRE_DESC(ch, cmdname) do { \
  if (!(ch)->desc) { \
    TB_WARN_MSG("act", "command-no-desc", (cmdname), \
                DIAG_CHAR_FMT " has no descriptor", DIAG_CHAR(ch)); \
    return; \
  } \
} while (0)

/* Number of elements of a true array (never a pointer parameter). Compile
 * time only: the argument is not evaluated. */
#ifndef TB_ARRAY_LEN
#define TB_ARRAY_LEN(a)          (sizeof(a) / sizeof((a)[0]))
#endif

/* tb_strftime() reports a NULL struct tm (localtime()/gmtime() of an
 * out-of-range time_t from a pfile or LAST_FILE) at the call site but
 * leaves dst unwritten, and every caller here prints dst. TIME_STR fills
 * dst with "?" first, so the caller prints that instead of stack garbage.
 * dst must be an array (sizeof); it is evaluated more than once. */
#define TIME_STR(dst, fmt, tmptr) do { \
  strlcpy((dst), "?", sizeof(dst)); \
  tb_strftime((dst), sizeof(dst), (fmt), (tmptr)); \
} while (0)

/** Checked lookup in one of the "\n"-terminated name tables in constants.c.
 * @param table   The table; count is its number of real entries (not
 *                counting the "\n" sentinel), e.g. NUM_APPLIES.
 * @param idx     The index. It comes from a world file, a player file, OLC
 *                or a DG script, none of which range-check it.
 * @param op      Stable diagnostic operation for this call site.
 * @param id_kind/id  The entity that owns the bad value ("player idnum",
 *                "zone vnum", ...). DIAG_CHAR(ch) supplies both.
 * @return table[idx], or "<invalid>" after a WARNING naming the table, the
 *         index and the entity, reported at the caller's file and line.
 * Unlike sprinttype(), a bad index is never silent. */
static const char *table_name_at(const char *const *table, int count, long idx,
                                 const char *table_name, const char *op,
                                 const char *id_kind, long id,
                                 const char *file, int line, const char *func)
{
  if (idx >= 0 && idx < count)
    return table[idx];
  tb_error_log_at("WARNING", "act", op, table_name, 0, file, line, func,
                  "%s %ld: %s index %ld outside 0..%d",
                  id_kind, id, table_name, idx, count - 1);
  return "<invalid>";
}
/* TABLE_NAME(apply_types, NUM_APPLIES, loc, "stat-apply", DIAG_CHAR(k)) or
 * TABLE_NAME(dirs, NUM_OF_DIRS, d, "op", "zone vnum", znum): the trailing
 * arguments are the (id_kind, id) pair. idx is evaluated once. */
#define TABLE_NAME(table, count, idx, op, ...) \
  table_name_at((table), (count), (long) (idx), #table, (op), \
                __VA_ARGS__, __FILE__, __LINE__, __func__)

/** Parse a vnum typed by an immortal (or passed by a forced/DG command).
 * Vnums are IDXTYPE (ush_int); IDXTYPE_MAX itself is NOWHERE/NOTHING/
 * NOBODY. tb_parse_int_or_zero() followed by the implicit conversion to
 * IDXTYPE turned 65537 into vnum 1 and an overflow into 0, i.e. silently
 * into another, valid entity.
 * @param what  "room", "mob", "object", "zone": names the entity in the
 *              reply and in the vnum-parse warning (resource).
 * @return TRUE and *out, or FALSE after telling ch and reporting
 *         vnum-parse with the caller (the raw text is not logged). */
static bool parse_vnum_arg(struct char_data *ch, const char *text,
                           const char *what, IDXTYPE *out)
{
  long v;

  if (!tb_parse_long_checked(text, &v) || v < IDXTYPE_MIN || v >= IDXTYPE_MAX) {
    TB_WARN_MSG("act", "vnum-parse", what,
                "caller " DIAG_CHAR_FMT ": %zu-char argument is not a vnum in %d..%d",
                DIAG_CHAR(ch), strlen(text), IDXTYPE_MIN, IDXTYPE_MAX - 1);
    send_to_char(ch, "That is not a valid %s number (%d-%d).\r\n", what,
                 IDXTYPE_MIN, IDXTYPE_MAX - 1);
    return FALSE;
  }
  *out = (IDXTYPE) v;
  return TRUE;
}

/* local utility functions with file scope */
static int perform_set(struct char_data *ch, struct char_data *vict, int mode, char *val_arg);
static void perform_immort_invis(struct char_data *ch, int level);
static void list_zone_commands_room(struct char_data *ch, room_vnum rvnum);
static void do_stat_room(struct char_data *ch, struct room_data *rm);
static void do_stat_object(struct char_data *ch, struct obj_data *j);
static void do_stat_character(struct char_data *ch, struct char_data *k);
static void stop_snooping(struct char_data *ch);
static size_t print_zone_to_buf(char *bufptr, size_t left, zone_rnum zone, int listall);
static struct char_data *is_in_game(long idnum);
static struct char_data *find_player_in_world(long idnum);
static void mob_checkload(struct char_data *ch, mob_vnum mvnum);
static void obj_checkload(struct char_data *ch, obj_vnum ovnum);
static void trg_checkload(struct char_data *ch, trig_vnum tvnum);
static void mod_llog_entry(struct last_entry *llast,int type);
static int  get_max_recent(void);
static void clear_recent(struct recent_player *this);
static struct recent_player *create_recent(void);
const char *get_spec_func_name(SPECIAL(*func));
bool zedit_get_levels(struct descriptor_data *d, char *buf);

/* Local Globals */
static struct recent_player *recent_list = NULL;  /** Global list of recent players */

/** Purge every NPC and object in room. Players are skipped.
 * Objects are extracted immediately. NPCs go through extract_char(), which
 * only marks them pending extraction (extract_pending_chars() frees them at
 * the end of the pulse), so following vict->next_in_room after it is safe.
 * If extract_char() ever becomes immediate this loop must snapshot first.
 * @return 0 for an invalid rnum, otherwise 1. */
static int purge_room(room_rnum room)
{
  int j;
  struct char_data *vict;

  if (room == NOWHERE || room > top_of_world) return 0;

  for (vict = world[room].people; vict; vict = vict->next_in_room) {
    if (!IS_NPC(vict))
      continue;

    /* Dump inventory. */
    while (vict->carrying)
      extract_obj(vict->carrying);

    /* Dump equipment. */
    for (j = 0; j < NUM_WEARS; j++)
      if (GET_EQ(vict, j))
        extract_obj(GET_EQ(vict, j));

    /* Dump character. */
    extract_char(vict);
  }

  /* Clear the ground. */
  while (world[room].contents)
    extract_obj(world[room].contents);

  return 1;
}

ACMD(do_wizhelp) 
{ 
  extern int *cmd_sort_info;

  (void)argument;
  (void)cmd;
  (void)subcmd; 
  int no = 1, i, cmd_num; 
  int level;

  ACMD_REQUIRE_DESC(ch, "wizhelp");

  send_to_char(ch, "The following privileged commands are available:\r\n"); 
  
  for (level = LVL_IMPL; level >= LVL_IMMORT; level--) { 
    send_to_char(ch, "%sLevel %d%s:\r\n", CCCYN(ch, C_NRM), level, CCNRM(ch, C_NRM)); 
    for (no = 1, cmd_num = 1; complete_cmd_info[cmd_sort_info[cmd_num]].command[0] != '\n'; cmd_num++) { 
      i = cmd_sort_info[cmd_num]; 
  
      if (complete_cmd_info[i].minimum_level != level) 
        continue;            
  
      send_to_char(ch, "%-14s%s", complete_cmd_info[i].command, no++ % 7 == 0 ? "\r\n" : ""); 
    } 
    if (no % 7 != 1) 
      send_to_char(ch, "\r\n"); 
    if (level != LVL_IMMORT) 
      send_to_char(ch, "\r\n"); 
  } 
}

ACMD(do_echo)
{
  skip_spaces(&argument);

  if (!*argument)
    send_to_char(ch, "Yes.. but what?\r\n");
  else {
    char buf[MAX_INPUT_LENGTH + 4];

    if (subcmd == SCMD_EMOTE)
      tb_snprintf(buf, sizeof(buf), "$n %s", argument);
    else {
      strlcpy(buf, argument, sizeof(buf));
      mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE, "(GC) %s echoed: %s", GET_NAME(ch), buf);
      }
    act(buf, FALSE, ch, 0, 0, TO_ROOM);

    if (NOREPEAT(ch))
      send_to_char(ch, "%s", CONFIG_OK);
    else
      act(buf, FALSE, ch, 0, 0, TO_CHAR);
  }
}

ACMD(do_send)
{
  char arg[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH];
  struct char_data *vict;

  half_chop(argument, arg, buf);

  if (!*arg) {
    send_to_char(ch, "Send what to who?\r\n");
    return;
  }
  if (!(vict = get_char_vis(ch, arg, NULL, FIND_CHAR_WORLD))) {
    send_to_char(ch, "%s", CONFIG_NOPERSON);
    return;
  }
  send_to_char(vict, "%s\r\n", buf);
  mudlog(CMP, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s sent %s: %s", GET_NAME(ch), GET_NAME(vict), buf);

  if (NOREPEAT(ch))
    send_to_char(ch, "Sent.\r\n");
  else
    send_to_char(ch, "You send '%s' to %s.\r\n", buf, GET_NAME(vict));
}

/** Resolve a room argument for goto, at, teleport, and the DG mgoto/mat/
 * mteleport/wteleport family.  -je 4/6/93
 * @param rawroomstr A room vnum, or a mob/object name (N.name allowed). It
 *        comes from the immortal or the script.
 * @return An rnum, valid only until the next room insert or delete, or
 *         NOWHERE. On NOWHERE this function has already told ch why, so
 *         callers must not send a second message.
 * Below LVL_GRGOD the GODROOM, PRIVATE (two or more occupants) and HOUSE
 * restrictions apply; at LVL_GRGOD and above there are none. */
room_rnum find_target_room(struct char_data *ch, char *rawroomstr)
{
  room_rnum location = NOWHERE;
  char roomstr[MAX_INPUT_LENGTH];

  one_argument(rawroomstr, roomstr);

  if (!*roomstr) {
    send_to_char(ch, "You must supply a room number or name.\r\n");
    return (NOWHERE);
  }

  if (isdigit(*roomstr) && !strchr(roomstr, '.')) {
    room_vnum rvnum;

    if (!parse_vnum_arg(ch, roomstr, "room", &rvnum))
      return (NOWHERE);
    if ((location = real_room(rvnum)) == NOWHERE) {
      send_to_char(ch, "No room exists with that number.\r\n");
      return (NOWHERE);
    }
  } else {
    struct char_data *target_mob;
    struct obj_data *target_obj;
    char *mobobjstr = roomstr;
    int num;

    num = get_number(&mobobjstr);
    if ((target_mob = get_char_vis(ch, mobobjstr, &num, FIND_CHAR_WORLD)) != NULL) {
      if ((location = IN_ROOM(target_mob)) == NOWHERE) {
        send_to_char(ch, "That character is currently lost.\r\n");
        return (NOWHERE);
      }
    } else if ((target_obj = get_obj_vis(ch, mobobjstr, &num)) != NULL) {
      if (IN_ROOM(target_obj) != NOWHERE)
        location = IN_ROOM(target_obj);
      else if (target_obj->carried_by && IN_ROOM(target_obj->carried_by) != NOWHERE)
        location = IN_ROOM(target_obj->carried_by);
      else if (target_obj->worn_by && IN_ROOM(target_obj->worn_by) != NOWHERE)
        location = IN_ROOM(target_obj->worn_by);

      if (location == NOWHERE) {
        send_to_char(ch, "That object is currently not in a room.\r\n");
        return (NOWHERE);
      }
    }

    if (location == NOWHERE) {
      send_to_char(ch, "Nothing exists by that name.\r\n");
      return (NOWHERE);
    }
  }

  /* A location has been found -- if you're >= GRGOD, no restrictions. */
  if (GET_LEVEL(ch) >= LVL_GRGOD)
    return (location);

  if (ROOM_FLAGGED(location, ROOM_GODROOM))
    send_to_char(ch, "You are not godly enough to use that room!\r\n");
  else if (ROOM_FLAGGED(location, ROOM_PRIVATE) && world[location].people && world[location].people->next_in_room)
    send_to_char(ch, "There's a private conversation going on in that room.\r\n");
  else if (ROOM_FLAGGED(location, ROOM_HOUSE) && !House_can_enter(ch, GET_ROOM_VNUM(location)))
    send_to_char(ch, "That's private property -- no trespassing!\r\n");
  else
    return (location);

  return (NOWHERE);
}

/** at <room> <command>: run command with ch temporarily in another room.
 * The command may move ch (then ch stays where it went), get ch extracted
 * (ch is left in NOWHERE pending extraction and not moved back), or add
 * or delete rooms (both rooms are remembered by vnum and re-resolved, see
 * G1-22; a vanished original room is reported as at-return). */
ACMD(do_at)
{
  char command[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH];
  room_rnum location, original_loc;
  room_vnum original_vnum, target_vnum;

  half_chop(argument, buf, command);
  if (!*buf) {
    send_to_char(ch, "You must supply a room number or a name.\r\n");
    return;
  }

  if (!*command) {
    send_to_char(ch, "What do you want to do there?\r\n");
    return;
  }

  if ((location = find_target_room(ch, buf)) == NOWHERE)
    return;

  /* a location has been found. Remember both rooms by vnum: the command
   * runs with ch moved and may add or delete rooms (dig, redit, rdelete),
   * which shifts every later rnum. */
  original_loc = IN_ROOM(ch);
  original_vnum = GET_ROOM_VNUM(original_loc);
  target_vnum = GET_ROOM_VNUM(location);
  char_from_room(ch);
  char_to_room(ch, location);
  command_interpreter(ch, command);

  /* check if the char is still there (it may have moved, or be pending
   * extraction in NOWHERE) */
  location = real_room(target_vnum);
  if (IN_ROOM(ch) == NOWHERE || IN_ROOM(ch) != location)
    return;
  if ((original_loc = real_room(original_vnum)) == NOWHERE) {
    TB_WARN_MSG("act", "at-return", "room",
                DIAG_CHAR_FMT " original room vnum %d vanished during 'at'",
                DIAG_CHAR(ch), original_vnum);
    send_to_char(ch, "Your original room no longer exists; you stay here.\r\n");
    return;
  }
  char_from_room(ch);
  char_to_room(ch, original_loc);
}

ACMD(do_goto)
{
  char buf[MAX_STRING_LENGTH];
  room_rnum location;

  if ((location = find_target_room(ch, argument)) == NOWHERE)
    return;

  if (ZONE_FLAGGED(GET_ROOM_ZONE(location), ZONE_NOIMMORT) && (GET_LEVEL(ch) >= LVL_IMMORT) && (GET_LEVEL(ch) < LVL_GRGOD)) {
    send_to_char(ch, "Sorry, that zone is off-limits for immortals!");
    return;
  }

  tb_snprintf(buf, sizeof(buf), "$n %s", POOFOUT(ch) ? POOFOUT(ch) : "disappears in a puff of smoke.");
  act(buf, TRUE, ch, 0, 0, TO_ROOM);

  char_from_room(ch);
  char_to_room(ch, location);

  tb_snprintf(buf, sizeof(buf), "$n %s", POOFIN(ch) ? POOFIN(ch) : "appears with an ear-splitting bang.");
  act(buf, TRUE, ch, 0, 0, TO_ROOM);

  look_at_room(ch, 0);
  enter_wtrigger(&world[IN_ROOM(ch)], ch, -1);
}

ACMD(do_trans)
{
  char buf[MAX_INPUT_LENGTH];
  struct descriptor_data *i;
  struct char_data *victim;

  one_argument(argument, buf);
  if (!*buf)
    send_to_char(ch, "Whom do you wish to transfer?\r\n");
  else if (str_cmp("all", buf)) {
    if (!(victim = get_char_vis(ch, buf, NULL, FIND_CHAR_WORLD)))
      send_to_char(ch, "%s", CONFIG_NOPERSON);
    else if (victim == ch)
      send_to_char(ch, "That doesn't make much sense, does it?\r\n");
    else {
      if ((GET_LEVEL(ch) < GET_LEVEL(victim)) && !IS_NPC(victim)) {
	send_to_char(ch, "Go transfer someone your own size.\r\n");
	return;
      }
      act("$n disappears in a mushroom cloud.", FALSE, victim, 0, 0, TO_ROOM);
      char_from_room(victim);
      char_to_room(victim, IN_ROOM(ch));
      act("$n arrives from a puff of smoke.", FALSE, victim, 0, 0, TO_ROOM);
      act("$n has transferred you!", FALSE, ch, 0, victim, TO_VICT);
      look_at_room(victim, 0);

      enter_wtrigger(&world[IN_ROOM(victim)], victim, -1);
    }
  } else {			/* Trans All */
    if (GET_LEVEL(ch) < LVL_GRGOD) {
      send_to_char(ch, "I think not.\r\n");
      return;
    }

    for (i = descriptor_list; i; i = i->next)
      if (STATE(i) == CON_PLAYING && i->character && i->character != ch) {
	victim = i->character;
	if (GET_LEVEL(victim) >= GET_LEVEL(ch))
	  continue;
	act("$n disappears in a mushroom cloud.", FALSE, victim, 0, 0, TO_ROOM);
	char_from_room(victim);
	char_to_room(victim, IN_ROOM(ch));
	act("$n arrives from a puff of smoke.", FALSE, victim, 0, 0, TO_ROOM);
	act("$n has transferred you!", FALSE, ch, 0, victim, TO_VICT);
	look_at_room(victim, 0);
        enter_wtrigger(&world[IN_ROOM(victim)], victim, -1);
      }
    send_to_char(ch, "%s", CONFIG_OK);
  }
}

ACMD(do_teleport)
{
  char buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  struct char_data *victim;
  room_rnum target;

  two_arguments(argument, buf, buf2);

  if (!*buf)
    send_to_char(ch, "Whom do you wish to teleport?\r\n");
  else if (!(victim = get_char_vis(ch, buf, NULL, FIND_CHAR_WORLD)))
    send_to_char(ch, "%s", CONFIG_NOPERSON);
  else if (victim == ch)
    send_to_char(ch, "Use 'goto' to teleport yourself.\r\n");
  else if (GET_LEVEL(victim) >= GET_LEVEL(ch))
    send_to_char(ch, "Maybe you shouldn't do that.\r\n");
  else if (!*buf2)
    send_to_char(ch, "Where do you wish to send this person?\r\n");
  else if ((target = find_target_room(ch, buf2)) != NOWHERE) {
    send_to_char(ch, "%s", CONFIG_OK);
    act("$n disappears in a puff of smoke.", FALSE, victim, 0, 0, TO_ROOM);
    char_from_room(victim);
    char_to_room(victim, target);
    act("$n arrives from a puff of smoke.", FALSE, victim, 0, 0, TO_ROOM);
    act("$n has teleported you!", FALSE, ch, 0, (char *) victim, TO_VICT);
    look_at_room(victim, 0);
    enter_wtrigger(&world[IN_ROOM(victim)], victim, -1);
  }
}

ACMD(do_vnum)
{
  char buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  int good_arg = 0;

  half_chop(argument, buf, buf2);

  if (!*buf || !*buf2) {
    send_to_char(ch, "Usage: vnum { obj | mob | room | trig } <name>\r\n");
    return;
  }
  if (is_abbrev(buf, "mob") && (good_arg = 1))
    if (!vnum_mobile(buf2, ch))
      send_to_char(ch, "No mobiles by that name.\r\n");

  if (is_abbrev(buf, "obj") && (good_arg =1 ))
    if (!vnum_object(buf2, ch))
      send_to_char(ch, "No objects by that name.\r\n");

  if (is_abbrev(buf, "room") && (good_arg = 1))
    if (!vnum_room(buf2, ch))
      send_to_char(ch, "No rooms by that name.\r\n");

  if (is_abbrev(buf, "trig") && (good_arg = 1))
    if (!vnum_trig(buf2, ch))
      send_to_char(ch, "No triggers by that name.\r\n");

  if (!good_arg)
     send_to_char(ch, "Usage: vnum { obj | mob | room | trig } <name>\r\n");
 }

#define ZOCMD zone_table[zrnum].cmd[subcmd]

/** List the zone reset commands that act on room rvnum ('stat room').
 * Like mob_checkload/obj_checkload/trg_checkload below, this trusts that
 * renum_zone_table() and reset_zone() disabled ('*') every command whose
 * rnums are invalid. Known exceptions, still checked here: an 'E' with an
 * invalid position (reset_zone logs it but keeps the command) and a 'D'
 * direction; both go through TABLE_NAME. cmd_room carries over from the
 * preceding M/O command, because G/E/P act on what that command loaded. */
static void list_zone_commands_room(struct char_data *ch, room_vnum rvnum)
{
  zone_rnum zrnum = real_zone_by_thing(rvnum);
  room_rnum rrnum = real_room(rvnum), cmd_room = NOWHERE;
  int subcmd = 0, count = 0;
  char cmd_id[64];   /* "zone vnum N cmd" for table diagnostics */

  if (zrnum == NOWHERE || rrnum == NOWHERE) {
    send_to_char(ch, "No zone information available.\r\n");
    return;
  }
  tb_snprintf(cmd_id, sizeof(cmd_id), "zone vnum %d cmd",
              zone_table[zrnum].number);

  get_char_colors(ch);

  send_to_char(ch, "Zone commands in this room:%s\r\n", yel);
  while (ZOCMD.command != 'S') {
    switch (ZOCMD.command) {
      case 'M':
      case 'O':
      case 'T':
      case 'V':
        cmd_room = ZOCMD.arg3;
        break;
      case 'D':
      case 'R':
        cmd_room = ZOCMD.arg1;
        break;
      default:
        break;
    }
    if (cmd_room == rrnum) {
      count++;
      /* start listing */
      switch (ZOCMD.command) {
        case 'M':
          send_to_char(ch, "%sLoad %s [%s%d%s], Max : %d\r\n",
                  ZOCMD.if_flag ? " then " : "",
                  mob_proto[ZOCMD.arg1].player.short_descr, cyn,
                  mob_index[ZOCMD.arg1].vnum, yel, ZOCMD.arg2
                  );
          break;
        case 'G':
          send_to_char(ch, "%sGive it %s [%s%d%s], Max : %d\r\n",
    	      ZOCMD.if_flag ? " then " : "",
    	      obj_proto[ZOCMD.arg1].short_description,
    	      cyn, obj_index[ZOCMD.arg1].vnum, yel,
    	      ZOCMD.arg2
    	      );
          break;
        case 'O':
          send_to_char(ch, "%sLoad %s [%s%d%s], Max : %d\r\n",
    	      ZOCMD.if_flag ? " then " : "",
    	      obj_proto[ZOCMD.arg1].short_description,
    	      cyn, obj_index[ZOCMD.arg1].vnum, yel,
    	      ZOCMD.arg2
    	      );
          break;
        case 'E':
          send_to_char(ch, "%sEquip with %s [%s%d%s], %s, Max : %d\r\n",
    	      ZOCMD.if_flag ? " then " : "",
    	      obj_proto[ZOCMD.arg1].short_description,
    	      cyn, obj_index[ZOCMD.arg1].vnum, yel,
    	      /* reset_zone logs a bad 'E' position but keeps the command,
    	       * so arg3 (from the zone file or zedit) is unchecked here. */
    	      TABLE_NAME(equipment_types, NUM_WEARS, ZOCMD.arg3,
    	                 "zone-cmd-arg", cmd_id, subcmd),
    	      ZOCMD.arg2
    	      );
          break;
        case 'P':
          send_to_char(ch, "%sPut %s [%s%d%s] in %s [%s%d%s], Max : %d\r\n",
    	      ZOCMD.if_flag ? " then " : "",
    	      obj_proto[ZOCMD.arg1].short_description,
    	      cyn, obj_index[ZOCMD.arg1].vnum, yel,
    	      obj_proto[ZOCMD.arg3].short_description,
    	      cyn, obj_index[ZOCMD.arg3].vnum, yel,
    	      ZOCMD.arg2
    	      );
          break;
        case 'R':
          send_to_char(ch, "%sRemove %s [%s%d%s] from room.\r\n",
    	      ZOCMD.if_flag ? " then " : "",
    	      obj_proto[ZOCMD.arg2].short_description,
    	      cyn, obj_index[ZOCMD.arg2].vnum, yel
    	      );
          break;
        case 'D':
          send_to_char(ch, "%sSet door %s as %s.\r\n",
    	      ZOCMD.if_flag ? " then " : "",
    	      TABLE_NAME(dirs, NUM_OF_DIRS, ZOCMD.arg2, "zone-cmd-arg",
    	                 cmd_id, subcmd),
    	      ZOCMD.arg3 ? ((ZOCMD.arg3 == 1) ? "closed" : "locked") : "open"
    	      );
          break;
        case 'T':
          send_to_char(ch, "%sAttach trigger %s%s%s [%s%d%s] to %s\r\n",
            ZOCMD.if_flag ? " then " : "",
            cyn, trig_index[ZOCMD.arg2]->proto->name, yel,
            cyn, trig_index[ZOCMD.arg2]->vnum, yel,
            ((ZOCMD.arg1 == MOB_TRIGGER) ? "mobile" :
              ((ZOCMD.arg1 == OBJ_TRIGGER) ? "object" :
                ((ZOCMD.arg1 == WLD_TRIGGER)? "room" : "????"))));
          break;
        case 'V':
          send_to_char(ch, "%sAssign global %s:%d to %s = %s\r\n",
            ZOCMD.if_flag ? " then " : "",
            ZOCMD.sarg1, ZOCMD.arg2,
            ((ZOCMD.arg1 == MOB_TRIGGER) ? "mobile" :
              ((ZOCMD.arg1 == OBJ_TRIGGER) ? "object" :
                ((ZOCMD.arg1 == WLD_TRIGGER)? "room" : "????"))),
            ZOCMD.sarg2);
          break;
        default:
          send_to_char(ch, "<Unknown Command>\r\n");
          break;
      }
    }
    subcmd++;
  }
  send_to_char(ch, "%s", nrm);
  if (!count)
    send_to_char(ch, "None!\r\n");
}
#undef ZOCMD

static void do_stat_room(struct char_data *ch, struct room_data *rm)
{
  char buf2[MAX_STRING_LENGTH];
  struct extra_descr_data *desc;
  int i, found, column;
  struct obj_data *j;
  struct char_data *k;

  send_to_char(ch, "Room name: %s%s%s\r\n", CCCYN(ch, C_NRM), rm->name, CCNRM(ch, C_NRM));

  sprinttype(rm->sector_type, sector_types, buf2, sizeof(buf2));
  send_to_char(ch, "Zone: [%3d], VNum: [%s%5d%s], RNum: [%5d], IDNum: [%5ld], Type: %s\r\n",
	  zone_table[rm->zone].number, CCGRN(ch, C_NRM), rm->number,
	  CCNRM(ch, C_NRM), real_room(rm->number), room_script_id(rm), buf2);

  sprintbitarray(rm->room_flags, room_bits, RF_ARRAY_MAX, buf2, sizeof(buf2));
  send_to_char(ch, "SpecProc: %s, Flags: %s\r\n", rm->func == NULL ? "None" : get_spec_func_name(rm->func), buf2);

  send_to_char(ch, "Description:\r\n%s", rm->description ? rm->description : "  None.\r\n");

  if (rm->ex_description) {
    send_to_char(ch, "Extra descs:%s", CCCYN(ch, C_NRM));
    for (desc = rm->ex_description; desc; desc = desc->next)
      send_to_char(ch, " [%s]", desc->keyword);
    send_to_char(ch, "%s\r\n", CCNRM(ch, C_NRM));
  }

  send_to_char(ch, "Chars present:%s", CCYEL(ch, C_NRM));
  column = 14;	/* ^^^ strlen ^^^ */
  for (found = FALSE, k = rm->people; k; k = k->next_in_room) {
    if (!CAN_SEE(ch, k))
      continue;

    column += send_to_char(ch, "%s %s(%s)", found++ ? "," : "", GET_NAME(k),
		!IS_NPC(k) ? "PC" : (!IS_MOB(k) ? "NPC" : "MOB"));
    if (column >= 62) {
      send_to_char(ch, "%s\r\n", k->next_in_room ? "," : "");
      found = FALSE;
      column = 0;
    }
  }
  send_to_char(ch, "%s", CCNRM(ch, C_NRM));

  if (rm->contents) {
    send_to_char(ch, "Contents:%s", CCGRN(ch, C_NRM));
    column = 9;	/* ^^^ strlen ^^^ */

    for (found = 0, j = rm->contents; j; j = j->next_content) {
      if (!CAN_SEE_OBJ(ch, j))
	continue;

      column += send_to_char(ch, "%s %s", found++ ? "," : "", j->short_description);
      if (column >= 62) {
	send_to_char(ch, "%s\r\n", j->next_content ? "," : "");
	found = FALSE;
        column = 0;
      }
    }
    send_to_char(ch, "%s", CCNRM(ch, C_NRM));
  }

  for (i = 0; i < DIR_COUNT; i++) {
    char buf1[128];

    if (!rm->dir_option[i])
      continue;

    if (rm->dir_option[i]->to_room == NOWHERE)
      tb_snprintf(buf1, sizeof(buf1), " %sNONE%s", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));
    else
      tb_snprintf(buf1, sizeof(buf1), "%s%5d%s", CCCYN(ch, C_NRM), GET_ROOM_VNUM(rm->dir_option[i]->to_room), CCNRM(ch, C_NRM));

    sprintbit(rm->dir_option[i]->exit_info, exit_bits, buf2, sizeof(buf2));

    send_to_char(ch, "Exit %s%-5s%s:  To: [%s], Key: [%5d], Keywords: %s, Type: %s\r\n%s",
	CCCYN(ch, C_NRM), dirs[i], CCNRM(ch, C_NRM), buf1,
	rm->dir_option[i]->key == NOTHING ? -1 : rm->dir_option[i]->key,
	rm->dir_option[i]->keyword ? rm->dir_option[i]->keyword : "None", buf2,
	rm->dir_option[i]->general_description ? rm->dir_option[i]->general_description : "  No exit description.\r\n");
  }

  /* check the room for a script */
  do_sstat_room(ch, rm);

  list_zone_commands_room(ch, rm->number);
}

static void do_stat_object(struct char_data *ch, struct obj_data *j)
{
  int i, found;
  obj_vnum vnum;
  struct obj_data *j2;
  struct extra_descr_data *desc;
  char buf[MAX_STRING_LENGTH];
  struct char_data *tempch;

  send_to_char(ch, "Name: '%s%s%s', Keywords: %s\r\n", CCYEL(ch, C_NRM),
	  j->short_description ? j->short_description : "<None>",
	  CCNRM(ch, C_NRM), j->name);

  vnum = GET_OBJ_VNUM(j);
  sprinttype(GET_OBJ_TYPE(j), item_types, buf, sizeof(buf));
  send_to_char(ch, "VNum: [%s%5d%s], RNum: [%5d], Idnum: [%5ld], Type: %s, SpecProc: %s\r\n",
    CCGRN(ch, C_NRM), vnum, CCNRM(ch, C_NRM), GET_OBJ_RNUM(j), obj_script_id(j), buf,
    GET_OBJ_SPEC(j) ? (get_spec_func_name(GET_OBJ_SPEC(j))) : "None");

  send_to_char(ch, "L-Desc: '%s%s%s'\r\n", CCYEL(ch, C_NRM),
	  j->description ? j->description : "<None>",
	  CCNRM(ch, C_NRM));

  send_to_char(ch, "A-Desc: '%s%s%s'\r\n", CCYEL(ch, C_NRM),
	  j->action_description ? j->action_description : "<None>",
	  CCNRM(ch, C_NRM));

  if (j->ex_description) {
    send_to_char(ch, "Extra descs:%s", CCCYN(ch, C_NRM));
    for (desc = j->ex_description; desc; desc = desc->next)
      send_to_char(ch, " [%s]", desc->keyword);
    send_to_char(ch, "%s\r\n", CCNRM(ch, C_NRM));
  }

  sprintbitarray(GET_OBJ_WEAR(j), wear_bits, TW_ARRAY_MAX, buf, sizeof(buf));
  send_to_char(ch, "Can be worn on: %s\r\n", buf);

  sprintbitarray(GET_OBJ_AFFECT(j), affected_bits, AF_ARRAY_MAX, buf, sizeof(buf));
  send_to_char(ch, "Set char bits : %s\r\n", buf);

  sprintbitarray(GET_OBJ_EXTRA(j), extra_bits, EF_ARRAY_MAX, buf, sizeof(buf));
  send_to_char(ch, "Extra flags   : %s\r\n", buf);

  send_to_char(ch, "Weight: %d, Value: %d, Cost/day: %d, Timer: %d, Min level: %d\r\n",
     GET_OBJ_WEIGHT(j), GET_OBJ_COST(j), GET_OBJ_RENT(j), GET_OBJ_TIMER(j), GET_OBJ_LEVEL(j));

  send_to_char(ch, "In room: %d (%s), ", GET_ROOM_VNUM(IN_ROOM(j)),
	IN_ROOM(j) == NOWHERE ? "Nowhere" : world[IN_ROOM(j)].name);

  /* In order to make it this far, we must already be able to see the character
   * holding the object. Therefore, we do not need CAN_SEE(). */
  send_to_char(ch, "In object: %s, ", j->in_obj ? j->in_obj->short_description : "None");
  send_to_char(ch, "Carried by: %s, ", j->carried_by ? GET_NAME(j->carried_by) : "Nobody");
  send_to_char(ch, "Worn by: %s\r\n", j->worn_by ? GET_NAME(j->worn_by) : "Nobody");

  switch (GET_OBJ_TYPE(j)) {
  case ITEM_LIGHT:
    if (GET_OBJ_VAL(j, 2) == -1)
      send_to_char(ch, "Hours left: Infinite\r\n");
    else
      send_to_char(ch, "Hours left: [%d]\r\n", GET_OBJ_VAL(j, 2));
    break;
  case ITEM_SCROLL:
  case ITEM_POTION:
    send_to_char(ch, "Spells: (Level %d) %s, %s, %s\r\n", GET_OBJ_VAL(j, 0),
	    skill_name(GET_OBJ_VAL(j, 1)), skill_name(GET_OBJ_VAL(j, 2)),
	    skill_name(GET_OBJ_VAL(j, 3)));
    break;
  case ITEM_WAND:
  case ITEM_STAFF:
    send_to_char(ch, "Spell: %s at level %d, %d (of %d) charges remaining\r\n",
	    skill_name(GET_OBJ_VAL(j, 3)), GET_OBJ_VAL(j, 0),
	    GET_OBJ_VAL(j, 2), GET_OBJ_VAL(j, 1));
    break;
  case ITEM_WEAPON: {
    /* value[3] comes from the world file, oedit or DG osetval; none of them
     * bound it to attack_hit_text[]. */
    int atype = GET_OBJ_VAL(j, 3);
    const char *aname = "<invalid>";

    if (VALID_ATTACK_TYPE(atype))
      aname = attack_hit_text[atype].singular;
    else
      TB_WARN_MSG("act", "stat-attack-type", "object",
                  "obj vnum %d value3 %d outside 0..%d",
                  GET_OBJ_VNUM(j), atype, NUM_ATTACK_TYPES - 1);
    send_to_char(ch, "Todam: %dd%d, Avg Damage: %.1f. Message type: %s\r\n",
	    GET_OBJ_VAL(j, 1), GET_OBJ_VAL(j, 2), ((GET_OBJ_VAL(j, 2) + 1) / 2.0) * GET_OBJ_VAL(j, 1), aname);
    break;
  }
  case ITEM_ARMOR:
    send_to_char(ch, "AC-apply: [%d]\r\n", GET_OBJ_VAL(j, 0));
    break;
  case ITEM_CONTAINER:
    sprintbit(GET_OBJ_VAL(j, 1), container_bits, buf, sizeof(buf));
    send_to_char(ch, "Weight capacity: %d, Lock Type: %s, Key Num: %d, Corpse: %s\r\n",
	    GET_OBJ_VAL(j, 0), buf, GET_OBJ_VAL(j, 2),
	    YESNO(GET_OBJ_VAL(j, 3)));
    break;
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
    sprinttype(GET_OBJ_VAL(j, 2), drinks, buf, sizeof(buf));
    send_to_char(ch, "Capacity: %d, Contains: %d, Poisoned: %s, Liquid: %s\r\n",
	    GET_OBJ_VAL(j, 0), GET_OBJ_VAL(j, 1), YESNO(GET_OBJ_VAL(j, 3)), buf);
    break;
  case ITEM_NOTE:
    send_to_char(ch, "Tongue: %d\r\n", GET_OBJ_VAL(j, 0));
    break;
  case ITEM_KEY: /* Nothing */
    break;
  case ITEM_FOOD:
    send_to_char(ch, "Makes full: %d, Poisoned: %s\r\n", GET_OBJ_VAL(j, 0), YESNO(GET_OBJ_VAL(j, 3)));
    break;
  case ITEM_MONEY:
    send_to_char(ch, "Coins: %d\r\n", GET_OBJ_VAL(j, 0));
    break;
  case ITEM_FURNITURE:
    send_to_char(ch, "Can hold: [%d] Num. of People in: [%d]\r\n", GET_OBJ_VAL(j, 0), GET_OBJ_VAL(j, 1));
    send_to_char(ch, "Holding : ");
    for (tempch = OBJ_SAT_IN_BY(j); tempch; tempch = NEXT_SITTING(tempch))
      send_to_char(ch, "%s ", GET_NAME(tempch));
    send_to_char(ch, "\r\n");
    break;
  default:
    send_to_char(ch, "Values 0-3: [%d] [%d] [%d] [%d]\r\n",
	    GET_OBJ_VAL(j, 0), GET_OBJ_VAL(j, 1),
	    GET_OBJ_VAL(j, 2), GET_OBJ_VAL(j, 3));
    break;
  }

  if (j->contains) {
    int column;

    send_to_char(ch, "\r\nContents:%s", CCGRN(ch, C_NRM));
    column = 9;	/* ^^^ strlen ^^^ */

    for (found = 0, j2 = j->contains; j2; j2 = j2->next_content) {
      column += send_to_char(ch, "%s %s", found++ ? "," : "", j2->short_description);
      if (column >= 62) {
	send_to_char(ch, "%s\r\n", j2->next_content ? "," : "");
	found = FALSE;
        column = 0;
      }
    }
    send_to_char(ch, "%s", CCNRM(ch, C_NRM));
  }

  found = FALSE;
  send_to_char(ch, "Affections:");
  for (i = 0; i < MAX_OBJ_AFFECT; i++)
    if (j->affected[i].modifier) {
      sprinttype(j->affected[i].location, apply_types, buf, sizeof(buf));
      send_to_char(ch, "%s %+d to %s", found++ ? "," : "", j->affected[i].modifier, buf);
    }
  if (!found)
    send_to_char(ch, " None");

  send_to_char(ch, "\r\n");

  /* check the object for a script */
  do_sstat_object(ch, j);
}

static void do_stat_character(struct char_data *ch, struct char_data *k)
{
  char buf[MAX_STRING_LENGTH];
  int i, i2, column, found = FALSE;
  struct obj_data *j;
  struct follow_type *fol;
  struct affected_type *aff;

  sprinttype(GET_SEX(k), genders, buf, sizeof(buf));
  send_to_char(ch, "%s %s '%s'  IDNum: [%5ld], In room [%5d], Loadroom : [%5d]\r\n",
	  buf, (!IS_NPC(k) ? "PC" : (!IS_MOB(k) ? "NPC" : "MOB")),
	  GET_NAME(k), IS_NPC(k) ? char_script_id(k) : GET_IDNUM(k), GET_ROOM_VNUM(IN_ROOM(k)), IS_NPC(k) ? NOWHERE : GET_LOADROOM(k));

  if (IS_MOB(k)) {
    send_to_char(ch, "Keyword: %s, VNum: [%5d], RNum: [%5d]\r\n", k->player.name, GET_MOB_VNUM(k), GET_MOB_RNUM(k));
    send_to_char(ch, "L-Des: %s", k->player.long_descr ? k->player.long_descr : "<None>\r\n");
  }

  if (!IS_MOB(k))
    send_to_char(ch, "Title: %s\r\n", k->player.title ? k->player.title : "<None>");

  send_to_char(ch, "D-Des: %s", k->player.description ? k->player.description : "<None>\r\n");

  sprinttype(k->player.chclass, pc_class_types, buf, sizeof(buf));
  send_to_char(ch, "%s%s, Lev: [%s%2d%s], XP: [%s%7d%s], Align: [%4d]\r\n",
	IS_NPC(k) ? "Mobile" : "Class: ", IS_NPC(k) ? "" : buf, CCYEL(ch, C_NRM), GET_LEVEL(k), CCNRM(ch, C_NRM),
	CCYEL(ch, C_NRM), GET_EXP(k), CCNRM(ch, C_NRM), GET_ALIGNMENT(k));

  if (!IS_NPC(k)) {
    char buf1[64], buf2[64];

    TIME_STR(buf1, "%a %b %d %Y", localtime(&(k->player.time.birth)));
    TIME_STR(buf2, "%a %b %d %Y", localtime(&(k->player.time.logon)));

    send_to_char(ch, "Created: [%s], Last Logon: [%s]\r\n", buf1, buf2);

    send_to_char(ch, "Played: [%dh %dm], Age: [%d], STL[%d]/per[%d]/NSTL[%d]",
            k->player.time.played / 3600, (k->player.time.played % 3600) / 60,
            age(k)->year, GET_PRACTICES(k), int_app[GET_INT(k)].learn,
	    wis_app[GET_WIS(k)].bonus);
    /* Display OLC zone for immorts. */
    if (GET_LEVEL(k) >= LVL_BUILDER) {
      if (GET_OLC_ZONE(k)==AEDIT_PERMISSION)
        send_to_char(ch, ", OLC[%sAedit%s]", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));
      else if (GET_OLC_ZONE(k)==HEDIT_PERMISSION)
        send_to_char(ch, ", OLC[%sHedit%s]", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));
      else if (GET_OLC_ZONE(k) == ALL_PERMISSION)
        send_to_char(ch, ", OLC[%sAll%s]", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));
      else if (GET_OLC_ZONE(k)==NOWHERE)
        send_to_char(ch, ", OLC[%sOFF%s]", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));
      else
        send_to_char(ch, ", OLC[%s%d%s]", CCCYN(ch, C_NRM), GET_OLC_ZONE(k), CCNRM(ch, C_NRM));
    }
    send_to_char(ch, "\r\n");
  }
  send_to_char(ch, "Str: [%s%d/%d%s]  Int: [%s%d%s]  Wis: [%s%d%s]  "
	  "Dex: [%s%d%s]  Con: [%s%d%s]  Cha: [%s%d%s]\r\n",
	  CCCYN(ch, C_NRM), GET_STR(k), GET_ADD(k), CCNRM(ch, C_NRM),
	  CCCYN(ch, C_NRM), GET_INT(k), CCNRM(ch, C_NRM),
	  CCCYN(ch, C_NRM), GET_WIS(k), CCNRM(ch, C_NRM),
	  CCCYN(ch, C_NRM), GET_DEX(k), CCNRM(ch, C_NRM),
	  CCCYN(ch, C_NRM), GET_CON(k), CCNRM(ch, C_NRM),
	  CCCYN(ch, C_NRM), GET_CHA(k), CCNRM(ch, C_NRM));

  send_to_char(ch, "Hit p.:[%s%d/%d+%d%s]  Mana p.:[%s%d/%d+%d%s]  Move p.:[%s%d/%d+%d%s]\r\n",
	  CCGRN(ch, C_NRM), GET_HIT(k), GET_MAX_HIT(k), hit_gain(k), CCNRM(ch, C_NRM),
	  CCGRN(ch, C_NRM), GET_MANA(k), GET_MAX_MANA(k), mana_gain(k), CCNRM(ch, C_NRM),
	  CCGRN(ch, C_NRM), GET_MOVE(k), GET_MAX_MOVE(k), move_gain(k), CCNRM(ch, C_NRM));

  send_to_char(ch, "Gold: [%9d], Bank: [%9d] (Total: %d), ",
	  GET_GOLD(k), GET_BANK_GOLD(k), GET_GOLD(k) + GET_BANK_GOLD(k));

  if (!IS_NPC(k))
    send_to_char(ch, "Screen %s[%s%d%sx%s%d%s]%s\r\n",
                      CCCYN(ch, C_NRM), CCYEL(ch, C_NRM), GET_SCREEN_WIDTH(k), CCNRM(ch, C_NRM),
                      CCYEL(ch, C_NRM), GET_PAGE_LENGTH(k), CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));

  send_to_char(ch, "AC: [%d%+d/10], Hitroll: [%2d], Damroll: [%2d], Saving throws: [%d/%d/%d/%d/%d]\r\n",
	  GET_AC(k), dex_app[GET_DEX(k)].defensive, k->points.hitroll,
	  k->points.damroll, GET_SAVE(k, 0), GET_SAVE(k, 1), GET_SAVE(k, 2),
	  GET_SAVE(k, 3), GET_SAVE(k, 4));

  sprinttype(GET_POS(k), position_types, buf, sizeof(buf));
  send_to_char(ch, "Pos: %s, Fighting: %s", buf, FIGHTING(k) ? GET_NAME(FIGHTING(k)) : "Nobody");

  if (IS_NPC(k)) {
    /* attack_type comes from the mob file ('BareHandAttack') or medit. */
    int atype = k->mob_specials.attack_type;

    if (VALID_ATTACK_TYPE(atype))
      send_to_char(ch, ", Attack type: %s", attack_hit_text[atype].singular);
    else {
      TB_WARN_MSG("act", "stat-attack-type", "mob",
                  "mob vnum %d attack type %d outside 0..%d",
                  GET_MOB_VNUM(k), atype, NUM_ATTACK_TYPES - 1);
      send_to_char(ch, ", Attack type: <invalid %d>", atype);
    }
  }

  if (k->desc) {
    sprinttype(STATE(k->desc), connected_types, buf, sizeof(buf));
    send_to_char(ch, ", Connected: %s", buf);
  }

  if (IS_NPC(k)) {
    sprinttype(k->mob_specials.default_pos, position_types, buf, sizeof(buf));
    send_to_char(ch, ", Default position: %s\r\n", buf);
    sprintbitarray(MOB_FLAGS(k), action_bits, PM_ARRAY_MAX, buf, sizeof(buf));
    send_to_char(ch, "NPC flags: %s%s%s\r\n", CCCYN(ch, C_NRM), buf, CCNRM(ch, C_NRM));
  } else {
    send_to_char(ch, ", Idle Timer (in tics) [%d]\r\n", k->char_specials.timer);

    sprintbitarray(PLR_FLAGS(k), player_bits, PM_ARRAY_MAX, buf, sizeof(buf));
    send_to_char(ch, "PLR: %s%s%s\r\n", CCCYN(ch, C_NRM), buf, CCNRM(ch, C_NRM));

    sprintbitarray(PRF_FLAGS(k), preference_bits, PR_ARRAY_MAX, buf, sizeof(buf));
    send_to_char(ch, "PRF: %s%s%s\r\n", CCGRN(ch, C_NRM), buf, CCNRM(ch, C_NRM));

    send_to_char(ch, "Quest Points: [%9d] Quests Completed: [%5d]\r\n",
       GET_QUESTPOINTS(k), GET_NUM_QUESTS(k));
    if (GET_QUEST(k) != NOTHING)
      send_to_char(ch, "Current Quest: [%5d] Time Left: [%5d]\r\n",
      GET_QUEST(k), GET_QUEST_TIME(k));
  }

  if (IS_MOB(k))
    send_to_char(ch, "Mob Spec-Proc: %s, NPC Bare Hand Dam: %dd%d\r\n",
        (mob_index[GET_MOB_RNUM(k)].func ? get_spec_func_name(mob_index[GET_MOB_RNUM(k)].func) : "None"),
	    k->mob_specials.damnodice, k->mob_specials.damsizedice);

  for (i = 0, j = k->carrying; j; j = j->next_content, i++);
  send_to_char(ch, "Carried: weight: %d, items: %d; Items in: inventory: %d, ", IS_CARRYING_W(k), IS_CARRYING_N(k), i);

  for (i = 0, i2 = 0; i < NUM_WEARS; i++)
    if (GET_EQ(k, i))
      i2++;
  send_to_char(ch, "eq: %d\r\n", i2);

  if (!IS_NPC(k))
    send_to_char(ch, "Hunger: %d, Thirst: %d, Drunk: %d\r\n", GET_COND(k, HUNGER), GET_COND(k, THIRST), GET_COND(k, DRUNK));

  column = send_to_char(ch, "Master is: %s, Followers are:", k->master ? GET_NAME(k->master) : "<none>");
  if (!k->followers)
    send_to_char(ch, " <none>\r\n");
  else {
    for (fol = k->followers; fol; fol = fol->next) {
      column += send_to_char(ch, "%s %s", found++ ? "," : "", PERS(fol->follower, ch));
      if (column >= 62) {
        send_to_char(ch, "%s\r\n", fol->next ? "," : "");
        found = FALSE;
        column = 0;
      }
    }
    if (column != 0)
      send_to_char(ch, "\r\n");
  }

  /* Showing the bitvector */
  sprintbitarray(AFF_FLAGS(k), affected_bits, AF_ARRAY_MAX, buf, sizeof(buf));
  send_to_char(ch, "AFF: %s%s%s\r\n", CCYEL(ch, C_NRM), buf, CCNRM(ch, C_NRM));

  /* Routine to show what spells a char is affected by */
  if (k->affected) {
    for (aff = k->affected; aff; aff = aff->next) {
      send_to_char(ch, "SPL: (%3dhr) %s%-21s%s ", aff->duration + 1, CCCYN(ch, C_NRM), skill_name(aff->spell), CCNRM(ch, C_NRM));

      if (aff->modifier)
	send_to_char(ch, "%+d to %s", aff->modifier,
	    /* location comes from the pfile 'Affs' line, unchecked at load. */
	    TABLE_NAME(apply_types, NUM_APPLIES, aff->location, "stat-apply",
	               DIAG_CHAR(k)));

      if (aff->bitvector[0] || aff->bitvector[1] || aff->bitvector[2] || aff->bitvector[3]) {
        if (aff->modifier)
          send_to_char(ch, ", ");
        for (i=1; i<NUM_AFF_FLAGS; i++) {
          if (IS_SET_AR(aff->bitvector, i)) {
            send_to_char(ch, "sets %s, ", affected_bits[i]);
          }
        }
      }
      send_to_char(ch, "\r\n");
    }
  }

  if (!IS_NPC(k) && (GET_LEVEL(k) >= LVL_IMMORT)) {
    if (POOFIN(k))
      send_to_char(ch, "%sPOOFIN:  %s%s %s%s\r\n", QYEL, QCYN, GET_NAME(k), POOFIN(k), QNRM);
    else
      send_to_char(ch, "%sPOOFIN:  %s%s appears with an ear-splitting bang.%s\r\n", QYEL, QCYN, GET_NAME(k), QNRM);

    if (POOFOUT(k))
      send_to_char(ch, "%sPOOFOUT: %s%s %s%s\r\n", QYEL, QCYN, GET_NAME(k), POOFOUT(k), QNRM);
    else
      send_to_char(ch, "%sPOOFOUT: %s%s disappears in a puff of smoke.%s\r\n", QYEL, QCYN, GET_NAME(k), QNRM);
  }

  /* check mobiles for a script */
  do_sstat_character(ch, k);
  if (SCRIPT_MEM(k)) {
    struct script_memory *mem = SCRIPT_MEM(k);
    send_to_char(ch, "Script memory:\r\n  Remember             Command\r\n");
    while (mem) {
      struct char_data *mc = find_char(mem->id);
      if (!mc)
        send_to_char(ch, "  ** Corrupted!\r\n");
      else {
        if (mem->cmd)
          send_to_char(ch, "  %-20.20s%s\r\n",GET_NAME(mc),mem->cmd);
        else
          send_to_char(ch, "  %-20.20s <default>\r\n",GET_NAME(mc));
      }
    mem = mem->next;
    }
  }
  if (!(IS_NPC(k))) {
    /* this is a PC, display their global variables */
    if (k->script && k->script->global_vars) {
      struct trig_var_data *tv;
      char uname[MAX_INPUT_LENGTH];
      char valbuf[MAX_STRING_LENGTH];

      send_to_char(ch, "Global Variables:\r\n");

      /* currently, variable context for players is always 0, so it is not
       * displayed here. in the future, this might change */
      for (tv = k->script->global_vars; tv; tv = tv->next) {
        if (*(tv->value) == UID_CHAR) {
          find_uid_name(tv->value, uname, sizeof(uname));
          send_to_char(ch, "    %10s:  [UID]: %s\r\n", tv->name, uname);
        } else
          send_to_char(ch, "    %10s:  %s\r\n", tv->name,
                       dg_display_value(tv->value, valbuf, sizeof(valbuf)));
      }
    }
  }
}

ACMD(do_stat)
{
  char buf1[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  struct char_data *victim;
  struct obj_data *object;
  struct room_data *room;

  half_chop(argument, buf1, buf2);

  if (!*buf1) {
    send_to_char(ch, "Stats on who or what or where?\r\n");
    return;
  } else if (is_abbrev(buf1, "room")) {
    if (!*buf2)
      room = &world[IN_ROOM(ch)];
    else {
      room_vnum rvnum;
      room_rnum rnum;

      if (!parse_vnum_arg(ch, buf2, "room", &rvnum))
        return;
      rnum = real_room(rvnum);
      if (rnum == NOWHERE) {
        send_to_char(ch, "That is not a valid room.\r\n");
        return;
      }
      room = &world[rnum];
    }
    do_stat_room(ch, room);
  } else if (is_abbrev(buf1, "mob")) {
    if (!*buf2)
      send_to_char(ch, "Stats on which mobile?\r\n");
    else {
      if ((victim = get_char_vis(ch, buf2, NULL, FIND_CHAR_WORLD)) != NULL)
	do_stat_character(ch, victim);
      else
	send_to_char(ch, "No such mobile around.\r\n");
    }
  } else if (is_abbrev(buf1, "player")) {
    if (!*buf2) {
      send_to_char(ch, "Stats on which player?\r\n");
    } else {
      if ((victim = get_player_vis(ch, buf2, NULL, FIND_CHAR_WORLD)) != NULL)
	do_stat_character(ch, victim);
      else
	send_to_char(ch, "No such player around.\r\n");
    }
  } else if (is_abbrev(buf1, "file")) {
    if (!*buf2)
      send_to_char(ch, "Stats on which player?\r\n");
    else if ((victim = get_player_vis(ch, buf2, NULL, FIND_CHAR_WORLD)) != NULL)
	do_stat_character(ch, victim);
    else {
      /* The file copy is read-only and never enters the world: it is not
       * placed in a room and is released with free_char(). Releasing it with
       * extract_char_final() would save_char() the stale copy over the pfile
       * and Crash_delete_crashfile() the player's RENT_CRASH objects. The
       * target name comes from the immortal; the level check runs before
       * anything is shown. */
      CREATE(victim, struct char_data, 1);
      clear_char(victim);
      CREATE(victim->player_specials, struct player_special_data, 1);
      new_mobile_data(victim);
      if (load_char(buf2, victim) >= 0) {
        if (GET_LEVEL(victim) > GET_LEVEL(ch)) {
          TB_WARN_MSG("act", "stat-file-denied", "player",
                      "caller " DIAG_CHAR_FMT " level %d < target idnum %ld level %d",
                      DIAG_CHAR(ch), GET_LEVEL(ch), GET_IDNUM(victim), GET_LEVEL(victim));
	  send_to_char(ch, "Sorry, you can't do that.\r\n");
        } else if (find_player_in_world(GET_IDNUM(victim))) {
          /* Online but not visible to the caller (e.g. wizinvis): the disk
           * copy is stale and must not be presented as current. */
          TB_WARN_MSG("act", "stat-file-online", "player",
                      "player idnum %ld is in game; caller " DIAG_CHAR_FMT,
                      GET_IDNUM(victim), DIAG_CHAR(ch));
          send_to_char(ch, "That player is in the game; use 'stat player'.\r\n");
        } else
	  do_stat_character(ch, victim);
	free_char(victim);
      } else {
	send_to_char(ch, "There is no such player.\r\n");
	free_char(victim);
      }
    }
  } else if (is_abbrev(buf1, "object")) {
    if (!*buf2)
      send_to_char(ch, "Stats on which object?\r\n");
    else {
      if ((object = get_obj_vis(ch, buf2, NULL)) != NULL)
	do_stat_object(ch, object);
      else
	send_to_char(ch, "No such object around.\r\n");
    }
  } else if (is_abbrev(buf1, "zone")) {
    if (!*buf2) {
      print_zone(ch, zone_table[world[IN_ROOM(ch)].zone].number);
      return;
    } else {
      print_zone(ch, tb_parse_int_or_zero(buf2));
      return;
    }
  } else {
    char *name = buf1;
    int number = get_number(&name);

    if ((object = get_obj_in_equip_vis(ch, name, &number, ch->equipment)) != NULL)
      do_stat_object(ch, object);
    else if ((object = get_obj_in_list_vis(ch, name, &number, ch->carrying)) != NULL)
      do_stat_object(ch, object);
    else if ((victim = get_char_vis(ch, name, &number, FIND_CHAR_ROOM)) != NULL)
      do_stat_character(ch, victim);
    else if ((object = get_obj_in_list_vis(ch, name, &number, world[IN_ROOM(ch)].contents)) != NULL)
      do_stat_object(ch, object);
    else if ((victim = get_char_vis(ch, name, &number, FIND_CHAR_WORLD)) != NULL)
      do_stat_character(ch, victim);
    else if ((object = get_obj_vis(ch, name, &number)) != NULL)
      do_stat_object(ch, object);
    else
      send_to_char(ch, "Nothing around by that name.\r\n");
  }
}

ACMD(do_shutdown)
{
  char arg[MAX_INPUT_LENGTH];

  if (subcmd != SCMD_SHUTDOWN) {
    send_to_char(ch, "If you want to shut something down, say so!\r\n");
    return;
  }
  one_argument(argument, arg);

  if (!*arg) {
    log("(GC) Shutdown by %s.", GET_NAME(ch));
    send_to_all("Shutting down.\r\n");
    circle_shutdown = 1;
  } else if (!str_cmp(arg, "reboot")) {
    log("(GC) Reboot by %s.", GET_NAME(ch));
    send_to_all("Rebooting.. come back in a few minutes.\r\n");
    touch(FASTBOOT_FILE);
    circle_shutdown = 1;
    circle_reboot = 2; /* do not autosave olc */
  } else if (!str_cmp(arg, "die")) {
    log("(GC) Shutdown by %s.", GET_NAME(ch));
    send_to_all("Shutting down for maintenance.\r\n");
    touch(KILLSCRIPT_FILE);
    circle_shutdown = 1;
  } else if (!str_cmp(arg, "now")) {
    log("(GC) Shutdown NOW by %s.", GET_NAME(ch));
    send_to_all("Rebooting.. come back in a minute or two.\r\n");
    circle_shutdown = 1;
    circle_reboot = 2; /* do not autosave olc */
  } else if (!str_cmp(arg, "pause")) {
    log("(GC) Shutdown by %s.", GET_NAME(ch));
    send_to_all("Shutting down for maintenance.\r\n");
    touch(PAUSE_FILE);
    circle_shutdown = 1;
  } else
    send_to_char(ch, "Unknown shutdown option.\r\n");
}

void snoop_check(struct char_data *ch)
{
  /*  This short routine is to ensure that characters that happen to be snooping
   *  (or snooped) and get advanced/demoted will not be snooping/snooped someone
   *  of a higher/lower level (and thus, not entitled to be snooping. */
  /* Callers: advance_level() (class.c) on promotion, do_advance and
   * 'set level' on any change. A peer descriptor may have no character
   * (it is mid-login or closing); its snoop link is dropped as well. */
  if (!ch || !ch->desc)
    return;
  if (ch->desc->snooping &&
     (!ch->desc->snooping->character ||
      GET_LEVEL(ch->desc->snooping->character) >= GET_LEVEL(ch))) {
    ch->desc->snooping->snoop_by = NULL;
    ch->desc->snooping = NULL;
    send_to_char(ch, "You can no longer snoop at your level; snoop stopped.\r\n");
  }

  if (ch->desc->snoop_by &&
     (!ch->desc->snoop_by->character ||
      GET_LEVEL(ch) >= GET_LEVEL(ch->desc->snoop_by->character))) {
    if (ch->desc->snoop_by->character)
      send_to_char(ch->desc->snoop_by->character,
                   "%s now outranks you; snoop stopped.\r\n", GET_NAME(ch));
    ch->desc->snoop_by->snooping = NULL;
    ch->desc->snoop_by = NULL;
  }
}

static void stop_snooping(struct char_data *ch)
{
  if (!ch->desc->snooping)
    send_to_char(ch, "You aren't snooping anyone.\r\n");
  else {
    send_to_char(ch, "You stop snooping.\r\n");

      mudlog(BRF, GET_LEVEL(ch), TRUE, "(GC) %s stops snooping", GET_NAME(ch));

    ch->desc->snooping->snoop_by = NULL;
    ch->desc->snooping = NULL;
  }
}

ACMD(do_snoop)
{
  char arg[MAX_INPUT_LENGTH];
  struct char_data *victim, *tch;

  ACMD_REQUIRE_DESC(ch, "snoop");

  one_argument(argument, arg);

  if (!*arg)
    stop_snooping(ch);
  else if (!(victim = get_char_vis(ch, arg, NULL, FIND_CHAR_WORLD)))
    send_to_char(ch, "No such person around.\r\n");
  else if (!victim->desc)
    send_to_char(ch, "There's no link.. nothing to snoop.\r\n");
  else if (victim == ch)
    stop_snooping(ch);
  else if (victim->desc->snoop_by)
    send_to_char(ch, "Busy already. \r\n");
  else if (victim->desc->snooping == ch->desc)
    send_to_char(ch, "Don't be stupid.\r\n");
  else {
    if (victim->desc->original)
      tch = victim->desc->original;
    else
      tch = victim;

    if (GET_LEVEL(tch) >= GET_LEVEL(ch)) {
      send_to_char(ch, "You can't.\r\n");
      return;
    }
    send_to_char(ch, "%s", CONFIG_OK);

      mudlog(BRF, GET_LEVEL(ch), TRUE, "(GC) %s snoops %s", GET_NAME(ch), GET_NAME(victim));

    if (ch->desc->snooping)
      ch->desc->snooping->snoop_by = NULL;

    ch->desc->snooping = victim->desc;
    victim->desc->snoop_by = ch->desc;
  }
}

ACMD(do_switch)
{
  char arg[MAX_INPUT_LENGTH];
  struct char_data *victim;

  /* Invariant: switching moves the caller's descriptor into another body,
   * so the caller must have one. A linkdead immortal can still reach this
   * through "force" or a DG mforce. */
  ACMD_REQUIRE_DESC(ch, "switch");

  one_argument(argument, arg);

  if (ch->desc->original)
    send_to_char(ch, "You're already switched.\r\n");
  else if (!*arg)
    send_to_char(ch, "Switch with who?\r\n");
  else if (!(victim = get_char_vis(ch, arg, NULL, FIND_CHAR_WORLD)))
    send_to_char(ch, "No such character.\r\n");
  else if (ch == victim)
    send_to_char(ch, "Hee hee... we are jolly funny today, eh?\r\n");
  else if (victim->desc)
    send_to_char(ch, "You can't do that, the body is already in use!\r\n");
  else if ((GET_LEVEL(ch) < LVL_IMPL) && !IS_NPC(victim))
    send_to_char(ch, "You are not holy enough to use their body.\r\n");
  else if (GET_LEVEL(ch) < LVL_GRGOD && ROOM_FLAGGED(IN_ROOM(victim), ROOM_GODROOM))
    send_to_char(ch, "You are not godly enough to use that room!\r\n");
  else if (GET_LEVEL(ch) < LVL_GRGOD && ROOM_FLAGGED(IN_ROOM(victim), ROOM_HOUSE)
		&& !House_can_enter(ch, GET_ROOM_VNUM(IN_ROOM(victim))))
    send_to_char(ch, "That's private property -- no trespassing!\r\n");
  else {
    send_to_char(ch, "%s", CONFIG_OK);
    mudlog(CMP, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s Switched into: %s", GET_NAME(ch), GET_NAME(victim));
    ch->desc->character = victim;
    ch->desc->original = ch;

    victim->desc = ch->desc;
    ch->desc = NULL;
  }
}

/** tbaMUD's implementor recovery: "return" while not switched restores
 * player idnum 1 to LVL_IMPL. Whoever holds idnum 1 (e.g. the first
 * character created after the player index is reset) gets IMPL this way,
 * so every restore that changes the level is logged like any other level
 * change ((GC) mudlog plus a cheat-restore warning with the idnum).
 * Build with -DTB_NO_CHEAT_RESTORE to remove the path entirely. */
static void do_cheat(struct char_data *ch)
{
#ifndef TB_NO_CHEAT_RESTORE
  int old_level = GET_LEVEL(ch);

  switch (GET_IDNUM(ch)) {
    case    1: // IMP
      GET_LEVEL(ch) = LVL_IMPL;
      break;
    default:
      send_to_char(ch, "You do not have access to this command.\r\n");
  return;
  }
  if (old_level != LVL_IMPL) {
    mudlog(BRF, LVL_IMPL, TRUE, "(GC) %s (idnum %ld) restored own level %d -> %d via 'return'",
           GET_NAME(ch), GET_IDNUM(ch), old_level, LVL_IMPL);
    TB_WARN_MSG("act", "cheat-restore", "level",
                "player idnum %ld restored from level %d to %d",
                GET_IDNUM(ch), old_level, LVL_IMPL);
  }
  send_to_char(ch, "Your level has been restored, for now!\r\n");
  save_char(ch);
#else
  send_to_char(ch, "You do not have access to this command.\r\n");
#endif
}

void return_to_char(struct char_data * ch)
{
  /* Invariant: ch is a switched body, so ch->desc is the immortal's
   * descriptor and ch->desc->original is the immortal. */
  if (!ch || !ch->desc || !ch->desc->original) {
    TB_ERROR_MSG("act", "return-to-char", "descriptor",
                 "character %s is not a switched body (desc %s, original %s)",
                 ch ? GET_NAME(ch) : "(null)",
                 (ch && ch->desc) ? "set" : "none",
                 (ch && ch->desc && ch->desc->original) ? "set" : "none");
    return;
  }

  /* If someone switched into your original body, disconnect them. - JE
  * Zmey: here we put someone switched in our body to disconnect state but
  * we must also NULL his pointer to our character, otherwise close_socket()
  * will damage our character's pointer to our descriptor (which is assigned
  * below in this function). */
  if (ch->desc->original->desc) {
    ch->desc->original->desc->character = NULL;
    STATE(ch->desc->original->desc) = CON_DISCONNECT;
  }

  /* Now our descriptor points to our original body. */
  ch->desc->character = ch->desc->original;
  ch->desc->original = NULL;

  /* And our body's pointer to descriptor now points to our descriptor. */
  ch->desc->character->desc = ch->desc;
  ch->desc = NULL;  
}

ACMD(do_return)
{
  /* Invariant: return needs the caller's descriptor. "return" is a level 0
   * POS_DEAD command, so "force <linkdead player> return" or a DG mforce on
   * a linkdead player reaches here with ch->desc == NULL. */
  ACMD_REQUIRE_DESC(ch, "return");

  if (!IS_NPC(ch) && !ch->desc->original) {
    int level, newlevel;
    level = GET_LEVEL(ch);
    do_cheat(ch);
    newlevel = GET_LEVEL(ch);
    if (!PLR_FLAGGED(ch, PLR_NOWIZLIST)&& level != newlevel)
      run_autowiz();
  }

  if (ch->desc && ch->desc->original) {
    send_to_char(ch, "You return to your original body.\r\n");
    return_to_char(ch);
  }
}

ACMD(do_load)
{
  char buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH], buf3[MAX_INPUT_LENGTH];
  int i=0, n=1;
  IDXTYPE vnum;

  one_argument(two_arguments(argument, buf, buf2), buf3);

  if (!*buf || !*buf2 || !isdigit(*buf2)) {
    send_to_char(ch, "Usage: load < obj | mob > <vnum> <number>\r\n");
    return;
  }
  if (!is_number(buf2)) {
    send_to_char(ch, "That is not a number.\r\n");
    return;
  }

  if (tb_parse_int_or_zero(buf3) > 0  && tb_parse_int_or_zero(buf3) <= 100) {
    n = tb_parse_int_or_zero(buf3);
  } else {
    n = 1;
  }

  if (is_abbrev(buf, "mob")) {
    struct char_data *mob=NULL;
    mob_rnum r_num;

	if (GET_LEVEL(ch) < LVL_GRGOD && !can_edit_zone(ch, world[IN_ROOM(ch)].zone)) {
	  send_to_char(ch, "Sorry, you can't load mobs here.\r\n");
	  return;
	}

    if (!parse_vnum_arg(ch, buf2, "mob", &vnum))
      return;
    if ((r_num = real_mobile(vnum)) == NOBODY) {
      send_to_char(ch, "There is no monster with that number.\r\n");
      return;
    }
    for (i=0; i < n; i++) {
      mob = read_mobile(r_num, REAL);
      char_to_room(mob, IN_ROOM(ch));

      act("$n makes a quaint, magical gesture with one hand.", TRUE, ch, 0, 0, TO_ROOM);
      act("$n has created $N!", FALSE, ch, 0, mob, TO_ROOM);
      act("You create $N.", FALSE, ch, 0, mob, TO_CHAR);
      load_mtrigger(mob);
    }
  } else if (is_abbrev(buf, "obj")) {
    struct obj_data *obj;
    obj_rnum r_num;

	if (GET_LEVEL(ch) < LVL_GRGOD && !can_edit_zone(ch, world[IN_ROOM(ch)].zone)) {
	  send_to_char(ch, "Sorry, you can't load objects here.\r\n");
	  return;
	}

    if (!parse_vnum_arg(ch, buf2, "object", &vnum))
      return;
    if ((r_num = real_object(vnum)) == NOTHING) {
      send_to_char(ch, "There is no object with that number.\r\n");
      return;
    }
    for (i=0; i < n; i++) {
      obj = read_object(r_num, REAL);
      if (CONFIG_LOAD_INVENTORY)
        obj_to_char(obj, ch);
      else
        obj_to_room(obj, IN_ROOM(ch));
      act("$n makes a strange magical gesture.", TRUE, ch, 0, 0, TO_ROOM);
      act("$n has created $p!", FALSE, ch, obj, 0, TO_ROOM);
      act("You create $p.", FALSE, ch, obj, 0, TO_CHAR);
      load_otrigger(obj);
    }
  } else
    send_to_char(ch, "That'll have to be either 'obj' or 'mob'.\r\n");
}

ACMD(do_vstat)
{
  char buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  struct char_data *mob;
  struct obj_data *obj;
  int r_num;
  IDXTYPE vnum;

  ACMD(do_tstat);

  two_arguments(argument, buf, buf2);

  if (!*buf || !*buf2 || !isdigit(*buf2)) {
    send_to_char(ch, "Usage: vstat { o | m | r | t | s | z } <number>\r\n");
    return;
  }
  if (!is_number(buf2)) {
    send_to_char(ch, "That's not a valid number.\r\n");
    return;
  }

  switch (LOWER(*buf)) {
  case 'm':
    if (!parse_vnum_arg(ch, buf2, "mob", &vnum))
      return;
    if ((r_num = real_mobile(vnum)) == NOBODY) {
      send_to_char(ch, "There is no monster with that number.\r\n");
      return;
    }
    mob = read_mobile(r_num, REAL);
    char_to_room(mob, 0);
    do_stat_character(ch, mob);
    extract_char(mob);
    break;
  case 'o':
    if (!parse_vnum_arg(ch, buf2, "object", &vnum))
      return;
    if ((r_num = real_object(vnum)) == NOTHING) {
      send_to_char(ch, "There is no object with that number.\r\n");
      return;
    }
    obj = read_object(r_num, REAL);
    do_stat_object(ch, obj);
    extract_obj(obj);
    break;
  case 'r':
    if (!parse_vnum_arg(ch, buf2, "room", &vnum))
      return;
    tb_snprintf(buf2, sizeof(buf2), "room %d", vnum);
    do_stat(ch, buf2, 0, 0);
    break;
  case 'z':
    tb_snprintf(buf2, sizeof(buf2), "zone %d", tb_parse_int_or_zero(buf2));
    do_stat(ch, buf2, 0, 0);
    break;
  case 't':
    tb_snprintf(buf2, sizeof(buf2), "%d", tb_parse_int_or_zero(buf2));
    do_tstat(ch, buf2, 0, 0);
    break;
  case 's':
    tb_snprintf(buf2, sizeof(buf2), "shops %d", tb_parse_int_or_zero(buf2));
    do_show(ch, buf2, 0, 0);
    break;
  default:
    send_to_char(ch, "Syntax: vstat { r | m | o | z | t | s } <number>\r\n");
    break;
  }
}

/* clean a room of all mobiles and objects */
ACMD(do_purge)
{
  char buf[MAX_INPUT_LENGTH];
  char *t;
  struct char_data *vict;
  struct obj_data *obj;
  int number;

  one_argument(argument, buf);

  if (GET_LEVEL(ch) < LVL_GRGOD && !can_edit_zone(ch, world[IN_ROOM(ch)].zone)) {
	send_to_char(ch, "Sorry, you can't purge anything here.\r\n");
	return;
  }

  /* argument supplied. destroy single object or char */
  if (*buf) {
    t = buf;
    number = get_number(&t);
    if ((vict = get_char_vis(ch, buf, &number, FIND_CHAR_ROOM)) != NULL) {      
      if (!IS_NPC(vict) && (GET_LEVEL(ch) <= GET_LEVEL(vict))) {
        send_to_char(ch, "You can't purge %s!\r\n", GET_NAME(vict));
	return;
      }
      act("$n disintegrates $N.", FALSE, ch, 0, vict, TO_NOTVICT);

      if (!IS_NPC(vict)) {
	mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s has purged %s.", GET_NAME(ch), GET_NAME(vict));
	if (vict->desc) {
	  STATE(vict->desc) = CON_CLOSE;
	  vict->desc->character = NULL;
	  vict->desc = NULL;
	}
      }
      extract_char(vict);
    } else if ((obj = get_obj_in_list_vis(ch, buf, &number, world[IN_ROOM(ch)].contents)) != NULL) {
      act("$n destroys $p.", FALSE, ch, obj, 0, TO_ROOM);
      extract_obj(obj);
    } else {
      send_to_char(ch, "Nothing here by that name.\r\n");
      return;
    }

    send_to_char(ch, "%s", CONFIG_OK);
  } else {			/* no argument. clean out the room */
    act("$n gestures... You are surrounded by scorching flames!",
	FALSE, ch, 0, 0, TO_ROOM);
    send_to_room(IN_ROOM(ch), "The world seems a little cleaner.\r\n");
    purge_room(IN_ROOM(ch));
  }
}

ACMD(do_advance)
{
  struct char_data *victim;
  char name[MAX_INPUT_LENGTH], level[MAX_INPUT_LENGTH];
  int newlevel, oldlevel, i;

  two_arguments(argument, name, level);

  if (*name) {
    if (!(victim = get_char_vis(ch, name, NULL, FIND_CHAR_WORLD))) {
      send_to_char(ch, "That player is not here.\r\n");
      return;
    }
  } else {
    send_to_char(ch, "Advance who?\r\n");
    return;
  }

  if (GET_LEVEL(ch) <= GET_LEVEL(victim)) {
    send_to_char(ch, "Maybe that's not such a great idea.\r\n");
    return;
  }
  if (IS_NPC(victim)) {
    send_to_char(ch, "NO!  Not on NPC's.\r\n");
    return;
  }
  if (!*level || (newlevel = tb_parse_int_or_zero(level)) <= 0) {
    send_to_char(ch, "That's not a level!\r\n");
    return;
  }
  if (newlevel > LVL_IMPL) {
    send_to_char(ch, "%d is the highest possible level.\r\n", LVL_IMPL);
    return;
  }
  if (newlevel > GET_LEVEL(ch)) {
    send_to_char(ch, "Yeah, right.\r\n");
    return;
  }
  if (newlevel == GET_LEVEL(victim)) {
    act("$E is already at that level.", FALSE, ch, 0, victim, TO_CHAR);
    return;
  }
  oldlevel = GET_LEVEL(victim);
  if (newlevel < GET_LEVEL(victim)) {
    do_start(victim);
    GET_LEVEL(victim) = newlevel;
    /* Promotion reaches snoop_check() through advance_level(); a
     * demotion has to drop snoops the new level no longer allows here. */
    snoop_check(victim);
    send_to_char(victim, "You are momentarily enveloped by darkness!\r\nYou feel somewhat diminished.\r\n");
  } else {
    act("$n makes some strange gestures. A strange feeling comes upon you,\r\n"
      "Like a giant hand, light comes down from above, grabbing your body,\r\n"
      "that begins to pulse with colored lights from inside.\r\n\r\n"
      "Your head seems to be filled with demons from another plane as\r\n"
      "your body dissolves to the elements of time and space itself.\r\n"
      "Suddenly a silent explosion of light snaps you back to reality.\r\n\r\n"
      "You feel slightly different.", FALSE, ch, 0, victim, TO_VICT);
}

  send_to_char(ch, "%s", CONFIG_OK);

  if (newlevel < oldlevel)
    log("(GC) %s demoted %s from level %d to %d.",
		GET_NAME(ch), GET_NAME(victim), oldlevel, newlevel);
  else
    log("(GC) %s has advanced %s to level %d (from %d)",
		GET_NAME(ch), GET_NAME(victim), newlevel, oldlevel);

  if (oldlevel >= LVL_IMMORT && newlevel < LVL_IMMORT) {
    /* If they are no longer an immortal, remove the immortal only flags. */
    REMOVE_BIT_AR(PRF_FLAGS(victim), PRF_LOG1);
    REMOVE_BIT_AR(PRF_FLAGS(victim), PRF_LOG2);
    REMOVE_BIT_AR(PRF_FLAGS(victim), PRF_NOHASSLE);
    REMOVE_BIT_AR(PRF_FLAGS(victim), PRF_HOLYLIGHT);
    REMOVE_BIT_AR(PRF_FLAGS(victim), PRF_SHOWVNUMS);
    if (!PLR_FLAGGED(victim, PLR_NOWIZLIST))
      run_autowiz();
  } else if (oldlevel < LVL_IMMORT && newlevel >= LVL_IMMORT) {
    SET_BIT_AR(PRF_FLAGS(victim), PRF_LOG2);
    SET_BIT_AR(PRF_FLAGS(victim), PRF_HOLYLIGHT);
    SET_BIT_AR(PRF_FLAGS(victim), PRF_SHOWVNUMS);
    SET_BIT_AR(PRF_FLAGS(victim), PRF_AUTOEXIT);
        for (i = 1; i <= MAX_SKILLS; i++)
          SET_SKILL(victim, i, 100);
   GET_OLC_ZONE(victim) = NOWHERE;
   GET_COND(victim, HUNGER) = -1;
   GET_COND(victim, THIRST) = -1;
   GET_COND(victim, DRUNK)  = -1;
  }

  gain_exp_regardless(victim, level_exp(GET_CLASS(victim), newlevel) - GET_EXP(victim));
  save_char(victim);
}

ACMD(do_restore)
{
  char buf[MAX_INPUT_LENGTH];
  struct char_data *vict;
  struct descriptor_data *j;
  int i;

  one_argument(argument, buf);
  if (!*buf)
    send_to_char(ch, "Whom do you wish to restore?\r\n");
   else if (is_abbrev(buf, "all"))
   {
    mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s restored all",GET_NAME(ch));

     for (j = descriptor_list; j; j = j->next)
    {
      if (!IS_PLAYING(j) || !(vict = j->character) || GET_LEVEL(vict) >= LVL_IMMORT)
     continue;

      GET_HIT(vict)  = GET_MAX_HIT(vict);
      GET_MANA(vict) = GET_MAX_MANA(vict);
      GET_MOVE(vict) = GET_MAX_MOVE(vict);

      update_pos(vict);
      send_to_char(ch, "%s has been fully healed.\r\n", GET_NAME(vict));
      act("You have been fully healed by $N!", FALSE, vict, 0, ch, TO_CHAR);
    }
  }
  else if (!(vict = get_char_vis(ch, buf, NULL, FIND_CHAR_WORLD)))
    send_to_char(ch, "%s", CONFIG_NOPERSON);
  else if (!IS_NPC(vict) && ch != vict && GET_LEVEL(vict) >= GET_LEVEL(ch))
    act("$E doesn't need your help.", FALSE, ch, 0, vict, TO_CHAR);
  else {
    mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s restored %s",GET_NAME(ch), GET_NAME(vict));

    GET_HIT(vict) = GET_MAX_HIT(vict);
    GET_MANA(vict) = GET_MAX_MANA(vict);
    GET_MOVE(vict) = GET_MAX_MOVE(vict);

    if (!IS_NPC(vict) && GET_LEVEL(ch) >= LVL_GRGOD) {
      if (GET_LEVEL(vict) >= LVL_IMMORT)
        for (i = 1; i <= MAX_SKILLS; i++)
          SET_SKILL(vict, i, 100);

      if (GET_LEVEL(vict) >= LVL_GRGOD) {
	vict->real_abils.str_add = 100;
	vict->real_abils.intel = 25;
	vict->real_abils.wis = 25;
	vict->real_abils.dex = 25;
	vict->real_abils.str = 25;
	vict->real_abils.con = 25;
	vict->real_abils.cha = 25;
      }
    }
    update_pos(vict);
    affect_total(vict);
    send_to_char(ch, "%s", CONFIG_OK);
    act("You have been fully healed by $N!", FALSE, vict, 0, ch, TO_CHAR);
  }
}

void perform_immort_vis(struct char_data *ch)
{
  if ((GET_INVIS_LEV(ch) == 0) && (!AFF_FLAGGED(ch, AFF_HIDE) && !AFF_FLAGGED(ch, AFF_INVISIBLE))) {
    send_to_char(ch, "You are already fully visible.\r\n");
    return;
  }

  GET_INVIS_LEV(ch) = 0;
  appear(ch);
  send_to_char(ch, "You are now fully visible.\r\n");
}

static void perform_immort_invis(struct char_data *ch, int level)
{
  struct char_data *tch;

  for (tch = world[IN_ROOM(ch)].people; tch; tch = tch->next_in_room) {
    if (tch == ch || IS_NPC(tch))
      continue;
    if (GET_LEVEL(tch) >= GET_INVIS_LEV(ch) && GET_LEVEL(tch) < level)
      act("You blink and suddenly realize that $n is gone.", FALSE, ch, 0,
	  tch, TO_VICT);
    if (GET_LEVEL(tch) < GET_INVIS_LEV(ch) && GET_LEVEL(tch) >= level)
      act("You suddenly realize that $n is standing beside you.", FALSE, ch, 0,
	  tch, TO_VICT);
  }

  GET_INVIS_LEV(ch) = level;
  send_to_char(ch, "Your invisibility level is %d.\r\n", level);
}

ACMD(do_invis)
{
  char arg[MAX_INPUT_LENGTH];
  int level;

  if (IS_NPC(ch)) {
    send_to_char(ch, "You can't do that!\r\n");
    return;
  }

  one_argument(argument, arg);
  if (!*arg) {
    if (GET_INVIS_LEV(ch) > 0)
      perform_immort_vis(ch);
    else
      perform_immort_invis(ch, GET_LEVEL(ch));
  } else {
    level = tb_parse_int_or_zero(arg);
    if (level > GET_LEVEL(ch))
      send_to_char(ch, "You can't go invisible above your own level.\r\n");
    else if (level < 1)
      perform_immort_vis(ch);
    else
      perform_immort_invis(ch, level);
  }
}

ACMD(do_gecho)
{
  struct descriptor_data *pt;

  skip_spaces(&argument);
  delete_doubledollar(argument);

  if (!*argument)
    send_to_char(ch, "That must be a mistake...\r\n");
  else {
    for (pt = descriptor_list; pt; pt = pt->next)
      if (IS_PLAYING(pt) && pt->character && pt->character != ch)
	send_to_char(pt->character, "%s\r\n", argument);

    mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE, "(GC) %s gechoed: %s", GET_NAME(ch), argument);

    if (NOREPEAT(ch))
      send_to_char(ch, "%s", CONFIG_OK);
    else
      send_to_char(ch, "%s\r\n", argument);
  }
}

ACMD(do_dc)
{
  char arg[MAX_INPUT_LENGTH];
  struct descriptor_data *d;
  int num_to_dc;

  one_argument(argument, arg);
  if (!(num_to_dc = tb_parse_int_or_zero(arg))) {
    send_to_char(ch, "Usage: DC <user number> (type USERS for a list)\r\n");
    return;
  }
  for (d = descriptor_list; d && d->desc_num != num_to_dc; d = d->next);

  if (!d) {
    send_to_char(ch, "No such connection.\r\n");
    return;
  }
  if (d->character && GET_LEVEL(d->character) >= GET_LEVEL(ch)) {
    if (!CAN_SEE(ch, d->character))
      send_to_char(ch, "No such connection.\r\n");
    else
      send_to_char(ch, "Umm.. maybe that's not such a good idea...\r\n");
    return;
  }

  /* We used to just close the socket here using close_socket(), but various
   * people pointed out this could cause a crash if you're closing the person
   * below you on the descriptor list.  Just setting to CON_CLOSE leaves things
   * in a massively inconsistent state so I had to add this new flag to the
   * descriptor. -je It is a much more logical extension for a CON_DISCONNECT
   * to be used for in-game socket closes and CON_CLOSE for out of game
   * closings. This will retain the stability of the close_me hack while being
   * neater in appearance. -gg For those unlucky souls who actually manage to
   * get disconnected by two different immortals in the same 1/10th of a
   * second, we have the below 'if' check. -gg */
  if (STATE(d) == CON_DISCONNECT || STATE(d) == CON_CLOSE)
    act("$E's already being disconnected.", FALSE, ch, 0, d->character, TO_CHAR);
  else {
    /* Remember that we can disconnect people not in the game and that rather
     * confuses the code when it expected there to be a character context. */
    if (STATE(d) == CON_PLAYING)
      STATE(d) = CON_DISCONNECT;
    else
      STATE(d) = CON_CLOSE;

    send_to_char(ch, "Connection #%d closed.\r\n", num_to_dc);
    log("(GC) Connection closed by %s.", GET_NAME(ch));
  }
}

ACMD(do_wizlock)
{
  char arg[MAX_INPUT_LENGTH];
  int value;
  const char *when;

  one_argument(argument, arg);
  if (*arg) {
    value = tb_parse_int_or_zero(arg);
    if (value < 0 || value > GET_LEVEL(ch)) {
      send_to_char(ch, "Invalid wizlock value.\r\n");
      return;
    }
    circle_restrict = value;
    when = "now";
  } else
    when = "currently";

  switch (circle_restrict) {
  case 0:
    send_to_char(ch, "The game is %s completely open.\r\n", when);
    break;
  case 1:
    send_to_char(ch, "The game is %s closed to new players.\r\n", when);
    break;
  default:
    send_to_char(ch, "Only level %d and above may enter the game %s.\r\n", circle_restrict, when);
    break;
  }
}

ACMD(do_date)
{
  char timestr[25];
  time_t mytime;
  int d, h, m;

  if (subcmd == SCMD_DATE)
    mytime = time(0);
  else
    mytime = boot_time;

  TIME_STR(timestr, "%c", localtime(&mytime));

  if (subcmd == SCMD_DATE)
    send_to_char(ch, "Current machine time: %s\r\n", timestr);
  else {
    mytime = time(0) - boot_time;
    d = mytime / 86400;
    h = (mytime / 3600) % 24;
    m = (mytime / 60) % 60;

    send_to_char(ch, "Up since %s: %d day%s, %d:%02d\r\n", timestr, d, d == 1 ? "" : "s", h, m);
  }
}

/* altered from stock to the following:
   last [name] [#]
   last without arguments displays the last 10 entries.
   last with a name only displays the 'stock' last entry.
   last with a number displays that many entries (combines with name) */
static const char *last_array[11] = {
  "Connect",
  "Enter Game",
  "Reconnect",
  "Takeover",
  "Quit",
  "Idleout",
  "Disconnect",
  "Shutdown",
  "Reboot",
  "Crash",
  "Playing"
};

static const char *last_type_name(int type)
{
  if (type < 0 || (size_t)type >= (sizeof(last_array) / sizeof(last_array[0])))
    return "Unknown";
  return last_array[type];
}

/* LAST_FILE is an array of raw struct last_entry records (host byte order
 * and padding; 304 bytes on LP64). Record k is at offset k * LLOG_RECSIZE
 * from the start, so every reader and writer below seeks from SEEK_SET;
 * walking back from SEEK_END would shift every record if the file ended
 * in a torn append. */
#define LLOG_RECSIZE ((long) sizeof(struct last_entry))

/** Count the whole records in the open LAST_FILE fp.
 * A size that is not a multiple of LLOG_RECSIZE means an append was torn
 * (ENOSPC, crash): it is reported as last-file-align and, when repair is
 * set and fp is writable, the partial tail is truncated away so later
 * appends stay aligned.
 * @return 1 and *recs on success, 0 (reported) on a seek error. */
static int llog_record_count(FILE *fp, long *recs, int repair)
{
  long size, tail;

  if (fseek(fp, 0L, SEEK_END) != 0 || (size = ftell(fp)) < 0) {
    TB_ERROR_ERRNO("act", "last-file-seek", LAST_FILE, "cannot find the file size");
    return 0;
  }
  *recs = size / LLOG_RECSIZE;
  tail = size % LLOG_RECSIZE;
  if (tail) {
    TB_ERROR_MSG("act", "last-file-align", LAST_FILE,
                 "size %ld not a multiple of %ld: %ld torn trailing bytes%s",
                 size, LLOG_RECSIZE, tail, repair ? "; truncating them" : "");
    if (repair && (fflush(fp) != 0 ||
                   ftruncate(fileno(fp), (off_t) (*recs * LLOG_RECSIZE)) != 0))
      TB_ERROR_ERRNO("act", "last-file-align", LAST_FILE,
                     "could not truncate to %ld records", *recs);
  }
  return 1;
}

/** Read record index (0-based from the start) of LAST_FILE into out. The
 * fixed-width name fields are NUL-terminated, since the file is data.
 * @return 1, or 0 (reported) on a seek or read error. */
static int llog_read_record(FILE *fp, long index, struct last_entry *out)
{
  if (fseek(fp, index * LLOG_RECSIZE, SEEK_SET) != 0) {
    TB_ERROR_ERRNO("act", "last-file-seek", LAST_FILE, "record %ld", index);
    return 0;
  }
  if (fread(out, sizeof(*out), 1, fp) != 1) {
    TB_ERROR_ERRNO("act", "last-file-read", LAST_FILE, "record %ld", index);
    return 0;
  }
  out->username[sizeof(out->username) - 1] = '\0';
  out->hostname[sizeof(out->hostname) - 1] = '\0';
  return 1;
}

struct last_entry *find_llog_entry(int punique, long idnum) {
  FILE *fp;
  struct last_entry mlast;
  struct last_entry *llast;
  long recs, tmp;

  if(!(fp=fopen(LAST_FILE,"r"))) {
    log("Error opening last_file for reading, will create.");
    return NULL;
  }
  if (!llog_record_count(fp, &recs, FALSE)) {
    tb_fclose(fp);
    return NULL;
  }

  /* we'll search last to first, since it's faster than any thing else we can
   * do (like searching for the last shutdown/etc..) */
  for(tmp=recs - 1; tmp >= 0; tmp--) {
    if (!llog_read_record(fp, tmp, &mlast)) {
      tb_fclose(fp);
      return NULL;
    }
    if(mlast.idnum == idnum && mlast.punique == punique) {
      /* then we've found a match */
      CREATE(llast,struct last_entry,1);
      memcpy(llast,&mlast,sizeof(struct last_entry));
      tb_fclose(fp);
      return llast;
    }
    /*not the one we seek. next */
  }
  /*not found, no problem, quit */
  tb_fclose(fp);
  return NULL;
}

/* mod_llog_entry assumes that llast is accurate */
static void mod_llog_entry(struct last_entry *llast,int type) {
  FILE *fp;
  struct last_entry mlast;
  long recs, tmp;

  if(!(fp=fopen(LAST_FILE,"r+"))) {
    TB_ERROR_ERRNO("act", "last-file-open", LAST_FILE,
                   "cannot update player idnum %d", llast->idnum);
    return;
  }
  if (!llog_record_count(fp, &recs, FALSE)) {
    tb_fclose(fp);
    return;
  }

  /* We'll search last to first, since it's faster than any thing else we can
   * do (like searching for the last shutdown/etc..) */
  for(tmp=recs - 1; tmp >= 0; tmp--) {
    if (!llog_read_record(fp, tmp, &mlast)) {
      tb_fclose(fp);
      return;
    }

    if(mlast.idnum == llast->idnum && mlast.punique == llast->punique) {
      /* Then we've found a match, lets assume quit is inviolate, mainly
       * because disconnect is called after each of these */
      if(mlast.close_type != LAST_QUIT &&
        mlast.close_type != LAST_IDLEOUT &&
        mlast.close_type != LAST_REBOOT &&
        mlast.close_type != LAST_SHUTDOWN) {
        mlast.close_type=type;
      }
      mlast.close_time=time(0);
      /* Write it back over the same record, and we're done. */
      if (fseek(fp, tmp * LLOG_RECSIZE, SEEK_SET) != 0 ||
          fwrite(&mlast, sizeof(mlast), 1, fp) != 1)
        TB_ERROR_ERRNO("act", "last-file-write", LAST_FILE,
                       "record %ld player idnum %d", tmp, mlast.idnum);
      tb_fclose_checked(fp, LAST_FILE);
      return;
    }
    /* Not the one we seek, next. */
  }
  tb_fclose(fp);

  /* Not found, no problem, quit. */
  return;
}

/** Record a login event for ch in LAST_FILE (see LLOG_RECSIZE).
 * Callers: interpreter.c (LAST_CONNECT on entering the game, LAST_QUIT,
 * LAST_RECONNECT), comm.c (LAST_DISCONNECT on a dropped link) and limits.c
 * (LAST_IDLEOUT). A login with no pref yet (bad password, early link loss)
 * is not recorded. A new session appends a record; a later event for the
 * same idnum and pref updates that record's close time and type in place.
 * The record is written raw (host byte order, padding zeroed by CREATE),
 * so LAST_FILE is specific to the build's ABI. */
void add_llog_entry(struct char_data *ch, int type) {
  FILE *fp;
  struct last_entry *llast;

  /* so if a char enteres a name, but bad password, otherwise loses link before
   * he gets a pref assinged, we won't record it */
  if(GET_PREF(ch) <= 0) {
    return;
  }

  /* See if we have a login stored */
  llast = find_llog_entry(GET_PREF(ch), GET_IDNUM(ch));

  /* we didn't - make a new one */
  if(llast == NULL) {  /* no entry found, add ..error if close! */
    CREATE(llast,struct last_entry,1);
    strncpy(llast->username,GET_NAME(ch),15);
    strncpy(llast->hostname,GET_HOST(ch),127);
    llast->username[15]='\0';
    llast->hostname[127]='\0';
    llast->idnum=GET_IDNUM(ch);
    llast->punique=GET_PREF(ch);
    llast->time=time(0);
    llast->close_time=0;
    llast->close_type=type;

    if(!(fp=fopen(LAST_FILE,"a"))) {
      TB_ERROR_ERRNO("act", "last-file-open", LAST_FILE,
                     "cannot append player idnum %d", llast->idnum);
      free(llast);
      return;
    }
    {
      long recs;
      /* Drop a torn tail first, or this record would be misaligned too. */
      if (llog_record_count(fp, &recs, TRUE) &&
          fwrite(llast, sizeof(struct last_entry), 1, fp) != 1)
        TB_ERROR_ERRNO("act", "last-file-write", LAST_FILE,
                       "append for player idnum %d", llast->idnum);
    }
    tb_fclose_checked(fp, LAST_FILE);
  } else {
    /* We've found a login - update it */
    mod_llog_entry(llast,type);
  }
  free(llast);
}

/** Trim LAST_FILE to its newest MAX_LAST_ENTRIES records. Called once at
 * boot (db.c); the copy goes through etc/nlast and is renamed into place
 * only when every record was copied. */
void clean_llog_entries(void) {
  FILE *ofp, *nfp;
  struct last_entry mlast;
  long size, recs, copied = 0;
  int failed = FALSE;
  const char *temp_name = "etc/nlast";

  if (!(ofp = fopen(LAST_FILE, "rb")))
    return; /* no file, no gripe */

  if (fseek(ofp, 0L, SEEK_END) != 0 || (size = ftell(ofp)) < 0 ||
      fseek(ofp, 0L, SEEK_SET) != 0) {
    TB_REPORT_ERRNO("seek", LAST_FILE);
    tb_fclose(ofp);
    return;
  }
  recs = size / (long)sizeof(struct last_entry);
  if (recs < MAX_LAST_ENTRIES) {
    tb_fclose(ofp);
    return;
  }

  if (!(nfp = fopen(temp_name, "wb"))) {
    TB_REPORT_ERRNO("open", temp_name);
    tb_fclose(ofp);
    return;
  }

  if (fseek(ofp, (recs - MAX_LAST_ENTRIES) *
                 (long)sizeof(struct last_entry), SEEK_SET) != 0) {
    TB_REPORT_ERRNO("seek", LAST_FILE);
    failed = TRUE;
  }

  while (!failed && fread(&mlast, sizeof(mlast), 1, ofp) == 1) {
    if (fwrite(&mlast, sizeof(mlast), 1, nfp) != 1) {
      TB_REPORT_ERRNO("write", temp_name);
      failed = TRUE;
    } else {
      copied++;
    }
  }
  if (copied != MAX_LAST_ENTRIES)
    failed = TRUE;
  if (ferror(ofp)) {
    TB_REPORT_ERRNO("read", LAST_FILE);
    failed = TRUE;
  }
  if (fclose(ofp) != 0)
    failed = TRUE;
  if (fflush(nfp) != 0)
    failed = TRUE;
  if (fclose(nfp) != 0)
    failed = TRUE;

  if (!failed && rename(temp_name, LAST_FILE) != 0) {
    TB_REPORT_ERRNO("rename", LAST_FILE);
    failed = TRUE;
  }
  if (failed) {
    log("SYSERR: last-login cleanup failed; original history preserved");
    if (remove(temp_name) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("remove", temp_name);
  }
}

/* debugging stuff, if you wanna see the whole file */
static void list_llog_entries(struct char_data *ch)
{
  FILE *fp;
  struct last_entry llast;
  char timestr[25];

  if(!(fp=fopen(LAST_FILE,"r"))) {
    log("llist_log_entries: could not open last log file %s.", LAST_FILE);
    send_to_char(ch, "Error! - no last log");
    return;
  }
  send_to_char(ch, "Last log\r\n");

  while(fread(&llast, sizeof(struct last_entry), 1, fp) == 1) {
    llast.username[sizeof(llast.username) - 1] = '\0';
    llast.hostname[sizeof(llast.hostname) - 1] = '\0';
    TIME_STR(timestr, "%a %b %d %Y %H:%M:%S", localtime(&llast.time));
    send_to_char(ch, "%10s    %d    %s    %s\r\n", llast.username, llast.punique,
        last_type_name(llast.close_type), timestr);
  }

  if(ferror(fp)) {
    log("llist_log_entries: error reading %s.", LAST_FILE);
    send_to_char(ch, "Error reading last_log file.");
  }
  tb_fclose(fp);
}

/** Find a player character in the world by idnum, linkless or not.
 * @param idnum The player's idnum.
 * @return The in-world character, or NULL when the player is offline. Unlike
 * is_in_game(), this also finds linkdead players (no descriptor). */
static struct char_data *find_player_in_world(long idnum)
{
  struct char_data *i;

  for (i = character_list; i; i = i->next)
    if (!IS_NPC(i) && GET_IDNUM(i) == idnum)
      return i;
  return NULL;
}

static struct char_data *is_in_game(long idnum) {
  struct descriptor_data *i;

  for (i = descriptor_list; i; i = i->next) {
    if (i->character && GET_IDNUM(i->character) == idnum) {
      return i->character;
    }
  }
  return NULL;
}

ACMD(do_last)
{
  char arg[MAX_INPUT_LENGTH], name[MAX_INPUT_LENGTH], timestr[25], to[6];
  char deltastr[6];
  time_t delta;
  struct char_data *vict = NULL;
  struct char_data *temp;
  long recs;
  int num = 0;
  FILE *fp;
  struct last_entry mlast;

  *name = '\0';

  if (*argument) { /* parse it */
    half_chop(argument, arg, argument);
    while (*arg) {
      if ((*arg == '*' || !strcmp(arg, "all")) && (GET_LEVEL(ch) == LVL_IMPL)) {
        list_llog_entries(ch);
        return;
      }
      if (isdigit(*arg)) {
        num = tb_parse_int_or_zero(arg);
        if (num < 0)
          num = 0;
      } else {
        strncpy(name, arg, sizeof(name)-1);
        name[sizeof(name) - 1] = '\0';
      }
      
      half_chop(argument, arg, argument);
    }
  }

  if (*name && !num) {
    CREATE(vict, struct char_data, 1);
    clear_char(vict);
    CREATE(vict->player_specials, struct player_special_data, 1);
    new_mobile_data(vict);
    if (load_char(name, vict) <  0) {
      send_to_char(ch, "There is no such player.\r\n");
      free_char(vict);
      return;
    }

    TIME_STR(timestr, "%a %b %d %H:%M:%S %Y", localtime(&(vict->player.time.logon)));

    send_to_char(ch, "[%5ld] [%2d %s] %-12s : %-18s : %-24s\r\n",
    GET_IDNUM(vict), (int) GET_LEVEL(vict),
    /* Sex and class come from the pfile 'Sex '/'Clas' tags, unchecked. */
    TABLE_NAME(class_abbrevs, NUM_CLASSES, GET_CLASS(vict), "last-player-field",
               DIAG_CHAR(vict)), GET_NAME(vict),
    GET_HOST(vict) && *GET_HOST(vict) ? GET_HOST(vict) : "(NOHOST)", timestr);
    free_char(vict);
    return;
    }

  if(num <= 0 || num >= 100) {
    num=10;
  }

  if(!(fp=fopen(LAST_FILE,"r"))) {
    send_to_char(ch, "No entries found.\r\n");
    return;
  }
  if (!llog_record_count(fp, &recs, FALSE)) {
    send_to_char(ch, "Error reading log file.\r\n");
    tb_fclose(fp);
    return;
  }

  send_to_char(ch, "Last log\r\n");
  while(num > 0 && recs > 0) {
    if (!llog_read_record(fp, recs - 1, &mlast)) {
      send_to_char(ch, "Error reading log file.\r\n");
      tb_fclose(fp);
      return;
    }
    if(!*name ||(*name && !str_cmp(name, mlast.username))) {
      TIME_STR(timestr, "%a %b %d %Y %H:%M", localtime(&mlast.time));
      send_to_char(ch, "%10.10s %20.20s %20.21s - ",
        mlast.username, mlast.hostname, timestr);
      if((temp=is_in_game(mlast.idnum)) && mlast.punique == GET_PREF(temp)) {
        send_to_char(ch, "Still Playing  ");
      } else {
        delta = mlast.close_time - mlast.time;
	TIME_STR(to, "%H:%M", localtime(&mlast.close_time));
	TIME_STR(deltastr, "%H:%M", gmtime(&delta));

        send_to_char(ch, "%5.5s (%5.5s) %s", to, deltastr,
          last_type_name(mlast.close_type));
      }

      send_to_char(ch, "\r\n");
      num--;
    }
    recs--;
  }
  tb_fclose(fp);
}

/** force <victim|room|all> <command>.
 * Forced commands run with the victim's privileges and without its
 * consent, can reach commands that assume ch->desc != NULL (each such
 * command checks it; see command-no-desc), and can fire DG command, enter
 * and leave triggers that move or extract other occupants. The 'room'
 * loop follows next_force across those callbacks; the safety guide's
 * snapshot-and-revalidate rule (section 2) applies and is not yet
 * implemented here (G1-07, open). Players at or above the caller's level
 * are never forced. */
ACMD(do_force)
{
  struct descriptor_data *i, *next_desc;
  struct char_data *vict, *next_force;
  char arg[MAX_INPUT_LENGTH], to_force[MAX_INPUT_LENGTH], buf1[MAX_INPUT_LENGTH + 32];

  half_chop(argument, arg, to_force);

  tb_snprintf(buf1, sizeof(buf1), "$n has forced you to '%s'.", to_force);

  if (!*arg || !*to_force)
    send_to_char(ch, "Whom do you wish to force do what?\r\n");
  else if ((GET_LEVEL(ch) < LVL_GRGOD) || (str_cmp("all", arg) && str_cmp("room", arg))) {
    if (!(vict = get_char_vis(ch, arg, NULL, FIND_CHAR_WORLD)))
      send_to_char(ch, "%s", CONFIG_NOPERSON);
    else if (!IS_NPC(vict) && GET_LEVEL(ch) < LVL_GOD)
      send_to_char(ch, "You cannot force players.\r\n");
    else if (!IS_NPC(vict) && GET_LEVEL(ch) <= GET_LEVEL(vict))
      send_to_char(ch, "No, no, no!\r\n");
    else {
      send_to_char(ch, "%s", CONFIG_OK);
      act(buf1, TRUE, ch, NULL, vict, TO_VICT);
      mudlog(CMP, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s forced %s to %s", GET_NAME(ch), GET_NAME(vict), to_force);
      command_interpreter(vict, to_force);
    }
  } else if (!str_cmp("room", arg)) {
    send_to_char(ch, "%s", CONFIG_OK);
    mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s forced room %d to %s",
		GET_NAME(ch), GET_ROOM_VNUM(IN_ROOM(ch)), to_force);

    for (vict = world[IN_ROOM(ch)].people; vict; vict = next_force) {
      next_force = vict->next_in_room;
      if (!IS_NPC(vict) && GET_LEVEL(vict) >= GET_LEVEL(ch))
	continue;
      act(buf1, TRUE, ch, NULL, vict, TO_VICT);
      command_interpreter(vict, to_force);
    }
  } else { /* force all */
    send_to_char(ch, "%s", CONFIG_OK);
    mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s forced all to %s", GET_NAME(ch), to_force);

    for (i = descriptor_list; i; i = next_desc) {
      next_desc = i->next;

      if (STATE(i) != CON_PLAYING || !(vict = i->character) || (!IS_NPC(vict) && GET_LEVEL(vict) >= GET_LEVEL(ch)))
	continue;
      act(buf1, TRUE, ch, NULL, vict, TO_VICT);
      command_interpreter(vict, to_force);
    }
  }
}

ACMD(do_wiznet)
{
  char buf1[MAX_INPUT_LENGTH + MAX_NAME_LENGTH + 32],
       buf2[MAX_INPUT_LENGTH + MAX_NAME_LENGTH + 32];
  struct descriptor_data *d;
  char emote = FALSE;
  int level = LVL_IMMORT;

  skip_spaces(&argument);

  if (!*argument) {
    send_to_char(ch, "Usage: wiznet [ #<level> ] [<text> | *<emotetext> | @ ]\r\n");
    return;
  }
  switch (*argument) {
  case '*':
    emote = TRUE;
    /* Intentional fall through: emotes share the #level parsing path. */
    /* fall through */
  case '#':
    one_argument(argument + 1, buf1);
    if (is_number(buf1)) {
      half_chop(argument+1, buf1, argument);
      level = MAX(tb_parse_int_or_zero(buf1), LVL_IMMORT);
      if (level > GET_LEVEL(ch)) {
	send_to_char(ch, "You can't wizline above your own level.\r\n");
	return;
      }
    } else if (emote)
      argument++;
    break;

  case '@':
    send_to_char(ch, "God channel status:\r\n");
    for (d = descriptor_list; d; d = d->next) {
      if (STATE(d) != CON_PLAYING || GET_LEVEL(d->character) < LVL_IMMORT)
        continue;
      if (!CAN_SEE(ch, d->character))
        continue;

      send_to_char(ch, "  %-*s%s%s%s\r\n", MAX_NAME_LENGTH, GET_NAME(d->character),
		PLR_FLAGGED(d->character, PLR_WRITING) ? " (Writing)" : "",
		PLR_FLAGGED(d->character, PLR_MAILING) ? " (Writing mail)" : "",
		PRF_FLAGGED(d->character, PRF_NOWIZ) ? " (Offline)" : "");
    }
    return;

  case '\\':
    ++argument;
    break;
  default:
    break;
  }
  if (PRF_FLAGGED(ch, PRF_NOWIZ)) {
    send_to_char(ch, "You are offline!\r\n");
    return;
  }
  skip_spaces(&argument);

  if (!*argument) {
    send_to_char(ch, "Don't bother the gods like that!\r\n");
    return;
  }
  if (level > LVL_IMMORT) {
    tb_snprintf(buf1, sizeof(buf1), "\tc%s: <%d> %s%s\tn\r\n", GET_NAME(ch), level, emote ? "<--- " : "", argument);
    tb_snprintf(buf2, sizeof(buf2), "\tcSomeone: <%d> %s%s\tn\r\n", level, emote ? "<--- " : "", argument);
} else {
    tb_snprintf(buf1, sizeof(buf1), "\tc%s: %s%s\tn\r\n", GET_NAME(ch), emote ? "<--- " : "", argument);
    tb_snprintf(buf2, sizeof(buf2), "\tcSomeone: %s%s\tn\r\n", emote ? "<--- " : "", argument);
  }

  for (d = descriptor_list; d; d = d->next) {
    if (IS_PLAYING(d) && (GET_LEVEL(d->character) >= level) &&
	(!PRF_FLAGGED(d->character, PRF_NOWIZ))
	&& (d != ch->desc || !NOREPEAT(d->character))) {
      if (CAN_SEE(d->character, ch)) {
        parse_at(buf1);
        send_to_char(d->character, "%s%s%s", CCCYN(d->character, C_NRM), buf1, CCNRM(d->character, C_NRM));
        add_history(d->character, buf1, HIST_WIZNET);
      } else {
        parse_at(buf2);
        send_to_char(d->character, "%s%s%s", CCCYN(d->character, C_NRM), buf2, CCNRM(d->character, C_NRM));
        add_history(d->character, buf2, HIST_WIZNET);
      }
    }
  }

  if (NOREPEAT(ch))
    send_to_char(ch, "%s", CONFIG_OK);
}

ACMD(do_zreset)
{
  char arg[MAX_INPUT_LENGTH];
  zone_rnum i;
  zone_vnum j;

  one_argument(argument, arg);

  if (*arg == '*') {
    if (GET_LEVEL(ch) < LVL_GOD){
      send_to_char(ch, "You do not have permission to reset the entire world.\r\n");
      return;
   } else {
      for (i = 0; i <= top_of_zone_table; i++)
      reset_zone(i);
    send_to_char(ch, "Reset world.\r\n");
    mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s reset entire world.", GET_NAME(ch));
    return; }
  } else if (*arg == '.' || !*arg)
    i = world[IN_ROOM(ch)].zone;
  else {
    j = tb_parse_int_or_zero(arg);
    for (i = 0; i <= top_of_zone_table; i++)
      if (zone_table[i].number == j)
	break;
  }
  if (i <= top_of_zone_table && (can_edit_zone(ch, i) || GET_LEVEL(ch) > LVL_IMMORT)) {
    reset_zone(i);
    send_to_char(ch, "Reset zone #%d: %s.\r\n", zone_table[i].number, zone_table[i].name);
    mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s reset zone %d (%s)", GET_NAME(ch), zone_table[i].number, zone_table[i].name);
  } else
    send_to_char(ch, "You do not have permission to reset this zone. Try %d.\r\n", GET_OLC_ZONE(ch));
}

/*  General fn for wizcommands of the sort: cmd <player> */
ACMD(do_wizutil)
{
  char arg[MAX_INPUT_LENGTH];
  struct char_data *vict;
  int taeller;
  long result;

  one_argument(argument, arg);

  if (!*arg)
    send_to_char(ch, "Yes, but for whom?!?\r\n");
  else if (!(vict = get_char_vis(ch, arg, NULL, FIND_CHAR_WORLD)))
    send_to_char(ch, "There is no such player.\r\n");
  else if (IS_NPC(vict))
    send_to_char(ch, "You can't do that to a mob!\r\n");
  else if (GET_LEVEL(vict) >= GET_LEVEL(ch) && vict != ch)
    send_to_char(ch, "Hmmm...you'd better not.\r\n");
  else {
    switch (subcmd) {
    case SCMD_REROLL:
      send_to_char(ch, "Rerolled...\r\n");
      roll_real_abils(vict);
      log("(GC) %s has rerolled %s.", GET_NAME(ch), GET_NAME(vict));
      send_to_char(ch, "New stats: Str %d/%d, Int %d, Wis %d, Dex %d, Con %d, Cha %d\r\n",
	      GET_STR(vict), GET_ADD(vict), GET_INT(vict), GET_WIS(vict),
	      GET_DEX(vict), GET_CON(vict), GET_CHA(vict));
      break;
    case SCMD_PARDON:
      if (!PLR_FLAGGED(vict, PLR_THIEF) && !PLR_FLAGGED(vict, PLR_KILLER)) {
	send_to_char(ch, "Your victim is not flagged.\r\n");
	return;
      }
      REMOVE_BIT_AR(PLR_FLAGS(vict), PLR_THIEF);
      REMOVE_BIT_AR(PLR_FLAGS(vict), PLR_KILLER);
      send_to_char(ch, "Pardoned.\r\n");
      send_to_char(vict, "You have been pardoned by the Gods!\r\n");
      mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s pardoned by %s", GET_NAME(vict), GET_NAME(ch));
      break;
    case SCMD_NOTITLE:
      result = PLR_TOG_CHK(vict, PLR_NOTITLE);
      mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) Notitle %s for %s by %s.",
		ONOFF(result), GET_NAME(vict), GET_NAME(ch));
      send_to_char(ch, "(GC) Notitle %s for %s by %s.\r\n", ONOFF(result), GET_NAME(vict), GET_NAME(ch));
      break;
    case SCMD_MUTE:
      result = PLR_TOG_CHK(vict, PLR_NOSHOUT);
      mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) Mute %s for %s by %s.",
		ONOFF(result), GET_NAME(vict), GET_NAME(ch));
      send_to_char(ch, "(GC) Mute %s for %s by %s.\r\n", ONOFF(result), GET_NAME(vict), GET_NAME(ch));
      break;
    case SCMD_FREEZE:
      if (ch == vict) {
	send_to_char(ch, "Oh, yeah, THAT'S real smart...\r\n");
	return;
      }
      if (PLR_FLAGGED(vict, PLR_FROZEN)) {
	send_to_char(ch, "Your victim is already pretty cold.\r\n");
	return;
      }
      SET_BIT_AR(PLR_FLAGS(vict), PLR_FROZEN);
      GET_FREEZE_LEV(vict) = GET_LEVEL(ch);
      send_to_char(vict, "A bitter wind suddenly rises and drains every erg of heat from your body!\r\nYou feel frozen!\r\n");
      send_to_char(ch, "Frozen.\r\n");
      act("A sudden cold wind conjured from nowhere freezes $n!", FALSE, vict, 0, 0, TO_ROOM);
      mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s frozen by %s.", GET_NAME(vict), GET_NAME(ch));
      break;
    case SCMD_THAW:
      if (!PLR_FLAGGED(vict, PLR_FROZEN)) {
	send_to_char(ch, "Sorry, your victim is not morbidly encased in ice at the moment.\r\n");
	return;
      }
      if (GET_FREEZE_LEV(vict) > GET_LEVEL(ch)) {
	send_to_char(ch, "Sorry, a level %d God froze %s... you can't unfreeze %s.\r\n",
		GET_FREEZE_LEV(vict), GET_NAME(vict), HMHR(vict));
	return;
      }
      mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s un-frozen by %s.", GET_NAME(vict), GET_NAME(ch));
      REMOVE_BIT_AR(PLR_FLAGS(vict), PLR_FROZEN);
      send_to_char(vict, "A fireball suddenly explodes in front of you, melting the ice!\r\nYou feel thawed.\r\n");
      send_to_char(ch, "Thawed.\r\n");
      act("A sudden fireball conjured from nowhere thaws $n!", FALSE, vict, 0, 0, TO_ROOM);
      break;
    case SCMD_UNAFFECT:
      for (taeller = 0; taeller < AF_ARRAY_MAX; taeller++)
        if (AFF_FLAGS(vict)[taeller])
          break;
      if (vict->affected || taeller < AF_ARRAY_MAX) {
	while (vict->affected)
	  affect_remove(vict, vict->affected);
    for(taeller=0; taeller < AF_ARRAY_MAX; taeller++)
      AFF_FLAGS(vict)[taeller] = 0;
    send_to_char(vict, "There is a brief flash of light!\r\nYou feel slightly different.\r\n");
	send_to_char(ch, "All spells removed.\r\n");
      } else {
	send_to_char(ch, "Your victim does not have any affections!\r\n");
	return;
      }
      break;
    default:
      /* Invariant: subcmd comes from the command table (interpreter.c),
       * which only passes SCMD_REROLL..SCMD_UNAFFECT here. Nothing was
       * changed, so do not save. */
      TB_ERROR_MSG("act", "wizutil-subcmd", "subcmd",
                   "subcmd %d caller " DIAG_CHAR_FMT " target player idnum %ld",
                   subcmd, DIAG_CHAR(ch), GET_IDNUM(vict));
      send_to_char(ch, "That command is miswired; it has been logged.\r\n");
      return;
    }
    save_char(vict);
  }
}

/* single zone printing fn used by "show zone" so it's not repeated in the
   code 3 times ... -je, 4/6/93 (the old FIXME overflow is fixed: G1-23) */
/** Account for one snprintf() into print_zone_to_buf's buffer: used bytes
 * were already there, n is snprintf's result for the remaining space.
 * @return the new length, never more than left - 1. A truncated or failed
 * write is reported as show-zone-truncate with the zone vnum (the old code
 * added the would-be length and wrapped 'left - tmp'). */
static size_t zone_buf_add(int n, size_t used, size_t left, zone_rnum zone)
{
  if (n < 0 || (size_t) n >= left - used) {
    TB_WARN_MSG("act", "show-zone-truncate", "zone",
                "zone vnum %d: listing needs more than %zu bytes",
                zone_table[zone].number, left);
    return left - 1;
  }
  return used + (size_t) n;
}

/** Print one zone's summary (listall: with its stats) into bufptr, which
 * has left bytes (left >= 1).
 * @return the bytes written, at most left - 1; left - 1 means it was
 *         truncated (and reported). */
static size_t print_zone_to_buf(char *bufptr, size_t left, zone_rnum zone, int listall)
{
  size_t tmp;

  if (listall) {
    int i, j, k, l, m, n, o;
    char buf[MAX_STRING_LENGTH];

    sprintbitarray(zone_table[zone].zone_flags, zone_bits, ZN_ARRAY_MAX, buf, sizeof(buf));

    tmp = zone_buf_add(snprintf(bufptr, left,
	"%3d %-30.30s%s By: %-10.10s%s Age: %3d; Reset: %3d (%s); Range: %5d-%5d\r\n",
	zone_table[zone].number, zone_table[zone].name, KNRM, zone_table[zone].builders, KNRM,
	zone_table[zone].age, zone_table[zone].lifespan,
        zone_table[zone].reset_mode ? ((zone_table[zone].reset_mode == 1) ? "Reset when no players are in zone" : "Normal reset") : "Never reset",
	zone_table[zone].bot, zone_table[zone].top), 0, left, zone);
        if (tmp >= left - 1)
          return tmp;
        j = k = l = m = n = o = 0;

        /* top_of_world/objt/mobt are the last valid rnums (inclusive);
         * top_of_trigt is a count (exclusive). */
        for (i = 0; i <= top_of_world; i++)
          if (world[i].number >= zone_table[zone].bot && world[i].number <= zone_table[zone].top)
            j++;

        for (i = 0; i <= top_of_objt; i++)
          if (obj_index[i].vnum >= zone_table[zone].bot && obj_index[i].vnum <= zone_table[zone].top)
            k++;

        for (i = 0; i <= top_of_mobt; i++)
          if (mob_index[i].vnum >= zone_table[zone].bot && mob_index[i].vnum <= zone_table[zone].top)
            l++;

        for (i = 0; i<= top_shop; i++)
          if (SHOP_NUM(i) >= zone_table[zone].bot && SHOP_NUM(i) <= zone_table[zone].top)
            m++;

        for (i = 0; i < top_of_trigt; i++)
          if (trig_index[i]->vnum >= zone_table[zone].bot && trig_index[i]->vnum <= zone_table[zone].top)
            n++;

        o = count_quests(zone_table[zone].bot, zone_table[zone].top);

	tmp = zone_buf_add(snprintf(bufptr + tmp, left - tmp,
                        "       Zone stats:\r\n"
                        "       ---------------\r\n"
                        "         Flags:    %s\r\n"
                        "         Min Lev:  %2d\r\n"
                        "         Max Lev:  %2d\r\n"
                        "         Rooms:    %2d\r\n"
                        "         Objects:  %2d\r\n"
                        "         Mobiles:  %2d\r\n"
                        "         Shops:    %2d\r\n"
                        "         Triggers: %2d\r\n"
                        "         Quests:   %2d\r\n",
			buf, zone_table[zone].min_level, zone_table[zone].max_level,
                        j, k, l, m, n, o), tmp, left, zone);

    return tmp;
  }

    return zone_buf_add(snprintf(bufptr, left,
        "%3d %-*s%s By: %-10.10s%s Range: %5d-%5d\r\n", zone_table[zone].number,
	count_color_chars(zone_table[zone].name)+30, zone_table[zone].name, KNRM,
	zone_table[zone].builders, KNRM, zone_table[zone].bot, zone_table[zone].top),
	0, left, zone);
}

/* Room kept at the end of a 'show' buffer for the truncation marker. */
#define SHOW_TRUNC_MARK "\r\n*** output truncated ***\r\n"

/* Start a 'show' listing in the array buf (sizeof is taken here), leaving
 * room for SHOW_TRUNC_MARK, with title as the first line. */
#define SHOW_BEGIN(out, buf, title) do { \
  tb_buf_init(&(out), (buf), sizeof(buf) - sizeof(SHOW_TRUNC_MARK)); \
  tb_buf_append(&(out), (title)); \
} while (0)

/** Page a 'show' listing to ch. A listing that filled its buffer used to
 * end mid-line with no sign of it; it now ends with SHOW_TRUNC_MARK and is
 * reported as show-truncate with the field and the caller. */
static void show_page(struct char_data *ch, char *buf, size_t bufsize,
                      int truncated, const char *field)
{
  if (truncated) {
    size_t pos = strlen(buf);

    if (pos > bufsize - sizeof(SHOW_TRUNC_MARK))
      pos = bufsize - sizeof(SHOW_TRUNC_MARK);
    strlcpy(buf + pos, SHOW_TRUNC_MARK, bufsize - pos);
    TB_WARN_MSG("act", "show-truncate", field,
                "caller " DIAG_CHAR_FMT ": listing exceeded %zu bytes",
                DIAG_CHAR(ch), bufsize);
  }
  page_string(ch->desc, buf, TRUE);
}

ACMD(do_show)
{
  int i, j, k, l, con, builder =0;		/* i, j, k to specifics? */
  int zones_truncated = FALSE;
  size_t len, nlen;
  zone_rnum zrn;
  zone_vnum zvn;
  byte self = FALSE;
  struct char_data *vict = NULL;
  struct obj_data *obj;
  struct descriptor_data *d;
  char field[MAX_INPUT_LENGTH], value[MAX_INPUT_LENGTH],
	arg[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH];
  int r, g, b;
  char colour[16];
  struct tb_buf out;   /* the list builders below append through this */

  struct show_struct {
    const char *cmd;
    const char level;
  } fields[] = {
    { "nothing",	0  },				/* 0 */
    { "zones",		LVL_IMMORT },			/* 1 */
    { "player",		LVL_IMMORT },
    { "rent",		LVL_IMMORT },
    { "stats",		LVL_IMMORT },
    { "errors",		LVL_IMMORT },			/* 5 */
    { "death",		LVL_IMMORT },
    { "godrooms",	LVL_IMMORT },
    { "shops",		LVL_IMMORT },
    { "houses",		LVL_IMMORT },
    { "snoop",		LVL_IMMORT },			/* 10 */
    { "thaco",      LVL_IMMORT },
    { "exp",        LVL_IMMORT },
    { "colour",     LVL_IMMORT },
    { "\n", 0 }
  };

  skip_spaces(&argument);

  if (!*argument) {
    send_to_char(ch, "Show options:\r\n");
    for (j = 0, i = 1; fields[i].level; i++)
      if (fields[i].level <= GET_LEVEL(ch))
	send_to_char(ch, "%-15s%s", fields[i].cmd, (!(++j % 5) ? "\r\n" : ""));
    send_to_char(ch, "\r\n");
    return;
  }

  strlcpy(arg, two_arguments(argument, field, value), sizeof(arg));	/* strcpy: OK (argument <= MAX_INPUT_LENGTH == arg) */

  for (l = 0; *(fields[l].cmd) != '\n'; l++)
    if (!strncmp(field, fields[l].cmd, strlen(field)))
      break;

  if (GET_LEVEL(ch) < fields[l].level) {
    send_to_char(ch, "You are not godly enough for that!\r\n");
    return;
  }
  if (!strcmp(value, "."))
    self = TRUE;
  buf[0] = '\0';

  switch (l) {
  /* show zone */
  case 1:
    /* tightened up by JE 4/6/93 */
    if (self)
      print_zone_to_buf(buf, sizeof(buf), world[IN_ROOM(ch)].zone, 1);
    else if (*value && is_number(value)) {
      for (zvn = tb_parse_int_or_zero(value), zrn = 0; zrn <= top_of_zone_table && zone_table[zrn].number != zvn; zrn++);
      if (zrn <= top_of_zone_table)
	print_zone_to_buf(buf, sizeof(buf), zrn, 1);
      else {
	send_to_char(ch, "That is not a valid zone.\r\n");
	return;
      }
    } else {
      char *buf2;
      if (*value)
        builder = 1;
      for (len = zrn = 0; zrn <= top_of_zone_table; zrn++) {
        if (*value) {
          char *builders_copy = strdup(zone_table[zrn].builders ? zone_table[zrn].builders : "");

          if (!builders_copy)
            continue;

          buf2 = strtok(builders_copy, " ");
          while (buf2) {
            if (!str_cmp(buf2, value)) {
              if (builder == 1)
                builder++;
              break;
            }
            buf2 = strtok(NULL, " ");
          }

          if (!buf2) {
            free(builders_copy);
            continue;
          }
          free(builders_copy);
	}
	nlen = print_zone_to_buf(buf + len, sizeof(buf) - len, zrn, 0);
        if (len + nlen >= sizeof(buf) - 1) {
          /* print_zone_to_buf already reported show-zone-truncate. */
          zones_truncated = TRUE;
          break;
        }
        len += nlen;
      }
    }
    if (builder == 1)
      send_to_char(ch, "%s has not built any zones here.\r\n", CAP(value));
    else if (builder == 2)
      send_to_char(ch, "The following zones have been built by: %s\r\n", CAP(value));
    show_page(ch, buf, sizeof(buf), zones_truncated, "zones");
    break;

  /* show player */
  case 2: {
    char buf1[64], buf2[64];

    if (!*value) {
      send_to_char(ch, "A name would help.\r\n");
      return;
    }

    CREATE(vict, struct char_data, 1);
    clear_char(vict);
    CREATE(vict->player_specials, struct player_special_data, 1);
    new_mobile_data(vict);
    if (load_char(value, vict) < 0) {
      send_to_char(ch, "There is no such player.\r\n");
      free_char(vict);
      return;
    }

    TIME_STR(buf1, "%a %b %d %H:%M:%S %Y", localtime(&(vict->player.time.birth)));
    TIME_STR(buf2, "%a %b %d %H:%M:%S %Y", localtime(&(vict->player.time.logon)));

    send_to_char(ch, "Player: %-12s (%s) [%2d %s]\r\n", GET_NAME(vict),
      /* Sex and class come from the pfile 'Sex '/'Clas' tags, unchecked. */
      TABLE_NAME(genders, NUM_GENDERS, GET_SEX(vict), "show-player-field",
                 DIAG_CHAR(vict)),
      GET_LEVEL(vict),
      TABLE_NAME(class_abbrevs, NUM_CLASSES, GET_CLASS(vict),
                 "show-player-field", DIAG_CHAR(vict)));
    send_to_char(ch, "Gold: %-8d  Bal: %-8d Exp: %-8d  Align: %-5d  Lessons: %-3d\r\n",
      GET_GOLD(vict), GET_BANK_GOLD(vict), GET_EXP(vict),
      GET_ALIGNMENT(vict), GET_PRACTICES(vict));
    send_to_char(ch, "Started: %-25.25s  Last: %-25.25s\r\n", buf1, buf2);
    send_to_char(ch, "Played: %dh %dm\r\n",
      (int) (vict->player.time.played / 3600),
      (int) (vict->player.time.played / 60 % 60));

    free_char(vict);
    break;
  }
  /* show rent */
  case 3:
    if (!*value) {
      send_to_char(ch, "A name would help.\r\n");
      return;
    }
    Crash_listrent(ch, value);
    break;

  /* show stats */
  case 4:
    i = 0;
    j = 0;
    k = 0;
    con = 0;
    for (vict = character_list; vict; vict = vict->next) {
      if (IS_NPC(vict))
	j++;
      else if (CAN_SEE(ch, vict)) {
	i++;
	if (vict->desc)
	  con++;
      }
    }
    for (obj = object_list; obj; obj = obj->next)
      k++;
    send_to_char(ch,
	"Current stats:\r\n"
	"  %5d players in game  %5d connected\r\n"
	"  %5d registered\r\n"
	"  %5d mobiles          %5d prototypes\r\n"
	"  %5d objects          %5d prototypes\r\n"
	"  %5d rooms            %5d zones\r\n"
  "  %5d triggers         %5d shops\r\n"
  "  %5d large bufs       %5d autoquests\r\n"
	"  %5d buf switches     %5d overflows\r\n"
	"  %5d lists\r\n",
	i, con,
	top_of_p_table + 1,
	j, top_of_mobt + 1,
	k, top_of_objt + 1,
	top_of_world + 1, top_of_zone_table + 1,
	top_of_trigt, top_shop + 1,   /* top_of_trigt is a count, not a top index */
	buf_largecount, total_quests,
	buf_switches, buf_overflows, global_lists->iSize
	);
    break;

  /* show errors */
  case 5:
    SHOW_BEGIN(out, buf, "Errant Rooms\r\n------------\r\n");
    /* A full buffer stops every loop, not just the innermost one. */
    for (i = 0, k = 0; i <= top_of_world && !tb_buf_truncated(&out); i++)
      for (j = 0; j < DIR_COUNT && !tb_buf_truncated(&out); j++) {
      	if (!W_EXIT(i,j))
      	  continue;
        if (W_EXIT(i,j)->to_room == 0)
	    tb_buf_appendf(&out, "%2d: (void   ) [%5d] %-*s%s (%s)\r\n", ++k, GET_ROOM_VNUM(i), count_color_chars(world[i].name)+40, world[i].name, QNRM, dirs[j]);
        if (W_EXIT(i,j)->to_room == NOWHERE && !W_EXIT(i,j)->general_description)
	    tb_buf_appendf(&out, "%2d: (Nowhere) [%5d] %-*s%s (%s)\r\n", ++k, GET_ROOM_VNUM(i), count_color_chars(world[i].name)+ 40, world[i].name, QNRM, dirs[j]);
      }
    show_page(ch, buf, sizeof(buf), tb_buf_truncated(&out), "errors");
    break;

  /* show death */
  case 6:
    SHOW_BEGIN(out, buf, "Death Traps\r\n-----------\r\n");
    for (i = 0, j = 0; i <= top_of_world && !tb_buf_truncated(&out); i++)
      if (ROOM_FLAGGED(i, ROOM_DEATH))
        tb_buf_appendf(&out, "%2d: [%5d] %s%s\r\n", ++j, GET_ROOM_VNUM(i), world[i].name, QNRM);
    show_page(ch, buf, sizeof(buf), tb_buf_truncated(&out), "death");
    break;

  /* show godrooms */
  case 7:
    SHOW_BEGIN(out, buf, "Godrooms\r\n--------------------------\r\n");
    for (i = 0, j = 0; i <= top_of_world && !tb_buf_truncated(&out); i++)
      if (ROOM_FLAGGED(i, ROOM_GODROOM))
        tb_buf_appendf(&out, "%2d: [%5d] %s%s\r\n", ++j, GET_ROOM_VNUM(i), world[i].name, QNRM);
    show_page(ch, buf, sizeof(buf), tb_buf_truncated(&out), "godrooms");
    break;

  /* show shops */
  case 8:
    show_shops(ch, value);
    break;

  /* show houses */
  case 9:
    hcontrol_list_houses(ch, value);
    break;

  /* show snoop */
  case 10:
    i = 0;
    send_to_char(ch, "People currently snooping:\r\n--------------------------\r\n");
    for (d = descriptor_list; d; d = d->next) {
      if (d->snooping == NULL || d->character == NULL)
	continue;
      if (STATE(d) != CON_PLAYING || GET_LEVEL(ch) < GET_LEVEL(d->character))
	continue;
      if (!CAN_SEE(ch, d->character) || IN_ROOM(d->character) == NOWHERE)
	continue;
      i++;
      send_to_char(ch, "%-10s%s - snooped by %s%s.\r\n", GET_NAME(d->snooping->character), QNRM, GET_NAME(d->character), QNRM);
    }
    if (i == 0)
      send_to_char(ch, "No one is currently snooping.\r\n");
    break;

  /* show thaco */
  case 11:
    SHOW_BEGIN(out, buf, "LvL - Mu Cl Th Wa\r\n----------------\r\n");

    for (j = 1; j < LVL_IMMORT && !tb_buf_truncated(&out); j++)
      tb_buf_appendf(&out, "%-3d - %-2d %-2d %-2d %-2d\r\n", j,
				thaco(CLASS_MAGIC_USER, j),
				thaco(CLASS_CLERIC, j),
				thaco(CLASS_THIEF, j),
				thaco(CLASS_WARRIOR, j));

    show_page(ch, buf, sizeof(buf), tb_buf_truncated(&out), "thaco");
    break;

  /* show experience tables */
  case 12:
    SHOW_BEGIN(out, buf, "LvL - Mu     Cl     Th     Wa\r\n--------------------------\r\n");

    for (i = 1; i < LVL_IMMORT && !tb_buf_truncated(&out); i++)
      tb_buf_appendf(&out, "%-3d - %-6d %-6d %-6d %-6d\r\n", i,
				level_exp(CLASS_MAGIC_USER, i) - level_exp(CLASS_MAGIC_USER, i - 1),
				level_exp(CLASS_CLERIC, i) - level_exp(CLASS_CLERIC, i - 1),
				level_exp(CLASS_THIEF, i) - level_exp(CLASS_THIEF, i - 1),
				level_exp(CLASS_WARRIOR, i) - level_exp(CLASS_WARRIOR, i - 1));

    show_page(ch, buf, sizeof(buf), tb_buf_truncated(&out), "exp");
    break;

  case 13:
    SHOW_BEGIN(out, buf, "Colours\r\n--------------------------\r\n");
    k = 0;
    for (r = 0; r < 6 && !tb_buf_truncated(&out); r++)
      for (g = 0; g < 6 && !tb_buf_truncated(&out); g++)
        for (b = 0; b < 6 && !tb_buf_truncated(&out); b++) {
          tb_snprintf(colour, sizeof(colour), "F%u%u%u",
                   (unsigned) r, (unsigned) g, (unsigned) b);
          tb_buf_appendf(&out, "%s%s%s", ColourRGB(ch->desc, colour), colour, ++k % 6 == 0 ? "\tn\r\n" : "    ");
        }
    show_page(ch, buf, sizeof(buf), tb_buf_truncated(&out), "colour");
    break;

  /* show what? */
  default:
    send_to_char(ch, "Sorry, I don't understand that.\r\n");
    break;
  }
}

/* The do_set function */

/* set_fields[].pcnpc: which kinds of character a field applies to. The
 * SET_ prefix keeps these from colliding with other headers; all of the
 * do_set macros are #undef'd after do_set. */
#define SET_PC     1
#define SET_NPC    2
#define SET_BOTH   3

/* set_fields[].type: how perform_set parses the value. */
#define SET_MISC   0
#define SET_BINARY 1
#define SET_NUMBER 2

/* Set or clear flag in flagset per the on/off word perform_set parsed.
 * A statement (do/while), so it is safe in if/else. */
#define SET_OR_REMOVE(flagset, flag) do { \
  if (on) SET_BIT_AR((flagset), (flag)); \
  else if (off) REMOVE_BIT_AR((flagset), (flag)); \
} while (0)

/* Clamp v to low..high, store it back and yield it. v is evaluated several
 * times: pass a plain variable (perform_set's value). The clamped value is
 * what gets stored and echoed. */
#define SET_RANGE(v, low, high) ((v) = MAX((low), MIN((high), (v))))

/* The set options available */
static struct set_struct {
    const char *cmd;
    const char level;
    const char pcnpc;
    const char type;
  } set_fields[] = {
   { "ac",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },  /* 0  */
   { "afk",             LVL_BUILDER,	SET_PC,	SET_BINARY },  /* 1  */
   { "age",		LVL_GOD,	SET_BOTH,	SET_NUMBER },
   { "align",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "bank",		LVL_BUILDER, 	SET_PC, 	SET_NUMBER },
   { "brief",		LVL_GOD, 	SET_PC, 	SET_BINARY },  /* 5  */
   { "cha",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "class",		LVL_BUILDER, 	SET_BOTH, 	SET_MISC },
   { "color",		LVL_GOD, 	SET_PC, 	SET_BINARY },
   { "con", 		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "damroll",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },  /* 10 */
   { "deleted",		LVL_IMPL, 	SET_PC, 	SET_BINARY },
   { "dex", 		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "drunk",		LVL_BUILDER, 	SET_BOTH, 	SET_MISC },
   { "exp", 		LVL_GOD, 	SET_BOTH, 	SET_NUMBER },
   { "frozen",		LVL_GRGOD, 	SET_PC,	SET_BINARY },  /* 15 */
   { "gold",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "height",		LVL_BUILDER,	SET_BOTH,	SET_NUMBER },
   { "hitpoints",       LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "hitroll",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "hunger",		LVL_BUILDER, 	SET_BOTH, 	SET_MISC },    /* 20 */
   { "int", 		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "invis",		LVL_GOD, 	SET_PC, 	SET_NUMBER },
   { "invstart",        LVL_BUILDER,	SET_PC, 	SET_BINARY },
   { "killer",		LVL_GOD, 	SET_PC, 	SET_BINARY },
   { "level",		LVL_GRGOD, 	SET_BOTH, 	SET_NUMBER },  /* 25 */
   { "loadroom",	LVL_BUILDER, 	SET_PC, 	SET_MISC },
   { "mana",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "maxhit",	        LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "maxmana",       	LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "maxmove",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },  /* 30 */
   { "move",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "name",	LVL_IMMORT, 	SET_PC, 	SET_MISC },
   { "nodelete",	LVL_GOD, 	SET_PC, 	SET_BINARY },
   { "nohassle",	LVL_GOD, 	SET_PC, 	SET_BINARY },
   { "nosummon",	LVL_BUILDER,	SET_PC,	SET_BINARY },  /* 35 */
   { "nowizlist", 	LVL_GRGOD, 	SET_PC, 	SET_BINARY },
   { "olc",		LVL_GRGOD,	SET_PC,	SET_MISC },
   { "password",	LVL_GRGOD,	SET_PC,	SET_MISC },
   { "poofin",		LVL_IMMORT,	SET_PC,	SET_MISC },
   { "poofout",         LVL_IMMORT,	SET_PC,	SET_MISC },   /* 40 */
   { "practices", 	LVL_GOD, 	SET_PC, 	SET_NUMBER },
   { "quest",		LVL_GOD, 	SET_PC, 	SET_BINARY },
   { "room",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "screenwidth", LVL_GOD,  SET_PC,   SET_NUMBER },
   { "sex", 		LVL_GOD, 	SET_BOTH, 	SET_MISC },  /* 45 */
   { "showvnums",  LVL_BUILDER,  SET_PC, SET_BINARY },
   { "siteok",   LVL_GOD,  SET_PC,   SET_BINARY },
   { "str",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "stradd",		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER },
   { "thief",		LVL_GOD, 	SET_PC, 	SET_BINARY }, /* 50 */
   { "thirst",		LVL_BUILDER, 	SET_BOTH, 	SET_MISC },
   { "title",		LVL_GOD, 	SET_PC, 	SET_MISC   },
   { "variable",        LVL_GRGOD,	SET_PC,	SET_MISC },
   { "weight",		LVL_BUILDER,	SET_BOTH,	SET_NUMBER },
   { "wis", 		LVL_BUILDER, 	SET_BOTH, 	SET_NUMBER }, /* 55 */
   { "questpoints",     LVL_GOD,        SET_PC,     SET_NUMBER },
   { "questhistory",    LVL_GOD,        SET_PC,   SET_NUMBER },
   { "\n", 0, SET_BOTH, SET_MISC }
  };

/** Apply one 'set' field to vict.
 * @param mode    Index into set_fields[]; do_set has already checked the
 *                field's level and its PC/NPC applicability.
 * @param val_arg The value text from the immortal. NUMBER fields are
 *                parsed with tb_parse_int_or_zero() and clamped by SET_RANGE(value, ),
 *                and the clamped value is what is stored.
 * @return 1 when vict was changed and do_set must save it, 0 when nothing
 *         changed (an error has been sent to ch).
 * vict may be a temporary file copy (do_set's 'file' form): fields with
 * world side effects (room) are refused for it, because the copy is freed
 * without being unlinked. GET_COND and other player_specials writes on an
 * NPC land in the shared dummy_mob specials. */
static int perform_set(struct char_data *ch, struct char_data *vict, int mode, char *val_arg)
{
  int i, on = 0, off = 0, value = 0, qvnum;
  room_rnum rnum;
  room_vnum rvnum;

  /* Check to make sure all the levels are correct */
  if (GET_LEVEL(ch) != LVL_IMPL) {
    if (!IS_NPC(vict) && GET_LEVEL(ch) <= GET_LEVEL(vict) && vict != ch) {
      send_to_char(ch, "Maybe that's not such a great idea...\r\n");
      return (0);
    }
  }
  if (GET_LEVEL(ch) < set_fields[mode].level) {
    send_to_char(ch, "You are not godly enough for that!\r\n");
    return (0);
  }

  /* Make sure the PC/NPC is correct */
  if (IS_NPC(vict) && !(set_fields[mode].pcnpc & SET_NPC)) {
    send_to_char(ch, "You can't do that to a beast!\r\n");
    return (0);
  } else if (!IS_NPC(vict) && !(set_fields[mode].pcnpc & SET_PC)) {
    send_to_char(ch, "That can only be done to a beast!\r\n");
    return (0);
  }

  /* Find the value of the argument */
  if (set_fields[mode].type == SET_BINARY) {
    if (!strcmp(val_arg, "on") || !strcmp(val_arg, "yes"))
      on = 1;
    else if (!strcmp(val_arg, "off") || !strcmp(val_arg, "no"))
      off = 1;
    if (!(on || off)) {
      send_to_char(ch, "Value must be 'on' or 'off'.\r\n");
      return (0);
    }
  } else if (set_fields[mode].type == SET_NUMBER) {
    value = tb_parse_int_or_zero(val_arg);
  }
  switch (mode) {
    case 0: /* ac */
      vict->points.armor = SET_RANGE(value, -100, 100);
      affect_total(vict);
      break;
    case 1: /* afk */
      SET_OR_REMOVE(PRF_FLAGS(vict), PRF_AFK);
      break;
    case 2: /* age */
      if (value < 2 || value > 200) {	/* Arbitrary limits. */
        send_to_char(ch, "Ages 2 to 200 accepted.\r\n");
        return (0);
      }
      /* NOTE: May not display the exact age specified due to the integer
       * division used elsewhere in the code.  Seems to only happen for
       * some values below the starting age (17) anyway. -gg 5/27/98 */
      vict->player.time.birth = time(0) - ((value - 17) * SECS_PER_MUD_YEAR);
      break;
    case 3: /* align */
      GET_ALIGNMENT(vict) = SET_RANGE(value, -1000, 1000);
      affect_total(vict);
      break;
    case 4: /* bank */
      GET_BANK_GOLD(vict) = SET_RANGE(value, 0, 100000000);
      break;
    case 5: /* brief */
      SET_OR_REMOVE(PRF_FLAGS(vict), PRF_BRIEF);
      break;
    case 6:  /* cha */
      if (IS_NPC(vict) || GET_LEVEL(vict) >= LVL_GRGOD)
        SET_RANGE(value, 3, 25);
      else
        SET_RANGE(value, 3, 18);
      vict->real_abils.cha = value;
      affect_total(vict);
      break;
    case 7: /* class */
      if ((i = parse_class(*val_arg)) == CLASS_UNDEFINED) {
        send_to_char(ch, "That is not a class.\r\n");
        return (0);
      }
      GET_CLASS(vict) = i;
      break;
    case 8:  /* color */
      SET_OR_REMOVE(PRF_FLAGS(vict), (PRF_COLOR_1));
      SET_OR_REMOVE(PRF_FLAGS(vict), (PRF_COLOR_2));
      break;
    case 9: /* con */
      if (IS_NPC(vict) || GET_LEVEL(vict) >= LVL_GRGOD)
        SET_RANGE(value, 3, 25);
      else
        SET_RANGE(value, 3, 18);
      vict->real_abils.con = value;
      affect_total(vict);
      break;
    case 10:  /* damroll */
      vict->points.damroll = SET_RANGE(value, -20, 20);
      affect_total(vict);
      break;
    case 11: /* delete */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_DELETED);
      break;
    case 12: /* dex */
      if (IS_NPC(vict) || GET_LEVEL(vict) >= LVL_GRGOD)
        SET_RANGE(value, 3, 25);
      else
        SET_RANGE(value, 3, 18);
      vict->real_abils.dex = value;
      affect_total(vict);
      break;
    case 13: /* drunk */
      if (!str_cmp(val_arg, "off")) {
        GET_COND(vict, DRUNK) = -1;
        send_to_char(ch, "%s's drunkenness is now off.\r\n", GET_NAME(vict));
      } else if (is_number(val_arg)) {
        value = tb_parse_int_or_zero(val_arg);
        SET_RANGE(value, 0, 24);
        GET_COND(vict, DRUNK) = value;
        send_to_char(ch, "%s's drunkenness set to %d.\r\n", GET_NAME(vict), value);
      } else {
        send_to_char(ch, "Must be 'off' or a value from 0 to 24.\r\n");
        return (0);
      }
      break;
    case 14: /* exp */
      vict->points.exp = SET_RANGE(value, 0, 50000000);
      break;
    case 15: /* frozen */
      if (ch == vict && on) {
        send_to_char(ch, "Better not -- could be a long winter!\r\n");
        return (0);
      }
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_FROZEN);
      break;
    case 16: /* gold */
      GET_GOLD(vict) = SET_RANGE(value, 0, 100000000);
      break;
    case 17: /* height */
      GET_HEIGHT(vict) = value;
      affect_total(vict);
      break;
    case 18: /* hit */
      vict->points.hit = SET_RANGE(value, -9, vict->points.max_hit);
      affect_total(vict);
      break;
    case 19: /* hitroll */
      vict->points.hitroll = SET_RANGE(value, -20, 20);
      affect_total(vict);
      break;
    case 20: /* hunger */
      if (!str_cmp(val_arg, "off")) {
        GET_COND(vict, HUNGER) = -1;
        send_to_char(ch, "%s's hunger is now off.\r\n", GET_NAME(vict));
      } else if (is_number(val_arg)) {
        value = tb_parse_int_or_zero(val_arg);
        SET_RANGE(value, 0, 24);
        GET_COND(vict, HUNGER) = value;
        send_to_char(ch, "%s's hunger set to %d.\r\n", GET_NAME(vict), value);
      } else {
        send_to_char(ch, "Must be 'off' or a value from 0 to 24.\r\n");
        return (0);
       }
       break;
   case 21: /* int */
      if (IS_NPC(vict) || GET_LEVEL(vict) >= LVL_GRGOD)
        SET_RANGE(value, 3, 25);
      else
        SET_RANGE(value, 3, 18);
      vict->real_abils.intel = value;
      affect_total(vict);
      break;
    case 22: /* invis */
      if (GET_LEVEL(ch) < LVL_IMPL && ch != vict) {
        send_to_char(ch, "You aren't godly enough for that!\r\n");
        return (0);
      }
      GET_INVIS_LEV(vict) = SET_RANGE(value, 0, GET_LEVEL(vict));
      break;
    case 23: /* invistart */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_INVSTART);
      break;
    case 24: /* killer */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_KILLER);
      break;
    case 25: /* level */
      if ((!IS_NPC(vict) && value > GET_LEVEL(ch)) || value > LVL_IMPL) {
        send_to_char(ch, "You can't do that.\r\n");
        return (0);
      }
      SET_RANGE(value, 1, LVL_IMPL);
      vict->player.level = value;
      snoop_check(vict);   /* drop snoops the new level no longer allows */
      break;
    case 26: /* loadroom */
      if (!str_cmp(val_arg, "off")) {
        REMOVE_BIT_AR(PLR_FLAGS(vict), PLR_LOADROOM);
      } else if (is_number(val_arg)) {
        if (!parse_vnum_arg(ch, val_arg, "room", &rvnum))
          return (0);
        if (real_room(rvnum) != NOWHERE) {
          SET_BIT_AR(PLR_FLAGS(vict), PLR_LOADROOM);
          GET_LOADROOM(vict) = rvnum;
          send_to_char(ch, "%s will enter at room #%d.\r\n", GET_NAME(vict), GET_LOADROOM(vict));
        } else {
        send_to_char(ch, "That room does not exist!\r\n");
	  return (0);
        }
      } else {
        send_to_char(ch, "Must be 'off' or a room's virtual number.\r\n");
        return (0);
      }
      break;
    case 27: /* mana */
      vict->points.mana = SET_RANGE(value, 0, vict->points.max_mana);
      affect_total(vict);
      break;
    case 28: /* maxhit */
      vict->points.max_hit = SET_RANGE(value, 1, 5000);
      affect_total(vict);
      break;
    case 29: /* maxmana */
      vict->points.max_mana = SET_RANGE(value, 1, 5000);
      affect_total(vict);
      break;
    case 30: /* maxmove */
      vict->points.max_move = SET_RANGE(value, 1, 5000);
      affect_total(vict);
      break;
    case 31: /* move */
      vict->points.move = SET_RANGE(value, 0, vict->points.max_move);
      affect_total(vict);
      break;
    case 32: /* name */
      if (ch != vict && GET_LEVEL(ch) < LVL_IMPL) {
        send_to_char(ch, "Only Imps can change the name of other players.\r\n");
        return (0);
      }
      if (!change_player_name(ch, vict, val_arg)) {
        send_to_char(ch, "Name has not been changed!\r\n");
        return (0);
      }
      break;
    case 33: /* nodelete */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_NODELETE);
      break;
    case 34: /* nohassle */
      if (GET_LEVEL(ch) < LVL_GOD && ch != vict) {
        send_to_char(ch, "You aren't godly enough for that!\r\n");
        return (0);
      }
      SET_OR_REMOVE(PRF_FLAGS(vict), PRF_NOHASSLE);
      break;
    case 35: /* nosummon */
      SET_OR_REMOVE(PRF_FLAGS(vict), PRF_SUMMONABLE);
      send_to_char(ch, "Nosummon %s for %s.\r\n", ONOFF(!on), GET_NAME(vict));
      break;
    case 36: /* nowiz */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_NOWIZLIST);
      break;
    case 37: /* olc */
      if (is_abbrev(val_arg, "socials") || is_abbrev(val_arg, "actions") || is_abbrev(val_arg, "aedit"))
        GET_OLC_ZONE(vict) = AEDIT_PERMISSION;
      else if (is_abbrev(val_arg, "hedit") || is_abbrev(val_arg, "help"))
        GET_OLC_ZONE(vict) = HEDIT_PERMISSION;
      else if (*val_arg == '*' || is_abbrev(val_arg, "all"))
        GET_OLC_ZONE(vict) = ALL_PERMISSION;
      else if (is_abbrev(val_arg, "off"))
        GET_OLC_ZONE(vict) = NOWHERE;
      else if (!is_number(val_arg))  {
        send_to_char(ch, "Value must be a zone number, 'aedit', 'hedit', 'off' or 'all'.\r\n");
        return (0);
      } else
        GET_OLC_ZONE(vict) = tb_parse_int_or_zero(val_arg);
      break;
    case 38: /* password */
      if (GET_LEVEL(vict) >= LVL_GRGOD) {
        send_to_char(ch, "You cannot change that.\r\n");
        return (0);
      }
      strncpy(GET_PASSWD(vict), CRYPT(val_arg, GET_NAME(vict)), MAX_PWD_LENGTH);	/* strncpy: OK (G_P:MAX_PWD_LENGTH) */
      *(GET_PASSWD(vict) + MAX_PWD_LENGTH) = '\0';
      send_to_char(ch, "Password changed to '%s'.\r\n", val_arg);
      break;
    case 39: /* poofin */
      if ((vict == ch) || (GET_LEVEL(ch) == LVL_IMPL)) {
        skip_spaces(&val_arg);
        parse_at(val_arg);

        if (POOFIN(vict))
          free(POOFIN(vict));

      if (!*val_arg)
          POOFIN(vict) = NULL;
        else
          POOFIN(vict) = strdup(val_arg);
      } else {
        /* Was a silent no-op that still answered "Okay." and saved. */
        send_to_char(ch, "Only Imps can set another player's poofin.\r\n");
        return (0);
      }
      break;
    case 40: /* poofout */
      if ((vict == ch) || (GET_LEVEL(ch) == LVL_IMPL)) {
        skip_spaces(&val_arg);
        parse_at(val_arg);

        if (POOFOUT(vict))
          free(POOFOUT(vict));

	if (!*val_arg)
          POOFOUT(vict) = NULL;
        else
          POOFOUT(vict) = strdup(val_arg);
      } else {
        /* Was a silent no-op that still answered "Okay." and saved. */
        send_to_char(ch, "Only Imps can set another player's poofout.\r\n");
        return (0);
      }
      break;
    case 41: /* practices */
      GET_PRACTICES(vict) = SET_RANGE(value, 0, 100);
      break;
    case 42: /* quest */
      SET_OR_REMOVE(PRF_FLAGS(vict), PRF_QUEST);
      break;
    case 43: /* room */
      if (!parse_vnum_arg(ch, val_arg, "room", &rvnum))
        return (0);
      if ((rnum = real_room(rvnum)) == NOWHERE) {
        send_to_char(ch, "No room exists with that number.\r\n");
        return (0);
      }
      if (IN_ROOM(vict) != NOWHERE)
        char_from_room(vict);
      char_to_room(vict, rnum);
      break;
    case 44: /* screenwidth */
      GET_SCREEN_WIDTH(vict) = SET_RANGE(value, 40, 200);
      break;
    case 45: /* sex */
      if ((i = search_block(val_arg, genders, FALSE)) < 0) {
        send_to_char(ch, "Must be 'male', 'female', or 'neutral'.\r\n");
        return (0);
      }
      GET_SEX(vict) = i;
      break;
    case 46: /* showvnums */
      SET_OR_REMOVE(PRF_FLAGS(vict), PRF_SHOWVNUMS);
      break;
    case 47: /* siteok */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_SITEOK);
      break;
    case 48: /* str */
      if (IS_NPC(vict) || GET_LEVEL(vict) >= LVL_GRGOD)
        SET_RANGE(value, 3, 25);
      else
        SET_RANGE(value, 3, 18);
      vict->real_abils.str = value;
      vict->real_abils.str_add = 0;
      affect_total(vict);
      break;
    case 49: /* stradd */
      vict->real_abils.str_add = SET_RANGE(value, 0, 100);
      if (value > 0)
        vict->real_abils.str = 18;
      affect_total(vict);
      break;
    case 50: /* thief */
      SET_OR_REMOVE(PLR_FLAGS(vict), PLR_THIEF);
      break;
    case 51: /* thirst */
      if (!str_cmp(val_arg, "off")) {
        GET_COND(vict, THIRST) = -1;
        send_to_char(ch, "%s's thirst is now off.\r\n", GET_NAME(vict));
      } else if (is_number(val_arg)) {
        value = tb_parse_int_or_zero(val_arg);
        SET_RANGE(value, 0, 24);
        GET_COND(vict, THIRST) = value;
        send_to_char(ch, "%s's thirst set to %d.\r\n", GET_NAME(vict), value);
      } else {
        send_to_char(ch, "Must be 'off' or a value from 0 to 24.\r\n");
        return (0);
      }
      break;
    case 52: /* title */
      set_title(vict, val_arg);
      send_to_char(ch, "%s's title is now: %s\r\n", GET_NAME(vict), GET_TITLE(vict));
      break;
    case 53: /* variable */
      return perform_set_dg_var(ch, vict, val_arg);
    case 54: /* weight */
      GET_WEIGHT(vict) = value;
      affect_total(vict);
      break;
    case 55: /* wis */
      if (IS_NPC(vict) || GET_LEVEL(vict) >= LVL_GRGOD)
        SET_RANGE(value, 3, 25);
      else
        SET_RANGE(value, 3, 18);
      vict->real_abils.wis = value;
      affect_total(vict);
      break;
    case 56: /* questpoints */
      GET_QUESTPOINTS(vict) = SET_RANGE(value, 0, 100000000);
      break;
    case 57: /* questhistory */
      qvnum = tb_parse_int_or_zero(val_arg);
      if (real_quest(qvnum) == NOTHING) {
        send_to_char(ch, "That quest doesn't exist.\r\n");
        return FALSE;
      } else {
        if (is_complete(vict, qvnum)) {
          remove_completed_quest(vict, qvnum);
          send_to_char(ch, "Quest %d removed from history for player %s.\r\n",
     qvnum, GET_NAME(vict));
        } else {
          add_completed_quest(vict, qvnum);
          send_to_char(ch, "Quest %d added to history for player %s.\r\n",
     qvnum, GET_NAME(vict));
        }
        break;
      }
    default:
      send_to_char(ch, "Can't set that!\r\n");
      return (0);
    }
  /* Show the new value of the variable */
  if (set_fields[mode].type == SET_BINARY) {
    send_to_char(ch, "%s %s for %s.\r\n", set_fields[mode].cmd, ONOFF(on), GET_NAME(vict));
  } else if (set_fields[mode].type == SET_NUMBER) {
    send_to_char(ch, "%s's %s set to %d.\r\n", GET_NAME(vict), set_fields[mode].cmd, value);
  } else
    send_to_char(ch, "%s", CONFIG_OK);

  return (1);
}

static void show_set_help(struct char_data *ch)
{
  const char *set_levels[] = {"Imm", "God", "GrGod", "IMP"};
  const char *set_targets[] = {"PC", "NPC", "BOTH"};
  const char *set_types[] = {"MISC", "BINARY", "NUMBER"};
  char buf[MAX_STRING_LENGTH];
  int i, len=0, add_len=0;

  len = snprintf(buf, sizeof(buf), "%sCommand             Lvl    Who?  Type%s\r\n", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM));
  for (i = 0; *(set_fields[i].cmd) != '\n'; i++) {
	if (set_fields[i].level <= GET_LEVEL(ch)) {
      add_len = snprintf(buf+len, sizeof(buf)-len, "%-20s%-5s  %-4s  %-6s\r\n", set_fields[i].cmd,
                                        set_levels[((int)(set_fields[i].level) - LVL_IMMORT)],
                                        set_targets[(int)(set_fields[i].pcnpc)-1],
                                        set_types[(int)(set_fields[i].type)]);
      len += add_len;
    }
  }
  page_string(ch->desc, buf, TRUE);
}

ACMD(do_set)
{
  struct char_data *vict = NULL, *cbuf = NULL;
  char field[MAX_INPUT_LENGTH], name[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH];
  int mode, len, player_i = 0, retval;
  char is_file = 0, is_player = 0;

  half_chop(argument, name, buf);

  if (!strcmp(name, "file")) {
    is_file = 1;
    half_chop(buf, name, buf);
  } else if (!str_cmp(name, "help")) {
    show_set_help(ch);
    return;
  } else if (!str_cmp(name, "player")) {
    is_player = 1;
    half_chop(buf, name, buf);
  } else if (!str_cmp(name, "mob"))
    half_chop(buf, name, buf);

  half_chop(buf, field, buf);

  if (!*name || !*field) {
    send_to_char(ch, "Usage: set <victim> <field> <value>\r\n");
    send_to_char(ch, "       %sset help%s will display valid fields\r\n", CCYEL(ch, C_NRM), CCNRM(ch, C_NRM));
    return;
  }

  /* find the target */
  if (!is_file) {
    if (is_player) {
      if (!(vict = get_player_vis(ch, name, NULL, FIND_CHAR_WORLD))) {
	send_to_char(ch, "There is no such player.\r\n");
	return;
      }
    } else { /* is_mob */
      if (!(vict = get_char_vis(ch, name, NULL, FIND_CHAR_WORLD))) {
	send_to_char(ch, "There is no such creature.\r\n");
	return;
      }
    }
  } else if (is_file) {
    /* try to load the player off disk */
    CREATE(cbuf, struct char_data, 1);
    clear_char(cbuf);
    CREATE(cbuf->player_specials, struct player_special_data, 1);
    new_mobile_data(cbuf);
    if ((player_i = load_char(name, cbuf)) > -1) {
      if (GET_LEVEL(cbuf) > GET_LEVEL(ch)) {
	free_char(cbuf);
	send_to_char(ch, "Sorry, you can't do that.\r\n");
	return;
      }
      vict = cbuf;
    } else {
      free_char(cbuf);
      send_to_char(ch, "There is no such player.\r\n");
      return;
    }
  }

  /* find the command in the list */
  len = strlen(field);
  for (mode = 0; *(set_fields[mode].cmd) != '\n'; mode++)
    if (!strncmp(field, set_fields[mode].cmd, len))
      break;

  if (*(set_fields[mode].cmd) == '\n') {
    retval = 0; /* skips saving below */
    send_to_char(ch, "Can't set that!\r\n");
  } else if (is_file && !strcmp(set_fields[mode].cmd, "room")) {
    /* Invariant: a file-loaded player (cbuf) is a temporary copy with no
     * world linkage, freed with free_char() below. free_char() does not
     * unlink from world[].people, so placing cbuf in a room would leave a
     * dangling occupant (use-after-free on the next look/goto). The caller
     * is an immortal; the target name comes from them. */
    TB_WARN_MSG("act", "set-file-room", set_fields[mode].cmd,
                "player idnum %ld: room needs an in-game character; caller "
                DIAG_CHAR_FMT, GET_IDNUM(cbuf), DIAG_CHAR(ch));
    send_to_char(ch, "A player loaded from file has no room; use 'set file <name> loadroom <vnum>'.\r\n");
    retval = 0;
  } else
  /* perform the set */
  retval = perform_set(ch, vict, mode, buf);

  /* save the character if a change was made */
  if (retval) {
    if (!is_file && !IS_NPC(vict))
      save_char(vict);
    if (is_file) {
      GET_PFILEPOS(cbuf) = player_i;
      save_char(cbuf);
      send_to_char(ch, "Saved in file.\r\n");
    }
  }

  /* free the memory if we allocated it earlier */
  if (is_file) {
    /* Defense in depth: free_char() leaves world[].people untouched, so a
     * temp char that somehow entered a room must leave it first. */
    if (IN_ROOM(cbuf) != NOWHERE) {
      TB_ERROR_MSG("act", "set-file-free", "cbuf",
                   "player idnum %ld still in room vnum %d before free",
                   GET_IDNUM(cbuf), GET_ROOM_VNUM(IN_ROOM(cbuf)));
      char_from_room(cbuf);
    }
    free_char(cbuf);
  }
}

#undef SET_PC
#undef SET_NPC
#undef SET_BOTH
#undef SET_MISC
#undef SET_BINARY
#undef SET_NUMBER
#undef SET_OR_REMOVE
#undef SET_RANGE

ACMD(do_saveall)
{
 if (GET_LEVEL(ch) < LVL_BUILDER)
    send_to_char (ch, "You are not holy enough to use this privelege.\n\r");
 else {
    save_all();
    House_save_all();
    send_to_char(ch, "World and house files saved.\n\r");
 }
}

ACMD(do_links)
{
  zone_rnum zrnum;
  zone_vnum zvnum;
  room_rnum nr, to_room;
  int first, last, j;
  char arg[MAX_INPUT_LENGTH];


  skip_spaces(&argument);
  one_argument(argument, arg);

  if (!is_number(arg)) {
    zrnum = world[IN_ROOM(ch)].zone;
    zvnum = zone_table[zrnum].number;
  } else {
    if (!parse_vnum_arg(ch, arg, "zone", &zvnum))
      return;
    zrnum = real_zone(zvnum);
  }

  if (zrnum == NOWHERE || zvnum == NOWHERE) {
    send_to_char(ch, "No zone was found with that number.\n\r");
    return;
  }

  last  = zone_table[zrnum].top;
  first = zone_table[zrnum].bot;

  send_to_char(ch, "Zone %d is linked to the following zones:\r\n", zvnum);
  for (nr = 0; nr <= top_of_world && (GET_ROOM_VNUM(nr) <= last); nr++) {
    if (GET_ROOM_VNUM(nr) >= first) {
      for (j = 0; j < DIR_COUNT; j++) {
        if (world[nr].dir_option[j]) {
          to_room = world[nr].dir_option[j]->to_room;
          if (to_room != NOWHERE && (zrnum != world[to_room].zone))
          send_to_char(ch, "%3d %-30s at %5d (%-5s) ---> %5d\r\n",
                       zone_table[world[to_room].zone].number,
                       zone_table[world[to_room].zone].name,
                       GET_ROOM_VNUM(nr), dirs[j], world[to_room].number);
        }
      }
    }
  }
}

/* Zone Checker Code below */
/*mob limits*/
#define MAX_DAMROLL_ALLOWED      MAX(GET_LEVEL(mob)/5, 1)
#define MAX_HITROLL_ALLOWED      MAX(GET_LEVEL(mob)/3, 1)
#define MAX_MOB_GOLD_ALLOWED     GET_LEVEL(mob)*3000
#define MAX_EXP_ALLOWED          GET_LEVEL(mob)*GET_LEVEL(mob) * 120
#define MAX_LEVEL_ALLOWED        LVL_IMPL
#define GET_OBJ_AVG_DAM(obj)     (((GET_OBJ_VAL(obj, 2) + 1) / 2.0) * GET_OBJ_VAL(obj, 1))
/* arbitrary limit for per round dam */
#define MAX_MOB_DAM_ALLOWED      500

#define ZCMD2 zone_table[zone].cmd[cmd_no]  /*fom DB.C*/

/* Room name for display from a zone command's room rnum. The checkload
 * listings trust renum_zone_table() to have disabled bad commands, but an
 * 'O' whose room is literally NOWHERE survives renum (db.c). rnum is
 * evaluated up to three times: pass a plain value. GET_ROOM_VNUM is the
 * matching checked accessor for the number. */
#define ROOM_NAME_SAFE(rnum) \
  ((VALID_ROOM_RNUM(rnum) && world[(rnum)].name) ? world[(rnum)].name : "<invalid room>")

/*item limits*/
#define MAX_DAM_ALLOWED            50    /* for weapons  - avg. dam*/
#define MAX_AFFECTS_ALLOWED        3
#define MAX_OBJ_GOLD_ALLOWED       1000000

/* Armor class limits*/
#define TOTAL_WEAR_CHECKS  (NUM_ITEM_WEARS-2)  /*minus Wield and Take*/
static struct zcheck_armor {
  bitvector_t bitvector;          /* from Structs.h                       */
  int ac_allowed;                 /* Max. AC allowed for this body part  */
  char *message;                  /* phrase for error message            */
} zarmor[] = {
  {ITEM_WEAR_FINGER, 10, "Ring"},
  {ITEM_WEAR_NECK,   10, "Necklace"},
  {ITEM_WEAR_BODY,   10, "Body armor"},
  {ITEM_WEAR_HEAD,   10, "Head gear"},
  {ITEM_WEAR_LEGS,   10, "Legwear"},
  {ITEM_WEAR_FEET,   10, "Footwear"},
  {ITEM_WEAR_HANDS,  10, "Glove"},
  {ITEM_WEAR_ARMS,   10, "Armwear"},
  {ITEM_WEAR_SHIELD, 10, "Shield"},
  {ITEM_WEAR_ABOUT,  10, "Cloak"},
  {ITEM_WEAR_WAIST,  10, "Belt"},
  {ITEM_WEAR_WRIST,  10, "Wristwear"},
  {ITEM_WEAR_HOLD,   10, "Held item"}
};

/* Applies limits !! Very Important:  Keep these in the same order as in Structs.h.
 * To ignore an apply, set max_aff to -99. These will be ignored if MAX_APPLIES_LIMIT = 0 */
static struct zcheck_affs {
  int aff_type;    /*from Structs.h*/
  int min_aff;     /*min. allowed value*/
  int max_aff;     /*max. allowed value*/
  char *message;   /*phrase for error message*/
} zaffs[] = {
  {APPLY_NONE,         0, -99, "unused0"},
  {APPLY_STR,         -5,   3, "strength"},
  {APPLY_DEX,         -5,   3, "dexterity"},
  {APPLY_INT,         -5,   3, "intelligence"},
  {APPLY_WIS,         -5,   3, "wisdom"},
  {APPLY_CON,         -5,   3, "constitution"},
  {APPLY_CHA,         -5,   3, "charisma"},
  {APPLY_CLASS,        0,   0, "class"},
  {APPLY_LEVEL,        0,   0, "level"},
  {APPLY_AGE,        -10,  10, "age"},
  {APPLY_CHAR_WEIGHT,-50,  50, "character weight"},
  {APPLY_CHAR_HEIGHT,-50,  50, "character height"},
  {APPLY_MANA,       -50,  50, "mana"},
  {APPLY_HIT,        -50,  50, "hit points"},
  {APPLY_MOVE,       -50,  50, "movement"},
  {APPLY_GOLD,         0,   0, "gold"},
  {APPLY_EXP,          0,   0, "experience"},
  {APPLY_AC,         -10,  10, "magical AC"},
  {APPLY_HITROLL,      0, -99, "hitroll"},       /* Handled seperately below */
  {APPLY_DAMROLL,      0, -99, "damroll"},       /* Handled seperately below */
  {APPLY_SAVING_PARA, -2,   2, "saving throw (paralysis)"},
  {APPLY_SAVING_ROD,  -2,   2, "saving throw (rod)"},
  {APPLY_SAVING_PETRI,-2,   2, "saving throw (death)"},
  {APPLY_SAVING_BREATH,-2,  2, "saving throw (breath)"},
  {APPLY_SAVING_SPELL,-2,   2, "saving throw (spell)"}
};

/* zaffs[] is indexed by APPLY_* location: one entry per apply. A new
 * APPLY_* without a zaffs entry fails the build here (C99 has no
 * _Static_assert, so a negative array size does the job). */
typedef char zaffs_needs_one_entry_per_apply
  [(TB_ARRAY_LEN(zaffs) == NUM_APPLIES) ? 1 : -1];

/* These are ABS() values. */
#define MAX_APPLY_HITROLL_TOTAL   5
#define MAX_APPLY_DAMROLL_TOTAL   5

/*room limits*/
/* Off limit zones are any zones a player should NOT be able to walk to (ex. Limbo) */
static const int offlimit_zones[] = {0,12,13,14,-1};  /*what zones can no room connect to (virtual num) */
#define MIN_ROOM_DESC_LENGTH   80       /* at least one line - set to 0 to not care. */
#define MAX_COLOUMN_WIDTH      80       /* at most 80 chars per line */

/** Send one zcheck entity report to ch. If the report filled its buffer,
 * the tail is missing: say so in game and log zcheck-truncate with the
 * entity kind and vnum and the zone vnum. */
static void zcheck_send(struct char_data *ch, const struct tb_buf *report,
                        const char *kind, int vnum, zone_rnum zrnum)
{
  send_to_char(ch, "%s", report->data);
  if (tb_buf_truncated(report)) {
    TB_WARN_MSG("act", "zcheck-truncate", kind,
                "zone vnum %d %s vnum %d: report truncated at %zu bytes",
                zone_table[zrnum].number, kind, vnum, report->capacity);
    send_to_char(ch, "*** report truncated ***\r\n");
  }
}
#define ZCHECK_SEND(ch, report, kind, vnum, zrnum) \
  zcheck_send((ch), &(report), (kind), (vnum), (zrnum))

ACMD (do_zcheck)
{
  zone_rnum zrnum;
  struct obj_data *obj;
  struct char_data *mob = NULL;
  room_vnum exroom=0;
  int ac=0;
  int affs=0, tohit, todam, value;
  int i = 0, j = 0, k = 0, l = 0, m = 0, found = 0; /* found is used as a 'send now' flag*/
  char arg[MAX_INPUT_LENGTH];
  /* One entity's findings. tb_buf keeps every append inside storage; the
   * open-coded 'len += snprintf' it replaces wrapped sizeof - len once a
   * report grew past the buffer (G1-03). */
  char storage[MAX_STRING_LENGTH];
  struct tb_buf report;
  float avg_dam;
  struct extra_descr_data *ext, *ext2;

  tb_buf_init(&report, storage, sizeof(storage));
  one_argument(argument, arg);

  if (!is_number(arg) || !strcmp(arg, "."))
    zrnum = world[IN_ROOM(ch)].zone;
  else {
    zone_vnum zvnum;

    if (!parse_vnum_arg(ch, arg, "zone", &zvnum))
      return;
    zrnum = real_zone(zvnum);
  }

  if (zrnum == NOWHERE) {
    send_to_char(ch, "Check what zone ?\r\n");
    return;
  } else
    send_to_char(ch, "Checking zone %d!\r\n", zone_table[zrnum].number);

 /* Check mobs */

  send_to_char(ch, "Checking Mobs for limits...\r\n");
  /*check mobs first*/
  for (i=0; i <= top_of_mobt;i++) {
      if (real_zone_by_thing(mob_index[i].vnum) == zrnum) {  /*is mob in this zone?*/
        mob = &mob_proto[i];
        /* Mob strings may be NULL when the file text was empty. */
        if (mob->player.name && !strcmp(mob->player.name, "mob unfinished") && (found=1))
          tb_buf_appendf(&report,
                          "- Alias hasn't been set.\r\n");

        if (mob->player.short_descr && !strcmp(mob->player.short_descr, "the unfinished mob") && (found=1))
          tb_buf_appendf(&report,
                          "- Short description hasn't been set.\r\n");

        if (mob->player.long_descr && !strncmp(mob->player.long_descr, "An unfinished mob stands here.", 30) && (found=1))
          tb_buf_appendf(&report,
                          "- Long description hasn't been set.\r\n");

        if (mob->player.description && *mob->player.description) {
          if (!strncmp(mob->player.description, "It looks unfinished.", 20) && (found=1))
            tb_buf_appendf(&report,
                            "- Description hasn't been set.\r\n");
          else if (strncmp(mob->player.description, "   ", 3) && (found=1))
            tb_buf_appendf(&report,
                            "- Description hasn't been formatted. (/fi)\r\n");
        }

        if (GET_LEVEL(mob)>MAX_LEVEL_ALLOWED && (found=1))
          tb_buf_appendf(&report,
                          "- Is level %d (limit: 1-%d)\r\n",
                          GET_LEVEL(mob), MAX_LEVEL_ALLOWED);

        if (GET_DAMROLL(mob)>MAX_DAMROLL_ALLOWED && (found=1))
          tb_buf_appendf(&report,
                          "- Damroll of %d is too high (limit: %d)\r\n",
                          GET_DAMROLL(mob), MAX_DAMROLL_ALLOWED);

        if (GET_HITROLL(mob)>MAX_HITROLL_ALLOWED && (found=1))
          tb_buf_appendf(&report,
                          "- Hitroll of %d is too high (limit: %d)\r\n",
                          GET_HITROLL(mob), MAX_HITROLL_ALLOWED);

        /* avg. dam including damroll per round of combat */
        avg_dam = (((mob->mob_specials.damsizedice / 2.0) * mob->mob_specials.damnodice)+GET_DAMROLL(mob));
        if (avg_dam>MAX_MOB_DAM_ALLOWED && (found=1))
          tb_buf_appendf(&report,
                          "- average damage of %4.1f is too high (limit: %d)\r\n",
                          avg_dam, MAX_MOB_DAM_ALLOWED);

        if (mob->mob_specials.damsizedice == 1 &&
            mob->mob_specials.damnodice == 1 &&
            GET_LEVEL(mob) == 0 &&
            (found=1))
          tb_buf_appendf(&report,
                          "- Needs to be fixed - %sAutogenerate!%s\r\n", CCYEL(ch, C_NRM), CCNRM(ch, C_NRM));

        if (MOB_FLAGGED(mob, MOB_AGGRESSIVE) && (MOB_FLAGGED(mob, MOB_AGGR_GOOD) || MOB_FLAGGED(mob, MOB_AGGR_EVIL) || MOB_FLAGGED(mob, MOB_AGGR_NEUTRAL)) && (found=1))
	 tb_buf_appendf(&report,
          "- Both aggresive and agressive to align.\r\n");

        if ((GET_GOLD(mob) > MAX_MOB_GOLD_ALLOWED) && (found=1))
          tb_buf_appendf(&report,
                          "- Set to %d Gold (limit : %d).\r\n",
                                  GET_GOLD(mob),
                                  MAX_MOB_GOLD_ALLOWED);

        if (GET_EXP(mob)>MAX_EXP_ALLOWED && (found=1))
          tb_buf_appendf(&report,
                          "- Has %d experience (limit: %d)\r\n",
                              GET_EXP(mob), MAX_EXP_ALLOWED);
        if ((AFF_FLAGGED(mob, AFF_CHARM) || AFF_FLAGGED(mob, AFF_POISON)) && (found = 1))
	  tb_buf_appendf(&report,
                          "- Has illegal affection bits set (%s %s)\r\n",
                              AFF_FLAGGED(mob, AFF_CHARM) ? "CHARM" : "",
                              AFF_FLAGGED(mob, AFF_POISON) ? "POISON" : "");


        if (!MOB_FLAGGED(mob, MOB_SENTINEL) && !MOB_FLAGGED(mob, MOB_STAY_ZONE) && (found = 1))
          tb_buf_appendf(&report,
                            "- Neither SENTINEL nor STAY_ZONE bits set.\r\n");

        if (MOB_FLAGGED(mob, MOB_SPEC) && (found = 1))
          tb_buf_appendf(&report,
                            "- SPEC flag needs to be removed.\r\n");

        /* Additional mob checks.*/
        if (found) {
          send_to_char(ch,
                  "%s[%5d]%s %-30s: %s\r\n",
                  CCCYN(ch, C_NRM), GET_MOB_VNUM(mob),
                  CCYEL(ch, C_NRM), GET_NAME(mob),
                  CCNRM(ch, C_NRM));
          ZCHECK_SEND(ch, report, "mob", GET_MOB_VNUM(mob), zrnum);
        }
        /* reset buffers and found flag */
        tb_buf_init(&report, storage, sizeof(storage));
        found = 0;
      }   /* mob is in zone */
    }  /* check mobs */

 /* Check objects */
  send_to_char(ch, "\r\nChecking Objects for limits...\r\n");
  for (i=0; i <= top_of_objt; i++) {
    if (real_zone_by_thing(obj_index[i].vnum) == zrnum) { /*is object in this zone?*/
      obj = &obj_proto[i];
      switch (GET_OBJ_TYPE(obj)) {
        case ITEM_MONEY:
          if ((value = GET_OBJ_VAL(obj, 0))>MAX_OBJ_GOLD_ALLOWED && (found=1))
            tb_buf_appendf(&report,
                            "- Is worth %d (money limit %d coins).\r\n",
                                 value, MAX_OBJ_GOLD_ALLOWED);
          break;
        case ITEM_WEAPON:
          if (!VALID_ATTACK_TYPE(GET_OBJ_VAL(obj, 3)) && (found=1))
            tb_buf_appendf(&report,
                            "- has out of range attack type %d.\r\n",
                                 GET_OBJ_VAL(obj, 3));

          if (GET_OBJ_AVG_DAM(obj)>MAX_DAM_ALLOWED && (found=1))
            tb_buf_appendf(&report,
                            "- Damroll is %2.1f (limit %d)\r\n",
                                 GET_OBJ_AVG_DAM(obj), MAX_DAM_ALLOWED);
          break;
        case ITEM_ARMOR:
          ac=GET_OBJ_VAL(obj,0);
          for (j=0; j<TOTAL_WEAR_CHECKS;j++) {
            if (CAN_WEAR(obj,zarmor[j].bitvector) && (ac>zarmor[j].ac_allowed) && (found=1))
              tb_buf_appendf(&report,
                                   "- Has AC %d (%s limit is %d)\r\n",
                                   ac, zarmor[j].message, zarmor[j].ac_allowed);
          }
          break;

      }  /*switch on Item_Type*/

      if (!CAN_WEAR(obj, ITEM_WEAR_TAKE)) {
        if ((GET_OBJ_COST(obj) || (GET_OBJ_WEIGHT(obj) && GET_OBJ_TYPE(obj) != ITEM_FOUNTAIN) ||
           GET_OBJ_RENT(obj)) && (found = 1))
          tb_buf_appendf(&report,
                          "- is NO_TAKE, but has cost (%d) weight (%d) or rent (%d) set.\r\n",
                          GET_OBJ_COST(obj), GET_OBJ_WEIGHT(obj), GET_OBJ_RENT(obj));
      } else {
        if (GET_OBJ_COST(obj) == 0 && (found=1) && GET_OBJ_TYPE(obj) != ITEM_TRASH)
          tb_buf_appendf(&report,
                          "- has 0 cost (min. 1).\r\n");

        if (GET_OBJ_WEIGHT(obj) == 0 && (found=1))
          tb_buf_appendf(&report,
                          "- has 0 weight (min. 1).\r\n");

        if (GET_OBJ_WEIGHT(obj) > MAX_OBJ_WEIGHT && (found=1))
          tb_buf_appendf(&report,
                          "  Weight is too high: %d (limit  %d).\r\n",
                          GET_OBJ_WEIGHT(obj), MAX_OBJ_WEIGHT);


        if (GET_OBJ_COST(obj) > MAX_OBJ_COST && (found=1))
          tb_buf_appendf(&report,
                          "- has %d cost (max %d).\r\n",
                          GET_OBJ_COST(obj), MAX_OBJ_COST);
      }

      if (GET_OBJ_LEVEL(obj) > LVL_IMMORT-1 && (found=1))
        tb_buf_appendf(&report,
                          "- has min level set to %d (max %d).\r\n",
                          GET_OBJ_LEVEL(obj), LVL_IMMORT-1);

      if (obj->action_description && *obj->action_description &&
          GET_OBJ_TYPE(obj) != ITEM_STAFF &&
          GET_OBJ_TYPE(obj) != ITEM_WAND &&
          GET_OBJ_TYPE(obj) != ITEM_SCROLL &&
          GET_OBJ_TYPE(obj) != ITEM_NOTE && (found=1))
        tb_buf_appendf(&report,
                          "- has action_description set, but is inappropriate type.\r\n");

      /*first check for over-all affections*/
      for (affs=0, j = 0; j < MAX_OBJ_AFFECT; j++)
        if (obj->affected[j].modifier) affs++;

      if (affs>MAX_AFFECTS_ALLOWED && (found=1))
        tb_buf_appendf(&report,
                          "- has %d affects (limit %d).\r\n",
                             affs, MAX_AFFECTS_ALLOWED);

      /*check for out of range affections. */
      for (j=0;j<MAX_OBJ_AFFECT;j++) {
        /* The location comes from the object file's 'A' line or oedit and
         * is not range-checked at load (db.c). */
        int loc = obj->affected[j].location;
        const struct zcheck_affs *za;

        if (!TABLE_INDEX_OK(loc, TB_ARRAY_LEN(zaffs))) {
          found = 1;
          TB_WARN_MSG("act", "zcheck-apply", "object",
                      "obj vnum %d apply slot %d location %d outside 0..%d",
                      GET_OBJ_VNUM(obj), j, loc, (int) TB_ARRAY_LEN(zaffs) - 1);
          tb_buf_appendf(&report, "- apply slot %d has invalid location %d.\r\n",
                         j, loc);
          continue;
        }
        za = &zaffs[loc];
        if (za->max_aff != -99 && /* only care if a range is set */
            (obj->affected[j].modifier > za->max_aff ||
             obj->affected[j].modifier < za->min_aff ||
             za->min_aff == za->max_aff) && (found=1))
          tb_buf_appendf(&report,
                          "- apply to %s is %d (limit %d - %d).\r\n",
                               za->message, obj->affected[j].modifier,
                               za->min_aff, za->max_aff);
      }

     /* special handling of +hit and +dam because of +hit_n_dam */
     for (todam=0, tohit=0, j=0;j<MAX_OBJ_AFFECT;j++) {
       if (obj->affected[j].location == APPLY_HITROLL)
         tohit += obj->affected[j].modifier;
       if (obj->affected[j].location == APPLY_DAMROLL)
         todam += obj->affected[j].modifier;
     }
     if (abs(todam) > MAX_APPLY_DAMROLL_TOTAL && (found=1))
       tb_buf_appendf(&report,
                       "- total damroll %d out of range (limit +/-%d.\r\n",
                       todam, MAX_APPLY_DAMROLL_TOTAL);
     if (abs(tohit) > MAX_APPLY_HITROLL_TOTAL && (found=1))
       tb_buf_appendf(&report,
                       "- total hitroll %d out of range (limit +/-%d).\r\n",
                       tohit, MAX_APPLY_HITROLL_TOTAL);


     /* A NULL extra description (empty '~' text) counts as unformatted. */
     for (ext2 = NULL, ext = obj->ex_description; ext; ext = ext->next)
       if (!ext->description || strncmp(ext->description, "   ", 3))
         ext2 = ext;

     if (ext2 && (found = 1))
       tb_buf_appendf(&report,
                       "- has unformatted extra description\r\n");
     /* Additional object checks. */
     if (found) {
        send_to_char(ch, "[%5d] %-30s: \r\n", GET_OBJ_VNUM(obj), obj->short_description);
        ZCHECK_SEND(ch, report, "obj", GET_OBJ_VNUM(obj), zrnum);
      }
      tb_buf_init(&report, storage, sizeof(storage));
      found = 0;
    }   /*object is in zone*/
  } /*check objects*/

  /* Check rooms */
  send_to_char(ch, "\r\nChecking Rooms for limits...\r\n");
  for (i=0; i <= top_of_world;i++) {
    if (world[i].zone==zrnum) {
      for (j = 0; j < DIR_COUNT; j++) {
        /*check for exit, but ignore off limits if you're in an offlimit zone*/
        if (!world[i].dir_option[j])
          continue;
        exroom=world[i].dir_option[j]->to_room;
        if (exroom==NOWHERE)
          continue;
        if (world[exroom].zone == zrnum)
          continue;
        if (world[exroom].zone == world[i].zone)
          continue;

        for (k=0;offlimit_zones[k] != -1;k++) {
          if (world[exroom].zone == real_zone(offlimit_zones[k]) && (found = 1))
            tb_buf_appendf(&report,
                            "- Exit %s cannot connect to %d (zone off limits).\r\n",
                            dirs[j], world[exroom].number);
        } /* for (k.. */
      } /* cycle directions */

     if ((ROOM_FLAGGED(i, ROOM_ATRIUM) || ROOM_FLAGGED(i, ROOM_HOUSE) || ROOM_FLAGGED(i, ROOM_HOUSE_CRASH) || ROOM_FLAGGED(i, ROOM_OLC) || ROOM_FLAGGED(i, ROOM_BFS_MARK)) && (found = 1))
         tb_buf_appendf(&report,
         "- Has illegal affection bits set (%s %s %s %s %s)\r\n",
                            ROOM_FLAGGED(i, ROOM_ATRIUM) ? "ATRIUM" : "",
                            ROOM_FLAGGED(i, ROOM_HOUSE) ? "HOUSE" : "",
                            ROOM_FLAGGED(i, ROOM_HOUSE_CRASH) ? "HCRSH" : "",
                            ROOM_FLAGGED(i, ROOM_OLC) ? "OLC" : "",
                            ROOM_FLAGGED(i, ROOM_BFS_MARK) ? "*" : "");

      /* fread_string() and redit store NULL for an empty ('~') text, so
       * every description below may be NULL. */
      if (!world[i].description) {
        found = 1;
        TB_WARN_MSG("act", "zcheck-null-desc", "room", "room vnum %d has no description",
                    world[i].number);
        tb_buf_appendf(&report, "- Room has no description.\r\n");
      } else {
        if ((MIN_ROOM_DESC_LENGTH) && strlen(world[i].description)<MIN_ROOM_DESC_LENGTH && (found=1))
          tb_buf_appendf(&report,
            "- Room description is too short. (%4.4d of min. %d characters).\r\n",
            (int)strlen(world[i].description), MIN_ROOM_DESC_LENGTH);

        if (strncmp(world[i].description, "   ", 3) && (found=1))
          tb_buf_appendf(&report,
                          "- Room description not formatted with indent (/fi in the editor).\r\n");

        /* strcspan = size of text in first arg before any character in second arg */
        if ((strcspn(world[i].description, "\r\n")>MAX_COLOUMN_WIDTH) && (found=1))
          tb_buf_appendf(&report,
                          "- Room description not wrapped at %d chars (/fi in the editor).\r\n",
                               MAX_COLOUMN_WIDTH);
      }

     /* A NULL extra description counts as unformatted. */
     for (ext2 = NULL, ext = world[i].ex_description; ext; ext = ext->next)
       if (!ext->description || strncmp(ext->description, "   ", 3))
         ext2 = ext;

     if (ext2 && (found = 1))
       tb_buf_appendf(&report,
                       "- has unformatted extra description\r\n");

      if (found) {
        send_to_char(ch, "[%5d] %-30s: \r\n",
                       world[i].number, world[i].name ? world[i].name : "An unnamed room");
        ZCHECK_SEND(ch, report, "room", world[i].number, zrnum);
      }
      /* Reset for every room, not only the ones that printed. */
      tb_buf_init(&report, storage, sizeof(storage));
      found = 0;
    } /*is room in this zone?*/
  } /*checking rooms*/

  for (i=0; i <= top_of_world;i++) {
    if (world[i].zone==zrnum) {
      m++;
      for (j = 0, k = 0; j < DIR_COUNT; j++)
        if (!world[i].dir_option[j])
          k++;

      if (k == DIR_COUNT)
        l++;
    }
  }
  if (l * 3 > m)
    send_to_char(ch, "More than 1/3 of the rooms are not linked.\r\n");

}

static void mob_checkload(struct char_data *ch, mob_vnum mvnum)
{
  int cmd_no;
  zone_rnum zone;
  mob_rnum mrnum = real_mobile(mvnum);

  if (mrnum == NOBODY) {
      send_to_char(ch, "That mob does not exist.\r\n");
      return;
  }

  send_to_char(ch, "Checking load info for the mob %s...\r\n",
                    mob_proto[mrnum].player.short_descr);

  for (zone=0; zone <= top_of_zone_table; zone++) {
    for (cmd_no = 0; ZCMD2.command != 'S'; cmd_no++) {
      if (ZCMD2.command != 'M')
        continue;

      /* read a mobile */
      if (ZCMD2.arg1 == mrnum) {
        send_to_char(ch, "  [%5d] %s (%d MAX)\r\n",
                         GET_ROOM_VNUM(ZCMD2.arg3),
                         ROOM_NAME_SAFE(ZCMD2.arg3),
                         ZCMD2.arg2);
      }
    }
  }
}

static void obj_checkload(struct char_data *ch, obj_vnum ovnum)
{
  int cmd_no;
  zone_rnum zone;
  obj_rnum ornum = real_object(ovnum);
  room_vnum lastroom_v = 0;
  room_rnum lastroom_r = 0;
  mob_rnum lastmob_r = 0;

  if (ornum ==NOTHING) {
    send_to_char(ch, "That object does not exist.\r\n");
    return;
  }

  send_to_char(ch, "Checking load info for the obj %s...\r\n",
                   obj_proto[ornum].short_description);

  for (zone=0; zone <= top_of_zone_table; zone++) {
    for (cmd_no = 0; ZCMD2.command != 'S'; cmd_no++) {
      switch (ZCMD2.command) {
        case 'M':
          lastroom_v = GET_ROOM_VNUM(ZCMD2.arg3);
          lastroom_r = ZCMD2.arg3;
          lastmob_r = ZCMD2.arg1;
          break;
        case 'O':                   /* read an object */
          lastroom_v = GET_ROOM_VNUM(ZCMD2.arg3);
          lastroom_r = ZCMD2.arg3;
          if (ZCMD2.arg1 == ornum)
            send_to_char(ch, "  [%5d] %s (%d Max)\r\n",
                             lastroom_v,
                             ROOM_NAME_SAFE(lastroom_r),
                             ZCMD2.arg2);
          break;
        case 'P':                   /* object to object */
          if (ZCMD2.arg1 == ornum)
            send_to_char(ch, "  [%5d] %s (Put in another object [%d Max])\r\n",
                             lastroom_v,
                             ROOM_NAME_SAFE(lastroom_r),
                             ZCMD2.arg2);
          break;
        case 'G':                   /* obj_to_char */
          if (ZCMD2.arg1 == ornum)
            send_to_char(ch, "  [%5d] %s (Given to %s [%d][%d Max])\r\n",
                             lastroom_v,
                             ROOM_NAME_SAFE(lastroom_r),
                             mob_proto[lastmob_r].player.short_descr,
                             mob_index[lastmob_r].vnum,
                             ZCMD2.arg2);
          break;
        case 'E':                   /* object to equipment list */
          if (ZCMD2.arg1 == ornum)
            send_to_char(ch, "  [%5d] %s (Equipped to %s [%d][%d Max])\r\n",
                             lastroom_v,
                             ROOM_NAME_SAFE(lastroom_r),
                             mob_proto[lastmob_r].player.short_descr,
                             mob_index[lastmob_r].vnum,
                             ZCMD2.arg2);
          break;
        case 'R': /* rem obj from room */
          lastroom_v = GET_ROOM_VNUM(ZCMD2.arg1);
          lastroom_r = ZCMD2.arg1;
          if (ZCMD2.arg2 == ornum)
            send_to_char(ch, "  [%5d] %s (Removed from room)\r\n",
                             lastroom_v,
                             ROOM_NAME_SAFE(lastroom_r));
          break;
      }/* switch */
    } /*for cmd_no......*/
  }  /*for zone...*/
}

static void trg_checkload(struct char_data *ch, trig_vnum tvnum)
{
  int cmd_no, found = 0;
  zone_rnum zone;
  trig_rnum trnum = real_trigger(tvnum);
  room_vnum lastroom_v = 0;
  room_rnum lastroom_r = 0, k;
  mob_rnum lastmob_r = 0, i;
  obj_rnum lastobj_r = 0, j;
  struct trig_proto_list *tpl;

  if (trnum == NOTHING) {
    send_to_char(ch, "That trigger does not exist.\r\n");
    return;
  }

  send_to_char(ch, "Checking load info for the %s trigger '%s':\r\n",
                    trig_index[trnum]->proto->attach_type == MOB_TRIGGER ? "mobile" :
                    (trig_index[trnum]->proto->attach_type == OBJ_TRIGGER ? "object" : "room"),
                    trig_index[trnum]->proto->name);

  for (zone=0; zone <= top_of_zone_table; zone++) {
    for (cmd_no = 0; ZCMD2.command != 'S'; cmd_no++) {
      switch (ZCMD2.command) {
        case 'M':
          lastroom_v = GET_ROOM_VNUM(ZCMD2.arg3);
          lastroom_r = ZCMD2.arg3;
          lastmob_r = ZCMD2.arg1;
          break;
        case 'O':                   /* read an object */
          lastroom_v = GET_ROOM_VNUM(ZCMD2.arg3);
          lastroom_r = ZCMD2.arg3;
          lastobj_r = ZCMD2.arg1;
          break;
        case 'P':                   /* object to object */
          lastobj_r = ZCMD2.arg1;
          break;
        case 'G':                   /* obj_to_char */
          lastobj_r = ZCMD2.arg1;
          break;
        case 'E':                   /* object to equipment list */
          lastobj_r = ZCMD2.arg1;
          break;
        case 'R':                   /* rem obj from room */
          lastroom_v = 0;
          lastroom_r = 0;
          lastobj_r = 0;
          lastmob_r = 0;
          break;
        case 'T':                   /* trigger to something */
          if (ZCMD2.arg2 != trnum)
            break;
          if (ZCMD2.arg1 == MOB_TRIGGER) {
            send_to_char(ch, "mob [%5d] %-60s (zedit room %5d)\r\n",
                               mob_index[lastmob_r].vnum,
                               mob_proto[lastmob_r].player.short_descr,
                               lastroom_v);
            found = 1;
          } else if (ZCMD2.arg1 == OBJ_TRIGGER) {
            send_to_char(ch, "obj [%5d] %-60s  (zedit room %d)\r\n",
                               obj_index[lastobj_r].vnum,
                               obj_proto[lastobj_r].short_description,
                               lastroom_v);
            found = 1;
          } else if (ZCMD2.arg1==WLD_TRIGGER) {
            send_to_char(ch, "room [%5d] %-60s (zedit)\r\n",
                               lastroom_v,
                               ROOM_NAME_SAFE(lastroom_r));
            found = 1;
          }
        break;
      } /* switch */
    } /*for cmd_no......*/
  }  /*for zone...*/

  for (i = 0; i <= top_of_mobt; i++) {
    if (!mob_proto[i].proto_script)
      continue;

    for (tpl = mob_proto[i].proto_script;tpl;tpl = tpl->next)
      if (tpl->vnum == tvnum) {
        send_to_char(ch, "mob [%5d] %s\r\n",
                         mob_index[i].vnum,
                         mob_proto[i].player.short_descr);
        found = 1;
      }
  }

  for (j = 0; j <= top_of_objt; j++) {
    if (!obj_proto[j].proto_script)
      continue;

    for (tpl = obj_proto[j].proto_script;tpl;tpl = tpl->next)
      if (tpl->vnum == tvnum) {
        send_to_char(ch, "obj [%5d] %s\r\n",
                         obj_index[j].vnum,
                         obj_proto[j].short_description);
        found = 1;
      }
  }

  for (k = 0;k <= top_of_world; k++) {
    if (!world[k].proto_script)
      continue;

    for (tpl = world[k].proto_script;tpl;tpl = tpl->next)
      if (tpl->vnum == tvnum) {
        send_to_char(ch, "room[%5d] %s\r\n",
                         world[k].number,
                         world[k].name);
        found = 1;
      }
  }

  if (!found)
    send_to_char(ch, "This trigger is not attached to anything.\r\n");
}

ACMD(do_checkloadstatus)
{
  char buf1[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  IDXTYPE vnum;

  two_arguments(argument, buf1, buf2);

  if ((!*buf1) || (!*buf2) || (!isdigit(*buf2))) {
    send_to_char(ch, "Checkload <M | O | T> <vnum>\r\n");
    return;
  }

  if (LOWER(*buf1) == 'm') {
    if (parse_vnum_arg(ch, buf2, "mob", &vnum))
      mob_checkload(ch, vnum);
    return;
  }

  if (LOWER(*buf1) == 'o') {
    if (parse_vnum_arg(ch, buf2, "object", &vnum))
      obj_checkload(ch, vnum);
    return;
  }

  if (LOWER(*buf1) == 't') {
    if (parse_vnum_arg(ch, buf2, "trigger", &vnum))
      trg_checkload(ch, vnum);
    return;
  }
}
/* Zone Checker code above. */

/* (c) 1996-97 Erwin S. Andreasen. */
/** copyover: save every player and exec a fresh server that inherits the
 * sockets.
 * COPYOVER_FILE format, read back by copyover_recover() in comm.c:
 *   line 1      boot_time as %ld
 *   per player  "<socket> <pref> <name> <host> <protocol state>"
 *   last line   -1
 * A switched immortal is returned to its own body first, so each record
 * names a player (an NPC record is refused: copyover-save). Descriptors
 * not in CON_PLAYING (login, OLC menus) are closed, not preserved. If the
 * file cannot be written completely the copyover is aborted before exec
 * (copyover-open / copyover-write). A failed chdir or execl exits the
 * process. */
ACMD(do_copyover)
{
  FILE *fp;
  struct descriptor_data *d, *d_next;
  char buf [100], buf2[100];
  int write_ok = 1;   /* every write of the state file succeeded */

  fp = fopen (COPYOVER_FILE, "w");
    if (!fp) {
      TB_ERROR_ERRNO("act", "copyover-open", COPYOVER_FILE,
                     "caller " DIAG_CHAR_FMT "; copyover aborted", DIAG_CHAR(ch));
      send_to_char (ch, "Copyover file not writeable, aborted.\n\r");
      return;
    }

   tb_snprintf(buf, sizeof(buf), "\n\r *** COPYOVER by %s - please remain seated!\n\r", GET_NAME(ch));

   /* write boot_time as first line in file */
   write_ok &= tb_fprintf_checked(fp, COPYOVER_FILE, "%ld\n", (long)boot_time);

   /* For each playing descriptor, save its state */
   for (d = descriptor_list; d ; d = d_next) {
     struct char_data *och;

   /* If d is currently in someone else's body, return them. The record
    * and the saves below are for the player's own body, so och is read
    * only after the return: d->character is then the original. */
   if (d->character && d->original)
     return_to_char(d->character);
   och = d->character;

   /* We delete from the list , so need to save this */
     d_next = d->next;

  /* drop those logging on */
   if (!och || d->connected > CON_PLAYING) {
     write_to_descriptor (d->descriptor, "\n\rSorry, we are rebooting. Come back in a few minutes.\n\r");
     close_socket (d); /* throw'em out */
   } else if (IS_NPC(och)) {
     /* Invariant: after the return above a playing descriptor holds its
      * player. copyover_recover parses "desc pref name host" and would
      * misread an NPC's multi-word name. */
     TB_ERROR_MSG("act", "copyover-save", "descriptor",
                  "desc %d character " DIAG_CHAR_FMT " is not a player; dropped",
                  d->desc_num, DIAG_CHAR(och));
     write_to_descriptor (d->descriptor, "\n\rSorry, we are rebooting. Come back in a few minutes.\n\r");
     close_socket (d);
   } else {
      write_ok &= tb_fprintf_checked(fp, COPYOVER_FILE, "%d %ld %s %s %s\n",
                                     d->descriptor, GET_PREF(och), GET_NAME(och),
                                     d->host, CopyoverGet(d));
      /* save och */
      GET_LOADROOM(och) = GET_ROOM_VNUM(IN_ROOM(och));
      Crash_rentsave(och,0);
      save_char(och);
      write_to_descriptor (d->descriptor, buf);
    }
  }

  write_ok &= tb_fprintf_checked(fp, COPYOVER_FILE, "-1\n");
  write_ok &= tb_fflush_checked(fp, COPYOVER_FILE);
  write_ok &= tb_fclose_checked(fp, COPYOVER_FILE);

  /* A short record list (e.g. ENOSPC) would make the new process drop or
   * mis-bind the sockets past the truncation, so do not exec. Everyone has
   * been saved above and stays connected to this process. */
  if (!write_ok) {
    TB_ERROR_MSG("act", "copyover-write", COPYOVER_FILE,
                 "caller " DIAG_CHAR_FMT ": state file incomplete; copyover aborted before exec",
                 DIAG_CHAR(ch));
    if (remove(COPYOVER_FILE) != 0)
      TB_ERROR_ERRNO("act", "copyover-remove", COPYOVER_FILE,
                     "incomplete state file left behind");
    send_to_all("Copyover aborted; carry on.\r\n");
    send_to_char(ch, "Copyover ABORTED: could not write %s.\r\n", COPYOVER_FILE);
    return;
  }

  /* exec - descriptors are inherited */
  tb_snprintf(buf, sizeof(buf), "%d", port);
  tb_snprintf(buf2, sizeof(buf2), "-C%d", mother_desc);

  /* Ugh, seems it is expected we are 1 step above lib - this may be dangerous! */
  if(chdir ("..") != 0) {
    {
      int saved_errno = errno;
      tb_report_error_at("chdir", "..", saved_errno, __FILE__, __LINE__, __func__);
      send_to_char(ch, "Error changing working directory: %s (errno=%d).",
                   strerror(saved_errno), saved_errno);
    }
    exit(1);
  }

  /* Close reserve and other always-open files and release other resources */
  execl (EXE_FILE, "circle", buf2, buf, (char *) NULL);

  /* Failed - successful exec will not return */
  TB_REPORT_ERRNO("execl", EXE_FILE);
  send_to_char (ch, "Copyover FAILED!\n\r");

  exit (1); /* too much trouble to try to recover! */
}

ACMD(do_peace)
{
  struct char_data *vict, *next_v;

  act ("As $n makes a strange arcane gesture, a golden light descends\r\n"
       "from the heavens stopping all the fighting.\r\n",FALSE, ch, 0, 0, TO_ROOM);
  send_to_room(IN_ROOM(ch), "Everything is quite peaceful now.\r\n");
  for(vict=world[IN_ROOM(ch)].people; vict; vict=next_v) {
    next_v = vict->next_in_room;
    if (FIGHTING(vict))
      stop_fighting(vict);
    if (IS_NPC(vict))
      clearMemory(vict);
  }
}

ACMD(do_zpurge)
{
  int vroom, room, zone = 0;
  char arg[MAX_INPUT_LENGTH];
  int purge_all = FALSE;
  one_argument(argument, arg);
  if (*arg == '.' || !*arg) {
    zone = world[IN_ROOM(ch)].zone;
  }
  else if (is_number(arg)) {
    zone_vnum zvnum;

    if (!parse_vnum_arg(ch, arg, "zone", &zvnum))
      return;
    zone = real_zone(zvnum);
    if (zone == NOWHERE || zone > top_of_zone_table) {
      send_to_char(ch, "That zone doesn't exist!\r\n");
      return;
    }
  }
  else if (*arg == '*') {
    purge_all = TRUE;
  }
  else {
    send_to_char(ch, "That isn't a valid zone number!\r\n");
    return;
  }
  /* zone stays 0 for '*', so the can_edit_zone() test below would let a
   * builder who owns zone 0 purge the whole world. */
  if (purge_all && GET_LEVEL(ch) < LVL_GOD) {
    TB_WARN_MSG("act", "zpurge-denied", "world",
                "caller " DIAG_CHAR_FMT " level %d below %d tried 'zpurge *'",
                DIAG_CHAR(ch), GET_LEVEL(ch), LVL_GOD);
    mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE,
           "(GC) %s was refused 'zpurge *' (level %d)", GET_NAME(ch), GET_LEVEL(ch));
    send_to_char(ch, "Only gods can purge the whole world.\r\n");
    return;
  }
  if (GET_LEVEL(ch) < LVL_GOD && !can_edit_zone(ch, zone)) {
    send_to_char(ch, "You can only purge your own zone!\r\n");
    return;
  }
  if (!purge_all) {
    for (vroom = zone_table[zone].bot; vroom <= zone_table[zone].top; vroom++) {
      purge_room(real_room(vroom));
    }
    send_to_char(ch, "Purged zone #%d: %s.\r\n", zone_table[zone].number, zone_table[zone].name);
    mudlog(NRM, MAX(LVL_GRGOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s purged zone %d (%s)", GET_NAME(ch), zone_table[zone].number, zone_table[zone].name);
  }
  else {
    for (room = 0; room <= top_of_world; room++) {
      purge_room(room);
    }
    send_to_char(ch, "Purged world.\r\n");
    mudlog(NRM, MAX(LVL_GRGOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s purged entire world.", GET_NAME(ch));
  }
}

/** Used to read and gather a bit of information about external log files while
 * in game.
 * Makes use of the '\t' color codes in the file status information.
 * Some of the methods used are a bit wasteful (reading through the file
 * multiple times to gather diagnostic information), but it is
 * assumed that the files read with this function will never be very large.
 * Files to be read are assumed to exist and be readable and if they aren't,
 * log the name of the missing file.
 */
ACMD(do_file)
{
  /* Local variables */
  int def_lines_to_read = 15;  /* Set the default num lines to be read. */
  int max_lines_to_read = 300; /* Maximum number of lines to read. */
  FILE *req_file;              /* Pointer to file to be read. */
  size_t req_file_size = 0;    /* Size of file to be read. */
  int req_file_lines = 0;      /* Number of total lines in file to be read. */
  int lines_read = 0;     /* Counts total number of lines read from the file. */
  int req_lines = 0;      /* Number of lines requested to be displayed. */
  int i;                  /* Generic loop counter. */
  int l;                  /* Marks choice of file in fields array. */
  char field[MAX_INPUT_LENGTH];  /* Holds users choice of file to be read. */
  char value[MAX_INPUT_LENGTH];  /* Holds # lines to be read, if requested. */
  char buf[MAX_STRING_LENGTH];   /* Display buffer for req_file. */

  /* Defines which files are available to read. */
  struct file_struct {
    char *cmd;          /* The 'name' of the file to view */
    char level;         /* Minimum level needed to view. */
    char *file;         /* The file location, relative to the working dir. */
    int read_backwards; /* Should the file be read backwards by default? */
  } fields[] = {
    { "xnames",         LVL_GOD,    XNAME_FILE,          TRUE},
    { "levels",         LVL_GOD,    LEVELS_LOGFILE,      TRUE},
    { "rip",            LVL_GOD,    RIP_LOGFILE,         TRUE},
    { "players",        LVL_GOD,    NEWPLAYERS_LOGFILE,  TRUE},
    { "rentgone",       LVL_GOD,    RENTGONE_LOGFILE,    TRUE},
    { "errors",         LVL_GOD,    ERRORS_LOGFILE,      TRUE},
    { "godcmds",        LVL_GOD,    GODCMDS_LOGFILE,     TRUE},
    { "syslog",         LVL_GOD,    SYSLOG_LOGFILE,      TRUE},
    { "crash",          LVL_GOD,    CRASH_LOGFILE,       TRUE},
    { "help",           LVL_GOD,    HELP_LOGFILE,        TRUE},
    { "changelog",      LVL_GOD,    CHANGE_LOG_FILE,     FALSE},
    { "deletes",        LVL_GOD,    DELETES_LOGFILE,     TRUE},
    { "restarts",       LVL_GOD,    RESTARTS_LOGFILE,    TRUE},
    { "usage",          LVL_GOD,    USAGE_LOGFILE,       TRUE},
    { "badpws",         LVL_GOD,    BADPWS_LOGFILE,      TRUE},
    { "olc",            LVL_GOD,    OLC_LOGFILE,         TRUE},
    { "trigger",        LVL_GOD,    TRIGGER_LOGFILE,     TRUE},
    { "\n", 0, "\n", FALSE } /* This must be the last entry */
  };

   /* Initialize buffer */
   buf[0] = '\0';

   /**/
   /* End function variable set-up and initialization. */

   skip_spaces(&argument);

   /* Display usage if no argument. */
   if (!*argument) {
     send_to_char(ch, "USAGE: file <filename> <num lines>\r\n\r\nFile options:\r\n");
     for (i = 0; fields[i].level; i++)
       if (fields[i].level <= GET_LEVEL(ch))
         send_to_char(ch, "%-15s%s\r\n", fields[i].cmd, fields[i].file);
     return;
   }

   /* Begin validity checks. Is the file choice valid and accessible? */
   /**/
   /* There are some arguments, deal with them. */
   two_arguments(argument, field, value);

   for (l = 0; *(fields[l].cmd) != '\n'; l++)
   {
     if (!strncmp(field, fields[l].cmd, strlen(field)))
       break;
   }

   if(*(fields[l].cmd) == '\n') {
     send_to_char(ch, "'%s' is not a valid file.\r\n", field);
     return;
   }

   if (GET_LEVEL(ch) < fields[l].level) {
     send_to_char(ch, "You have not achieved a high enough level to view '%s'.\r\n",
         fields[l].cmd);
     return;
   }

   /* Number of lines to view. Default is 15. */
   if(!*value)
     req_lines = def_lines_to_read;
   else if (!isdigit(*value))
   {
     /* This check forces the requisite positive digit and prevents negative
      * numbers of lines from being read. */
     send_to_char(ch, "'%s' is not a valid number of lines to view.\r\n", value);
     return;
   }
   else
   {
     req_lines = tb_parse_int_or_zero(value);
     /* Limit the maximum number of lines */
     req_lines = MIN( req_lines, max_lines_to_read );
   }

   /* Must be able to access the file on disk. */
   if (!(req_file=fopen(fields[l].file,"r"))) {
     send_to_char(ch, "The file %s can not be opened.\r\n", fields[l].file);
     mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: Error opening file %s using 'file' command.", fields[l].file);
     return;
   }
   /**/
   /* End validity checks. From here on, the file should be viewable. */

   /* Diagnostic information about the file */
   req_file_size = file_sizeof(req_file);
   req_file_lines = file_numlines(req_file);

   tb_snprintf( buf, sizeof(buf),
       "\tgFile:\tn %s\tg; Min. Level to read:\tn %d\tg; File Location:\tn %s\tg\r\n"
       "File size (bytes):\tn %ld\tg; Total num lines:\tn %d\r\n",
       fields[l].cmd, fields[l].level, fields[l].file, (long) req_file_size,
       req_file_lines);

   /* Should the file be 'headed' or 'tailed'? */
   if ( (fields[l].read_backwards == TRUE) && (req_lines < req_file_lines) )
   {
     tb_snprintf( buf + strlen(buf), sizeof(buf) - strlen(buf),
               "\tgReading from the tail of the file.\tn\r\n\r\n" );
     lines_read = file_tail( req_file, buf, sizeof(buf), req_lines );
   }
   else
   {
     tb_snprintf( buf + strlen(buf), sizeof(buf) - strlen(buf),
              "\tgReading from the head of the file.\tn\r\n\r\n" );
     lines_read = file_head( req_file, buf, sizeof(buf), req_lines );
   }

   /* Since file_head and file_tail will add the overflow message, we
    * don't check for status here. */
   if ( lines_read == req_file_lines )
   {
     /* We're reading the entire file */
     tb_snprintf( buf + strlen(buf), sizeof(buf) - strlen(buf),
         "\r\n\tgEntire file returned (\tn%d \tglines).\tn\r\n",
         lines_read );
   }
   else if ( lines_read == max_lines_to_read )
   {
     tb_snprintf( buf + strlen(buf), sizeof(buf) - strlen(buf),
         "\r\n\tgMaximum number of \tn%d \tglines returned.\tn\r\n",
         lines_read );
   }
   else
   {
     tb_snprintf( buf + strlen(buf), sizeof(buf) - strlen(buf),
         "\r\n%d \tglines returned.\tn\r\n",
         lines_read );
   }

   /* Clean up before return */
   tb_fclose(req_file);

   page_string(ch->desc, buf, 1);
}

/** Put the changelog backup back after a failed update, so a failure never
 * leaves CHANGE_LOG_FILE missing. */
static void changelog_restore(const char *bak)
{
  if (rename(bak, CHANGE_LOG_FILE) != 0)
    TB_ERROR_ERRNO("act", "changelog-restore", CHANGE_LOG_FILE,
                   "could not restore from %s; the changelog is only in the backup", bak);
}

/** changelog <text>: insert a dated entry by the caller into
 * CHANGE_LOG_FILE, after the preamble (every line before the first '['
 * header). The text comes from an implementor and is written as is.
 * The old file is kept as CHANGE_LOG_FILE.bak. Lines are copied raw, so
 * comments, blank lines and lines of any length survive. On any I/O
 * failure the new file is discarded, the backup is renamed back, and the
 * failure is reported as changelog-* with errno. */
ACMD(do_changelog)
{
  time_t rawtime;
  char timestr[12], line[READ_SIZE], header[READ_SIZE], bak[READ_SIZE];
  FILE *fl, *new;
  int ok = 1, at_line_start = 1, found_header = 0;
  size_t n;

  skip_spaces(&argument);

  if (!*argument) {
    send_to_char(ch, "Usage: changelog <change>\r\n");
    return;
  }

  tb_snprintf(bak, sizeof(bak), "%s.bak", CHANGE_LOG_FILE);
  if (rename(CHANGE_LOG_FILE, bak)) {
    TB_ERROR_ERRNO("act", "changelog-backup", CHANGE_LOG_FILE,
                   "rename to %s failed; caller " DIAG_CHAR_FMT, bak, DIAG_CHAR(ch));
    send_to_char(ch, "Could not update the changelog (see the log).\r\n");
    return;
  }

  if (!(fl = fopen(bak, "r"))) {
    TB_ERROR_ERRNO("act", "changelog-open", bak, "cannot read the backup");
    changelog_restore(bak);
    send_to_char(ch, "Could not update the changelog (see the log).\r\n");
    return;
  }

  if (!(new = fopen(CHANGE_LOG_FILE, "w"))) {
    TB_ERROR_ERRNO("act", "changelog-open", CHANGE_LOG_FILE, "cannot create the new file");
    tb_fclose(fl);
    changelog_restore(bak);
    send_to_char(ch, "Could not update the changelog (see the log).\r\n");
    return;
  }

  /* Copy the preamble up to the first line that starts with '['. */
  while (fgets(line, sizeof(line), fl)) {
    if (at_line_start && *line == '[') {
      found_header = 1;
      strlcpy(header, line, sizeof(header));
      break;
    }
    ok &= tb_fprintf_checked(new, CHANGE_LOG_FILE, "%s", line);
    n = strlen(line);
    at_line_start = (n > 0 && line[n - 1] == '\n');
  }

  rawtime = time(0);
  TIME_STR(timestr, "%b %d %Y", localtime(&rawtime));

  tb_snprintf(line, sizeof(line), "[%s] - %s", timestr, GET_NAME(ch));
  ok &= tb_fprintf_checked(new, CHANGE_LOG_FILE, "%s\n  %s\n", line, argument);

  /* Same date and author as the newest entry: the new line joins it. */
  if (found_header) {
    size_t hlen = strcspn(header, "\r\n");
    if (hlen != strlen(line) || strncmp(header, line, hlen))
      ok &= tb_fprintf_checked(new, CHANGE_LOG_FILE, "%s", header);
  }

  /* The rest of the file, byte for byte. */
  while ((n = fread(line, 1, sizeof(line), fl)) > 0)
    if (fwrite(line, 1, n, new) != n) {
      TB_ERROR_ERRNO("act", "changelog-write", CHANGE_LOG_FILE, "copy of the old entries failed");
      ok = 0;
      break;
    }
  if (ferror(fl)) {
    TB_ERROR_ERRNO("act", "changelog-read", bak, "read of the old entries failed");
    ok = 0;
  }

  tb_fclose(fl);
  ok &= tb_fflush_checked(new, CHANGE_LOG_FILE);
  ok &= tb_fclose_checked(new, CHANGE_LOG_FILE);

  if (!ok) {
    TB_ERROR_MSG("act", "changelog-write", CHANGE_LOG_FILE,
                 "update by " DIAG_CHAR_FMT " discarded; restoring %s", DIAG_CHAR(ch), bak);
    if (remove(CHANGE_LOG_FILE) != 0)
      TB_ERROR_ERRNO("act", "changelog-remove", CHANGE_LOG_FILE, "partial file left behind");
    changelog_restore(bak);
    send_to_char(ch, "Could not update the changelog (see the log).\r\n");
    return;
  }
  send_to_char(ch, "Change added.\r\n");
}

/* Parse a nonnegative number or a nonnegative low-high range. */
static int parse_plist_range(const char *arg, int *low, int *high)
{
  char *end;
  long first, last;
  if (!arg || !isdigit((unsigned char)*arg))
    return 0;
  errno = 0;
  first = strtol(arg, &end, 10);
  if (errno == ERANGE || first > INT_MAX)
    return 0;
  if (*end == '\0') {
    *low = *high = (int)first;
    return 1;
  }
  if (*end != '-' || !isdigit((unsigned char)end[1]))
    return 0;
  arg = end + 1;
  errno = 0;
  last = strtol(arg, &end, 10);
  if (errno == ERANGE || last > INT_MAX || *end != '\0')
    return 0;
  *low = (int)first;
  *high = (int)last;
  return 2;
}

#define PLIST_FORMAT \
  "Usage: plist [minlev[-maxlev]] [-n name] [-d days] [-h hours] [-i] [-m]"

ACMD(do_plist)
{
  int i, len = 0, count = 0;
  char mode, buf[MAX_STRING_LENGTH * 20], name_search[MAX_INPUT_LENGTH] /* half_chop() output */, timestr[MAX_STRING_LENGTH];
  struct time_info_data time_away;
  int low = 0, high = LVL_IMPL, low_day = 0, high_day = 10000, low_hr = 0, high_hr = 24;

  skip_spaces(&argument);
  strlcpy(buf, argument, sizeof(buf));        /* strcpy: OK (sizeof: argument == buf) */
  name_search[0] = '\0';

  while (*buf) {
    char arg[MAX_INPUT_LENGTH], buf1[MAX_INPUT_LENGTH];

    half_chop(buf, arg, buf1);
    if (isdigit(*arg)) {
      if (!parse_plist_range(arg, &low, &high)) {
        send_to_char(ch, "Invalid level range.\r\n");
        return;
      }
      strlcpy(buf, buf1, sizeof(buf));        /* strcpy: OK (sizeof: buf1 == buf) */
    } else if (*arg == '-') {
      mode = *(arg + 1);        /* just in case; we destroy arg in the switch */
      switch (mode) {
      case 'l': {
        int prior_high = high;
        int parsed = 0;
        half_chop(buf1, arg, buf);
        parsed = parse_plist_range(arg, &low, &high);
        if (!parsed) {
          send_to_char(ch, "Invalid level range.\r\n");
          return;
        }
        /* Historically, -l 10 changed only the lower limit. */
        if (parsed == 1)
          high = prior_high;
        break;
      }
      case 'n':
        half_chop(buf1, name_search, buf);
        break;
      case 'i':
        strlcpy(buf, buf1, sizeof(buf));
        low = LVL_IMMORT;
        break;
      case 'm':
        strlcpy(buf, buf1, sizeof(buf));
        high = LVL_IMMORT - 1;
        break;
      case 'd':
        half_chop(buf1, arg, buf);
        if (!parse_plist_range(arg, &low_day, &high_day)) {
          send_to_char(ch, "Invalid day range.\r\n");
          return;
        }
        break;
      case 'h':
        half_chop(buf1, arg, buf);
        if (!parse_plist_range(arg, &low_hr, &high_hr)) {
          send_to_char(ch, "Invalid hour range.\r\n");
          return;
        }
        break;
      default:
        send_to_char(ch, "%s\r\n", PLIST_FORMAT);
        return;
      }
    } else {
      send_to_char(ch, "%s\r\n", PLIST_FORMAT);
      return;
    }
  }

  len = 0;
  len += snprintf(buf + len, sizeof(buf) - len, "\tW[ Id] (Lv) Name         Last\tn\r\n"
                  "%s-------------------------------------%s\r\n", CCCYN(ch, C_NRM),
                  CCNRM(ch, C_NRM));

  for (i = 0; i <= top_of_p_table; i++) {
    if (player_table[i].level < low || player_table[i].level > high)
      continue;

    time_away = *real_time_passed(time(0), player_table[i].last);

    if (*name_search && str_cmp(name_search, player_table[i].name))
      continue;

    if (time_away.day > high_day || time_away.day < low_day)
      continue;
    if (time_away.hours > high_hr || time_away.hours < low_hr)
      continue;

    TIME_STR(timestr, "%c", localtime(&player_table[i].last));

    len += snprintf(buf + len, sizeof(buf) - len, "[%3ld] (%2d) %c%-15s %s\r\n",
                    player_table[i].id, player_table[i].level,
                    UPPER(*player_table[i].name), player_table[i].name + 1, timestr);
    count++;
  }
  tb_snprintf(buf + len, sizeof(buf) - len, "%s-------------------------------------%s\r\n"
           "%d players listed.\r\n", CCCYN(ch, C_NRM), CCNRM(ch, C_NRM), count);
  page_string(ch->desc, buf, TRUE);
}

ACMD(do_wizupdate)
{
  run_autowiz();
  send_to_char(ch, "Wizlists updated.\n\r");
}

/* Per-player files that follow the name (get_filename() modes). The pfile
 * is first: perform_set re-saves it under the new name anyway. */
static const int player_file_modes[] = { PLR_FILE, CRASH_FILE, SCRIPT_VARS_FILE, ETEXT_FILE };

/** Rename every per-player file of old_name to new_name. A file that does
 * not exist (no objects, no saved variables) is skipped. A destination that
 * already exists is never overwritten. On any failure the files already
 * moved are moved back and rename-player-file is reported with errno, the
 * paths and the player idnum.
 * @return TRUE when every existing file was moved. */
static bool rename_player_files(struct char_data *vict, const char *old_name,
                                const char *new_name)
{
  char from[PATH_MAX], to[PATH_MAX];
  int moved[TB_ARRAY_LEN(player_file_modes)];
  size_t m, undo;

  for (m = 0; m < TB_ARRAY_LEN(player_file_modes); m++) {
    moved[m] = 0;
    if (!get_filename(from, sizeof(from), player_file_modes[m], old_name) ||
        !get_filename(to, sizeof(to), player_file_modes[m], new_name)) {
      TB_ERROR_MSG("act", "rename-player-file", "path",
                   "player idnum %ld: no file name for mode %d", GET_IDNUM(vict),
                   player_file_modes[m]);
      goto rollback;
    }
    if (access(from, F_OK) != 0) {
      if (errno == ENOENT)
        continue;
      TB_ERROR_ERRNO("act", "rename-player-file", from,
                     "player idnum %ld: cannot check the old file", GET_IDNUM(vict));
      goto rollback;
    }
    if (access(to, F_OK) == 0) {
      TB_ERROR_MSG("act", "rename-player-file", to,
                   "player idnum %ld: destination exists; not overwriting it",
                   GET_IDNUM(vict));
      goto rollback;
    }
    if (rename(from, to) != 0) {
      TB_ERROR_ERRNO("act", "rename-player-file", to,
                     "player idnum %ld: %s -> %s", GET_IDNUM(vict), from, to);
      goto rollback;
    }
    moved[m] = 1;
  }
  return TRUE;

rollback:
  for (undo = 0; undo < m; undo++) {
    if (!moved[undo])
      continue;
    if (get_filename(from, sizeof(from), player_file_modes[undo], old_name) &&
        get_filename(to, sizeof(to), player_file_modes[undo], new_name) &&
        rename(to, from) != 0)
      TB_ERROR_ERRNO("act", "rename-player-rollback", from,
                     "player idnum %ld: could not move %s back", GET_IDNUM(vict), to);
  }
  return FALSE;
}

/** Rename vict to new_name: 'set <player> name <new>' (perform_set).
 * Preconditions: vict is in the player index; ch is an IMPL or vict itself.
 * new_name is validated here (length, valid_name, fill/reserved words, not
 * online, not in the index).
 * Order: the per-player files are moved first (rename_player_files), then
 * the index entry and in-memory name change and the index is saved. If a
 * file cannot be moved, everything is rolled back and FALSE is returned;
 * the index is untouched. perform_set saves the pfile under the new name.
 * The player is told only when online.
 * @return TRUE when the rename is complete. */
bool change_player_name(struct char_data *ch, struct char_data *vict, char *new_name)
{
  int i, k;
  char old_name[MAX_NAME_LENGTH + 1];

  if (!ch)
  {
    log("SYSERR: No char passed to change_player_name.");
    return FALSE;
  }

  if (!vict)
  {
    log("SYSERR: No victim passed to change_player_name.");
    send_to_char(ch, "Invalid victim.\r\n");
    return FALSE;
  }

  if (!new_name || !(*new_name) || strlen(new_name) < 2 ||
      strlen(new_name) > MAX_NAME_LENGTH || !valid_name(new_name) ||
      fill_word(new_name) || reserved_word(new_name) ) {
    send_to_char(ch, "Invalid new name.\r\n");
    return FALSE;
  }

  /* Check that nobody online or in the index already has new_name.
   * load_char() finds players only through the index, so the index check
   * is the same test without allocating a probe character. */
  if (get_player_vis(ch, new_name, NULL, FIND_CHAR_WORLD) != NULL ||
      get_ptable_by_name(new_name) >= 0) {
    send_to_char(ch, "Sorry, the new name already exists.\r\n");
    return FALSE;
  }

  /* New playername is OK - find the entry in the index */
  for (i = 0; i <= top_of_p_table; i++)
    if (player_table[i].id == GET_IDNUM(vict))
      break;

  /* Invariant: vict came from the index (online or load_char), so it is
   * there. i == top_of_p_table + 1 is one past the end of player_table. */
  if (i > top_of_p_table)
  {
    send_to_char(ch, "Your target was not found in the player index.\r\n");
    TB_ERROR_MSG("act", "rename-index", "player_table",
                 "player idnum %ld not in index (top %d); caller " DIAG_CHAR_FMT,
                 GET_IDNUM(vict), top_of_p_table, DIAG_CHAR(ch));
    return FALSE;
  }

  /* Set up a few variables that will be needed */
  tb_snprintf(old_name, sizeof(old_name), "%s", GET_NAME(vict));

  /* Move the per-player files before touching the index, so a failure
   * leaves the player exactly as before. */
  if (!rename_player_files(vict, old_name, new_name)) {
    send_to_char(ch, "Could not rename the player's files (see the log).\r\n");
    return FALSE;
  }

  /* Now start changing the name over - all checks and setup have passed */
  free(player_table[i].name);              // Free the old name in the index
  player_table[i].name = strdup(new_name); // Insert the new name into the index
  for (k=0; (*(player_table[i].name+k) = LOWER(*(player_table[i].name+k))); k++);

  free(GET_PC_NAME(vict));
  GET_PC_NAME(vict) = strdup(CAP(new_name));    // Change the name in the victims char struct

  /* Save the changed player index - the pfile is saved by perform_set */
  save_player_index();

  mudlog(BRF, LVL_IMMORT, TRUE, "(GC) %s changed the name of %s to %s", GET_NAME(ch), old_name, new_name);

  if (vict->desc)  /* Descriptor is set if the victim is logged in */
    send_to_char(vict, "Your login name has changed from %s%s%s to %s%s%s.\r\n", CCYEL(vict, C_NRM), old_name, CCNRM(vict, C_NRM),
                                                                                 CCYEL(vict, C_NRM), new_name, CCNRM(vict, C_NRM));

  return TRUE;
}

ACMD(do_zlock)
{
  zone_vnum znvnum;
  zone_rnum zn;
  char      arg[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
  int       counter = 0;
  bool      fail = FALSE;

  two_arguments(argument, arg, arg2);

  if (!*arg) {
    send_to_char(ch, "Usage: %szlock <zone number>%s\r\n", QYEL, QNRM);
    send_to_char(ch, "%s       zlock list%s\r\n\r\n", QYEL, QNRM);
    send_to_char(ch, "Locks a zone so that building or editing is not possible.\r\n");
    send_to_char(ch, "The 'list' shows all currently locked zones.\r\n");
    send_to_char(ch, "'zlock all' will lock every zone with the GRID flag set.\r\n");
    send_to_char(ch, "'zlock all all' will lock every zone in the MUD.\r\n");
    return;
  }
  if (is_abbrev(arg, "all")) {
    if (GET_LEVEL(ch) < LVL_GRGOD) {
      send_to_char(ch, "You do not have sufficient access to lock all zones.\r\n");
      return;
    }
    if (!*arg2) {
      for (zn = 0; zn <= top_of_zone_table; zn++) {
        if (!ZONE_FLAGGED(zn, ZONE_NOBUILD) && ZONE_FLAGGED(zn, ZONE_GRID)) {
          counter++;
          SET_BIT_AR(ZONE_FLAGS(zn), ZONE_NOBUILD);
          if (save_zone(zn)) {
            log("(GC) %s has locked zone %d", GET_NAME(ch), zone_table[zn].number);
          } else {
            fail = TRUE;
          }
        }
      }
    } else if (is_abbrev(arg2, "all")) {
      for (zn = 0; zn <= top_of_zone_table; zn++) {
        if (!ZONE_FLAGGED(zn, ZONE_NOBUILD)) {
          counter++;
          SET_BIT_AR(ZONE_FLAGS(zn), ZONE_NOBUILD);
          if (save_zone(zn)) {
            log("(GC) %s has locked zone %d", GET_NAME(ch), zone_table[zn].number);
          } else {
            fail = TRUE;
          }
        }
      }
    }
    if (counter == 0) {
      send_to_char(ch, "There are no unlocked zones to lock!\r\n");
      return;
    }
    if (fail) {
      send_to_char(ch, "Unable to save zone changes.  Check syslog!\r\n");
      return;
    }
    send_to_char(ch, "%d zones have now been locked.\r\n", counter);
    mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s has locked ALL zones!", GET_NAME(ch));
    return;
  }
  if (is_abbrev(arg, "list")) {
    /* Show all locked zones */
    for (zn = 0; zn <= top_of_zone_table; zn++) {
      if (ZONE_FLAGGED(zn, ZONE_NOBUILD)) {
        if (!counter) send_to_char(ch, "Locked Zones\r\n");

        send_to_char(ch, "[%s%3d%s] %s%-*s %s%-1s%s\r\n",
          QGRN, zone_table[zn].number, QNRM, QCYN, count_color_chars(zone_table[zn].name)+30, zone_table[zn].name,
          QYEL, zone_table[zn].builders ? zone_table[zn].builders : "None.", QNRM);
        counter++;
      }
    }
    if (counter == 0) {
      send_to_char(ch, "There are currently no locked zones!\r\n");
    }
    return;
  }
  else if ((znvnum = tb_parse_int_or_zero(arg)) == 0) {
    send_to_char(ch, "Usage: %szlock <zone number>%s\r\n", QYEL, QNRM);
    return;
  }

  if ((zn = real_zone(znvnum)) == NOWHERE) {
    send_to_char(ch, "That zone does not exist!\r\n");
    return;
  }

  /* Check the builder list */
  if (GET_LEVEL(ch) < LVL_GRGOD && !is_name(GET_NAME(ch), zone_table[zn].builders) && GET_OLC_ZONE(ch) != znvnum) {
    send_to_char(ch, "You do not have sufficient access to lock that zone!\r\n");
    return;
  }

  /* If we get here, player has typed 'zlock <num>' */
  if (ZONE_FLAGGED(zn, ZONE_NOBUILD)) {
    send_to_char(ch, "Zone %d is already locked!\r\n", znvnum);
    return;
  }
  SET_BIT_AR(ZONE_FLAGS(zn), ZONE_NOBUILD);
  if (save_zone(zn)) {
    mudlog(NRM, MAX(LVL_GRGOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s has locked zone %d", GET_NAME(ch), znvnum);
  }
  else
  {
    send_to_char(ch, "Unable to save zone changes.  Check syslog!\r\n");
  }
}

ACMD(do_zunlock)
{
  zone_vnum znvnum;
  zone_rnum zn;
  char      arg[MAX_INPUT_LENGTH];
  int       counter = 0;
  bool      fail = FALSE;

  one_argument(argument, arg);

  if (!*arg) {
    send_to_char(ch, "Usage: %szunlock <zone number>%s\r\n", QYEL, QNRM);
    send_to_char(ch, "%s       zunlock list%s\r\n\r\n", QYEL, QNRM);
    send_to_char(ch, "Unlocks a 'locked' zone to allow building or editing.\r\n");
    send_to_char(ch, "The 'list' shows all currently unlocked zones.\r\n");
    send_to_char(ch, "'zunlock all' will unlock every zone in the MUD.\r\n");
    return;
  }
  if (is_abbrev(arg, "all")) {
    if (GET_LEVEL(ch) < LVL_GRGOD) {
      send_to_char(ch, "You do not have sufficient access to lock zones.\r\n");
      return;
    }
    for (zn = 0; zn <= top_of_zone_table; zn++) {
      if (ZONE_FLAGGED(zn, ZONE_NOBUILD)) {
        counter++;
        REMOVE_BIT_AR(ZONE_FLAGS(zn), ZONE_NOBUILD);
        if (save_zone(zn)) {
          log("(GC) %s has unlocked zone %d", GET_NAME(ch), zone_table[zn].number);
        } else {
          fail = TRUE;
        }
      }
    }
    if (counter == 0) {
      send_to_char(ch, "There are no locked zones to unlock!\r\n");
      return;
    }
    if (fail) {
      send_to_char(ch, "Unable to save zone changes.  Check syslog!\r\n");
      return;
    }
    send_to_char(ch, "%d zones have now been unlocked.\r\n", counter);
    mudlog(BRF, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s has unlocked ALL zones!", GET_NAME(ch));
    return;
  }
  if (is_abbrev(arg, "list")) {
    /* Show all unlocked zones */
    for (zn = 0; zn <= top_of_zone_table; zn++) {
      if (!ZONE_FLAGGED(zn, ZONE_NOBUILD)) {
        if (!counter) send_to_char(ch, "Unlocked Zones\r\n");

        send_to_char(ch, "[%s%3d%s] %s%-*s %s%-1s%s\r\n",
          QGRN, zone_table[zn].number, QNRM, QCYN, count_color_chars(zone_table[zn].name)+30, zone_table[zn].name,
          QYEL, zone_table[zn].builders ? zone_table[zn].builders : "None.", QNRM);
        counter++;
      }
    }
    if (counter == 0) {
      send_to_char(ch, "There are currently no unlocked zones!\r\n");
    }
    return;
  }
  else if ((znvnum = tb_parse_int_or_zero(arg)) == 0) {
    send_to_char(ch, "Usage: %szunlock <zone number>%s\r\n", QYEL, QNRM);
    return;
  }

  if ((zn = real_zone(znvnum)) == NOWHERE) {
    send_to_char(ch, "That zone does not exist!\r\n");
    return;
  }

  /* Check the builder list */
  if (GET_LEVEL(ch) < LVL_GRGOD && !is_name(GET_NAME(ch), zone_table[zn].builders) && GET_OLC_ZONE(ch) != znvnum) {
    send_to_char(ch, "You do not have sufficient access to unlock that zone!\r\n");
    return;
  }

  /* If we get here, player has typed 'zunlock <num>' */
  if (!ZONE_FLAGGED(zn, ZONE_NOBUILD)) {
    send_to_char(ch, "Zone %d is already unlocked!\r\n", znvnum);
    return;
  }
  REMOVE_BIT_AR(ZONE_FLAGS(zn), ZONE_NOBUILD);
  if (save_zone(zn)) {
    mudlog(NRM, MAX(LVL_GRGOD, GET_INVIS_LEV(ch)), TRUE, "(GC) %s has unlocked zone %d", GET_NAME(ch), znvnum);
  }
  else
  {
    send_to_char(ch, "Unable to save zone changes.  Check syslog!\r\n");
  }
}

/* get highest vnum in recent player list  */
static int get_max_recent(void)
{
  struct recent_player *this;
  int iRet=0;

  this = recent_list;

  while (this)
  {
   if (this->vnum > iRet) iRet = this->vnum;
   this = this->next;
  }

  return iRet;
}

/* clear an item in recent player list */
static void clear_recent(struct recent_player *this)
{
  this->vnum = 0;
  this->time = 0;
  this->name[0] = '\0';
  this->host[0] = '\0';
  this->next = NULL;
}

/* create new blank player in recent players list */
static struct recent_player *create_recent(void)
{
  struct recent_player *newrecent;

  CREATE(newrecent, struct recent_player, 1);
  clear_recent(newrecent);
  newrecent->next = recent_list;
  recent_list = newrecent;

  newrecent->vnum = get_max_recent();
  newrecent->vnum++;
  return newrecent;
}

/** Add a player to the recent-players list shown by 'recent'.
 * Callers: interpreter.c on login (newplr for a new character) and comm.c
 * on copyover recovery (cpyplr). The list grows for the life of the
 * process (one entry per login) and its numbering restarts after a
 * copyover. name and host are copied with strlcpy into fixed fields and
 * cut silently at their size.
 * @return FALSE for a connection that never gave a name. */
bool AddRecentPlayer(char *chname, char *chhost, bool newplr, bool cpyplr)
{
  struct recent_player *this;
  time_t ct;
  int max_vnum;

  if (!chname || !*chname) // dropped connection with no name given
       return FALSE;

  ct = time(0);  /* Grab the current time */

  this = create_recent();

  if (!this) return FALSE;

  this->time = ct;
  this->new_player = newplr;
  this->copyover_player = cpyplr;
  strlcpy(this->host, chhost ? chhost : "", sizeof(this->host));
  strlcpy(this->name, chname, sizeof(this->name));
  max_vnum = get_max_recent();
  this->vnum = max_vnum;   /* Possibly should be +1 ? */

  return TRUE;
}

/** Free the recent-players list. Called at shutdown (comm.c). The list
 * head is cleared, so a later AddRecentPlayer() starts a new list instead
 * of linking to freed memory. */
void free_recent_players(void)
{
  struct recent_player *this;
  struct recent_player *temp;

  this = recent_list;
  recent_list = NULL;

  while((temp = this) != NULL)
  {
	this = this->next;
	free(temp);
  }
}

ACMD(do_recent)
{
  time_t ct;
  char timestr[MAX_INPUT_LENGTH], arg[MAX_INPUT_LENGTH];
  int hits = 0, limit = 0, count = 0;
  struct recent_player *this;
  bool loc;

  one_argument(argument, arg);
  if (!*arg) {
    limit = 0;
  } else {
    limit = tb_parse_int_or_zero(arg);
  }

  if (GET_LEVEL(ch) >= LVL_GRGOD) {  /* If High-Level Imm, then show Host IP */
    send_to_char(ch, " ID | DATE/TIME                | HOST IP                          | Player Name\r\n");
  } else {
    send_to_char(ch, " ID | DATE/TIME                | Player Name\r\n");
  }

  this = recent_list;
  while(this)
  {
    loc = FALSE;
    hits++;
    ct = this->time;
    TIME_STR(timestr, "%a %b %d %H:%M:%S %Y", localtime(&ct));

    if (*(this->host)) {
      if (!strcmp(this->host, "localhost")) loc = TRUE;
    }

    if ((limit == 0) || (count < limit))
    {
      if (GET_LEVEL(ch) >= LVL_GRGOD)   /* If High-Level Imm, then show Host IP */
      {
        if (this->new_player == TRUE) {
          send_to_char(ch, "%3d | %-24.24s | %s%-32s%s | %s %s(New Player)%s\r\n", this->vnum, timestr, loc ? QRED : "", this->host, QNRM, this->name, QYEL, QNRM);
        } else if (this->copyover_player == TRUE) {
          send_to_char(ch, "%3d | %-24.24s | %s%-32s%s | %s %s(Copyover)%s\r\n", this->vnum, timestr, loc ? QRED : "", this->host, QNRM, this->name, QCYN, QNRM);
        } else {
          send_to_char(ch, "%3d | %-24.24s | %s%-32s%s | %s\r\n", this->vnum, timestr, loc ? QRED : "", this->host, QNRM, this->name);
        }
      }
      else
      {
        if (this->new_player == TRUE) {
          send_to_char(ch, "%3d | %-24.24s | %s %s(New Player)%s\r\n", this->vnum, timestr, this->name, QYEL, QNRM);
        } else if (this->copyover_player == TRUE) {
          send_to_char(ch, "%3d | %-24.24s | %s %s(Copyover)%s\r\n", this->vnum, timestr, this->name, QCYN, QNRM);
        } else {
          send_to_char(ch, "%3d | %-24.24s | %s\r\n", this->vnum, timestr, this->name);
        }
      }
      count++;

      this = this->next;
    }
    else
    {
      this = NULL;
    }
  }

  ct = time(0);  /* Grab the current time */
  TIME_STR(timestr, "%c", localtime(&ct));
  send_to_char(ch, "Current Server Time: %s\r\nShowing %d players since last copyover/reboot\r\n", timestr, hits);
}


ACMD(do_oset)
{
  char arg[MAX_INPUT_LENGTH];
  char arg2[MAX_INPUT_LENGTH];
  const char usage[] = "Usage: \r\n"
                       "Options: alias, apply, longdesc, shortdesc\r\n"
                       "> oset <object> <option> <value>\r\n";
  struct obj_data *obj;
  bool success = TRUE;

  if (IS_NPC(ch) || ch->desc == NULL) {
    send_to_char(ch, "oset is only usable by connected players.\r\n");
    return;
  }

  argument = one_argument(argument, arg);

  if (!*arg)
    send_to_char(ch, usage);
  else if (!(obj = get_obj_in_list_vis(ch, arg, NULL, ch->carrying)) && 
    !(obj = get_obj_in_list_vis(ch, arg, NULL, world[IN_ROOM(ch)].contents)))
    send_to_char(ch, "You don't seem to have %s %s.\r\n", AN(arg), arg);
  else {
     argument = one_argument(argument, arg2);
     
     if (!*arg2) 
       send_to_char(ch, usage);
     else {
       if (is_abbrev(arg2, "alias") && (success = oset_alias(obj, argument)))
         send_to_char(ch, "Object alias set.\r\n");
       else if (is_abbrev(arg2, "longdesc") && (success = oset_long_description(obj, argument)))
         send_to_char(ch, "Object long description set.\r\n");
       else if (is_abbrev(arg2, "shortdesc") && (success = oset_short_description(obj, argument)))
         send_to_char(ch, "Object short description set.\r\n");
       else if (is_abbrev(arg2, "apply") && (success = oset_apply(obj, argument)))
         send_to_char(ch, "Object apply set.\r\n");           
       else {
         if (!success) 
           send_to_char(ch, "%s was unsuccessful.\r\n", arg2);
         else
           send_to_char(ch, usage);
         return;
       }
     }
  }
}
