/**************************************************************************
*  File: dg_objcmd.c                                       Part of tbaMUD *
*  Usage: Contains the command_interpreter for objects, object commands.  *
*                                                                         *
*  $Author: galion/Mark A. Heilpern/egreen/Welcor $                       *
*  $Date: 2004/10/11 12:07:00$                                            *
*  $Revision: 1.0.14 $                                                    *
**************************************************************************/

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
#include "genzon.h" /* for access to real_zone_by_thing */
#include "fight.h" /* for die() */
#include "spells.h" /* for NUM_ATTACK_TYPES */
#include "genobj.h" /* for free_object_strings[_proto]() */



/* Local functions */
#define OCMD(name)  \
   void (name)(obj_data *obj, char *argument, int cmd, int subcmd)

static void obj_log(obj_data *obj, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));
static room_rnum find_obj_target_room(obj_data *obj, char *rawroomstr);
static OCMD(do_oecho);
static OCMD(do_oforce);
static OCMD(do_ozoneecho);
static OCMD(do_osend);
static OCMD(do_orecho);
static OCMD(do_otimer);
static OCMD(do_otransform);
static OCMD(do_opurge);
static OCMD(do_oteleport);
static OCMD(do_dgoload);
static OCMD(do_odamage);
static OCMD(do_oasound);
static OCMD(do_odoor);
static OCMD(do_osetval);
static OCMD(do_oat);
static OCMD(do_omove);
static OCMD(do_olog);

struct obj_command_info {
   char *command;
   void        (*command_pointer)(obj_data *obj, char *argument, int cmd, int subcmd);
   int        subcmd;
};

/* do_osend */
#define SCMD_OSEND         0
#define SCMD_OECHOAROUND   1

/* attaches object name and vnum to msg and sends it to script_log */
static void obj_log(obj_data *obj, const char *format, ...)
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

  script_log("Obj (%s, VNum %d):: %s", obj->short_description, GET_OBJ_VNUM(obj), message);
}

/* returns the real room number that the object or object's carrier is in */
room_rnum obj_room(obj_data *obj)
{
    if (IN_ROOM(obj) != NOWHERE)
        return IN_ROOM(obj);
    else if (obj->carried_by)
        return IN_ROOM(obj->carried_by);
    else if (obj->worn_by)
        return IN_ROOM(obj->worn_by);
    else if (obj->in_obj)
        return obj_room(obj->in_obj);
    else
        return NOWHERE;
}

/* returns the real room number, or NOWHERE if not found or invalid */
static room_rnum find_obj_target_room(obj_data *obj, char *rawroomstr)
{
    int tmp;
    room_rnum location;
    char_data *target_mob;
    obj_data *target_obj;
    char roomstr[MAX_INPUT_LENGTH];

    one_argument(rawroomstr, roomstr);

    if (!*roomstr)
        return NOWHERE;

    if (isdigit(*roomstr) && !strchr(roomstr, '.'))
    {
        tmp = tb_parse_int_or_zero(roomstr);
        if ((location = real_room(tmp)) == NOWHERE)
            return NOWHERE;
    }

    else if ((target_mob = get_char_by_obj(obj, roomstr)))
        location = IN_ROOM(target_mob);
    else if ((target_obj = get_obj_by_obj(obj, roomstr)))
    {
        if (IN_ROOM(target_obj) != NOWHERE)
            location = IN_ROOM(target_obj);
        else
            return NOWHERE;
    }
    else
        return NOWHERE;

    /* a room has been found.  Check for permission */
    if (ROOM_FLAGGED(location, ROOM_GODROOM) ||
#ifdef ROOM_IMPROOM
        ROOM_FLAGGED(location, ROOM_IMPROOM) ||
#endif
        ROOM_FLAGGED(location, ROOM_HOUSE))
        return NOWHERE;

    if (ROOM_FLAGGED(location, ROOM_PRIVATE) &&
        world[location].people && world[location].people->next_in_room)
        return NOWHERE;

    return location;
}

