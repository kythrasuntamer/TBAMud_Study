/**************************************************************************
*  File: dg_wldcmd.c                                       Part of tbaMUD *
*  Usage: Contains the command_interpreter for rooms, room commands.      *
*                                                                         *
*  $Author: galion/Mark A. Heilpern/egreen/Welcor $                       *
*  $Date: 2004/10/11 12:07:00$                                            *
*  $Revision: 1.0.14 $                                                    *
**************************************************************************/

/**
 * @file dg_wldcmd.c
 * The room script commands (wecho, wload, wpurge, wteleport, wdoor, wat,
 * ...) and wld_command_interpreter(), which script_driver() calls for each
 * command line of a room trigger after variable substitution.
 *
 * Who controls the input: builder-written trigger code, which after
 * var_subst() can carry player text (%speech%, %arg%, %actor.name%). All
 * arguments are parsed as untrusted; free text is logged only as data.
 *
 * Lifetime rules: a room is never freed while its script runs, but anything
 * in it can be. Commands that act on every occupant ("all") walk a UID
 * snapshot (dg_cmd.h) and re-acquire each character, because the callback
 * they run (a forced command, an enter trigger) can move or extract the
 * next one.
 *
 * Failures are reported with wld_log(): one script_log line for builders
 * plus a structured "cmd-rejected" warning naming the room and call site.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "screen.h"
#include "dg_scripts.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "constants.h"
#include "genzon.h" /* for zone_rnum real_zone_by_thing */
#include "fight.h"  /* for die() */
#include "dg_cmd.h" /* occupant snapshots for the "all" commands */

/* Local functions, macros, defines and structs */

#define WCMD(name)  \
    void (name)(room_data *room, char *argument, int cmd, int subcmd)

/* for do_wsend */
#define SCMD_WSEND        0
#define SCMD_WECHOAROUND  1

struct wld_command_info {
    char *command;
    void (*command_pointer)
           (room_data *room, char *argument, int cmd, int subcmd);
    int        subcmd;
};

static void wld_log_at(room_data *room, const char *src_file, int src_line,
                       const char *src_func, const char *format, ...)
  __attribute__ ((format (printf, 5, 6)));
/** Reports a rejected w-command; the diagnostic names the calling line. */
#define wld_log(room, ...) \
  wld_log_at((room), __FILE__, __LINE__, __func__, __VA_ARGS__)
void act_to_room(char *str, room_data *room);
WCMD(do_wasound);
WCMD(do_wecho);
WCMD(do_wsend);
WCMD(do_wzoneecho);
WCMD(do_wrecho);
WCMD(do_wdoor);
WCMD(do_wteleport);
WCMD(do_wforce);
WCMD(do_wpurge);
WCMD(do_wload);
WCMD(do_wdamage);
WCMD(do_wat);
WCMD(do_wmove);
WCMD(do_wlog);


/**
 * Reports a w-command that refused or could not finish what the script
 * asked. The line goes to the script log (syslog and builders) and, as a
 * structured "cmd-rejected" warning, to the error log with the room vnum and
 * UID; the script_driver breadcrumb supplies the trigger vnum. wlog, whose
 * text is the script author's own message, calls script_log itself.
 * @param room The script owner.
 * @param src_file,src_line,src_func The refusing call site (from wld_log()).
 * @param format printf format for the reason.
 */
static void wld_log_at(room_data *room, const char *src_file, int src_line,
                       const char *src_func, const char *format, ...)
{
  va_list args;
  char message[MAX_STRING_LENGTH];

  /* Format the caller's message on its own, then log it as data. Embedding
   * it in the format would let a '%' in player text a script passes along be
   * interpreted as a conversion. */
  va_start(args, format);
  if (vsnprintf(message, sizeof(message), format, args) < 0)
    strlcpy(message, "(message formatting failed)", sizeof(message));
  va_end(args);

  script_log("Room %d :: %s", room->number, message);
  tb_error_log_at("WARNING", "dg", "cmd-rejected", "w-command", 0,
                  src_file, src_line, src_func, "room vnum %d uid %ld: %s",
                  room->number, room_script_id(room), message);
}

