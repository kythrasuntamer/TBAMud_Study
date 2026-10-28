/**************************************************************************
*  File: dg_mobcmd.c                                       Part of tbaMUD *
*  Usage: Contains the mobile script commands.                            *
*                                                                         *
*  $Author: N'Atas-ha/Mark A. Heilpern/egreen/Welcor $                    *
*  $Date: 2004/10/11 12:07:00$                                            *
*  $Revision: 1.0.14 $                                                    *
**************************************************************************/

/**
 * @file dg_mobcmd.c
 * The mobile script commands (mecho, mload, mpurge, mforce, mtransform,
 * mdoor, ...). Unlike object and room commands these are ordinary entries in
 * the main command table (interpreter.c), so a mob's script runs them
 * through command_interpreter() like any typed command.
 *
 * Who controls the input:
 *  - From a script: builder-written trigger code, which after var_subst()
 *    can carry player text. All arguments are parsed as untrusted.
 *  - Typed by a person: the commands are level 0, so anyone may type them.
 *    MOB_OR_IMPL() admits only NPCs not switched into by a lower immortal
 *    and characters that own triggers, and the latter includes players when
 *    CONFIG_SCRIPT_PLAYERS is on. The commands that create, destroy or force
 *    additionally require mob_cmd_privileged(); a charmed mob runs none of
 *    them (MOB_CMD_CHARMED()).
 *
 * Lifetime rules: the acting character ch can be extracted by its own
 * command (mpurge self, a forced command, damage). Characters are extracted
 * lazily (MOB_NOTDEADYET / PLR_NOTDEADYET), so ch stays readable until the
 * next extract_pending_chars(), but no command may act on a character after
 * a callback without re-acquiring it by UID. "all" loops walk a UID snapshot
 * (dg_cmd.h).
 *
 * Failures are reported with mob_log(): one script_log line for builders
 * plus a structured "cmd-rejected" warning naming the mob and call site.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "screen.h"
#include "dg_scripts.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "comm.h"
#include "spells.h"
#include "constants.h"
#include "genzon.h" /* for real_zone_by_thing */
#include "act.h"
#include "fight.h"
#include "dg_cmd.h" /* occupant snapshots for the "all" commands */


/* Local file scope functions. */
static void mob_log_at(char_data *mob, const char *src_file, int src_line,
                       const char *src_func, const char *format, ...)
  __attribute__ ((format (printf, 5, 6)));
/** Reports a rejected m-command; the diagnostic names the calling line. */
#define mob_log(mob, ...) \
  mob_log_at((mob), __FILE__, __LINE__, __func__, __VA_ARGS__)
static void mob_log_line(char_data *mob, const char *message);


/**
 * Sends one already-formatted line to the script log (syslog and builders),
 * prefixed with the mob's name and vnum. Used directly only by mlog, whose
 * text is the script author's own message and not a rejection.
 * @param mob The script owner (a mob, or a player who owns triggers).
 * @param message Finished text; logged as data, never as a format.
 */
static void mob_log_line(char_data *mob, const char *message)
{
  /* A player who owns triggers can reach here too: players have no short
   * description, so name them by their name. */
  script_log("Mob (%s, VNum %d):: %s",
             IS_NPC(mob) ? (GET_SHORT(mob) ? GET_SHORT(mob) : "(no short desc)")
                         : (GET_NAME(mob) ? GET_NAME(mob) : "(no name)"),
             GET_MOB_VNUM(mob), message);
}

/**
 * Reports an m-command that refused or could not finish what the script
 * asked (bad target, bad argument, missing prototype). The line goes to the
 * script log for builders and, as a structured "cmd-rejected" warning, to
 * the error log with the owner's identity and UID; the script_driver
 * breadcrumb supplies the trigger vnum.
 * @param mob The script owner.
 * @param src_file,src_line,src_func The refusing call site (from mob_log()).
 * @param format printf format for the reason.
 */
static void mob_log_at(char_data *mob, const char *src_file, int src_line,
                       const char *src_func, const char *format, ...)
{
  va_list args;
  char message[MAX_STRING_LENGTH];

  /* Format the caller's message on its own, then log it as data. Embedding
   * the name in the format would let a '%' in a builder-written name, or in
   * player text a script passes along, be interpreted as a conversion. */
  va_start(args, format);
  if (vsnprintf(message, sizeof(message), format, args) < 0)
    strlcpy(message, "(message formatting failed)", sizeof(message));
  va_end(args);

  mob_log_line(mob, message);
  /* script_id is read, not allocated: 0 means the mob was never given one. */
  tb_error_log_at("WARNING", "dg", "cmd-rejected", "m-command", 0,
                  src_file, src_line, src_func,
                  DIAG_CHAR_FMT " uid %ld: %s", DIAG_CHAR(mob),
                  mob->script_id, message);
}

/* Script target lookups. A script names a target either by UID ("%actor%"
 * expands to UID_CHAR followed by the id, found anywhere in the world) or by
 * name, which is looked up the way the mob ch would see it. arg is
 * evaluated more than once: pass a plain char array. */
#define DG_FIND_CHAR_ROOM(ch, arg) \
  (*(arg) == UID_CHAR ? get_char(arg) : get_char_room_vis((ch), (arg), NULL))
#define DG_FIND_CHAR_WORLD(ch, arg) \
  (*(arg) == UID_CHAR ? get_char(arg) \
                      : get_char_vis((ch), (arg), NULL, FIND_CHAR_WORLD))