/* Object commands */
static OCMD(do_oecho)
{
    int room;

    skip_spaces(&argument);

    if (!*argument)
        obj_log(obj, "oecho called with no args");

    else if ((room = obj_room(obj)) != NOWHERE)
    {
	  if (world[room].people) {
		sub_write(argument, world[room].people, TRUE, TO_ROOM);
		sub_write(argument, world[room].people, TRUE, TO_CHAR);
	  }
    }

    else
        obj_log(obj, "oecho called by object in NOWHERE");
}

static OCMD(do_olog)
{
  skip_spaces(&argument);

  if (*argument)
    obj_log(obj, "%s", argument); /* script text, may hold player input */
}

static OCMD(do_oforce)
{
    char_data *ch, *next_ch;
    int room;
    char arg1[MAX_INPUT_LENGTH], *line;

    line = one_argument(argument, arg1);

    if (!*arg1 || !*line)
    {
        obj_log(obj, "oforce called with too few args");
        return;
    }

    if (!str_cmp(arg1, "all"))
    {
        if ((room = obj_room(obj)) == NOWHERE)
            obj_log(obj, "oforce called by object in NOWHERE");
        else
        {
            for (ch = world[room].people; ch; ch = next_ch)
            {
                next_ch = ch->next_in_room;
                if (valid_dg_target(ch, 0))
                {
                    command_interpreter(ch, line);
                }
            }
        }
    }

    else
    {
        if ((ch = get_char_by_obj(obj, arg1)))
        {
            if (valid_dg_target(ch, 0))
            {
                command_interpreter(ch, line);
            }
        }

        else
            obj_log(obj, "oforce: no target found");
    }
}

static OCMD(do_ozoneecho)
{
    int zone;
    char room_number[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH], *msg;

    msg = any_one_arg(argument, room_number);
    skip_spaces(&msg);

    if (!*room_number || !*msg)
	obj_log(obj, "ozoneecho called with too few args");

    else if ((zone = real_zone_by_thing(tb_parse_int_or_zero(room_number))) == NOWHERE)
	obj_log(obj, "ozoneecho called for nonexistant zone");

    else {
	tb_snprintf(buf, sizeof(buf), "%s\r\n", msg);
	send_to_zone(buf, zone);
    }
}

static OCMD(do_osend)
{
    char buf[MAX_INPUT_LENGTH], *msg;
    char_data *ch;

    msg = any_one_arg(argument, buf);

    if (!*buf)
    {
        obj_log(obj, "osend called with no args");
        return;
    }

    skip_spaces(&msg);

    if (!*msg)
    {
        obj_log(obj, "osend called without a message");
        return;
    }

    if ((ch = get_char_by_obj(obj, buf)))
    {
        if (subcmd == SCMD_OSEND)
            sub_write(msg, ch, TRUE, TO_CHAR);
        else if (subcmd == SCMD_OECHOAROUND)
            sub_write(msg, ch, TRUE, TO_ROOM);
    }

    else
        obj_log(obj, "no target found for osend");
}

/* Prints the message to everyone in the range of numbers. Thanks to Jamie 
 * Nelson of 4D for this contribution. */
static OCMD(do_orecho)
{
    char start[MAX_INPUT_LENGTH], finish[MAX_INPUT_LENGTH], *msg;

    msg = two_arguments(argument, start, finish);

    skip_spaces(&msg);

    if (!*msg || !*start || !*finish || !is_number(start) || !is_number(finish))
      obj_log(obj, "orecho: too few args");
    else
      send_to_range(tb_parse_int_or_zero(start), tb_parse_int_or_zero(finish), "%s\r\n", msg);

}

/* set the object's timer value */
static OCMD(do_otimer)
{
  char arg[MAX_INPUT_LENGTH];

  one_argument(argument, arg);

  if (!*arg)
    obj_log(obj, "otimer: missing argument");
  else if (!isdigit(*arg))
    obj_log(obj, "otimer: bad argument");
  else
    GET_OBJ_TIMER(obj) = tb_parse_int_or_zero(arg);
}