/* sends str to room */
/**
 * Sends an act() message to everyone in a room. act() needs an actor, so the
 * message goes out through the room's first occupant: TO_ROOM for everyone
 * else, then TO_CHAR for that occupant. $n and the other actor codes
 * therefore name the first occupant, not the room; callers pass text with no
 * act() codes. An empty room gets nothing.
 * @param str act() format text.
 * @param room Any room.
 */
void act_to_room(char *str, room_data *room)
{
    /* no one is in the room */
    if (!room->people)
        return;

    /* Since you can't use act(..., TO_ROOM) for an room, send it TO_ROOM and 
     * TO_CHAR for some char in the room. (just dont use $n or you might get 
     * strange results). */
    act(str, FALSE, room->people, 0, 0, TO_ROOM);
    act(str, FALSE, room->people, 0, 0, TO_CHAR);
}

/* World commands */
/* prints the argument to all the rooms aroud the room */
WCMD(do_wasound)
{
    int  door;

    skip_spaces(&argument);

    if (!*argument) {
        wld_log(room, "wasound called with no argument");
        return;
    }

    for (door = 0; door < DIR_COUNT; door++) {
        struct room_direction_data *newexit;

        if ((newexit = room->dir_option[door]) && (newexit->to_room != NOWHERE) &&
            room != &world[newexit->to_room])
            act_to_room(argument, &world[newexit->to_room]);
    }
}

WCMD(do_wecho)
{
    skip_spaces(&argument);

    if (!*argument)
        wld_log(room, "wecho called with no args");

    else
        act_to_room(argument, room);
}

WCMD(do_wlog)
{
  skip_spaces(&argument);

  if (!*argument)
    return;

  /* Script text, may hold player input: logged as data, and not a
   * rejection, so it bypasses wld_log's cmd-rejected warning. */
  script_log("Room %d :: %s", room->number, argument);
}

WCMD(do_wsend)
{
    char buf[MAX_INPUT_LENGTH], *msg;
    char_data *ch;

    msg = any_one_arg(argument, buf);

    if (!*buf)
    {
        wld_log(room, "wsend called with no args");
        return;
    }

    skip_spaces(&msg);

    if (!*msg)
    {
        wld_log(room, "wsend called without a message");
        return;
    }

    if ((ch = get_char_by_room(room, buf)))
    {
        if (subcmd == SCMD_WSEND)
            sub_write(msg, ch, TRUE, TO_CHAR);
        else if (subcmd == SCMD_WECHOAROUND)
            sub_write(msg, ch, TRUE, TO_ROOM);
    }

    else
        wld_log(room, "no target found for wsend");
}

WCMD(do_wzoneecho)
{
    zone_rnum zone;
    int zone_vnum;
    char room_num[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH], *msg;

    msg = any_one_arg(argument, room_num);
    skip_spaces(&msg);

    if (!*room_num || !*msg)
        wld_log(room, "wzoneecho called with too few args");

    else if (!DG_PARSE_VNUM_OR_REPORT(room_num, &zone_vnum, "do_wzoneecho", "room vnum %d", room->number) ||
             (zone = real_zone_by_thing(zone_vnum)) == NOWHERE)
        wld_log(room, "wzoneecho called for nonexistant zone");

    else {
        tb_snprintf(buf, sizeof(buf), "%s\r\n", msg);
        send_to_zone(buf, zone);
    }
}

/* prints the message to everyone in the range of numbers */
/* Thx to Jamie Nelson of 4D for this contribution */
WCMD(do_wrecho)
{
    char start[MAX_INPUT_LENGTH], finish[MAX_INPUT_LENGTH], *msg;
    int lo, hi;

    msg = two_arguments(argument, start, finish);

    skip_spaces(&msg);

    if (!*msg || !*start || !*finish || !is_number(start) || !is_number(finish))
      wld_log(room, "wrecho: too few args");
    else if (!DG_PARSE_INT_OR_REPORT(start, &lo, "do_wrecho", "room vnum %d", room->number) ||
             !DG_PARSE_INT_OR_REPORT(finish, &hi, "do_wrecho", "room vnum %d", room->number))
      wld_log(room, "wrecho: room range out of range");
    else
      send_to_range(lo, hi, "%s\r\n", msg);

}