#define DG_FIND_OBJ_VIS(ch, arg) \
  (*(arg) == UID_CHAR ? get_obj(arg) : get_obj_vis((ch), (arg), NULL))

/* Macro to determine if a mob is permitted to use these commands.
 * Note: the second clause also admits a *player* who owns triggers (allowed
 * when CONFIG_SCRIPT_PLAYERS is on), and such a player can type every
 * m-command by hand, because a typed command cannot be told apart from one
 * the player's own script runs. The commands that create, destroy or force
 * therefore check mob_cmd_privileged() as well. */
#define MOB_OR_IMPL(ch) \
 ((IS_NPC(ch) && (!(ch)->desc || GET_LEVEL((ch)->desc->original) >= LVL_IMPL)) || (SCRIPT(ch) && TRIGGERS(SCRIPT(ch))))

/* mload, mpurge, mforce, mhunt, mremember and mforget must not be usable by
 * a person typing them. A character with a descriptor is controlled by a
 * person: an immortal switched into a mob (desc->original is the immortal),
 * or a player who owns triggers (desc->original is NULL; the old check read
 * GET_LEVEL(desc->original) and dereferenced NULL). Only an implementor may
 * go on; GET_REAL_LEVEL() picks the controlling person in both cases. A
 * script-driven mob has no descriptor and is always allowed. */
static int mob_cmd_privileged(char_data *ch, const char *cmd)
{
  if (!ch->desc || GET_REAL_LEVEL(ch) >= LVL_IMPL)
    return TRUE;

  TB_WARN_MSG("dg", "mob-cmd-denied", cmd,
              DIAG_CHAR_FMT " is controlled by a %s of level %d (< %d); "
              "%s refused", DIAG_CHAR(ch),
              ch->desc->original ? "switched immortal" : "player",
              GET_REAL_LEVEL(ch), LVL_IMPL, cmd);
  mob_log(ch, "%s: refused, controlled by a person below implementor level", cmd);
  send_to_char(ch, "%s", CONFIG_HUH);
  return FALSE;
}

/* A charmed mob does not run m-commands: otherwise a player could 'order'
 * a charmed scripted mob to mload, mpurge or mforce. The refusal used to be
 * a silent return; it is reported so a builder can see why a script on a
 * charmed mob does nothing. The charm comes from a player (a spell), so this
 * is a warning, not an error.
 *
 * MOB_CMD_CHARMED(ch) is TRUE, after reporting, when ch is charmed. It is a
 * macro, not a function, so the diagnostic's source line and function are
 * the refusing ACMD itself. ch is evaluated several times: pass a variable. */
#define MOB_CMD_CHARMED(ch) \
  (AFF_FLAGGED((ch), AFF_CHARM) \
   ? (TB_WARN_MSG("dg", "mob-cmd-charmed", "m-command", \
                  DIAG_CHAR_FMT " uid %ld is charmed%s; m-command refused", \
                  DIAG_CHAR(ch), (ch)->script_id, \
                  (ch)->master ? " and has a master" : ""), TRUE) \
   : FALSE)

/* A character stranded in NOWHERE by an earlier, already reported failure
 * (char_to_room() refuses an invalid room and leaves its character there,
 * e.g. a return from the void to a room OLC deleted) cannot be moved:
 * char_from_room() deliberately aborts on it, and NOWHERE is no source room
 * for "mteleport all" (dg_teleport_all() would read world[NOWHERE]). A
 * script finds such a character by name or UID anywhere in the world.
 *
 * MOB_CMD_STRANDED(ch, who, op, cmd) is TRUE, after reporting, when who is
 * in no valid room: an ERROR dg/op at the refusing ACMD's line names who
 * and the script owner ch (the script_driver breadcrumb adds the trigger
 * vnum), and mob_log() tells builders. op is "stranded-target" when who is
 * the character to be moved, "stranded-actor" when who is ch and its room
 * is the source. ch and who are evaluated several times: pass variables. */
#define MOB_CMD_STRANDED(ch, who, op, cmd) \
  (!VALID_ROOM_RNUM(IN_ROOM(who)) \
   ? (TB_ERROR_MSG("dg", (op), (cmd), \
                   DIAG_CHAR_FMT " is in no valid room (rnum %d); %s by " \
                   DIAG_CHAR_FMT " uid %ld refused", DIAG_CHAR(who), \
                   (int) IN_ROOM(who), (cmd), DIAG_CHAR(ch), (ch)->script_id), \
      mob_log((ch), "%s: %s is stranded in no valid room", (cmd), \
              (who) == (ch) ? "the mob" : "the target"), TRUE) \
   : FALSE)

/* mob commands */
/* prints the argument to all the rooms aroud the mobile */
ACMD(do_masound)
{
    room_rnum was_in_room;
    int  door;

    if (!MOB_OR_IMPL(ch))
    {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!*argument)
    {
        mob_log(ch, "masound called with no argument");
        return;
    }

    skip_spaces(&argument);

    was_in_room = IN_ROOM(ch);
    for (door = 0; door < DIR_COUNT; door++)
    {
        struct room_direction_data *newexit;

        if (((newexit = world[was_in_room].dir_option[door]) != NULL) &&
            newexit->to_room != NOWHERE && newexit->to_room != was_in_room)
        {
            IN_ROOM(ch) = newexit->to_room;
            sub_write(argument, ch, TRUE, TO_ROOM);
        }
    }

    IN_ROOM(ch) = was_in_room;
}