/* Transform into a different object. Note: this shouldn't be used with
 * containers unless both objects are containers!
 *
 * A fresh instance o of the new prototype is copied over obj, so obj keeps
 * its address and every pointer other code holds to it stays valid. obj
 * keeps: its place in the world (room, carrier, container, wearer) and in
 * object_list, its contents, script_id, script and proto_script, pending
 * events (extract_obj() cancels them; losing them left them running), and
 * anyone sitting on it, provided it is still furniture (the sitters'
 * SITTING() points at obj; losing sitting_here left them pointing at obj
 * after it was freed). Everything else comes from the new prototype.
 *
 * Bookkeeping the copy would otherwise corrupt:
 *  - weights: obj is taken out of its carrier or container and its contents
 *    out of it, then all are put back, so obj_to_char()/obj_to_obj()
 *    recompute IS_CARRYING_W and container weights for the new weight;
 *  - obj_index counts: obj stops being an instance of the old prototype
 *    and becomes one of the new (o's own count is dropped by extract_obj);
 *  - strings obj owned (not the old prototype's) are freed before o's
 *    prototype strings replace them. */
static OCMD(do_otransform)
{
  char arg[MAX_INPUT_LENGTH];
  obj_data *o, tmpobj, *carried_contents = NULL, *c, *container = NULL;
  struct char_data *wearer = NULL, *carrier = NULL, *sitter, *next_sitter;
  obj_rnum old_rnum;
  obj_vnum old_vnum;
  int pos = 0, unseated = 0;

  one_argument(argument, arg);

  if (!*arg)
    obj_log(obj, "otransform: missing argument");
  else if (!isdigit(*arg))
    obj_log(obj, "otransform: bad argument");
  else {
    o = read_object(dg_parse_vnum(arg), VIRTUAL);
    if (o==NULL) {
      obj_log(obj, "otransform: bad object vnum");
      return;
    }

    old_rnum = GET_OBJ_RNUM(obj);
    old_vnum = GET_OBJ_VNUM(obj);
    tb_breadcrumbf("dg", "otransform obj uid=%ld vnum %d -> %d",
                   obj_script_id(obj), old_vnum, GET_OBJ_VNUM(o));

    if (obj->worn_by) {
      pos = obj->worn_on;
      wearer = obj->worn_by;
      unequip_char(obj->worn_by, pos);
    }
    if (obj->carried_by) {
      carrier = obj->carried_by;
      obj_from_char(obj);
    } else if (obj->in_obj) {
      container = obj->in_obj;
      obj_from_obj(obj);
    }
    /* Reversed here, and reversed again by obj_to_obj() below. */
    while ((c = obj->contains) != NULL) {
      obj_from_obj(c);
      c->next_content = carried_contents;
      carried_contents = c;
    }

    if (old_rnum != NOTHING && old_rnum <= top_of_objt)
      free_object_strings_proto(obj);
    else
      free_object_strings(obj);

    /* move new obj info over to old object and delete new obj */
    memcpy(&tmpobj, o, sizeof(*o));
    tmpobj.in_room = IN_ROOM(obj);
    tmpobj.carried_by = NULL;
    tmpobj.worn_by = NULL;
    tmpobj.worn_on = obj->worn_on;
    tmpobj.in_obj = NULL;
    tmpobj.contains = NULL;
    tmpobj.script_id = obj->script_id;
    tmpobj.proto_script = obj->proto_script;
    tmpobj.script = obj->script;
    tmpobj.next_content = obj->next_content;
    tmpobj.next = obj->next;
    tmpobj.events = obj->events;
    tmpobj.sitting_here = obj->sitting_here;
    if (tmpobj.sitting_here) {
      if (GET_OBJ_TYPE(&tmpobj) == ITEM_FURNITURE)
        GET_OBJ_VAL(&tmpobj, 1) = GET_OBJ_VAL(obj, 1);   /* seats taken */
      else {
        /* Not furniture any more: unseat everyone, as extract_obj() does. */
        for (sitter = tmpobj.sitting_here; sitter; sitter = next_sitter) {
          next_sitter = NEXT_SITTING(sitter);
          SITTING(sitter) = NULL;
          NEXT_SITTING(sitter) = NULL;
          unseated++;
        }
        tmpobj.sitting_here = NULL;
      }
    }
    memcpy(obj, &tmpobj, sizeof(*obj));
    o->events = NULL;          /* read_object() gave o none; obj owns them */
    o->sitting_here = NULL;

    /* obj is now an instance of the new prototype. */
    if (old_rnum != NOTHING && old_rnum <= top_of_objt)
      obj_index[old_rnum].number--;
    if (GET_OBJ_RNUM(obj) != NOTHING)
      obj_index[GET_OBJ_RNUM(obj)].number++;

    if (unseated)
      TB_WARN_MSG("dg", "otransform", "do_otransform",
                  "obj uid %ld vnum %d -> %d is no longer furniture; %d "
                  "sitter(s) unseated", obj->script_id, old_vnum,
                  GET_OBJ_VNUM(obj), unseated);

    if (carrier)
      obj_to_char(obj, carrier);
    else if (container)
      obj_to_obj(obj, container);
    while ((c = carried_contents) != NULL) {
      carried_contents = c->next_content;
      c->next_content = NULL;
      obj_to_obj(c, obj);
    }
    if (wearer) {
      equip_char(wearer, obj, pos);
    }

    extract_obj(o);
  }
}