/* Shared body of mdoor, odoor and wdoor: apply one field edit to exit dir of
 * room rm. field indexes the door_field[] table the commands share (0 purge,
 * 1 description, 2 flags, 3 key, 4 name, 5 room); value is script text.
 *
 * A missing exit is created for any field but "purge". It is created
 * leading NOWHERE: CREATE() zero-fills, and to_room 0 is a real room, so an
 * exit made by "flags" or "name" alone used to be a walkable exit into
 * rnum 0. Such an exit is reported, since it stays unusable until its room
 * is set. Returns FALSE (after reporting, with cmd as the resource) when
 * the room or key value is rejected; the caller adds its builder-log line. */
int dg_door_edit(room_data *rm, int dir, int field, char *value,
                 const char *cmd)
{
  struct room_direction_data *exit_p = rm->dir_option[dir];
  long key;
  int vnum;
  room_rnum to_room;

  if (field == 0) {               /* purge */
    if (exit_p) {
      free(exit_p->general_description);
      free(exit_p->keyword);
      free(exit_p);
      rm->dir_option[dir] = NULL;
    }
    return TRUE;
  }

  if (!exit_p) {
    CREATE(exit_p, struct room_direction_data, 1);
    exit_p->to_room = NOWHERE;
    exit_p->key = NOTHING;
    rm->dir_option[dir] = exit_p;
    if (field != 5)
      TB_WARN_MSG("dg", "door-create", cmd,
                  "room vnum %d dir %s: field %d set on a missing exit; "
                  "created it leading nowhere until its room is set",
                  rm->number, dirs[dir], field);
  }

  switch (field) {
  case 1:  /* description */
    {
      size_t desc_size = strlen(value) + 3;
      free(exit_p->general_description);
      CREATE(exit_p->general_description, char, desc_size);
      strlcpy(exit_p->general_description, value, desc_size);
      strlcat(exit_p->general_description, "\r\n", desc_size);
    }
    break;
  case 2:  /* flags       */
    exit_p->exit_info = (sh_int)asciiflag_conv(value);
    break;
  case 3:  /* key: an object vnum, or -1 for none */
    if (!tb_parse_long_checked(value, &key) || key < -1 ||
        key >= (long)IDXTYPE_MAX) {
      TB_ERROR_MSG("dg", "parse-vnum", cmd,
                   "room vnum %d dir %s: key argument of %zu bytes is not an "
                   "object vnum or -1; key unchanged", rm->number, dirs[dir],
                   strlen(value));
      return FALSE;
    }
    exit_p->key = key < 0 ? NOTHING : (obj_vnum)key;
    break;
  case 4:  /* name        */
    {
      size_t keyword_size = strlen(value) + 1;
      free(exit_p->keyword);
      CREATE(exit_p->keyword, char, keyword_size);
      strlcpy(exit_p->keyword, value, keyword_size);
    }
    break;
  case 5:  /* room        */
    if (dg_parse_vnum_strict(value, &vnum) && (to_room = real_room(vnum)) != NOWHERE)
      exit_p->to_room = to_room;
    else {
      exit_p->to_room = NOWHERE;
      TB_ERROR_MSG("dg", "door-target", cmd,
                   "room vnum %d dir %s: room argument of %zu bytes is not an "
                   "existing room vnum; exit leads nowhere", rm->number,
                   dirs[dir], strlen(value));
      return FALSE;
    }
    break;
  }
  return TRUE;
}

WCMD(do_wdoor)
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


    argument = two_arguments(argument, target, direction);
    value = one_argument(argument, field);
    skip_spaces(&value);

    if (!*target || !*direction || !*field) {
        wld_log(room, "wdoor called with too few args");
        return;
    }

    if ((rm = get_room(target)) == NULL) {
        wld_log(room, "wdoor: invalid target (arg == %s)", target);
        return;
    }

    if ((dir = search_block(direction, dirs, FALSE)) == -1) {
        char dirs_str[256];
        int di, doff = 0;
        dirs_str[0] = '\0';
        for (di = 0; *dirs[di] != '\n'; di++)
            doff += snprintf(dirs_str + doff, sizeof(dirs_str) - doff, "%s%s", doff ? " " : "", dirs[di]);
        wld_log(room, "wdoor: invalid direction (arg == %s) not found in: [ %s ]", direction, dirs_str);
        return;
    }

    if ((fd = search_block(field, door_field, FALSE)) == -1) {
        char fields_str[256];
        int fi, foff = 0;
        fields_str[0] = '\0';
        for (fi = 0; *door_field[fi] != '\n'; fi++)
            foff += snprintf(fields_str + foff, sizeof(fields_str) - foff, "%s%s", foff ? " " : "", door_field[fi]);
        wld_log(room, "wdoor: invalid field (arg == %s) not found in: [ %s ]", field, fields_str);
        return;
    }

    if (!dg_door_edit(rm, dir, fd, value, "do_wdoor"))
        wld_log(room, "wdoor: invalid %s (arg == %s)", door_field[fd], value);
}