/* lets the mobile kill any player or mobile */
ACMD(do_mkill)
{
    char arg[MAX_INPUT_LENGTH];
    char_data *victim;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    one_argument(argument, arg);

    if (!*arg) {
        mob_log(ch, "mkill called with no argument");
        return;
    }

    if (!(victim = DG_FIND_CHAR_ROOM(ch, arg))) {
      mob_log(ch, "mkill: victim (%s) not found",arg);
      return;
    }

    if (victim == ch) {
        mob_log(ch, "mkill: victim is self");
        return;
    }

    if (!IS_NPC(victim) && PRF_FLAGGED(victim, PRF_NOHASSLE)) {
        mob_log(ch, "mkill: target has nohassle on");
        return;
    }

    if (FIGHTING(ch)) {
        mob_log(ch, "mkill: already fighting");
        return;
    }

    hit(ch, victim, TYPE_UNDEFINED);
    return;
}

/* Lets the mobile destroy an object in its inventory it can also destroy a 
 * worn object and it can destroy items using all.xxxxx or just plain all of 
 * them. */
ACMD(do_mjunk)
{
    char arg[MAX_INPUT_LENGTH];
    int pos, dots, junked = 0;
    obj_data *obj;
    obj_data *obj_next;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    one_argument(argument, arg);

    if (!*arg) {
        mob_log(ch, "mjunk called with no argument");
        return;
    }

    /* The argument is script text. find_all_dots() classifies it and strips
     * a leading "all.", leaving the bare name. The test used to be inverted,
     * sending plain names down the "all." path, where arg[3] and arg + 4 read
     * past a short name's terminator and 'mjunk axe' junked everything. */
    dots = find_all_dots(arg);

    if (dots == FIND_INDIV) {
      /* One item: worn first, then carried. */
      /* Thanks to Carlos Myers for fixing the line below */
      if ((pos = get_obj_pos_in_equip_vis(ch, arg, NULL, ch->equipment)) >= 0) {
        extract_obj(unequip_char(ch, pos));
        junked++;
      } else if ((obj = get_obj_in_list_vis(ch, arg, NULL, ch->carrying)) != NULL) {
        extract_obj(obj);
        junked++;
      }
    } else {
      /* "all" junks everything; "all.<name>" junks what matches name. */
      for (obj = ch->carrying; obj != NULL; obj = obj_next) {
        obj_next = obj->next_content;
        if (dots == FIND_ALL || isname(arg, obj->name)) {
          extract_obj(obj);
          junked++;
        }
      }
      if (dots == FIND_ALL) {
        for (pos = 0; pos < NUM_WEARS; pos++)
          if (GET_EQ(ch, pos)) {
            extract_obj(unequip_char(ch, pos));
            junked++;
          }
      } else {
        while ((pos = get_obj_pos_in_equip_vis(ch, arg, NULL, ch->equipment)) >= 0) {
          extract_obj(unequip_char(ch, pos));
          junked++;
        }
      }
    }

    if (!junked && dots != FIND_ALL) {   /* an empty inventory is not an error */
      TB_WARN_MSG("dg", "mjunk-nomatch", "do_mjunk",
                  DIAG_CHAR_FMT " has nothing matching the %zu-byte argument; "
                  "nothing junked", DIAG_CHAR(ch), strlen(arg));
      mob_log(ch, "mjunk: nothing matches '%s'", arg);
    }
}

/* prints the message to everyone in the room other than the mob and victim */
ACMD(do_mechoaround)
{
    char arg[MAX_INPUT_LENGTH];
    char_data *victim;
    char *p;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    p = one_argument(argument, arg);
    skip_spaces(&p);

    if (!*arg) {
        mob_log(ch, "mechoaround called with no argument");
        return;
    }

    if (!(victim = DG_FIND_CHAR_ROOM(ch, arg))) {
      mob_log(ch, "mechoaround: victim (%s) does not exist",arg);
      return;
    }

    sub_write(p, victim, TRUE, TO_ROOM);
}

/* sends the message to only the victim */
ACMD(do_msend)
{
    char arg[MAX_INPUT_LENGTH];
    char_data *victim;
    char *p;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    p = one_argument(argument, arg);
    skip_spaces(&p);

    if (!*arg) {
        mob_log(ch, "msend called with no argument");
        return;
    }

    if (!(victim = DG_FIND_CHAR_ROOM(ch, arg))) {
      mob_log(ch, "msend: victim (%s) does not exist",arg);
      return;
    }

    sub_write(p, victim, TRUE, TO_CHAR);
}

/* prints the message to the room at large */
ACMD(do_mecho)
{
    char *p;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!*argument) {
        mob_log(ch, "mecho called with no arguments");
        return;
    }
    p = argument;
    skip_spaces(&p);

    sub_write(p, ch, TRUE, TO_ROOM);
    sub_write(p, ch, TRUE, TO_CHAR);
}

ACMD(do_mlog)
{
    char *p;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!*argument)
      return;

    p = argument;
    skip_spaces(&p);

    mob_log_line(ch, p); /* script text, may hold player input */

}