/* purge all objects an npcs in room, or specified object or mob */
static OCMD(do_opurge)
{
    char arg[MAX_INPUT_LENGTH];
    char_data *ch, *next_ch;
    obj_data *o, *next_obj;
    int rm;

    one_argument(argument, arg);

    if (!*arg) {
      /* purge all */
      if ((rm = obj_room(obj)) != NOWHERE) {
        for (ch = world[rm].people; ch; ch = next_ch ) {
           next_ch = ch->next_in_room;
           if (IS_NPC(ch))
             extract_char(ch);
        }

        for (o = world[rm].contents; o; o = next_obj ) {
           next_obj = o->next_content;
           if (o != obj)
             extract_obj(o);
        }
      }

      return;
    } /* no arg */

    ch = get_char_by_obj(obj, arg);
    if (!ch) {
      o = get_obj_by_obj(obj, arg);
      if (o) {
        extract_obj(o);
      } else
        obj_log(obj, "opurge: bad argument");

      return;
    }

    if (!IS_NPC(ch)) {
      obj_log(obj, "opurge: purging a PC");
      return;
    }

    extract_char(ch);
}

static OCMD(do_oteleport)
{
    char_data *ch;
    room_rnum target, rm;
    char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];

    two_arguments(argument, arg1, arg2);

    if (!*arg1 || !*arg2)
    {
        obj_log(obj, "oteleport called with too few args");
        return;
    }

    target = find_obj_target_room(obj, arg2);

    if (target == NOWHERE)
        obj_log(obj, "oteleport target is an invalid room");

    else if (!str_cmp(arg1, "all"))
    {
        rm = obj_room(obj);
        if (rm == NOWHERE) {
            obj_log(obj, "oteleport all: the object is in no room");
            return;
        }
        if (target == rm) {
            obj_log(obj, "oteleport target is itself");
            return;
        }

        dg_teleport_all(rm, target);
    }

    else
    {
        if ((ch = get_char_by_obj(obj, arg1))) {
          if (valid_dg_target(ch, DG_ALLOW_GODS)) {
            char_from_room(ch);
            char_to_room(ch, target);
            enter_wtrigger(&world[IN_ROOM(ch)], ch, -1);
          }
        }

        else
            obj_log(obj, "oteleport: no target found");
    }
}