WCMD(do_wteleport)
{
    char_data *ch;
    room_rnum target, nr;
    int vnum;
    char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];

    two_arguments(argument, arg1, arg2);

    if (!*arg1 || !*arg2) {
        wld_log(room, "wteleport called with too few args");
        return;
    }

    if (!DG_PARSE_VNUM_OR_REPORT(arg2, &vnum, "do_wteleport", "room vnum %d", room->number)) {
        wld_log(room, "wteleport target is not a room vnum");
        return;
    }
    nr = vnum;
    target = real_room(nr);

    if (target == NOWHERE)
        wld_log(room, "wteleport target is an invalid room");

    else if (!str_cmp(arg1, "all")) {
        if (nr == room->number) {
            wld_log(room, "wteleport all target is itself");
            return;
        }

        dg_teleport_all(real_room(room->number), target);
    }

    else
    {
        if ((ch = get_char_by_room(room, arg1))) {
          if (valid_dg_target(ch, DG_ALLOW_GODS)) {
            char_from_room(ch);
            char_to_room(ch, target);
            enter_wtrigger(&world[IN_ROOM(ch)], ch, -1);
          }
        }

        else
            wld_log(room, "wteleport: no target found");
    }
}

WCMD(do_wforce)
{
    char_data *ch;
    struct dg_memory_target *targets, *t;
    room_rnum origin;
    char arg1[MAX_INPUT_LENGTH], *line;

    line = one_argument(argument, arg1);

    if (!*arg1 || !*line) {
        wld_log(room, "wforce called with too few args");
        return;
    }

    if (!str_cmp(arg1, "all"))
    {
        /* A forced command can move the next occupant too (a leader's move
         * takes the followers along): walk a UID snapshot (dg_cmd.h). */
        origin = real_room(room->number);
        targets = dg_room_occupant_uids(origin);
        for (t = targets; t; t = t->next)
        {
            if (!(ch = dg_room_occupant_live(t->uid, origin))) {
                dg_report_occupant_skipped(t->uid, origin, "do_wforce");
                continue;
            }
            if (valid_dg_target(ch, 0))
            {
                command_interpreter(ch, line);
            }
        }
        dg_free_room_occupant_uids(targets);
    }

    else
    {
        if ((ch = get_char_by_room(room, arg1)))
        {
            if (valid_dg_target(ch, 0))
            {
                command_interpreter(ch, line);
            }
        }

        else
            wld_log(room, "wforce: no target found");
    }
}

/* purge all objects an npcs in room, or specified object or mob */
WCMD(do_wpurge)
{
  char arg[MAX_INPUT_LENGTH];
  char_data *ch, *next_ch;
  obj_data *obj = NULL, *next_obj;

  one_argument(argument, arg);

  if (!*arg) {
    /* purge all */
    for (ch = room->people; ch; ch = next_ch ) {
      next_ch = ch->next_in_room;
      if (IS_NPC(ch))
        extract_char(ch);
    }

    for (obj = room->contents; obj; obj = next_obj ) {
      next_obj = obj->next_content;
      extract_obj(obj);
    }

    return;
  }

  if (*arg == UID_CHAR)
    ch = get_char(arg);
  else
    ch = get_char_in_room(room, arg);

  if (!ch) {
    if (*arg == UID_CHAR)
      obj = get_obj(arg);
    else
      obj = get_obj_in_room(room, arg);

    if (obj) {
      extract_obj(obj);
    } else
      wld_log(room, "wpurge: bad argument");

    return;
  }

  if (!IS_NPC(ch)) {
    wld_log(room, "wpurge: purging a PC");
    return;
  }

  extract_char(ch);
}