ACMD(do_mzoneecho)
{
    int zone, zone_vnum;
    char room_number[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH], *msg;
        
    if (!MOB_OR_IMPL(ch))
    {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }
    msg = any_one_arg(argument, room_number);
    skip_spaces(&msg);

    if (!*room_number || !*msg)
        mob_log(ch, "mzoneecho called with too few args");

    else if (!DG_PARSE_VNUM_OR_REPORT(room_number, &zone_vnum, "do_mzoneecho", DIAG_CHAR_FMT, DIAG_CHAR(ch)) ||
             (zone = real_zone_by_thing(zone_vnum)) == NOWHERE)
        mob_log(ch, "mzoneecho called for nonexistant zone");

    else {
        tb_snprintf(buf, sizeof(buf), "%s\r\n", msg);
        send_to_zone(buf, zone);
    }
}

/* Lets the mobile load an item or mobile.  All items are loaded into 
 * inventory, unless it is NO-TAKE. */
ACMD(do_mload)
{
    char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
    int number = 0;
    char_data *mob;
    obj_data *object;
    char *target;
    char_data *tch;
    obj_data *cnt;
    int pos;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!mob_cmd_privileged(ch, "mload"))
        return;

    target = two_arguments(argument, arg1, arg2);
    /* two_arguments() leaves target at the separating space; without this
     * the room-target form "load mob <vnum> <room>" never matched isdigit(). */
    if (target)
      skip_spaces(&target);

    if (!*arg1 || !*arg2 ||
        !DG_PARSE_VNUM_OR_REPORT(arg2, &number, "do_mload", DIAG_CHAR_FMT, DIAG_CHAR(ch))) {
        mob_log(ch, "mload: bad syntax");
        return;
    }

     /* load mob to target room - Jamie Nelson, April 13 2004 */
    if (is_abbrev(arg1, "mob")) {
      room_rnum rnum;
      if (!target || !*target) {
        rnum = IN_ROOM(ch);
      } else {
        int target_vnum;
        if (!isdigit(*target) ||
            !DG_PARSE_VNUM_OR_REPORT(target, &target_vnum, "do_mload", DIAG_CHAR_FMT, DIAG_CHAR(ch)) ||
            (rnum = real_room(target_vnum)) == NOWHERE) {
          mob_log(ch, "mload: room target vnum doesn't exist "
                      "(loading mob vnum %d to room %s)", number, target);
          return;
        }
      }
      if ((mob = read_mobile(number, VIRTUAL)) == NULL) {
        mob_log(ch, "mload: bad mob vnum");
        return;
      }
      char_to_room(mob, rnum);
      if (SCRIPT(ch)) { /* It _should_ have, but it might be detached. */
        char buf[MAX_INPUT_LENGTH];
        tb_snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, char_script_id(mob));
        add_var(&(SCRIPT(ch)->global_vars), "lastloaded", buf, 0);
      }
      load_mtrigger(mob);
    }

    else if (is_abbrev(arg1, "obj")) {
      if ((object = read_object(number, VIRTUAL)) == NULL) {
          mob_log(ch, "mload: bad object vnum");
          return;
      }
      if (SCRIPT(ch)) { /* It _should_ have, but it might be detached. */
        char buf[MAX_INPUT_LENGTH];
        tb_snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, obj_script_id(object));
        add_var(&(SCRIPT(ch)->global_vars), "lastloaded", buf, 0);
      }
      /* special handling to make objects able to load on a person/in a container/worn etc. */
      if (!target || !*target) {
        if (CAN_WEAR(object, ITEM_WEAR_TAKE)) {
            obj_to_char(object, ch);
        } else {
            obj_to_room(object, IN_ROOM(ch));
        }
        load_otrigger(object);
        return;
      }
      two_arguments(target, arg1, arg2); /* recycling ... */
      tch = DG_FIND_CHAR_ROOM(ch, arg1);
      if (tch) {
        if (*arg2 && (pos = find_eq_pos_script(arg2)) >= 0 &&
            !GET_EQ(tch, pos) && can_wear_on_pos(object, pos)) {
          equip_char(tch, object, pos);
          load_otrigger(object);
          return;
        }
        obj_to_char(object, tch);
        load_otrigger(object);
        return;
      }
      cnt = DG_FIND_OBJ_VIS(ch, arg1);
      /* get_obj_vis() falls back to scanning object_list, where the new,
       * still unplaced object is at the head: loading a container "into"
       * its own name found the object itself. obj_to_obj() refuses that,
       * which left the object in no location at all. */
      if (cnt == object) {
        TB_WARN_MSG("dg", "mload-place", "do_mload",
                    DIAG_CHAR_FMT " obj vnum %d uid %ld: container argument "
                    "matched the object being loaded; placed in room vnum %d",
                    DIAG_CHAR(ch), GET_OBJ_VNUM(object), obj_script_id(object),
                    GET_ROOM_VNUM(IN_ROOM(ch)));
        cnt = NULL;
      }
      if (cnt && GET_OBJ_TYPE(cnt) == ITEM_CONTAINER) {
      	obj_to_obj(object, cnt);
        load_otrigger(object);
      	return;
      }
      /* neither char nor container found - just dump it in room */
      obj_to_room(object, IN_ROOM(ch));
      load_otrigger(object);
      return;
    }

    else
        mob_log(ch, "mload: bad type");
}

/* Lets the mobile purge all objects and other npcs in the room, or purge a 
 * specified object or mob in the room.  It can purge itself, but this will 
 * be the last command it does. */