static OCMD(do_dgoload)
{
    char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
    int number = 0, room;
    char_data *mob;
    obj_data *object;
    char *target;
    char_data *tch;
    obj_data *cnt;
    int pos;

    target = two_arguments(argument, arg1, arg2);

    if (!*arg1 || !*arg2 || !is_number(arg2) || ((number = dg_parse_vnum(arg2)) == NOTHING))
    {
        obj_log(obj, "oload: bad syntax");
        return;
    }

    if ((room = obj_room(obj)) == NOWHERE)
    {
        obj_log(obj, "oload: object in NOWHERE trying to load");
        return;
    }

    /* load mob to target room - Jamie Nelson, April 13 2004 */
    if (is_abbrev(arg1, "mob")) {
      room_rnum rnum;
      if (!target || !*target) {
        rnum = room;
      } else {
        if (!isdigit(*target) || (rnum = real_room(dg_parse_vnum(target))) == NOWHERE) {
          obj_log(obj, "oload: room target vnum doesn't exist "
                       "(loading mob vnum %d to room %s)", number, target);
          return;
        }
      }
      if ((mob = read_mobile(number, VIRTUAL)) == NULL) {
        obj_log(obj, "oload: bad mob vnum");
        return;
      }
      char_to_room(mob, rnum);

      if (SCRIPT(obj)) { /* It _should_ have, but it might be detached. */
        char buf[MAX_INPUT_LENGTH];
        tb_snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, char_script_id(mob));
        add_var(&(SCRIPT(obj)->global_vars), "lastloaded", buf, 0);
      }

      load_mtrigger(mob);
    }

    else if (is_abbrev(arg1, "obj")) {
      if ((object = read_object(number, VIRTUAL)) == NULL) {
        obj_log(obj, "oload: bad object vnum");
        return;
      }

      if (SCRIPT(obj)) { /* It _should_ have, but it might be detached. */
        char buf[MAX_INPUT_LENGTH];
        tb_snprintf(buf, sizeof(buf), "%c%ld", UID_CHAR, obj_script_id(object));
        add_var(&(SCRIPT(obj)->global_vars), "lastloaded", buf, 0);
      }

      /* special handling to make objects able to load on a person/in a container/worn etc. */
      if (!target || !*target) {
        obj_to_room(object, room);
        load_otrigger(object);
        return;
      }
      two_arguments(target, arg1, arg2); /* recycling ... */
      tch = get_char_near_obj(obj, arg1);
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
      cnt = get_obj_near_obj(obj, arg1);
      if (cnt && GET_OBJ_TYPE(cnt) == ITEM_CONTAINER) {
      	obj_to_obj(object, cnt);
        load_otrigger(object);
      	return;
      }
      /* neither char nor container found - just dump it in room */
      obj_to_room(object, room);
      load_otrigger(object);
      return;
    }

    else
        obj_log(obj, "oload: bad type");

}

static OCMD(do_odamage) {
  char name[MAX_INPUT_LENGTH], amount[MAX_INPUT_LENGTH];
  int dam = 0;
  char_data *ch;

  two_arguments(argument, name, amount);

  /* who cares if it's a number ? if not it'll just be 0 */
  if (!*name || !*amount) {
      obj_log(obj, "odamage: bad syntax");
      return;
  }

  dam = tb_parse_int_or_zero(amount);
  ch = get_char_by_obj(obj, name);

  if (!ch) {
    obj_log(obj, "odamage: target not found");
    return;
  }
  script_damage(ch, dam);
}

static OCMD(do_oasound)
{
  room_rnum room;
  int door;

  skip_spaces(&argument);

  if (!*argument) {
    obj_log(obj, "oasound called with no args");
    return;
  }

  if ((room = obj_room(obj)) == NOWHERE) {
    obj_log(obj, "oasound called by object in NOWHERE");
    return;
  }

  for (door = 0; door < DIR_COUNT; door++) {
    if (world[room].dir_option[door] != NULL &&
       (world[room].dir_option[door])->to_room != NOWHERE &&
       (world[room].dir_option[door])->to_room != room &&
        world[(world[room].dir_option[door])->to_room].people) {
      sub_write(argument, world[(world[room].dir_option[door])->to_room].people, TRUE, TO_ROOM);
      sub_write(argument, world[(world[room].dir_option[door])->to_room].people, TRUE, TO_CHAR); 
    }
  }
}