/* loads a mobile or object into the room */
WCMD(do_wload)
{
    char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
    int number = 0;
    char_data *mob;
    obj_data *object;
    char *target;
    char_data *tch;
    obj_data *cnt;
    int pos;

    target = two_arguments(argument, arg1, arg2);
    /* two_arguments() leaves target at the separating space; without this
     * the room-target form "load mob <vnum> <room>" never matched isdigit(). */
    if (target)
      skip_spaces(&target);

    if (!*arg1 || !*arg2 ||
        !DG_PARSE_VNUM_OR_REPORT(arg2, &number, "do_wload", "room vnum %d", room->number)) {
        wld_log(room, "wload: bad syntax");
        return;
    }

    /* load mob to target room - Jamie Nelson, April 13 2004 */
    if (is_abbrev(arg1, "mob")) {
      room_rnum rnum;
      if (!target || !*target) {
        rnum = real_room(room->number);
      } else {
        int target_vnum;
        if (!isdigit(*target) ||
            !DG_PARSE_VNUM_OR_REPORT(target, &target_vnum, "do_wload", "room vnum %d", room->number) ||
            (rnum = real_room(target_vnum)) == NOWHERE) {
          wld_log(room, "wload: room target vnum doesn't exist (loading mob vnum %d to room %s)", number, target);
          return;
        }
      }
      if ((mob = read_mobile(number, VIRTUAL)) == NULL) {
        wld_log(room, "mload: bad mob vnum");
        return;
      }
      char_to_room(mob, rnum);
      if (SCRIPT(room)) { /* It _should_ have, but it might be detached. */
        char buf[MAX_INPUT_LENGTH];
        tb_snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, char_script_id(mob));
        add_var(&(SCRIPT(room)->global_vars), "lastloaded", buf, 0);
      }
      load_mtrigger(mob);
    }

    else if (is_abbrev(arg1, "obj")) {
      if ((object = read_object(number, VIRTUAL)) == NULL) {
          wld_log(room, "wload: bad object vnum");
          return;
      }
      /* Set lastloaded for every placement, as mload and oload do; it was
       * set only when no target was given, so '%lastloaded%' named the
       * previous load after 'wload obj <vnum> <char|container>'. */
      if (SCRIPT(room)) { /* It _should_ have, but it might be detached. */
        char buf[MAX_INPUT_LENGTH];
        tb_snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, obj_script_id(object));
        add_var(&(SCRIPT(room)->global_vars), "lastloaded", buf, 0);
      }
      /* special handling to make objects able to load on a person/in a container/worn etc. */
      if (!target || !*target) {
        obj_to_room(object, real_room(room->number));
        load_otrigger(object);
        return;
      }

      two_arguments(target, arg1, arg2); /* recycling ... */
      tch = get_char_in_room(room, arg1);
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
      cnt = get_obj_in_room(room, arg1);
      if (cnt && GET_OBJ_TYPE(cnt) == ITEM_CONTAINER) {
      	obj_to_obj(object, cnt);
        load_otrigger(object);
      	return;
      }
      /* neither char nor container found - just dump it in room */
      obj_to_room(object, real_room(room->number));
      load_otrigger(object);
      return;
    }

    else
        wld_log(room, "wload: bad type");
}

WCMD(do_wdamage) {
  char name[MAX_INPUT_LENGTH], amount[MAX_INPUT_LENGTH];
  int dam = 0;
  char_data *ch;

  two_arguments(argument, name, amount);

  if (!*name || !*amount) {
    wld_log(room, "wdamage: bad syntax");
    return;
  }

  /* Not a number used to mean 0 damage, silently; now it is reported. */
  if (!DG_PARSE_INT_OR_REPORT(amount, &dam, "do_wdamage", "room vnum %d", room->number)) {
    wld_log(room, "wdamage: amount is not a number");
    return;
  }
  ch = get_char_by_room(room, name);

  if (!ch) {
    wld_log(room, "wdamage: target not found");
    return;
  }

  script_damage(ch, dam);
}