ACMD(do_mpurge)
{
  char arg[MAX_INPUT_LENGTH];
  char_data *victim;
  obj_data  *obj;

  if (!MOB_OR_IMPL(ch)) {
    send_to_char(ch, "%s", CONFIG_HUH);
    return;
  }

  if (MOB_CMD_CHARMED(ch))
    return;

  if (!mob_cmd_privileged(ch, "mpurge"))
    return;

  one_argument(argument, arg);

  if (!*arg) {
    /* 'purge' */
    char_data *vnext;
    obj_data  *obj_next;

    for (victim = world[IN_ROOM(ch)].people; victim; victim = vnext) {
      vnext = victim->next_in_room;
      if (IS_NPC(victim) && victim != ch)
        extract_char(victim);
    }

    for (obj = world[IN_ROOM(ch)].contents; obj; obj = obj_next) {
      obj_next = obj->next_content;
      extract_obj(obj);
    }

    return;
  }

  victim = DG_FIND_CHAR_ROOM(ch, arg);

  if (victim == NULL) {
    obj = DG_FIND_OBJ_VIS(ch, arg);

    if (obj) {
      extract_obj(obj);
      obj = NULL;
    } else
      mob_log(ch, "mpurge: bad argument");

    return;
  }

  if (!IS_NPC(victim)) {
      mob_log(ch, "mpurge: purging a PC");
      return;
  }

  extract_char(victim);
}

/* lets the mobile goto any location it wishes that is not private */
ACMD(do_mgoto)
{
    char arg[MAX_INPUT_LENGTH];
    room_rnum location;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    one_argument(argument, arg);

    if (!*arg) {
        mob_log(ch, "mgoto called with no argument");
        return;
    }

    if ((location = find_target_room(ch, arg)) == NOWHERE) {
        mob_log(ch, "mgoto: invalid location");
        return;
    }

    if (MOB_CMD_STRANDED(ch, ch, "stranded-target", "mgoto"))
        return;

    if (FIGHTING(ch))
        stop_fighting(ch);

    char_from_room(ch);
    char_to_room(ch, location);
    enter_wtrigger(&world[IN_ROOM(ch)], ch, -1);
}

/* lets the mobile do a command at another location. Very useful */
ACMD(do_mat)
{
    char arg[MAX_INPUT_LENGTH];
    room_rnum location, original;

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    argument = one_argument( argument, arg );

    if (!*arg || !*argument) {
        mob_log(ch, "mat: bad argument");
        return;
    }

    if ((location = find_target_room(ch, arg)) == NOWHERE) {
        mob_log(ch, "mat: invalid location");
        return;
    }

    if (MOB_CMD_STRANDED(ch, ch, "stranded-target", "mat"))
        return;

    original = IN_ROOM(ch);
    char_from_room(ch);
    char_to_room(ch, location);
    command_interpreter(ch, argument);

    /* See if 'ch' still exists before continuing! Handles 'at XXXX quit' case. */
    if (IN_ROOM(ch) == location) {
        char_from_room(ch);
        char_to_room(ch, original);
    }
}

/* Lets the mobile transfer people. The all argument transfers everyone in the 
 * current room to the specified location. */
ACMD(do_mteleport)
{
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
  room_rnum target;
  char_data *vict;

  if (!MOB_OR_IMPL(ch)) {
    send_to_char(ch, "%s", CONFIG_HUH);
    return;
  }

  if (MOB_CMD_CHARMED(ch))
    return;

  two_arguments(argument, arg1, arg2);

  if (!*arg1 || !*arg2) {
    mob_log(ch, "mteleport: bad syntax");
    return;
  }

  target = find_target_room(ch, arg2);

  if (target == NOWHERE) {
    mob_log(ch, "mteleport target is an invalid room");
    return;
  }

  if (!str_cmp(arg1, "all")) {
    if (MOB_CMD_STRANDED(ch, ch, "stranded-actor", "mteleport all"))
      return;
    if (target == IN_ROOM(ch)) {
      mob_log(ch, "mteleport all target is itself");
      return;
    }

    dg_teleport_all(IN_ROOM(ch), target);
  } else {
    if (!(vict = DG_FIND_CHAR_WORLD(ch, arg1))) {
      mob_log(ch, "mteleport: victim (%s) does not exist",arg1);
      return;
    }

    if (MOB_CMD_STRANDED(ch, vict, "stranded-target", "mteleport"))
      return;

    if (valid_dg_target(vict, DG_ALLOW_GODS)) {
      char_from_room(vict);
      char_to_room(vict, target);
      enter_wtrigger(&world[IN_ROOM(vict)], vict, -1);
    }
  }
}

ACMD(do_mdamage) {
  char name[MAX_INPUT_LENGTH], amount[MAX_INPUT_LENGTH];
  int dam = 0;
  char_data *vict;

  if (!MOB_OR_IMPL(ch)) {
    send_to_char(ch, "%s", CONFIG_HUH);
    return;
  }

  if (MOB_CMD_CHARMED(ch))
    return;

  two_arguments(argument, name, amount);

  if (!*name || !*amount) {
      mob_log(ch, "mdamage: bad syntax");
      return;
  }

  /* Not a number used to mean 0 damage, silently; now it is reported. */
  if (!DG_PARSE_INT_OR_REPORT(amount, &dam, "do_mdamage", DIAG_CHAR_FMT, DIAG_CHAR(ch))) {
      mob_log(ch, "mdamage: amount is not a number");
      return;
  }
  if (!(vict = DG_FIND_CHAR_ROOM(ch, name))) {
    mob_log(ch, "mdamage: victim (%s) does not exist", name);
    return;
  }
  script_damage(vict, dam);
}