static OCMD(do_odoor)
{
    char target[MAX_INPUT_LENGTH], direction[MAX_INPUT_LENGTH];
    char field[MAX_INPUT_LENGTH], *value;
    room_data *rm;
    struct room_direction_data *newexit;
    int dir, fd, to_room;

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
        obj_log(obj, "odoor called with too few args");
        return;
    }

    if ((rm = get_room(target)) == NULL) {
        obj_log(obj, "odoor: invalid target (arg == %s)", target);
        return;
    }

    if ((dir = search_block(direction, dirs, FALSE)) == -1) {
        char dirs_str[256];
        int di, doff = 0;
        dirs_str[0] = '\0';
        for (di = 0; *dirs[di] != '\n'; di++)
            doff += snprintf(dirs_str + doff, sizeof(dirs_str) - doff, "%s%s", doff ? " " : "", dirs[di]);
        obj_log(obj, "odoor: invalid direction (arg == %s) not found in: [ %s ]", direction, dirs_str);
        return;
    }

    if ((fd = search_block(field, door_field, FALSE)) == -1) {
        char fields_str[256];
        int fi, foff = 0;
        fields_str[0] = '\0';
        for (fi = 0; *door_field[fi] != '\n'; fi++)
            foff += snprintf(fields_str + foff, sizeof(fields_str) - foff, "%s%s", foff ? " " : "", door_field[fi]);
        obj_log(obj, "odoor: invalid field (arg == %s) not found in: [ %s ]", field, fields_str);
        return;
    }

    newexit = rm->dir_option[dir];

    /* purge exit */
    if (fd == 0) {
        if (newexit) {
            if (newexit->general_description)
                free(newexit->general_description);
            if (newexit->keyword)
                free(newexit->keyword);
            free(newexit);
            rm->dir_option[dir] = NULL;
        }
    }

    else {
        if (!newexit) {
            CREATE(newexit, struct room_direction_data, 1);
            rm->dir_option[dir] = newexit;
        }

        switch (fd) {
        case 1:  /* description */
            if (newexit->general_description)
                free(newexit->general_description);
            {
                size_t desc_size = strlen(value) + 3;
                CREATE(newexit->general_description, char, desc_size);
                strlcpy(newexit->general_description, value, desc_size);
                strlcat(newexit->general_description, "\r\n", desc_size);
            } /* strcat : OK */
            break;
        case 2:  /* flags       */
            newexit->exit_info = (sh_int)asciiflag_conv(value);
            break;
        case 3:  /* key         */
            newexit->key = tb_parse_int_or_zero(value);
            break;
        case 4:  /* name        */
            if (newexit->keyword)
                free(newexit->keyword);
            {
                size_t keyword_size = strlen(value) + 1;
                CREATE(newexit->keyword, char, keyword_size);
                strlcpy(newexit->keyword, value, keyword_size);
            }
            break;
        case 5:  /* room        */
            if ((to_room = real_room(dg_parse_vnum(value))) != NOWHERE)
                newexit->to_room = to_room;
            else {
                newexit->to_room = NOWHERE;
                obj_log(obj, "odoor: invalid door target (arg == %s)", value);
            }
            break;
        }
    }
}

/* Some object values are used, unchecked, as array indexes by the game:
 * a drink container's or fountain's liquid (value 2) indexes drinks[],
 * drinknames[] and drink_aff[] (act.item.c, shop.c), and a weapon's attack
 * type (value 3) indexes attack_hit_text[] (fight.c). World files are
 * checked when they are loaded; osetval takes the value from script text,
 * which builders and the players it quotes control, so it must check too.
 * Returns TRUE if value may be stored in position pos of obj; otherwise
 * stores the permitted range in *lo..*hi. */