WCMD(do_wat)     
{
  room_rnum loc = NOWHERE;
  struct char_data *ch;
  char arg[MAX_INPUT_LENGTH], *command; 

  command = any_one_arg(argument, arg); 

  if (!*arg) {
    wld_log(room, "wat called with no args");
    return;
  }

  skip_spaces(&command); 

  if (!*command) {
    wld_log(room, "wat called without a command");
    return;
  }

  if (isdigit(*arg)) {
    int vnum;
    if (DG_PARSE_VNUM_OR_REPORT(arg, &vnum, "do_wat", "room vnum %d", room->number))
      loc = real_room(vnum);
  }
  else if ((ch = get_char_by_room(room, arg))) loc = IN_ROOM(ch);
  
  if (loc == NOWHERE) {
    wld_log(room, "wat: location not found (%s)", arg);
    return;
  }
  wld_command_interpreter(&world[loc], command);
}

WCMD(do_wmove)
{
    obj_data *obj, *next_obj;
    room_rnum target, nr;
    int vnum;
    char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];

    two_arguments(argument, arg1, arg2);

    if (!*arg1 || !*arg2) {
        wld_log(room, "wmove called with too few args");
        return;
    }

    if (!DG_PARSE_VNUM_OR_REPORT(arg2, &vnum, "do_wmove", "room vnum %d", room->number)) {
        wld_log(room, "wmove target is not a room vnum");
        return;
    }
    nr = vnum;
    target = real_room(nr);

    if (target == NOWHERE) {
        wld_log(room, "wmove target is an invalid room");
    }
    else if (nr == room->number) {
        wld_log(room, "wmove target room is itself");
    }
    else if (!str_cmp(arg1, "all")) {

        for (obj = room->contents; obj; obj = next_obj)
        {
            next_obj = obj->next_content;
            obj_from_room(obj);
            obj_to_room(obj, target);
        }
    }

    else
    {
        if ((obj = get_obj_by_room(room, arg1))) {
            obj_from_room(obj);
            obj_to_room(obj, target);
        }

        else
            wld_log(room, "wmove: no target found");
    }
}

static const struct wld_command_info wld_cmd_info[] = {
    { "RESERVED", 0, 0 },/* this must be first -- for specprocs */

    { "wasound "    , do_wasound   , 0 },
    { "wdoor "      , do_wdoor     , 0 },
    { "wecho "      , do_wecho     , 0 },
    { "wechoaround ", do_wsend     , SCMD_WECHOAROUND },
    { "wforce "     , do_wforce    , 0 },
    { "wload "      , do_wload     , 0 },
    { "wpurge "     , do_wpurge    , 0 },
    { "wrecho "     , do_wrecho    , 0 },
    { "wsend "      , do_wsend     , SCMD_WSEND },
    { "wteleport "  , do_wteleport , 0 },
    { "wzoneecho "  , do_wzoneecho , 0 },
    { "wdamage "    , do_wdamage,    0 },
    { "wat "        , do_wat,        0 },
    { "wmove "      , do_wmove     , 0 },
    { "wlog"        , do_wlog      , 0 },
    { "\n", 0, 0 }        /* this must be last */
};

/**
 * Runs one room-script command line; called by script_driver().
 *
 * The first word (lowercased by any_one_arg()) is matched as a prefix
 * against wld_cmd_info[] in table order, so an abbreviation runs the first
 * entry it abbreviates: "w" is wasound, "wd" is wdoor. The trailing space in
 * most entries has no effect (a word holds no space); wlog has none. The
 * leading "RESERVED" placeholder has no function, and no lowercased word can
 * match it. An unknown word is reported with wld_log().
 * @param room The script owner.
 * @param argument The substituted command line; commands may modify it.
 */
void wld_command_interpreter(room_data *room, char *argument)
{
    int cmd, length;
    char *line, arg[MAX_INPUT_LENGTH];

    skip_spaces(&argument);

    /* just drop to next line for hitting CR */
    if (!*argument)
        return;

    line = any_one_arg(argument, arg);


    /* find the command */
    for (length = strlen(arg), cmd = 0;
         *wld_cmd_info[cmd].command != '\n'; cmd++)
        if (!strncmp(wld_cmd_info[cmd].command, arg, length))
            break;

    if (*wld_cmd_info[cmd].command == '\n')
        wld_log(room, "Unknown world cmd: '%s'", argument);
    else
      ((*wld_cmd_info[cmd].command_pointer)
       (room, line, cmd, wld_cmd_info[cmd].subcmd));
}