/* Lets the mobile force someone to do something.  must be mortal level and the 
 * all argument only affects those in the room with the mobile. */
ACMD(do_mforce)
{
    char arg[MAX_INPUT_LENGTH];

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!mob_cmd_privileged(ch, "mforce"))
        return;

    argument = one_argument(argument, arg);

    if (!*arg || !*argument) {
        mob_log(ch, "mforce: bad syntax");
        return;
    }

    if (!str_cmp(arg, "all")) {
        struct descriptor_data *i;
        char_data *vch;

        for (i = descriptor_list; i ; i = i->next) {
            if ((i->character != ch) && !i->connected &&
                (IN_ROOM(i->character) == IN_ROOM(ch))) {
                vch = i->character;
                if (GET_LEVEL(vch) < GET_LEVEL(ch) && CAN_SEE(ch, vch) &&
                    valid_dg_target(vch, 0)) {
                    command_interpreter(vch, argument);
                }
            }
        }
    } else {
        char_data *victim;

        if (!(victim = DG_FIND_CHAR_ROOM(ch, arg))) {
          mob_log(ch, "mforce: victim (%s) does not exist",arg);
          return;
        }

        if (victim == ch) {
            mob_log(ch, "mforce: forcing self");
            return;
        }

        if (valid_dg_target(victim, 0))
            command_interpreter(victim, argument);
    }
}

/* hunt for someone */
ACMD(do_mhunt)
{
    char_data *victim;
    char arg[MAX_INPUT_LENGTH];

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!mob_cmd_privileged(ch, "mhunt"))
        return;

    one_argument(argument, arg);

    if (!*arg) {
        mob_log(ch, "mhunt called with no argument");
        return;
    }


    if (FIGHTING(ch)) return;

    if (!(victim = DG_FIND_CHAR_WORLD(ch, arg))) {
      mob_log(ch, "mhunt: victim (%s) does not exist", arg);
      return;
    }
    HUNTING(ch) = victim;


}

/* place someone into the mob's memory list */
ACMD(do_mremember)
{
    char_data *victim;
    struct script_memory *mem;
    char arg[MAX_INPUT_LENGTH];

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!mob_cmd_privileged(ch, "mremember"))
        return;

    argument = one_argument(argument, arg);

    if (!*arg) {
        mob_log(ch, "mremember: bad syntax");
        return;
    }

    if (!(victim = DG_FIND_CHAR_WORLD(ch, arg))) {
      mob_log(ch, "mremember: victim (%s) does not exist", arg);
      return;
    }

    /* create a structure and add it to the list */
    CREATE(mem, struct script_memory, 1);
    if (!SCRIPT_MEM(ch)) SCRIPT_MEM(ch) = mem;
    else {
      struct script_memory *tmpmem = SCRIPT_MEM(ch);
      while (tmpmem->next) tmpmem = tmpmem->next;
      tmpmem->next = mem;
    }

    /* fill in the structure */
    mem->id = char_script_id(victim);
    if (argument && *argument) {
      mem->cmd = strdup(argument);
    }
}

/* remove someone from the list */
ACMD(do_mforget)
{
    char_data *victim;
    struct script_memory *mem, *prev;
    char arg[MAX_INPUT_LENGTH];

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    if (!mob_cmd_privileged(ch, "mforget"))
        return;

    one_argument(argument, arg);

    if (!*arg) {
        mob_log(ch, "mforget: bad syntax");
        return;
    }

    if (!(victim = DG_FIND_CHAR_WORLD(ch, arg))) {
      mob_log(ch, "mforget: victim (%s) does not exist", arg);
      return;
    }

    mem = SCRIPT_MEM(ch);
    prev = NULL;
    while (mem) {
      if (mem->id == char_script_id(victim)) {
        if (mem->cmd) free(mem->cmd);
        if (prev==NULL) {
          SCRIPT_MEM(ch) = mem->next;
          free(mem);
          mem = SCRIPT_MEM(ch);
        } else {
          prev->next = mem->next;
          free(mem);
          mem = prev->next;
        }
      } else {
        prev = mem;
        mem = mem->next;
      }
   }
}

/* Free the description strings ch owns before mtransform overwrites them.
 * Same rule as free_char(): strings that are not the prototype's belong to
 * this instance (an earlier mtransform strdup()ed them). */
static void mtransform_release_strings(char_data *ch, mob_rnum rnum)
{
  struct char_player_data *own = &ch->player;
  const struct char_player_data *proto =
    (rnum != NOBODY && rnum <= top_of_mobt) ? &mob_proto[rnum].player : NULL;

#define MTRANSFORM_RELEASE(field) \
  do { \
    if (own->field && (!proto || own->field != proto->field)) \
      free(own->field); \
    own->field = NULL; \
  } while (0)
  MTRANSFORM_RELEASE(name);
  MTRANSFORM_RELEASE(title);
  MTRANSFORM_RELEASE(short_descr);
  MTRANSFORM_RELEASE(long_descr);
  MTRANSFORM_RELEASE(description);
#undef MTRANSFORM_RELEASE
}