static int obj_value_in_range(const obj_data *obj, int pos, int value,
                              int *lo, int *hi)
{
  *lo = INT_MIN;
  *hi = INT_MAX;

  switch (GET_OBJ_TYPE(obj)) {
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
    if (pos == 2) {
      *lo = 0;
      *hi = NUM_LIQ_TYPES - 1;
    }
    break;
  case ITEM_WEAPON:
    if (pos == 3) {
      *lo = 0;
      *hi = NUM_ATTACK_TYPES - 1;
    }
    break;
  default:
    break;
  }
  return value >= *lo && value <= *hi;
}

static OCMD(do_osetval)
{
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
  int position, new_value, worn_on;
  struct char_data *worn_by = NULL;

  two_arguments(argument, arg1, arg2);
  if (!*arg1 || !*arg2 || !is_number(arg1) || !is_number(arg2)) {
    obj_log(obj, "osetval: bad syntax");
    return;
  }

  position = tb_parse_int_or_zero(arg1);
  new_value = tb_parse_int_or_zero(arg2);

  if (position>=0 && position<NUM_OBJ_VAL_POSITIONS) {
    int lo, hi;

    if (!obj_value_in_range(obj, position, new_value, &lo, &hi)) {
      TB_ERROR_MSG("dg", "osetval-range", "do_osetval",
                   "obj vnum %d uid %ld type %d: value[%d]=%d outside [%d, %d]; "
                   "not stored", GET_OBJ_VNUM(obj), obj_script_id(obj),
                   GET_OBJ_TYPE(obj), position, new_value, lo, hi);
      obj_log(obj, "osetval: value %d for position %d out of range [%d, %d]",
              new_value, position, lo, hi);
      return;
    }

    worn_by = obj->worn_by;
    worn_on = obj->worn_on;

    if (worn_by != NULL) {
      unequip_char(worn_by, worn_on);
    }

    GET_OBJ_VAL(obj, position) = new_value;

    if (worn_by != NULL) {
      equip_char(worn_by, obj, worn_on);
    }
  } else
    obj_log(obj, "osetval: position out of bounds!");
}

/* Submitted by PurpleOnyx */
static OCMD(do_oat)
{
  room_rnum loc = NOWHERE;
  struct char_data *ch;
  struct obj_data *object;
  char arg[MAX_INPUT_LENGTH], *command;
  long copy_uid;

  command = any_one_arg(argument, arg);

  if (!*arg) {
    obj_log(obj, "oat called with no args");
    return;
  }

  skip_spaces(&command);

  if (!*command) {
    obj_log(obj, "oat called without a command");
    return;
  }

  if (isdigit(*arg)) loc = real_room(dg_parse_vnum(arg));
  else if ((ch = get_char_by_obj(obj, arg))) loc = IN_ROOM(ch); 

  if (loc == NOWHERE) {
    obj_log(obj, "oat: location not found (%s)", arg);
    return;
  }

  if (!(object = read_object(GET_OBJ_VNUM(obj), VIRTUAL))) {
    TB_ERROR_MSG("dg", "oat-clone", "do_oat",
                 "obj vnum %d uid %ld: read_object could not clone the owner; "
                 "command (%zu bytes) not run", GET_OBJ_VNUM(obj),
                 obj_script_id(obj), strlen(command));
    obj_log(obj, "oat: cannot create a copy of object %d", GET_OBJ_VNUM(obj));
    return;
  }

  /* The command runs through a temporary copy.  It may purge the copy
   * (reading object afterwards was a use-after-free) or move it (the copy
   * was then never extracted), so check it by UID and always remove it. */
  copy_uid = obj_script_id(object);
  obj_to_room(object, loc);
  obj_command_interpreter(object, command);

  if (obj_uid_matches(copy_uid, object))
    extract_obj(object);
}

static OCMD(do_omove)
{
    room_rnum target;
    char arg1[MAX_INPUT_LENGTH];

    one_argument(argument, arg1);

    if (!*arg1)
    {
        obj_log(obj, "omove called with too few args");
        return;
    }

    target = find_obj_target_room(obj, arg1);

    if (target == NOWHERE) {
        /* Carrying on moved the object into world[NOWHERE]. */
        obj_log(obj, "omove target is an invalid room");
        return;
    }

    // Remove the object from it's current location
    if (obj->carried_by != NULL) {
      obj_from_char(obj);
    } else if (IN_ROOM(obj) != NOWHERE) {
      obj_from_room(obj);
    } else if (obj->in_obj != NULL) {
      obj_from_obj(obj);
    } else {
      obj_log(obj, "omove: target object is not in a room, held or in a container!");
      return;
    }

    obj_to_room(obj, target);
}