/* Transform into a different mobile.
 *
 * The new prototype instance m is copied over ch field by field, so ch keeps
 * its address and every pointer other code holds to it stays valid. What ch
 * keeps (everything else comes from m):
 *  - identity and links: script_id, script, proto_script, script memory,
 *    events, room/world/combat list links, followers, master, group;
 *  - inventory and equipment, affects, hp/max hp/exp (unless "-vnum"), gold,
 *    position, carried weight and count, fighting and hunting targets;
 *  - the furniture it sits on (SITTING/NEXT_SITTING): the furniture's
 *    sitting_here list points at ch, and losing the link left the furniture
 *    pointing at ch after ch was freed;
 *  - mob memory (MEMORY), which only ch's extraction frees;
 *  - a pending extraction (MOB_NOTDEADYET), which extract_pending_chars()
 *    has already counted.
 * Strings: m's strings are prototype strings; ch gets private copies, and
 * the copies an earlier mtransform gave ch are freed first.
 * Only NPCs can be transformed: a player (one who owns triggers can reach
 * here when link-dead) would lose player_specials. */
ACMD(do_mtransform)
{
  char arg[MAX_INPUT_LENGTH];
  char_data *m, tmpmob;
  obj_data *obj[NUM_WEARS];
  mob_rnum this_rnum = GET_MOB_RNUM(ch);
  int keep_hp = 1; /* new mob keeps the old mob's hp/max hp/exp */
  int pos, vnum;

  if (!MOB_OR_IMPL(ch)) {
    send_to_char(ch, "%s", CONFIG_HUH);
    return;
  }

  if (MOB_CMD_CHARMED(ch))
    return;

  if (ch->desc) {
    send_to_char(ch, "You've got no VNUM to return to, dummy! try 'switch'\r\n");
    return;
  }

  if (!IS_NPC(ch)) {
    TB_ERROR_MSG("dg", "mtransform", "do_mtransform",
                 DIAG_CHAR_FMT " is not an NPC; transform refused",
                 DIAG_CHAR(ch));
    mob_log(ch, "mtransform: only NPCs can transform");
    return;
  }

  one_argument(argument, arg);

  if (!*arg)
    mob_log(ch, "mtransform: missing argument");
  else if (!isdigit(*arg) && *arg!='-')
    mob_log(ch, "mtransform: bad argument");
  else if (!DG_PARSE_VNUM_OR_REPORT(arg + (*arg == '-'), &vnum, "do_mtransform", DIAG_CHAR_FMT, DIAG_CHAR(ch)))
    mob_log(ch, "mtransform: argument is not a mobile vnum");
  else {
    if (*arg == '-')
      keep_hp = 0;
    m = read_mobile(vnum, VIRTUAL);
    if (m==NULL) {
      mob_log(ch, "mtransform: bad mobile vnum");
      return;
    }

    /* move new obj info over to old object and delete new obj */
    for (pos = 0; pos < NUM_WEARS; pos++) {
      if (GET_EQ(ch, pos))
        obj[pos] = unequip_char(ch, pos);
      else
        obj[pos] = NULL;
    }

    /* put the mob in the same room as ch so extract will work */
    char_to_room(m, IN_ROOM(ch));

    memcpy(&tmpmob, m, sizeof(*m));
    mtransform_release_strings(ch, this_rnum);

    /* Thanks to Russell Ryan for this fix. RRfon we need to copy the
       the strings so we don't end up free'ing the prototypes later */
    if(m->player.name)
      tmpmob.player.name = strdup(m->player.name);
    if(m->player.title)
      tmpmob.player.title = strdup(m->player.title);
    if(m->player.short_descr)
      tmpmob.player.short_descr = strdup(m->player.short_descr);
    if(m->player.long_descr)
      tmpmob.player.long_descr = strdup(m->player.long_descr);
    if(m->player.description)
      tmpmob.player.description = strdup(m->player.description);

    tmpmob.script_id = ch->script_id;
    tmpmob.affected = ch->affected;
    tmpmob.carrying = ch->carrying;
    tmpmob.proto_script = ch->proto_script;
    tmpmob.script = ch->script;
    tmpmob.memory = ch->memory;
    tmpmob.events = ch->events;
    tmpmob.next_in_room = ch->next_in_room;
    tmpmob.next = ch->next;
    tmpmob.next_fighting = ch->next_fighting;
    tmpmob.followers = ch->followers;
    tmpmob.master = ch->master;
    tmpmob.group = ch->group;

    GET_WAS_IN(&tmpmob) = GET_WAS_IN(ch);
    if (keep_hp) {
      GET_HIT(&tmpmob) = GET_HIT(ch);
      GET_MAX_HIT(&tmpmob) = GET_MAX_HIT(ch);
      GET_EXP(&tmpmob) = GET_EXP(ch);
    }
    GET_GOLD(&tmpmob) = GET_GOLD(ch);
    GET_POS(&tmpmob) = GET_POS(ch);
    /* A sitting mob stays on its furniture's sitter chain; taking these from
     * the fresh mob broke the chain and left the furniture pointing at a
     * character that no longer knew it was sitting there. */
    SITTING(&tmpmob) = SITTING(ch);
    NEXT_SITTING(&tmpmob) = NEXT_SITTING(ch);
    IS_CARRYING_W(&tmpmob) = IS_CARRYING_W(ch);
    IS_CARRYING_N(&tmpmob) = IS_CARRYING_N(ch);
    FIGHTING(&tmpmob) = FIGHTING(ch);
    HUNTING(&tmpmob) = HUNTING(ch);
    SITTING(&tmpmob) = SITTING(ch);
    NEXT_SITTING(&tmpmob) = NEXT_SITTING(ch);
    MEMORY(&tmpmob) = MEMORY(ch);
    if (MOB_FLAGGED(ch, MOB_NOTDEADYET))
      SET_BIT_AR(MOB_FLAGS(&tmpmob), MOB_NOTDEADYET);
    memcpy(ch, &tmpmob, sizeof(*ch));

    for (pos = 0; pos < NUM_WEARS; pos++) {
      if (obj[pos])
        equip_char(ch, obj[pos], pos);
    }

    ch->nr = this_rnum;
    extract_char(m);
  }
}

ACMD(do_mdoor)
{
    char target[MAX_INPUT_LENGTH], direction[MAX_INPUT_LENGTH];
    char field[MAX_INPUT_LENGTH], *value;
    room_data *rm;
    int dir, fd;

    const char *door_field[] = {
        "purge",
        "description",
        "flags",
        "key",
        "name",
        "room",
        "\n"
    };

    if (!MOB_OR_IMPL(ch)) {
        send_to_char(ch, "%s", CONFIG_HUH);
        return;
    }

    if (MOB_CMD_CHARMED(ch))
        return;

    argument = two_arguments(argument, target, direction);
    value = one_argument(argument, field);
    skip_spaces(&value);

    if (!*target || !*direction || !*field) {
        mob_log(ch, "mdoor called with too few args");
        return;
    }

    if ((rm = get_room(target)) == NULL) {
        mob_log(ch, "mdoor: invalid target (arg == %s)", target);
        return;
    }

    if ((dir = search_block(direction, dirs, FALSE)) == -1) {
        char dirs_str[256];
        int di, doff = 0;
        dirs_str[0] = '\0';
        for (di = 0; *dirs[di] != '\n'; di++)
            doff += snprintf(dirs_str + doff, sizeof(dirs_str) - doff, "%s%s", doff ? " " : "", dirs[di]);
        mob_log(ch, "mdoor: invalid direction (arg == %s) not found in: [ %s ]", direction, dirs_str);
        return;
    }

    if ((fd = search_block(field, door_field, FALSE)) == -1) {
        char fields_str[256];
        int fi, foff = 0;
        fields_str[0] = '\0';
        for (fi = 0; *door_field[fi] != '\n'; fi++)
            foff += snprintf(fields_str + foff, sizeof(fields_str) - foff, "%s%s", foff ? " " : "", door_field[fi]);
        mob_log(ch, "mdoor: invalid field (arg == %s) not found in: [ %s ]", field, fields_str);
        return;
    }

    if (!dg_door_edit(rm, dir, fd, value, "do_mdoor"))
        mob_log(ch, "mdoor: invalid %s (arg == %s)", door_field[fd], value);
}

ACMD(do_mfollow)
{
  char buf[MAX_INPUT_LENGTH];
  struct char_data *leader;
  struct follow_type *k;

  if (!MOB_OR_IMPL(ch)) {
    send_to_char(ch, "%s", CONFIG_HUH);
    return;
  }

  if (MOB_CMD_CHARMED(ch))
    return;

  one_argument(argument, buf);

  if (!*buf) {
    mob_log(ch, "mfollow: bad syntax");
    return;
  }

  if (!(leader = DG_FIND_CHAR_ROOM(ch, buf))) {
    mob_log(ch, "mfollow: victim (%s) does not exist", buf);
    return;
  }

  if (ch->master == leader) /* already following */
    return;

  if (ch->master && MOB_CMD_CHARMED(ch))  /* can't override charm */
    return;


  /* stop following someone else first */
  if (ch->master) {
    struct follow_type **link;

    /* The old unlink assumed ch was on the list and dereferenced NULL when
     * the list was empty or did not hold it. */
    for (link = &ch->master->followers; *link && (*link)->follower != ch;
         link = &(*link)->next);
    if (*link) {
      k = *link;
      *link = k->next;
      free(k);
    } else
      TB_ERROR_MSG("dg", "follower-list", "mfollow",
                   DIAG_CHAR_FMT " is not in its master's follower list",
                   DIAG_CHAR(ch));
    ch->master = NULL;
  }

  if (ch == leader)
    return;

  if (circle_follow(ch, leader)) {
    mob_log(ch, "mfollow: Following in circles.");
    return;
  }

  ch->master = leader;

  CREATE(k, struct follow_type, 1);

  k->follower = ch;
  k->next = leader->followers;
  leader->followers = k;
}

/* Prints the message to everyone in the range of numbers. Thanks to Jamie 
 * Nelson of 4D for this contribution */
ACMD(do_mrecho)
{
    char start[MAX_INPUT_LENGTH], finish[MAX_INPUT_LENGTH], *msg;
    int lo, hi;

    if (!MOB_OR_IMPL(ch)) {
      send_to_char(ch, "%s", CONFIG_HUH);
      return;
    }
    msg = two_arguments(argument, start, finish);

    skip_spaces(&msg);

    if (!*msg || !*start || !*finish || !is_number(start) || !is_number(finish))
      mob_log(ch, "mrecho called with too few args");
    else if (!DG_PARSE_INT_OR_REPORT(start, &lo, "do_mrecho", DIAG_CHAR_FMT, DIAG_CHAR(ch)) ||
             !DG_PARSE_INT_OR_REPORT(finish, &hi, "do_mrecho", DIAG_CHAR_FMT, DIAG_CHAR(ch)))
      mob_log(ch, "mrecho: room range out of range");
    else
      send_to_range(lo, hi, "%s\r\n", msg);
}