static const struct obj_command_info obj_cmd_info[] = {
    { "RESERVED", 0, 0 },/* this must be first -- for specprocs */

    { "oasound "    , do_oasound  , 0 },
    { "oat "        , do_oat      , 0 },
    { "odoor "      , do_odoor    , 0 },
    { "odamage "    , do_odamage,   0 },
    { "oecho "      , do_oecho    , 0 },
    { "oechoaround ", do_osend    , SCMD_OECHOAROUND },
    { "oforce "     , do_oforce   , 0 },
    { "oload "      , do_dgoload  , 0 },
    { "opurge "     , do_opurge   , 0 },
    { "orecho "     , do_orecho   , 0 },
    { "osend "      , do_osend    , SCMD_OSEND },
    { "osetval "    , do_osetval  , 0 },
    { "oteleport "  , do_oteleport, 0 },
    { "otimer "     , do_otimer   , 0 },
    { "otransform " , do_otransform, 0 },
    { "ozoneecho "  , do_ozoneecho , 0 }, /* fix by Rumble */
    { "omove "      , do_omove     , 0 },
    { "olog "       , do_olog       , 0 },
    { "\n", 0, 0 }        /* this must be last */
};

/* This is the command interpreter used by objects, called by script_driver
 * (and by do_oat for its temporary clone).
 *
 * Owner lifetime contract: an object's own command can free the object in
 * more ways than 'opurge self': purging the container it is in
 * (extract_obj() frees contents recursively), purging the room it sits in
 * inside a container, or forcing a character to junk the container holding
 * it. script_driver() checks its own owner by UID after every command
 * (dg_frame_owner_gone()) and stops, so callers must not use obj after this
 * returns unless obj_uid_matches() still holds. Here the owner's UID is
 * captured before the command only to report an indirect self-destruction,
 * which is handled but usually a script bug. */
void obj_command_interpreter(obj_data *obj, char *argument)
{
    int cmd, length;
    char *line, arg[MAX_INPUT_LENGTH];
    long owner_uid;
    obj_vnum owner_vnum;
    int owner_gone;

    skip_spaces(&argument);

    /* just drop to next line for hitting CR */
    if (!*argument)
        return;

    line = any_one_arg(argument, arg);


    /* find the command */
    for (length = strlen(arg),cmd = 0;
         *obj_cmd_info[cmd].command != '\n'; cmd++)
        if (!strncmp(obj_cmd_info[cmd].command, arg, length))
            break;

    if (*obj_cmd_info[cmd].command == '\n') {
      obj_log(obj, "Unknown object cmd: '%s'", argument);
      return;
    }

    owner_uid = obj_script_id(obj);
    owner_vnum = GET_OBJ_VNUM(obj);

    ((*obj_cmd_info[cmd].command_pointer)
     (obj, line, cmd, obj_cmd_info[cmd].subcmd));

    /* 'opurge' is the one command meant to destroy its owner; any other
     * command that did so destroyed it indirectly. */
    owner_gone = !obj_uid_matches(owner_uid, obj);
    if (owner_gone && obj_cmd_info[cmd].command_pointer != do_opurge) {
      /* Handled (the script stops), but it means the script destroyed its
       * own owner indirectly; say which object and which command. */
      TB_WARN_MSG("dg", "owner-extracted", "obj_command_interpreter",
                  "obj vnum %d uid %ld was extracted by its own '%.*s' command "
                  "(not 'opurge self'); its script stops here", owner_vnum,
                  owner_uid, (int)strcspn(obj_cmd_info[cmd].command, " "),
                  obj_cmd_info[cmd].command);
    }
}
