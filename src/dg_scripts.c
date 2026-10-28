/**
* @file dg_scripts.c
* Contains the main script driver interface.
*
* Ownership and input rules (maintainer summary):
* - Trigger command lists belong to the prototype (trig_index[rn]->proto) and
*   are shared by every instance; script_driver() copies each line into a
*   MAX_INPUT_LENGTH buffer and runs var_subst() on the copy before dispatch.
* - Script text is builder input (trigedit, world files). Variable values can
*   be player text (%speech%, %arg%, %cmd%); dg_triggers.c stores them with
*   '%' made inert so they never become variable syntax again.
* - A trigger extracted while any script runs is retired, not freed, until
*   dg_script_execution_leave(); owners (mob, obj, room) can be extracted by
*   any command a script runs, so pointers to them are revalidated after
*   every callback (script_driver, random/time dispatch).
* - UIDs live in disjoint namespaces (see dg_scripts.h); each resolver accepts
*   only its own range.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
* 
* This source code, which was not part of the CircleMUD legacy code,
* was created by the following people:                                      
* $Author: Mark A. Heilpern/egreen/Welcor $                              
* $Date: 2004/10/11 12:07:00$                                            
* $Revision: 1.0.14 $                                                    
*/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "dg_scripts.h"
#include "dg_cmd.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "dg_event.h"
#include "db.h"
#include "screen.h"
#include "constants.h"
#include "spells.h"
#include "oasis.h"
#include "genzon.h" /* for real_zone_by_thing */
#include "act.h"
#include "modify.h"

#define PULSES_PER_MUD_HOUR     (SECS_PER_MUD_HOUR*PASSES_PER_SEC)

/* 'wait until' accepts 0:00 through 24:00 (stock scripts use "24:00"). */
#define DG_WAIT_MAX_MINUTES     (24 * 60)

/* Local functions not used elsewhere */
static obj_data *find_obj(long n);
static room_data *find_room(long n);
static void do_stat_trigger(struct char_data *ch, trig_data *trig);
static void script_stat(char_data *ch, struct script_data *sc);
static int remove_trigger(struct script_data *sc, char *name,
                          trig_vnum *vnum_out, char *name_out,
                          size_t name_size);
static int is_num(char *arg);
static void eval_op(char *op, char *lhs, char *rhs, char *result, void *go,
          struct script_data *sc, trig_data *trig);
static char *matching_paren(char *p);
static void eval_expr(char *line, char *result, void *go, struct script_data *sc,
          trig_data *trig, int type);
static int eval_lhs_op_rhs(char *expr, char *result, void *go, struct script_data *sc,
          trig_data *trig, int type);
static int process_if(char *cond, void *go, struct script_data *sc,
          trig_data *trig, int type);
static struct cmdlist_element *find_end(trig_data *trig, struct cmdlist_element *cl);
static struct cmdlist_element *find_else_end(trig_data *trig,
          struct cmdlist_element *cl, void *go, struct script_data *sc, int type);
static int process_wait(void *go, trig_data *trig, int type, char *cmd,
          struct cmdlist_element *cl);
static void process_set(struct script_data *sc, trig_data *trig, char *cmd);
static void process_attach(void *go, struct script_data *sc, trig_data *trig,
          int type, char *cmd);
static void process_detach(void *go, struct script_data *sc, trig_data *trig,
          int type, char *cmd);
static void makeuid_var(void *go, struct script_data *sc, trig_data *trig,
          int type, char *cmd);
static int process_return(trig_data *trig, char *cmd);
static void process_unset(struct script_data *sc, trig_data *trig, char *cmd);
static void process_remote(struct script_data *sc, trig_data *trig, char *cmd);
static void process_rdelete(struct script_data *sc, trig_data *trig, char *cmd);
static void process_global(struct script_data *sc, trig_data *trig, char *cmd, long id);
static void process_context(struct script_data *sc, trig_data *trig, char *cmd);
static void extract_value(struct script_data *sc, trig_data *trig, char *cmd);
static void dg_letter_value(struct script_data *sc, trig_data *trig, char *cmd);
static struct cmdlist_element * find_case(struct trig_data *trig, struct cmdlist_element *cl,
          void *go, struct script_data *sc, int type, char *cond);
static struct cmdlist_element *find_done(struct cmdlist_element *cl);
static void report_unterminated_block(trig_data *trig, const char *keyword);
static int dg_line_number(const trig_data *trig, const struct cmdlist_element *cl);
static struct char_data *find_char_by_uid_in_lookup_table(long uid);
static struct obj_data *find_obj_by_uid_in_lookup_table(long uid);
static EVENTFUNC(trig_wait_event);


/* Return pointer to first occurrence of string ct in cs, or NULL if not 
 * present.  Case insensitive. All of ct must be found in cs for it to be 
 * a match.
 * @todo Move this function to string util library.
 * @param cs The string to search.
 * @param ct What to search for in cs.
 * @retval char * NULL if ct is not a substring of cs, or pointer to the
 * location in cs where substring ct begins. */
char *str_str(char *cs, char *ct)
{
  char *s, *t;

  if (!cs || !ct || !*ct)
    return NULL;

  while (*cs) {
    t = ct;

    while (*cs && (LOWER(*cs) != LOWER(*t)))
      cs++;

    s = cs;

    while (*t && *cs && (LOWER(*cs) == LOWER(*t))) {
      t++;
      cs++;
    }

    /* If there we haven reached the end of ct via t,
     * then the whole string was found. */
    if (!*t)
      return s;
  }

  return NULL;
}

/** Returns the number of people in a room.
 * @param vnum The virtual number of a room.
 * @retval int Returns -1 if the room does not exist, or the total number of
 * PCs and NPCs in the room. */
int trgvar_in_room(room_vnum vnum) 
{
    room_rnum rnum = real_room(vnum);
    int i = 0;
    char_data *ch;

    if (rnum == NOWHERE) {
        script_log("people.vnum: world[rnum] does not exist");
        return (-1);
    }

    for (ch = world[rnum].people; ch !=NULL; ch = ch->next_in_room)
        i++;

    return i;
}


/** Find out if an object is within a list of objects.
 * @param name Either the unique id of an object or a string identifying the
 * object. Note the unique id must be prefixed with UID_CHAR.
 * @param list The list of objects to look through.
 * @retval obj_data * Pointer to the object if it is found in the list of
 * objects, NULL if the object is not found in the list.
 */
obj_data *get_obj_in_list(char *name, obj_data *list)
{
    obj_data *i;
    long id;

    if (*name == UID_CHAR){
      id = PARSE_UID(name + 1);

      for (i = list; i; i = i->next_content)
        if (id == i->script_id)
          return i;
      
    } else {
      for (i = list; i; i = i->next_content)
        if (isname(name, i->name))
          return i;
    }

    return NULL;
}

/** Find out if an NPC or PC is carrying an object.
 * @param ch Pointer to the NPC/PC to search through.
 * @param name String describing either the name of the object or the unique
 * id of the object. Note the unique id must be prefixed with UID_CHAR.
 * @retval obj_data * Either a pointer to the first object found that matches
 * the name argument, or the NULL if the object isn't found.
 */
obj_data *get_object_in_equip(char_data * ch, char *name)
{
  int j, n = 0, number;
  obj_data *obj;
  char tmpname[MAX_INPUT_LENGTH];
  char *tmp = tmpname;
  long id;

  if (*name == UID_CHAR) {
    id = PARSE_UID(name + 1);

    for (j = 0; j < NUM_WEARS; j++)
      if ((obj = GET_EQ(ch, j)))
        if (id == obj->script_id)
          return (obj);
  } else if (is_number(name)) {
    obj_vnum ovnum = tb_parse_int_or_zero(name);
    for (j = 0; j < NUM_WEARS; j++)
      if ((obj = GET_EQ(ch, j)))
        if (GET_OBJ_VNUM(obj) == ovnum)
          return (obj);
  } else {
    tb_snprintf(tmpname, sizeof(tmpname), "%s", name);
    if (!(number = get_number(&tmp)))
      return NULL;

    for (j = 0; (j < NUM_WEARS) && (n <= number); j++)
      if ((obj = GET_EQ(ch, j)))
        if (isname(tmp, obj->name))
          if (++n == number)
            return (obj);
  }

  return NULL;
}

/** Figures out if the argument is a valid location to 'wear' equipment.
 * Handles 'held', 'light' and 'wield' positions - Welcor. After idea from 
 * Byron Ellacott. 
 * @param arg Either the name of the position, or the number of a wear
 * location definition to check for.
 * @retval int If arg is not a valid wear location name or number, return
 * -1, else return the defined number of the wear location.
 */
int find_eq_pos_script(char *arg)
{
  int i;
  struct eq_pos_list {
    const char *pos;
    int where;
  } eq_pos[] = {
    {"hold",     WEAR_HOLD},
    {"held",     WEAR_HOLD},
    {"light",    WEAR_LIGHT},
    {"wield",    WEAR_WIELD},
    {"rfinger",  WEAR_FINGER_R},
    {"lfinger",  WEAR_FINGER_L},
    {"neck1",    WEAR_NECK_1},
    {"neck2",    WEAR_NECK_2},
    {"body",     WEAR_BODY},
    {"head",     WEAR_HEAD},
    {"legs",     WEAR_LEGS},
    {"feet",     WEAR_FEET},
    {"hands",    WEAR_HANDS},
    {"arms",     WEAR_ARMS},
    {"shield",   WEAR_SHIELD},
    {"about",    WEAR_ABOUT},
    {"waist",    WEAR_WAIST},
    {"rwrist",   WEAR_WRIST_R},
    {"lwrist",   WEAR_WRIST_L},
    {"none", -1}
  };

  if (is_number(arg) && (i = tb_parse_int_or_zero(arg)) >= 0 && i < NUM_WEARS)
    return i;

  for (i = 0;eq_pos[i].where != -1;i++) {
    if (!str_cmp(eq_pos[i].pos, arg))
      return eq_pos[i].where;
  }
  return (-1);
}

/** Figures out if an object can be worn on a defined wear location.
 * @param obj The object to check.
 * @param pos The defined wear location to check.
 * @retval int TRUE if obj can be worn on pos, FALSE if not.
 */
int can_wear_on_pos(struct obj_data *obj, int pos)
{
  switch (pos) {
    case WEAR_HOLD:
    case WEAR_LIGHT:    return CAN_WEAR(obj, ITEM_WEAR_HOLD);
    case WEAR_WIELD:    return CAN_WEAR(obj, ITEM_WEAR_WIELD);
    case WEAR_FINGER_R:
    case WEAR_FINGER_L: return CAN_WEAR(obj, ITEM_WEAR_FINGER);
    case WEAR_NECK_1:
    case WEAR_NECK_2:   return CAN_WEAR(obj, ITEM_WEAR_NECK);
    case WEAR_BODY:     return CAN_WEAR(obj, ITEM_WEAR_BODY);
    case WEAR_HEAD:     return CAN_WEAR(obj, ITEM_WEAR_HEAD);
    case WEAR_LEGS:     return CAN_WEAR(obj, ITEM_WEAR_LEGS);
    case WEAR_FEET:     return CAN_WEAR(obj, ITEM_WEAR_FEET);
    case WEAR_HANDS:    return CAN_WEAR(obj, ITEM_WEAR_HANDS);
    case WEAR_ARMS:     return CAN_WEAR(obj, ITEM_WEAR_ARMS);
    case WEAR_SHIELD:   return CAN_WEAR(obj, ITEM_WEAR_SHIELD);
    case WEAR_ABOUT:    return CAN_WEAR(obj, ITEM_WEAR_ABOUT);
    case WEAR_WAIST:    return CAN_WEAR(obj, ITEM_WEAR_WAIST);
    case WEAR_WRIST_R:
    case WEAR_WRIST_L:  return CAN_WEAR(obj, ITEM_WEAR_WRIST);
    default: return FALSE;
  }
}

/** Search for an NPC or PC by number routines.
 * @param n The unique ID (PC or NPC) to look for.
 * @retval char_data * Pointer to the character structure if it exists, or NULL
 * if it cannot be found.
 */
struct char_data *find_char(long n)
{
  if (!UID_IS_CHAR(n)) /* See note in dg_scripts.h */
    return NULL;

  return find_char_by_uid_in_lookup_table(n);
}

/** Search for an object by number routines.
 * @param n The unique ID to look for.
 * @retval obj_data * Pointer to the object if it exists, or NULL if it cannot
 * be found.
 */
static obj_data *find_obj(long n)
{
  if (!UID_IS_OBJ(n)) /* see note in dg_scripts.h */
    return NULL;

  return find_obj_by_uid_in_lookup_table(n);
}

/* Teleports every valid target in room from to room to, firing their enter
 * triggers.  Those triggers can move or extract anyone, so the occupants are
 * snapshotted by UID and each is re-found, still in from, before it moves;
 * the old loops followed a saved next_in_room into whatever room that
 * character had been sent to. */
void dg_teleport_all(room_rnum from, room_rnum to)
{
  long *uids;
  size_t count = 0, i;
  char_data *ch;

  for (ch = world[from].people; ch; ch = ch->next_in_room)
    count++;
  if (!count)
    return;

  CREATE(uids, long, count);
  for (i = 0, ch = world[from].people; ch && i < count; ch = ch->next_in_room)
    uids[i++] = char_script_id(ch);

  for (i = 0; i < count; i++) {
    ch = find_char(uids[i]);
    if (!ch || IN_ROOM(ch) != from || DEAD(ch)) {
      /* An earlier enter trigger moved or extracted this occupant. */
      dg_report_occupant_skipped(uids[i], from, "teleport all");
      continue;
    }
    if (!valid_dg_target(ch, DG_ALLOW_GODS))
      continue;
    char_from_room(ch);
    char_to_room(ch, to);
    enter_wtrigger(&world[IN_ROOM(ch)], ch, -1);
  }
  free(uids);
}

IDXTYPE dg_parse_vnum(const char *text)
{
  long vnum = tb_parse_long_or_zero(text);

  if (vnum < 0 || vnum >= IDXTYPE_MAX)
    return NOTHING;
  return (IDXTYPE)vnum;
}

/* Search for a room with UID n.
 * @param n the Unique ID to look for.
 * @retval room_data * Pointer to the room if it exists, or NULL if it cannot
 * be found.
 */
static room_data *find_room(long n)
{
  room_rnum rnum;

  /* Room UIDs are ROOM_ID_BASE + vnum, below OBJ_ID_BASE. An object UID such
   * as 30030009 used to truncate onto room_vnum 3001, so remote, rdelete,
   * attach and detach acted on that room. Callers try several namespaces in
   * turn, so a UID outside this one is not an error here. The vnum bound is
   * IDXTYPE_MAX, as in dg_parse_vnum(), so it also holds for signed builds
   * where NOWHERE is -1. */
  if (!UID_IS_ROOM(n))
    return NULL;
  n -= ROOM_ID_BASE;
  if (n >= IDXTYPE_MAX) {
    TB_WARN_MSG("dg", "uid-namespace", "find_room",
                "room uid %ld names vnum %ld, outside room_vnum [0,%d)",
                n + ROOM_ID_BASE, n, (int)IDXTYPE_MAX);
    return NULL;
  }
  rnum = real_room((room_vnum)n);

  if (rnum != NOWHERE)
    return &world[rnum];

  return NULL;
}

/* Generic searches based only on name. */
/** Search the entire world for an NPC or PC by name. 
 * @param name String describing the name or the unique id of the char. 
 * Note the unique id must be prefixed with UID_CHAR.
 * @retval char_data * Pointer to the char or NULL if char is not found. */
char_data *get_char(char *name)
{
  char_data *i;

  if (*name == UID_CHAR) {
    i = find_char(PARSE_UID(name + 1));

    if (i && valid_dg_target(i, DG_ALLOW_GODS))
      return i;
  } else {
    for (i = character_list; i; i = i->next)
      if (isname(name, i->player.name) &&
          valid_dg_target(i, DG_ALLOW_GODS))
        return i;
  }

  return NULL;
}

/** Find a character by name in the same room as a known object.
 * @todo Should this function not be constrained to the same room as an object
 * if 'name' is a unique id? 
 * @param obj An object that will constrain the search to the location that
 * the object is in *if* the name argument is not a unique id.
 * @param name Character name keyword to search for, or unique ID. Unique
 * id must be prefixed with UID_CHAR.
 * @retval char_data * Pointer to the the char if found, NULL if not. Will
 * only find god characters if DG_ALLOW_GODS is on. */
char_data *get_char_near_obj(obj_data *obj, char *name)
{
  char_data *ch;

  if (*name == UID_CHAR) {
    ch = find_char(PARSE_UID(name + 1));

    if (ch && valid_dg_target(ch, DG_ALLOW_GODS))
      return ch;
  } else {
    room_rnum num;
    if ((num = obj_room(obj)) != NOWHERE)
      for (ch = world[num].people; ch; ch = ch->next_in_room)
        if (isname(name, ch->player.name) &&
            valid_dg_target(ch, DG_ALLOW_GODS))
          return ch;
  }

  return NULL;
}

/** Find a character by name in a specific room.
 * @todo Should this function not be constrained to the room 
 * if 'name' is a unique id? 
 * @param room A room that will constrain the search to that location 
 * *if* the name argument is not a unique id.
 * @param name Character name keyword to search for, or unique ID. Unique
 * id must be prefixed with UID_CHAR.
 * @retval char_data * Pointer to the the char if found, NULL if not. Will
 * only find god characters if DG_ALLOW_GODS is on. */
char_data *get_char_in_room(room_data *room, char *name)
{
  char_data *ch;


  if (*name == UID_CHAR) {
    ch = find_char(PARSE_UID(name + 1));

    if (ch && valid_dg_target(ch, DG_ALLOW_GODS))
      return ch;
  } else {
    for (ch = room->people; ch; ch = ch->next_in_room)
      if (isname(name, ch->player.name) &&
          valid_dg_target(ch, DG_ALLOW_GODS))
        return ch;
    }

  return NULL;
}

/** Find a named object near another object (either in the same room, as
 * a container or contained by).
 * @param obj The obj with which to constrain the search.
 * @param name The keyword of the object to search for. If 'self' or 'me'
 * are passed in as arguments, obj is returned. Can also be a unique object
 * id, and if so it must be prefixed with UID_CHAR.
* @retval obj_data * Pointer to the object if found, NULL if not. */
obj_data *get_obj_near_obj(obj_data *obj, char *name)
{
  obj_data *i = NULL;
  char_data *ch;
  int rm;
  long id;

  if (!str_cmp(name, "self") || !str_cmp(name, "me"))
    return obj;

  /* is it inside ? */
  if (obj->contains && (i = get_obj_in_list(name, obj->contains)))
    return i;

  /* or outside ? */
  if (obj->in_obj) {
    if (*name == UID_CHAR) {
       id = PARSE_UID(name + 1);

      if (id == obj->in_obj->script_id)
        return obj->in_obj;
    } else if (isname(name, obj->in_obj->name))
      return obj->in_obj;
  }
  /* or worn ?*/
  else if (obj->worn_by && (i = get_object_in_equip(obj->worn_by, name)))
    return i;
  /* or carried ? */
  else if (obj->carried_by &&
          (i = get_obj_in_list(name, obj->carried_by->carrying)))
    return i;
  else if ((rm = obj_room(obj)) != NOWHERE) {
    /* check the floor */
    if ((i = get_obj_in_list(name, world[rm].contents)))
      return i;

    /* check peoples' inventory */
    for (ch = world[rm].people;ch ; ch = ch->next_in_room)
      if ((i = get_object_in_equip(ch, name)))
        return i;
  }
  return NULL;
}

/* returns the object in the world with name name, or NULL if not found */
obj_data *get_obj(char *name)
{
  obj_data *obj;

  if (*name == UID_CHAR)
    return find_obj(PARSE_UID(name + 1));
  else {
    for (obj = object_list; obj; obj = obj->next)
      if (isname(name, obj->name))
        return obj;
  }

  return NULL;
}

/* finds room by id or vnum.  returns NULL if not found */
room_data *get_room(char *name)
{
  room_rnum nr;
  long vnum;

  if (*name == UID_CHAR)
    return find_room(PARSE_UID(name + 1));

  /* A plain number is a room vnum. Script text controls it: reject values
   * that would wrap when narrowed to room_vnum (70537 used to be 5001),
   * with the same bound as dg_parse_vnum(). */
  vnum = tb_parse_long_or_zero(name);
  if (vnum < 0 || vnum >= IDXTYPE_MAX) {
    TB_WARN_MSG("dg", "uid-namespace", "get_room",
                "room vnum %ld outside room_vnum [0,%d)", vnum, (int)IDXTYPE_MAX);
    return NULL;
  }
  if ((nr = real_room((room_vnum)vnum)) == NOWHERE)
    return NULL;
  return &world[nr];
}

/* Returns a pointer to the first character in world by name name, or NULL if 
 * none found.  Starts searching with the person owing the object. */
char_data *get_char_by_obj(obj_data *obj, char *name)
{
  char_data *ch;

  if (*name == UID_CHAR) {
    ch = find_char(PARSE_UID(name + 1));

    if (ch && valid_dg_target(ch, DG_ALLOW_GODS))
      return ch;
  } else {
    if (obj->carried_by &&
        isname(name, obj->carried_by->player.name) &&
        valid_dg_target(obj->carried_by, DG_ALLOW_GODS))
      return obj->carried_by;

    if (obj->worn_by &&
        isname(name, obj->worn_by->player.name) &&
        valid_dg_target(obj->worn_by, DG_ALLOW_GODS))
      return obj->worn_by;

    for (ch = character_list; ch; ch = ch->next)
      if (isname(name, ch->player.name) &&
          valid_dg_target(ch, DG_ALLOW_GODS))
        return ch;
  }

  return NULL;
}

/* Returns a pointer to the first character in world by name name, or NULL if 
 * none found.  Starts searching in room room first. */
char_data *get_char_by_room(room_data *room, char *name)
{
  char_data *ch;

  if (*name == UID_CHAR) {
    ch = find_char(PARSE_UID(name + 1));

    if (ch && valid_dg_target(ch, DG_ALLOW_GODS))
      return ch;
  } else {
    for (ch = room->people; ch; ch = ch->next_in_room)
      if (isname(name, ch->player.name) &&
          valid_dg_target(ch, DG_ALLOW_GODS))
        return ch;

    for (ch = character_list; ch; ch = ch->next)
      if (isname(name, ch->player.name) &&
          valid_dg_target(ch, DG_ALLOW_GODS))
        return ch;
  }

  return NULL;
}

/* Returns the object in the world with name name, or NULL if not found search 
 * based on obj. */
obj_data *get_obj_by_obj(obj_data *obj, char *name)
{
  obj_data *i = NULL;
  int rm;

  if (*name == UID_CHAR)
    return find_obj(PARSE_UID(name + 1));

  if (!str_cmp(name, "self") || !str_cmp(name, "me"))
    return obj;

  if (obj->contains && (i = get_obj_in_list(name, obj->contains)))
    return i;

  if (obj->in_obj && isname(name, obj->in_obj->name))
      return obj->in_obj;

  if (obj->worn_by && (i = get_object_in_equip(obj->worn_by, name)))
    return i;

  if (obj->carried_by &&
     (i = get_obj_in_list(name, obj->carried_by->carrying)))
    return i;

  if (((rm = obj_room(obj)) != NOWHERE) &&
      (i = get_obj_in_list(name, world[rm].contents)))
    return i;

  return get_obj(name);
}

/* only searches the room */
obj_data *get_obj_in_room(room_data *room, char *name)
{
  obj_data *obj;
  long id;

  if (*name == UID_CHAR) {
      id = PARSE_UID(name + 1);
      for (obj = room->contents; obj; obj = obj->next_content)
          if (id == obj->script_id)
              return obj;
  } else {
      for (obj = room->contents; obj; obj = obj->next_content)
          if (isname(name, obj->name))
              return obj;
  }

  return NULL;
}

/* returns obj with name - searches room, then world */
obj_data *get_obj_by_room(room_data *room, char *name)
{
  obj_data *obj;

  if (*name == UID_CHAR)
    return find_obj(PARSE_UID(name + 1));

  for (obj = room->contents; obj; obj = obj->next_content)
    if (isname(name, obj->name))
      return obj;

  for (obj = object_list; obj; obj = obj->next)
    if (isname(name, obj->name))
      return obj;

  return NULL;
}

/* checks every PULSE_SCRIPT for random triggers */
/* Runs fire() on every object whose script has trigger type flag.  A
 * trigger can extract any object -- its own, or the next one in
 * object_list -- so the candidates are snapshotted as (UID, address) pairs
 * first and each is re-validated before it is dispatched. */
struct dg_object_target { long uid; obj_data *obj; };

static void fire_object_triggers(long flag, void (*fire)(obj_data *obj))
{
  struct dg_object_target *targets;
  obj_data *obj;
  size_t count = 0, i;

  for (obj = object_list; obj; obj = obj->next)
    if (SCRIPT(obj) && IS_SET(SCRIPT_TYPES(SCRIPT(obj)), flag))
      count++;
  if (!count)
    return;

  CREATE(targets, struct dg_object_target, count);
  for (i = 0, obj = object_list; obj && i < count; obj = obj->next)
    if (SCRIPT(obj) && IS_SET(SCRIPT_TYPES(SCRIPT(obj)), flag)) {
      targets[i].uid = obj_script_id(obj);
      targets[i++].obj = obj;
    }

  for (i = 0; i < count; i++)
    if (obj_uid_matches(targets[i].uid, targets[i].obj) &&
        SCRIPT(targets[i].obj) &&
        IS_SET(SCRIPT_TYPES(SCRIPT(targets[i].obj)), flag))
      fire(targets[i].obj);

  free(targets);
}

void script_trigger_check(void)
{
  char_data *ch;
  struct room_data *room=NULL;
  room_rnum nr;
  struct script_data *sc;

  for (ch = character_list; ch; ch = ch->next) {
    if (SCRIPT(ch)) {
      sc = SCRIPT(ch);

      if (IS_SET(SCRIPT_TYPES(sc), WTRIG_RANDOM) &&
          ((IN_ROOM(ch) != NOWHERE && !is_empty(world[IN_ROOM(ch)].zone)) ||
           IS_SET(SCRIPT_TYPES(sc), WTRIG_GLOBAL)))
        random_mtrigger(ch);
    }
  }

  fire_object_triggers(OTRIG_RANDOM, random_otrigger);

  for (nr = 0; nr <= top_of_world; nr++) {
    if (SCRIPT(&world[nr])) {
      room = &world[nr];
      sc = SCRIPT(room);

      if (IS_SET(SCRIPT_TYPES(sc), WTRIG_RANDOM) &&
          (!is_empty(room->zone) ||
           IS_SET(SCRIPT_TYPES(sc), WTRIG_GLOBAL)))
        random_wtrigger(room);
    }
  }
}

void check_time_triggers(void)
{
  char_data *ch;
  struct room_data *room=NULL;
  int nr;
  struct script_data *sc;

  for (ch = character_list; ch; ch = ch->next) {
    if (SCRIPT(ch)) {
      sc = SCRIPT(ch);

      if (IS_SET(SCRIPT_TYPES(sc), WTRIG_TIME) &&
          ((IN_ROOM(ch) != NOWHERE && !is_empty(world[IN_ROOM(ch)].zone)) ||
           IS_SET(SCRIPT_TYPES(sc), WTRIG_GLOBAL)))
        time_mtrigger(ch);
    }
  }

  fire_object_triggers(OTRIG_TIME, time_otrigger);

  for (nr = 0; nr <= top_of_world; nr++) {
    if (SCRIPT(&world[nr])) {
      room = &world[nr];
      sc = SCRIPT(room);

      if (IS_SET(SCRIPT_TYPES(sc), WTRIG_TIME) &&
          (!is_empty(room->zone) ||
           IS_SET(SCRIPT_TYPES(sc), WTRIG_GLOBAL)))
        time_wtrigger(room);
    }
  }
}

/* TRUE when trig is one of sc's triggers (sc may be NULL). */
static int dg_trig_in_script(const struct script_data *sc, const trig_data *trig)
{
  const trig_data *t;

  for (t = sc ? TRIGGERS(sc) : NULL; t; t = t->next)
    if (t == trig)
      return TRUE;
  return FALSE;
}

static EVENTFUNC(trig_wait_event)
{
  struct wait_event_data *wait_event_obj = (struct wait_event_data *)event_obj;
  trig_data *trig;
  void *go;
  int type;
  room_vnum owner_vnum;
  room_rnum owner_rnum = NOWHERE;

  trig = wait_event_obj->trigger;
  go = wait_event_obj->go;
  type = wait_event_obj->type;
  owner_vnum = wait_event_obj->owner_vnum;

  free(wait_event_obj);
  GET_TRIG_WAIT(trig) = NULL;

  /* A room owner is found again by vnum, never through the saved pointer:
   * OLC add_room()/delete_room() RECREATE world[], which can move every room,
   * and update_wait_events() repoints only the rooms they shift, so go may
   * point into the freed old array, or at another room that now occupies
   * the address. The room the vnum names must still hold the trigger;
   * otherwise go is NULL, which script_driver() refuses. */
  if (type == WLD_TRIGGER) {
    owner_rnum = real_room(owner_vnum);
    go = (owner_rnum != NOWHERE &&
          dg_trig_in_script(SCRIPT(&world[owner_rnum]), trig)) ?
         &world[owner_rnum] : NULL;
  }

#if 1  /* debugging */
  {
    int found = FALSE;
    if (type == MOB_TRIGGER) {
      struct char_data *tch;
      for (tch = character_list;tch && !found;tch = tch->next)
        if (tch == (struct char_data *)go)
          found = TRUE;
    } else if (type == OBJ_TRIGGER) {
      struct obj_data *obj;
      for (obj = object_list;obj && !found;obj = obj->next)
        if (obj == (struct obj_data *)go)
          found = TRUE;
    } else
      found = (go != NULL);   /* resolved by vnum above */
    if (!found) {
      /* Extraction cancels a trigger's wait event, so a missing owner is a
       * broken invariant. trig itself is still live (the event owned no
       * reference to free); end its suspended run so TRIGGER_CHECK() does
       * not refuse it forever with depth 1. */
      if (type == WLD_TRIGGER)
        TB_ERROR_MSG("dg", "wait-owner-lost", "trig_wait_event",
                     "trig vnum %d: owner room vnum %d %s on resume; "
                     "trigger ended", DIAG_TRIG_VNUM(trig), owner_vnum,
                     owner_rnum == NOWHERE ? "no longer exists"
                                           : "no longer holds the trigger");
      else
        TB_ERROR_MSG("dg", "wait-resume", "trig_wait_event",
                     "trig vnum %d type %d: owner not live on resume; "
                     "trigger ended (%d instances attached)",
                     DIAG_TRIG_VNUM(trig), type,
                     VALID_TRIG_RNUM(trig->nr) ? trig_index[trig->nr]->number : -1);
      script_log("Trigger VNum %d: restart attempt on unknown entity.",
                 GET_TRIG_VNUM(trig));
      free_varlist(GET_TRIG_VARS(trig));
      GET_TRIG_VARS(trig) = NULL;
      GET_TRIG_DEPTH(trig) = 0;
      return 0;
    }
  }
#endif

  script_driver(&go, trig, type, TRIG_RESTART);

  /* Do not reenqueue*/
  return 0;
}

static void do_stat_trigger(struct char_data *ch, trig_data *trig)
{
    /* Room kept free at the end of sb for the overflow notice. */
    static const char overflow_notice[] = "*** Overflow - script too long! ***\r\n";
    struct cmdlist_element *cmd_list;
    char sb[MAX_STRING_LENGTH], buf[MAX_STRING_LENGTH];
    struct tb_buf out;

    if (!trig)
    {
        log("SYSERR: NULL trigger passed to do_stat_trigger.");
        return;
    }

    /* The command list is builder text of any total length. Summing raw
     * snprintf() returns let one long line push the offset past sb, and the
     * overflow notice was then written beyond it with a wrapped size. The
     * bounded builder stops at its capacity; the notice fits in the space
     * reserved after it. */
    tb_buf_init(&out, sb, sizeof(sb) - sizeof(overflow_notice));

    tb_buf_appendf(&out, "Name: '%s%s%s',  VNum: [%s%5d%s], RNum: [%5d]\r\n",
              CCYEL(ch, C_NRM), GET_TRIG_NAME(trig), CCNRM(ch, C_NRM),
              CCGRN(ch, C_NRM), GET_TRIG_VNUM(trig), CCNRM(ch, C_NRM),
              GET_TRIG_RNUM(trig));

    if (trig->attach_type==OBJ_TRIGGER) {
      tb_buf_append(&out, "Trigger Intended Assignment: Objects\r\n");
      sprintbit(GET_TRIG_TYPE(trig), otrig_types, buf, sizeof(buf));
    } else if (trig->attach_type==WLD_TRIGGER) {
      tb_buf_append(&out, "Trigger Intended Assignment: Rooms\r\n");
      sprintbit(GET_TRIG_TYPE(trig), wtrig_types, buf, sizeof(buf));
    } else {
      tb_buf_append(&out, "Trigger Intended Assignment: Mobiles\r\n");
      sprintbit(GET_TRIG_TYPE(trig), trig_types, buf, sizeof(buf));
    }

    tb_buf_appendf(&out, "Trigger Type: %s, Numeric Arg: %d, Arg list: %s\r\n",
                     buf, GET_TRIG_NARG(trig),
                    ((GET_TRIG_ARG(trig) && *GET_TRIG_ARG(trig))
                     ? GET_TRIG_ARG(trig) : "None"));

    tb_buf_append(&out, "Commands:\r\n");

    for (cmd_list = trig->cmdlist; cmd_list && !tb_buf_truncated(&out);
         cmd_list = cmd_list->next)
      if (cmd_list->cmd)
        tb_buf_appendf(&out, "%s\r\n", cmd_list->cmd);

    if (tb_buf_truncated(&out)) {
      TB_WARN_MSG("dg", "tstat-truncate", "do_stat_trigger",
                  "trig vnum %d: listing truncated at %zu bytes",
                  GET_TRIG_VNUM(trig), out.length);
      strlcat(sb, overflow_notice, sizeof(sb));
    }

    page_string(ch->desc, sb, 1);
}

/* find the name of what the uid points to */
void find_uid_name(char *uid, char *name, size_t nlen)
{
  char_data *ch;
  obj_data *obj;

  if ((ch = get_char(uid)))
    tb_snprintf(name, nlen, "%s", ch->player.name);
  else if ((obj = get_obj(uid)))
    tb_snprintf(name, nlen, "%s", obj->name);
  else
    tb_snprintf(name, nlen, "uid = %s, (not found)", uid + 1);
}

/* general function to display stats on script sc */
static void script_stat (char_data *ch, struct script_data *sc)
{
  struct trig_var_data *tv;
  trig_data *t;
  char name[MAX_INPUT_LENGTH];
  char namebuf[512];
  char buf1[MAX_STRING_LENGTH];
  char valbuf[MAX_STRING_LENGTH];

  send_to_char(ch, "Global Variables: %s\r\n", sc->global_vars ? "" : "None");
  send_to_char(ch, "Global context: %ld\r\n", sc->context);

  for (tv = sc->global_vars; tv; tv = tv->next) {
    tb_snprintf(namebuf, sizeof(namebuf), "%s:%ld", tv->name, tv->context);
    if (*(tv->value) == UID_CHAR) {
      find_uid_name(tv->value, name, sizeof(name));
      send_to_char(ch, "    %15s:  %s\r\n", tv->context?namebuf:tv->name, name);
    } else
      send_to_char(ch, "    %15s:  %s\r\n", tv->context?namebuf:tv->name,
                   dg_display_value(tv->value, valbuf, sizeof(valbuf)));
  }

  for (t = TRIGGERS(sc); t; t = t->next) {
    send_to_char(ch, "\r\n  Trigger: %s%s%s, VNum: [%s%5d%s], RNum: [%5d]\r\n",
            CCYEL(ch, C_NRM), GET_TRIG_NAME(t), CCNRM(ch, C_NRM),
            CCGRN(ch, C_NRM), GET_TRIG_VNUM(t), CCNRM(ch, C_NRM),
            GET_TRIG_RNUM(t));

    if (t->attach_type==OBJ_TRIGGER) {
      send_to_char(ch, "  Trigger Intended Assignment: Objects\r\n");
      sprintbit(GET_TRIG_TYPE(t), otrig_types, buf1, sizeof(buf1));
    } else if (t->attach_type==WLD_TRIGGER) {
      send_to_char(ch, "  Trigger Intended Assignment: Rooms\r\n");
      sprintbit(GET_TRIG_TYPE(t), wtrig_types, buf1, sizeof(buf1));
    } else {
      send_to_char(ch, "  Trigger Intended Assignment: Mobiles\r\n");
      sprintbit(GET_TRIG_TYPE(t), trig_types, buf1, sizeof(buf1));
    }

    send_to_char(ch, "  Trigger Type: %s, Numeric Arg: %d, Arg list: %s\r\n",
            buf1, GET_TRIG_NARG(t),
            ((GET_TRIG_ARG(t) && *GET_TRIG_ARG(t)) ? GET_TRIG_ARG(t) :
             "None"));

    if (GET_TRIG_WAIT(t)) {
      send_to_char(ch, "    Wait: %ld, Current line: %s\r\n",
              event_time(GET_TRIG_WAIT(t)),
              t->curr_state ? t->curr_state->cmd : "End of Script");
      send_to_char(ch, "  Variables: %s\r\n", GET_TRIG_VARS(t) ? "" : "None");

      for (tv = GET_TRIG_VARS(t); tv; tv = tv->next) {
        if (*(tv->value) == UID_CHAR) {
          find_uid_name(tv->value, name, sizeof(name));
          send_to_char(ch, "    %15s:  %s\r\n", tv->name, name);
        } else
          send_to_char(ch, "    %15s:  %s\r\n", tv->name,
                       dg_display_value(tv->value, valbuf, sizeof(valbuf)));
      }
    }
  }
}

void do_sstat_room(struct char_data * ch, struct room_data *rm)
{
  send_to_char(ch, "Triggers:\r\n");
  if (!SCRIPT(rm)) {
    send_to_char(ch, "  None.\r\n");
    return;
  }

  script_stat(ch, SCRIPT(rm));
}

void do_sstat_object(char_data *ch, obj_data *j)
{
  send_to_char(ch, "Triggers:\r\n");
  if (!SCRIPT(j)) {
    send_to_char(ch, "  None.\r\n");
    return;
  }

  script_stat(ch, SCRIPT(j));
}

void do_sstat_character(char_data *ch, char_data *k)
{
  send_to_char(ch, "Triggers:\r\n");
  if (!SCRIPT(k)) {
    send_to_char(ch, "  None.\r\n");
    return;
  }

  script_stat(ch, SCRIPT(k));
}

/* Adds the trigger t to script sc in in location loc.  loc = -1 means add to 
 * the end, loc = 0 means add before all other triggers. */
void add_trigger(struct script_data *sc, trig_data *t, int loc)
{
  trig_data *i;
  int n;

  for (n = loc, i = TRIGGERS(sc); i && i->next && (n != 0); n--, i = i->next);

  if (!loc) {
          t->next = TRIGGERS(sc);
    TRIGGERS(sc) = t;
  } else if (!i)
    TRIGGERS(sc) = t;
  else {
    t->next = i->next;
    i->next = t;
  }

  SCRIPT_TYPES(sc) |= GET_TRIG_TYPE(t);

  t->next_in_world = trigger_list;
  trigger_list = t;
}

/* Zone whose builders may attach or detach triggers on a character: a mob's
 * prototype zone, or the zone of the room a player stands in. NOWHERE (no
 * prototype, no room) makes can_edit_zone() refuse everyone. */
static zone_rnum dg_char_edit_zone(const char_data *victim)
{
  if (IS_NPC(victim))
    return real_zone_by_thing(GET_MOB_VNUM(victim));
  return VALID_ROOM_RNUM(IN_ROOM(victim)) ? world[IN_ROOM(victim)].zone
                                          : NOWHERE;
}

ACMD(do_attach)
{
  char_data *victim;
  obj_data *object;
  room_data *room;
  trig_data *trig;
  char targ_name[MAX_INPUT_LENGTH], trig_name[MAX_INPUT_LENGTH];
  char loc_name[MAX_INPUT_LENGTH], arg[MAX_INPUT_LENGTH];
  int loc, tn, rn, num_arg;
  room_rnum rnum;

  argument = two_arguments(argument, arg, trig_name);
  two_arguments(argument, targ_name, loc_name);

  if (!*arg || !*targ_name || !*trig_name) {
    send_to_char(ch, "Usage: attach { mob | obj | room } { trigger } { name } [ location ]\r\n");
    return;
  }

  num_arg = tb_parse_int_or_zero(targ_name);
  tn = tb_parse_int_or_zero(trig_name);
  loc = (*loc_name) ? tb_parse_int_or_zero(loc_name) : -1;

  if (is_abbrev(arg, "mobile") || is_abbrev(arg, "mtr")) {
    victim = get_char_vis(ch, targ_name, NULL, FIND_CHAR_WORLD);
    if (!victim) { /* search room for one with this vnum */
      for (victim = world[IN_ROOM(ch)].people;victim;victim=victim->next_in_room)
        if (GET_MOB_VNUM(victim) == num_arg)
          break;

      if (!victim) {
        send_to_char(ch, "That mob does not exist.\r\n");
        return;
      }
    }
    if (!IS_NPC(victim) && !CONFIG_SCRIPT_PLAYERS)  {
      send_to_char(ch, "Players can't have scripts.\r\n");
      return;
    }
    /* The target's zone, as do_detach checks: the builder's own room let
     * anyone standing in their zone attach to a mob anywhere. */
    if (!can_edit_zone(ch, dg_char_edit_zone(victim))) {
      TB_WARN_MSG("dg", "attach-denied", "do_attach",
                  "builder " DIAG_CHAR_FMT ": trigger %d on " DIAG_CHAR_FMT
                  " outside the builder's zones; refused", DIAG_CHAR(ch), tn,
                  DIAG_CHAR(victim));
      send_to_char(ch, "You can only attach triggers in your own zone.\r\n");
      return;
    }
    /* have a valid mob, now get trigger */
    rn = real_trigger(tn);
    if ((rn == NOTHING) || !(trig = read_trigger(rn))) {
      send_to_char(ch, "That trigger does not exist.\r\n");
      return;
    }

    if (!SCRIPT(victim))
      CREATE(SCRIPT(victim), struct script_data, 1);
    add_trigger(SCRIPT(victim), trig, loc);

    if (IS_NPC(victim))
    send_to_char(ch, "Trigger %d (%s) attached to %s [%d].\r\n",
                 tn, GET_TRIG_NAME(trig), GET_SHORT(victim), GET_MOB_VNUM(victim));
    else 
    send_to_char(ch, "Trigger %d (%s) attached to player named %s.\r\n", 
                 tn, GET_TRIG_NAME(trig), GET_NAME(victim));
  }

  else if (is_abbrev(arg, "object") || is_abbrev(arg, "otr")) {
    object = get_obj_vis(ch, targ_name, NULL);
    if (!object) { /* search room for one with this vnum */
      for (object = world[IN_ROOM(ch)].contents;object;object=object->next_content)
        if (GET_OBJ_VNUM(object) == num_arg)
          break;

      if (!object) { /* search inventory for one with this vnum */
        for (object = ch->carrying;object;object=object->next_content)
          if (GET_OBJ_VNUM(object) == num_arg)
            break;

        if (!object) {
          send_to_char(ch, "That object does not exist.\r\n");
          return;
        }
      }
    }

    if (!can_edit_zone(ch, real_zone_by_thing(GET_OBJ_VNUM(object)))) {
      TB_WARN_MSG("dg", "attach-denied", "do_attach",
                  "builder " DIAG_CHAR_FMT ": trigger %d on obj vnum %d uid "
                  "%ld outside the builder's zones; refused", DIAG_CHAR(ch),
                  tn, GET_OBJ_VNUM(object), object->script_id);
      send_to_char(ch, "You can only attach triggers in your own zone.\r\n");
      return;
    }
    /* have a valid obj, now get trigger */
    rn = real_trigger(tn);
    if ((rn == NOTHING) || !(trig = read_trigger(rn))) {
      send_to_char(ch, "That trigger does not exist.\r\n");
      return;
    }

    if (!SCRIPT(object))
      CREATE(SCRIPT(object), struct script_data, 1);
    add_trigger(SCRIPT(object), trig, loc);

    send_to_char(ch, "Trigger %d (%s) attached to %s [%d].\r\n",
                 tn, GET_TRIG_NAME(trig),
                 (object->short_description ?
                  object->short_description : object->name),
                  GET_OBJ_VNUM(object));
  }

  else if (is_abbrev(arg, "room") || is_abbrev(arg, "wtr")) {
    if (strchr(targ_name, '.'))
      rnum = IN_ROOM(ch);
    else if (isdigit(*targ_name))
      rnum = find_target_room(ch, targ_name);
    else
      rnum = NOWHERE;

    if (rnum == NOWHERE) {
      send_to_char(ch, "You need to supply a room number or . for current room.\r\n");
      return;
    }

    if (!can_edit_zone(ch, world[rnum].zone)) {
      send_to_char(ch, "You can only attach triggers in your own zone.\r\n");
      return;
    }
    /* have a valid room, now get trigger */
    rn = real_trigger(tn);
    if ((rn == NOTHING) || !(trig = read_trigger(rn))) {
      send_to_char(ch, "That trigger does not exist.\r\n");
      return;
    }

    room = &world[rnum];

    if (!SCRIPT(room))
      CREATE(SCRIPT(room), struct script_data, 1);
    add_trigger(SCRIPT(room), trig, loc);

    send_to_char(ch, "Trigger %d (%s) attached to room %d.\r\n",
                 tn, GET_TRIG_NAME(trig), world[rnum].number);
  }

  else
    send_to_char(ch, "Please specify 'mob', 'obj', or 'room'.\r\n");
}

/* name is modified in place (a '.' is replaced by NUL) and must not be NULL.
 * A number matches either a list position or a vnum; "N.name" matches the
 * Nth trigger with that name. extract_trigger() defers the free while any
 * script runs.
 *
 * Removes the trigger specified by name, and the script of o if it removes the
 * last trigger.  name can either be a number, or a 'silly' name for the 
 * trigger, including things like 2.beggar-death. Returns 0 if did not find the
 * trigger, otherwise 1.  If it matters, you might need to check to see if all 
 * the triggers were removed after this function returns, in order to remove 
 * the script.
 *
 * When vnum_out / name_out are non-NULL they receive the removed trigger's
 * vnum and name, captured before the trigger is freed, so callers can report
 * what they removed without instantiating a prototype copy. */
static int remove_trigger(struct script_data *sc, char *name,
                          trig_vnum *vnum_out, char *name_out,
                          size_t name_size)
{
  trig_data *i, *j;
  int num = 0, string = FALSE, n;
  char *cname;


  if (!sc)
    return 0;

  /* name is immortal or script text; every caller must supply one. */
  if (!name) {
    TB_ERROR_MSG("dg", "detach-arg", "remove_trigger",
                 "called with a NULL trigger name; nothing removed");
    return 0;
  }

  if ((cname = strstr(name,".")) || (!isdigit(*name)) ) {
    string = TRUE;
    if (cname) {
      *cname = '\0';
      num = tb_parse_int_or_zero(name);
      name = ++cname;
    }
  } else
    num = tb_parse_int_or_zero(name);

  for (n = 0, j = NULL, i = TRIGGERS(sc); i; j = i, i = i->next) {
    if (string) {
      if (isname(name, GET_TRIG_NAME(i)))
        if (++n >= num)
          break;
    }

    /* This isn't clean. A numeric value will match if it's position OR vnum 
     * is found. originally the number was position-only. */
    else if (++n >= num)
      break;
    else if (trig_index[i->nr]->vnum == num)
      break;
  }

  if (i) {
    if (vnum_out)
      *vnum_out = GET_TRIG_VNUM(i);
    if (name_out && name_size)
      strlcpy(name_out, GET_TRIG_NAME(i) ? GET_TRIG_NAME(i) : "(unnamed)",
              name_size);
    if (j) {
      j->next = i->next;
      extract_trigger(i);
    }

    /* this was the first trigger */
    else {
      TRIGGERS(sc) = i->next;
      extract_trigger(i);
    }

    /* update the script type bitvector */
    SCRIPT_TYPES(sc) = 0;
    for (i = TRIGGERS(sc); i; i = i->next)
      SCRIPT_TYPES(sc) |= GET_TRIG_TYPE(i);

    return 1;
  } else
    return 0;
}

ACMD(do_detach)
{
  char_data *victim = NULL;
  obj_data *object = NULL;
  struct room_data *room;
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH], arg3[MAX_INPUT_LENGTH], *snum;
  char *trigger = 0;
  /* Vnum and name of the trigger remove_trigger() actually removed. This
   * used to instantiate the prototype named by arg3 (a read_trigger call) on
   * every call, leaking one trigger (and bumping its live count) each time. */
  char removed_name[MAX_INPUT_LENGTH];
  trig_vnum removed_vnum = NOTHING;
  int num_arg;
  room_rnum rnum;

  argument = two_arguments(argument, arg1, arg2);
  one_argument(argument, arg3);

  if (!*arg1 || !*arg2) {
    send_to_char(ch, "Usage: detach [ mob | object | room ] { target } { trigger |"
                 " 'all' }\r\n");
    return;
  }

  /* vnum of mob/obj, if given */
  num_arg = tb_parse_int_or_zero(arg2);

  if (!str_cmp(arg1, "room") || !str_cmp(arg1, "wtr")) {
    if (!*arg3 || (strchr(arg2, '.')))
      rnum = IN_ROOM(ch);
    else if (isdigit(*arg2))
      rnum = find_target_room(ch, arg2);
    else
      rnum = NOWHERE;

    if (rnum == NOWHERE) {
      send_to_char(ch, "That's not a valid room.\r\n");
      return;
    }

    room = &world[rnum]; 
    if (!can_edit_zone(ch, room->zone)) {
      send_to_char(ch, "You can only detach triggers in your own zone\r\n");
      return;
    }
    if (!SCRIPT(room))
      send_to_char(ch, "This room does not have any triggers.\r\n");
    else if (!str_cmp(arg2, "all") || !str_cmp(arg3, "all")) {
      extract_script(room, WLD_TRIGGER);
      send_to_char(ch, "All triggers removed from room %d.\r\n", world[rnum].number);
    } else {
      if (*arg3)
        snum = arg3;
      else
        snum = arg2;

    if (remove_trigger(SCRIPT(room), snum, &removed_vnum, removed_name,
                       sizeof(removed_name))) {
      send_to_char(ch, "Trigger %d (%s) removed from %d.\r\n", removed_vnum,
                   removed_name, world[rnum].number);
      
      if (!TRIGGERS(SCRIPT(room)))
        extract_script(room, WLD_TRIGGER);

    } else
      send_to_char(ch, "That trigger was not found.\r\n");
   }
  }

  else {
    if (is_abbrev(arg1, "mobile") || !str_cmp(arg1, "mtr")) {
      victim = get_char_vis(ch, arg2, NULL, FIND_CHAR_WORLD);
      if (!victim) { /* search room for one with this vnum */
        for (victim = world[IN_ROOM(ch)].people;victim;victim=victim->next_in_room)
          if (GET_MOB_VNUM(victim) == num_arg)
            break;

        if (!victim) {
          send_to_char(ch, "No such mobile around.\r\n");
          return;
        }
      }

      if (!*arg3) {
        send_to_char(ch, "You must specify a trigger to remove.\r\n");
        return;
      }
      trigger = arg3;
    }

    else if (is_abbrev(arg1, "object") || !str_cmp(arg1, "otr")) {
      object = get_obj_vis(ch, arg2, NULL);
      if (!object) { /* search room for one with this vnum */
        for (object = world[IN_ROOM(ch)].contents;object;object=object->next_content)
          if (GET_OBJ_VNUM(object) == num_arg)
            break;

        if (!object) { /* search inventory for one with this vnum */
          for (object = ch->carrying;object;object=object->next_content)
            if (GET_OBJ_VNUM(object) == num_arg)
              break;

          if (!object) { /* give up */
            send_to_char(ch, "No such object around.\r\n");
            return;
          }
        }
      }

      if (!*arg3) {
        send_to_char(ch, "You must specify a trigger to remove.\r\n");
        return;
      }
      trigger = arg3;
    }
    else  {
      /* Thanks to Carlos Myers for fixing the line below */
      if ((object = get_obj_in_equip_vis(ch, arg1, NULL, ch->equipment)));
      else if ((object = get_obj_in_list_vis(ch, arg1, NULL, ch->carrying)));
      else if ((victim = get_char_room_vis(ch, arg1, NULL)));
      else if ((object = get_obj_in_list_vis(ch, arg1, NULL, world[IN_ROOM(ch)].contents)));
      else if ((victim = get_char_vis(ch, arg1, NULL, FIND_CHAR_WORLD)));
      else if ((object = get_obj_vis(ch, arg1, NULL)));
      else
        send_to_char(ch, "Nothing around by that name.\r\n");

      trigger = arg2;
    }

    if (victim) {

      if (!SCRIPT(victim)) 
        send_to_char(ch, "That %s doesn't have any triggers.\r\n", IS_NPC(victim) ? "mob" : "player");  
      /* A player used to skip the zone check, so any builder could strip
       * another player's triggers; players now follow attach's rule. */
      else if (!can_edit_zone(ch, dg_char_edit_zone(victim))) {
        TB_WARN_MSG("dg", "detach-denied", "do_detach",
                    "builder " DIAG_CHAR_FMT ": triggers of " DIAG_CHAR_FMT
                    " outside the builder's zones; refused", DIAG_CHAR(ch),
                    DIAG_CHAR(victim));
        send_to_char(ch, "You can only detach triggers in your own zone\r\n");
        return;
      }
      else if (trigger && !str_cmp(trigger, "all")) {
        extract_script(victim, MOB_TRIGGER);
        send_to_char(ch, "All triggers removed from %s.\r\n", IS_NPC(victim) ? GET_SHORT(victim) : GET_NAME(victim));
      }

      else if (trigger && remove_trigger(SCRIPT(victim), trigger, &removed_vnum,
                                         removed_name, sizeof(removed_name))) {
        send_to_char(ch, "Trigger %d (%s) removed from %s.\r\n", 
          removed_vnum, removed_name, IS_NPC(victim) ? GET_SHORT(victim) : GET_NAME(victim));

        if (!TRIGGERS(SCRIPT(victim))) {
          extract_script(victim, MOB_TRIGGER);
        }
      } else
        send_to_char(ch, "That trigger was not found.\r\n");
    }

    else if (object) {
      if (!SCRIPT(object))
        send_to_char(ch, "That object doesn't have any triggers.\r\n");

      else if (!can_edit_zone(ch, real_zone_by_thing(GET_OBJ_VNUM(object)))) {
        send_to_char(ch, "You can only detach triggers in your own zone\r\n");
        return;
      }
      else if (trigger && !str_cmp(trigger, "all")) {
        extract_script(object, OBJ_TRIGGER);
        send_to_char(ch, "All triggers removed from %s.\r\n",
                object->short_description ? object->short_description :
                object->name);
      }

      else if (trigger && remove_trigger(SCRIPT(object), trigger, &removed_vnum,
                                         removed_name, sizeof(removed_name))) {
        send_to_char(ch, "Trigger %d (%s) removed from %s.\r\n", 
          removed_vnum, removed_name, object->short_description ? object->short_description : 
          object->name);
        if (!TRIGGERS(SCRIPT(object))) {
          extract_script(object, OBJ_TRIGGER);
        }
      } else
        send_to_char(ch, "That trigger was not found.\r\n");
    }
  }
}

/* Logs any errors caused by scripts to the system log. Will eventually allow 
 * on-line view of script errors. */
void script_vlog(const char *format, va_list args)
{
  char output[MAX_STRING_LENGTH];
  struct descriptor_data *i;

  /* Parse the args without letting a logging failure hide the script error. */
  {
    int result = vsnprintf(output, sizeof(output), format, args);

    if (result < 0) {
      TB_REPORT_ERRNO("vsnprintf", "script_vlog");
      strlcpy(output, "(script error message formatting failed)", sizeof(output));
    } else if ((size_t)result >= sizeof(output)) {
      basic_mud_log("SCRIPT ERROR LOGGER: message truncated (required=%d, capacity=%zu)",
                    result + 1, sizeof(output));
    }
  }

  /* Save to the syslog file */ 
  basic_mud_log("SCRIPT ERROR: %s", output); 

  /* And send to imms */ 
  for (i = descriptor_list; i; i = i->next) { 
    if (STATE(i) != CON_PLAYING || IS_NPC(i->character)) /* switch */ 
      continue; 
    if (GET_LEVEL(i->character) < LVL_BUILDER) 
      continue; 
    if (PLR_FLAGGED(i->character, PLR_WRITING)) 
      continue; 
    if (NRM > (PRF_FLAGGED(i->character, PRF_LOG1) ? 1 : 0) + (PRF_FLAGGED(i->character, PRF_LOG2) ? 2 : 0)) 
      continue; 

    send_to_char(i->character, "%s[ %s ]%s\r\n", CCGRN(i->character, C_NRM), output, CCNRM(i->character, C_NRM)); 
  }
}

void script_log(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  script_vlog(format, args);
  va_end(args);
}

/* Returns 1 if string is all digits, else 0. Bugfixed - would have returned 
 * true on num="------". */
static int is_num(char *arg)
{
   if (*arg == '\0')
      return FALSE;

   if (*arg == '+' || *arg == '-') {
      arg++;
      if (*arg == '\0')
         return FALSE;
   }

   for (; *arg != '\0'; arg++)
   {
      if (!isdigit(*arg))
         return FALSE;
   }

   return TRUE;
}

/* Script arithmetic is int. Operands and results outside int saturate to
 * INT_MIN / INT_MAX, as they always have, but now say so: a saturated
 * value is never what the builder meant. Operand text is not logged, since
 * either side may be player text. */
static void report_script_arith(const trig_data *trig, const char *op,
                                const char *what)
{
  TB_WARN_MSG("dg", "script-arith", "eval_op",
              "trig vnum %d: %s (op %s) clamped to int range",
              DIAG_TRIG_VNUM(trig), what, op);
  script_log("Trigger VNum %d: %s (op %s) clamped to int range.",
             DIAG_TRIG_VNUM(trig), what, op);
}

static int script_int_value(const trig_data *trig, const char *op,
                            const char *arg)
{
  long value;

  errno = 0;
  value = strtol(arg, NULL, 10);

  if (errno == ERANGE || value > INT_MAX || value < INT_MIN) {
    report_script_arith(trig, op, "operand");
    if (errno == ERANGE)
      return (*arg == '-') ? INT_MIN : INT_MAX;
    return value > INT_MAX ? INT_MAX : INT_MIN;
  }

  return (int)value;
}

static int clamp_script_int(const trig_data *trig, const char *op,
                            long long value)
{
  if (value > INT_MAX || value < INT_MIN) {
    report_script_arith(trig, op, "result");
    return value > INT_MAX ? INT_MAX : INT_MIN;
  }
  return (int)value;
}

/* evaluates 'lhs op rhs', and copies to result */
static void eval_op(char *op, char *lhs, char *rhs, char *result, void *go,
             struct script_data *sc, trig_data *trig)
{
  unsigned char *p;
  int lhs_num, rhs_num;
  long long arithmetic;

  /* Every result below is a number, so player text can be compared as
   * typed without its '%' becoming live again. */
  dg_restore_percent(lhs);
  dg_restore_percent(rhs);

  /* strip off extra spaces at begin and end */
  while (*lhs && isspace(*lhs))
    lhs++;
  while (*rhs && isspace(*rhs))
    rhs++;

  for (p = (unsigned char *) lhs; *p; p++);
  while (p > (unsigned char *) lhs && isspace((unsigned char) p[-1]))
    *--p = '\0';
  for (p = (unsigned char *) rhs; *p; p++);
  while (p > (unsigned char *) rhs && isspace((unsigned char) p[-1]))
    *--p = '\0';


  /* find the op, and figure out the value */
  if (!strcmp("||", op)) {
    if ((!*lhs || (*lhs == '0')) && (!*rhs || (*rhs == '0')))
      strlcpy(result, "0", MAX_INPUT_LENGTH);
    else
      strlcpy(result, "1", MAX_INPUT_LENGTH);
  }

  else if (!strcmp("&&", op)) {
    if (!*lhs || (*lhs == '0') || !*rhs || (*rhs == '0'))
      strlcpy(result, "0", MAX_INPUT_LENGTH);
    else
      strlcpy(result, "1", MAX_INPUT_LENGTH);
  }

  else if (!strcmp("==", op)) {
    if (is_num(lhs) && is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", script_int_value(trig, op, lhs) == script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", !str_cmp(lhs, rhs));
  }

  else if (!strcmp("!=", op)) {
    if (is_num(lhs) && is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", script_int_value(trig, op, lhs) != script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", str_cmp(lhs, rhs));
  }

  else if (!strcmp("<=", op)) {
    if (is_num(lhs) && is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", script_int_value(trig, op, lhs) <= script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", str_cmp(lhs, rhs) <= 0);
  }

  else if (!strcmp(">=", op)) {
    if (is_num(lhs) && is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", script_int_value(trig, op, lhs) >= script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", str_cmp(lhs, rhs) >= 0);
  }

  else if (!strcmp("<", op)) {
    if (is_num(lhs) && is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", script_int_value(trig, op, lhs) < script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", str_cmp(lhs, rhs) < 0);
  }

  else if (!strcmp(">", op)) {
    if (is_num(lhs) && is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", script_int_value(trig, op, lhs) > script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", str_cmp(lhs, rhs) > 0);
  }

  else if (!strcmp("/=", op))
    tb_snprintf(result, MAX_INPUT_LENGTH, "%c", str_str(lhs, rhs) ? '1' : '0');

  else if (!strcmp("*", op)) {
    lhs_num = script_int_value(trig, op, lhs);
    rhs_num = script_int_value(trig, op, rhs);
    arithmetic = (long long)lhs_num * (long long)rhs_num;
    tb_snprintf(result, MAX_INPUT_LENGTH, "%d", clamp_script_int(trig, op, arithmetic));
  }

  else if (!strcmp("/", op)) {
    lhs_num = script_int_value(trig, op, lhs);
    rhs_num = script_int_value(trig, op, rhs);
    if (rhs_num == 0) {
      /* Division by zero yields 0 (legacy), reported. */
      report_script_arith(trig, op, "division by zero");
      strlcpy(result, "0", MAX_INPUT_LENGTH);
    } else {
      arithmetic = (long long)lhs_num / (long long)rhs_num;
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", clamp_script_int(trig, op, arithmetic));
    }
  }

  else if (!strcmp("+", op)) {
    lhs_num = script_int_value(trig, op, lhs);
    rhs_num = script_int_value(trig, op, rhs);
    arithmetic = (long long)lhs_num + (long long)rhs_num;
    tb_snprintf(result, MAX_INPUT_LENGTH, "%d", clamp_script_int(trig, op, arithmetic));
  }

  else if (!strcmp("-", op)) {
    lhs_num = script_int_value(trig, op, lhs);
    rhs_num = script_int_value(trig, op, rhs);
    arithmetic = (long long)lhs_num - (long long)rhs_num;
    tb_snprintf(result, MAX_INPUT_LENGTH, "%d", clamp_script_int(trig, op, arithmetic));
  }

  else if (!strcmp("!", op)) {
    if (is_num(rhs))
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", !script_int_value(trig, op, rhs));
    else
      tb_snprintf(result, MAX_INPUT_LENGTH, "%d", !*rhs);
  }
}

/* p points to the first quote, returns the matching end quote, or the last 
 * non-null char in p.*/
char *matching_quote(char *p)
{
  char *start = p;

  for (p++; *p && (*p != '"'); p++) {
    if (*p == '\\' && p[1] != '\0')
      p++;
  }

  if (!*p && p > start)
    p--;

  return p;
}

/* p points to the first paren.  returns a pointer to the matching closing 
 * paren, or the last non-null char in p. */
static char *matching_paren(char *p)
{
  int i;

  for (p++, i = 1; *p && i; p++) {
    if (*p == '(')
      i++;
    else if (*p == ')')
      i--;
    else if (*p == '"')
      p = matching_quote(p);
  }

  return --p;
}

/* evaluates line, and returns answer in result.
 * result must hold MAX_INPUT_LENGTH bytes. line has normally been through
 * var_subst() already; operators are split on copies. Precedence, lowest
 * first: ||, &&, comparisons (== != <= >= /= < >), + -, * /, then unary !.
 * Parentheses recurse, so depth is bounded by the line length. A leaf with
 * no operator goes through var_subst() a second time (the legacy %%var%%
 * idiom); player-authored values are stored with '%' inert, so only
 * script-built text expands twice. Arithmetic is int and saturates,
 * reporting dg/script-arith (see eval_op()). */
static void eval_expr(char *line, char *result, void *go, struct script_data *sc,
               trig_data *trig, int type)
{
  char expr[MAX_INPUT_LENGTH], *p;

  while (*line && isspace(*line))
    line++;

  if (eval_lhs_op_rhs(line, result, go, sc, trig, type));

  else if (*line == '(') {
    strlcpy(expr, line, sizeof(expr));
    p = matching_paren(expr);
    *p = '\0';
    eval_expr(expr + 1, result, go, sc, trig, type);
  }

  else
    var_subst(go, sc, trig, type, line, result);
}

/* Evaluates expr if it is in the form lhs op rhs, and copies answer in result.
 * Returns 1 if expr is evaluated, else 0. */
static int eval_lhs_op_rhs(char *expr, char *result, void *go, struct script_data *sc,
                    trig_data *trig, int type)
{
  char *p, *tokens[MAX_INPUT_LENGTH];
  char line[MAX_INPUT_LENGTH], lhr[MAX_INPUT_LENGTH], rhr[MAX_INPUT_LENGTH];
  int i, j, k;
  char *match, *q;
  const char *match_op;

  /*
   * Binary operators grouped from lowest to highest precedence.
   * Operators within a group are evaluated left-to-right by splitting on
   * the rightmost top-level operator.
   */
  static const char *ops[][8] = {
    { "||", NULL },
    { "&&", NULL },
    { "==", "!=", "<=", ">=", "/=", "<", ">", NULL },
    { "+", "-", NULL },
    { "*", "/", NULL },
    { NULL }
  };

  strlcpy(line, expr, sizeof(line));
  p = line;

  /* Initialize tokens, an array of pointers to locations in line where the 
   * ops could possibly occur. */
  for (j = 0; *p; j++) {
    tokens[j] = p;
    if (*p == '(')
      p = matching_paren(p) + 1;
    else if (*p == '"')
      p = matching_quote(p) + 1;
    else if (isalnum(*p))
      for (p++; *p && (isalnum(*p) || isspace(*p)); p++);
    else
      p++;
  }
  tokens[j] = NULL;

  for (i = 0; ops[i][0] != NULL; i++) {
    match = NULL;
    match_op = NULL;

    for (j = 0; tokens[j]; j++) {
      for (k = 0; ops[i][k] != NULL; k++) {
        if (!strn_cmp(ops[i][k], tokens[j], strlen(ops[i][k]))) {
          /*
           * A leading + or -, or one following another operator, is a unary
           * sign rather than the binary operator for this precedence level.
           */
          if (ops[i][k][1] == '\0' &&
              (ops[i][k][0] == '+' || ops[i][k][0] == '-')) {
            q = tokens[j];
            while (q > line && isspace((unsigned char)q[-1]))
              q--;
            if (q == line || strchr("!<>=+-*/&|(", q[-1]) != NULL)
              break;
          }

          match = tokens[j];
          match_op = ops[i][k];
          break;
        }
      }
    }

    if (match != NULL) {
      *match = '\0';
      p = match + strlen(match_op);

      eval_expr(line, lhr, go, sc, trig, type);
      eval_expr(p, rhr, go, sc, trig, type);
      eval_op((char *)match_op, lhr, rhr, result, go, sc, trig);

      return 1;
    }
  }

  /* Unary logical NOT is right-associative. */
  for (j = 0; tokens[j]; j++) {
    if (tokens[j][0] == '!' && tokens[j][1] != '=') {
      *tokens[j] = '\0';
      p = tokens[j] + 1;

      eval_expr(line, lhr, go, sc, trig, type);
      eval_expr(p, rhr, go, sc, trig, type);
      eval_op("!", lhr, rhr, result, go, sc, trig);

      return 1;
    }
  }

  return 0;
}

/* returns 1 if cond is true, else 0 */
static int process_if(char *cond, void *go, struct script_data *sc,
               trig_data *trig, int type)
{
  char result[MAX_INPUT_LENGTH], *p;

  eval_expr(cond, result, go, sc, trig, type);

  p = result;
  skip_spaces(&p);

  if (!*p || *p == '0')
    return 0;
  else
    return 1;
}

/* TRUE when the script line p (builder text, leading blanks skipped) is the
 * control keyword kw as a whole word: kw followed by blank or the end of the
 * line. Prefix tests made 'else' match "elsewhere", 'end' match "endgame",
 * and the block scanners' three-letter 'don' match "donate", so ordinary
 * commands opened or closed blocks; the driver and the scanners now agree.
 * "endif" (stock 14.trg) is still an 'end', and DG_LINE_IS_ELSEIF() accepts
 * a condition glued to 'elseif' (stock 27.trg). */
static int dg_line_is(const char *p, const char *kw)
{
  int n = (int) strlen(kw);   /* a short literal keyword */

  return !strn_cmp(p, kw, n) && (!p[n] || isspace((unsigned char) p[n]));
}

/** 'end' or its common spelling 'endif'. p is evaluated twice. */
#define DG_LINE_IS_END(p)     (dg_line_is((p), "end") || dg_line_is((p), "endif"))
/** 'elseif', with or without a blank before its condition (the condition
 * starts at p + 6). No command begins with "elseif". */
#define DG_LINE_IS_ELSEIF(p)  (!strn_cmp((p), "elseif", 6))

/* Scans for end of if-block.  returns the line containg 'end', or the last
 * line of the trigger if not found (reported to the builder). Nested if
 * blocks are skipped. Never returns NULL for a non-NULL cl. */
static struct cmdlist_element *find_end(trig_data *trig, struct cmdlist_element *cl)
{
  struct cmdlist_element *c;
  char *p;

  if (!(cl->next)) { /* rryan: if this is the last line, theres no end */
    script_log("Trigger VNum %d has 'if' without 'end'. (error 1)", GET_TRIG_VNUM(trig));
    return cl;
  }

  for (c = cl->next; c; c = c->next) {
    for (p = c->cmd; *p && isspace(*p); p++);

    if (!strn_cmp("if ", p, 3))
      c = find_end(trig, c);
    else if (DG_LINE_IS_END(p))
      return c;

    /* thanks to Russell Ryan for this fix */
    if(!c->next) { /* rryan: this is the last line, we didn't find an end. */
      script_log("Trigger VNum %d has 'if' without 'end'. (error 2)", GET_TRIG_VNUM(trig));
      return c;
    }
  }

  /* rryan: we didn't find an end */
  script_log("Trigger VNum %d has 'if' without 'end'. (error 3)", GET_TRIG_VNUM(trig));
  return c;
}

/* Searches for valid elseif, else, or end to continue execution at. Returns
 * line of elseif, else, or end if found, or last line of trigger.
 * Side effect: when it stops on a true elseif or an else it increments
 * GET_TRIG_DEPTH(trig), entering that branch as script_driver() would. */
static struct cmdlist_element *find_else_end(trig_data *trig,
    struct cmdlist_element *cl, void *go, struct script_data *sc, int type)
{
  struct cmdlist_element *c;
  char *p;

  if (!(cl->next))
    return cl;

  for (c = cl->next;c->next; c = c->next) {
    for (p = c->cmd; *p && isspace(*p); p++); /* skip spaces */

    if (!strn_cmp("if ", p, 3))
      c = find_end(trig, c);

    else if (DG_LINE_IS_ELSEIF(p)) {
      if (process_if(p + 6, go, sc, trig, type)) {
        GET_TRIG_DEPTH(trig)++;
        return c;
      }
    }

    else if (dg_line_is(p, "else")) {
      GET_TRIG_DEPTH(trig)++;
      return c;
    }

    else if (DG_LINE_IS_END(p))
      return c;

    /* thanks to Russell Ryan for this fix */
    if(!c->next) { /* rryan: this is the last line, return. */
      script_log("Trigger VNum %d has 'if' without 'end'. (error 4)", GET_TRIG_VNUM(trig));
      return c;
    }
  }

  /* rryan: if we got here, it's the last line, if its not an end, log it. */
  for (p = c->cmd; *p && isspace(*p); p++); /* skip spaces */
  if (!DG_LINE_IS_END(p))
    script_log("Trigger VNum %d has 'if' without 'end'. (error 5)", GET_TRIG_VNUM(trig));
  return c;
}

/* UID of a script owner for diagnostics; never allocates an ID. */
static long dg_owner_uid(void *go, int type)
{
  if (!go)
    return 0;
  switch (type) {
    case MOB_TRIGGER: return ((char_data *) go)->script_id;
    case OBJ_TRIGGER: return ((obj_data *) go)->script_id;
    case WLD_TRIGGER: return room_script_id((room_data *) go);
    default:          return 0;
  }
}

/* Read one bounded long, retaining a pointer to the remaining suffix. */
static int script_wait_number(const char *text, long *number, const char **rest)
{
  char *end;
  if (!text || !number || !rest)
    return FALSE;
  while (isspace((unsigned char)*text))
    text++;
  errno = 0;
  *number = strtol(text, &end, 10);
  if (end == text || errno == ERANGE)
    return FALSE;
  *rest = end;
  return TRUE;
}

/* Accepted forms: "wait N" (pulses), "wait Ns" (seconds), "wait Nt" (mud
 * hours), "wait until H[:MM]" (next mud time of day). A wait below one
 * pulse waits one pulse, and a scaled wait above DG_MAX_WAIT_PULSES (one
 * mud year) waits that long; both are reported as dg/wait-range warnings.
 * A missing or malformed number or an unknown suffix is rejected with a
 * script_log, an out-of-day 'until' time with dg/wait-range, and a wait
 * whose queue key (when + pulse) would pass LONG_MAX with dg/wait-overflow.
 * The wait_event_data allocated here is owned by the event and freed by
 * trig_wait_event() (or by event_cancel() on extraction).
 *
 * Processes a 'wait' command. Returns TRUE when the wait event was created
 * and the trigger is now suspended at cl->next; FALSE when the wait was
 * rejected (already reported), in which case the caller must end the trigger
 * through its normal cleanup rather than leave it half-suspended. */
static int process_wait(void *go, trig_data *trig, int type, char *cmd,
                  struct cmdlist_element *cl)
{
  char buf[MAX_INPUT_LENGTH], *arg;
  struct wait_event_data *wait_event_obj;
  long when, hr, min, ntime;
  char c;

  arg = any_one_arg(cmd, buf);
  skip_spaces(&arg);

  if (!*arg) {
    script_log("Trigger: %s, VNum %d. wait w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
    return FALSE;
  }

  if (!strn_cmp(arg, "until ", 6)) {
    const char *rest;
    if (!script_wait_number(arg + 6, &hr, &rest)) {
      script_log("Trigger: %s, VNum %d. wait until w/o a time: '%s'",
              GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
      return FALSE;
    }
    while (isspace((unsigned char)*rest))
      rest++;
    if (*rest == ':') {
      if (!script_wait_number(rest + 1, &min, &rest) ||
          hr > LONG_MAX / 60 || hr < LONG_MIN / 60 ||
          (min > 0 && hr * 60 > LONG_MAX - min) ||
          (min < 0 && hr * 60 < LONG_MIN - min)) {
        script_log("Trigger: %s, VNum %d. invalid wait until time: '%s'",
                GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
        return FALSE;
      }
      min += hr * 60;
    } else {
      min = (hr % 100) + ((hr / 100) * 60);
    }
    while (isspace((unsigned char)*rest))
      rest++;
    if (*rest) {
      script_log("Trigger: %s, VNum %d. invalid wait until suffix: '%s'",
              GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
      return FALSE;
    }

    /* The time is script text: keep it within one day before scaling it to
     * pulses, so the multiplication below cannot overflow. */
    if (min < 0 || min > DG_WAIT_MAX_MINUTES) {
      TB_ERROR_MSG("dg", "wait-range", "process_wait",
                   "trig vnum %d owner type %d uid %ld: wait until %ld "
                   "minutes after midnight is outside [0,%d]; wait rejected",
                   DIAG_TRIG_VNUM(trig), type, dg_owner_uid(go, type), min,
                   DG_WAIT_MAX_MINUTES);
      script_log("Trigger: %s, VNum %d. wait until time out of range: '%s'",
              GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
      return FALSE;
    }

    /* calculate the pulse of the day of "until" time */
    ntime = (min * SECS_PER_MUD_HOUR * PASSES_PER_SEC) / 60;

    /* calculate pulse of day of current time */
    when = (pulse % (SECS_PER_MUD_HOUR * PASSES_PER_SEC)) +
      (time_info.hours * SECS_PER_MUD_HOUR * PASSES_PER_SEC);

    if (when >= ntime) /* adjust for next day */
      when = (SECS_PER_MUD_DAY * PASSES_PER_SEC) - when + ntime;
    else
      when = ntime - when;
  }

  else {
    const char *rest;
    int fields;
    if (!script_wait_number(arg, &when, &rest)) {
      script_log("Trigger: %s, VNum %d. wait w/o a number: '%s'",
              GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
      return FALSE;
    }
    while (isspace((unsigned char)*rest))
      rest++;
    /* Only trailing whitespace is ignored. A second token is invalid. */
    const char *suffix_end = rest;
    char suffix[16];
    size_t suffix_len;
    while (*suffix_end && !isspace((unsigned char)*suffix_end))
      suffix_end++;
    suffix_len = (size_t)(suffix_end - rest);
    while (isspace((unsigned char)*suffix_end))
      suffix_end++;
    if (*suffix_end || suffix_len >= sizeof(suffix)) {
      script_log("Trigger: %s, VNum %d. invalid wait suffix: '%s'",
              GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
      return FALSE;
    }
    memcpy(suffix, rest, suffix_len);
    suffix[suffix_len] = '\0';
    fields = suffix_len ? 2 : 1;
    c = suffix[0];
    /* Preserve single-letter s/t and legacy seconds spellings. */
    if (fields == 2 &&
        !(strcmp(suffix, "s") == 0 || strcmp(suffix, "sec") == 0 ||
          strcmp(suffix, "secs") == 0 || strcmp(suffix, "second") == 0 ||
          strcmp(suffix, "seconds") == 0 || strcmp(suffix, "t") == 0)) {
      script_log("Trigger: %s, VNum %d. invalid wait suffix: '%s'",
              GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
      return FALSE;
    }
    /* A wait shorter than one pulse has always run on the next pulse
     * (event_create() clamps it); keep that, but say so. */
    if (when < 1) {
      TB_WARN_MSG("dg", "wait-range", "process_wait",
                  "trig vnum %d owner type %d uid %ld: wait %ld is below 1; "
                  "waiting 1 pulse", DIAG_TRIG_VNUM(trig), type,
                  dg_owner_uid(go, type), when);
      when = 1;
      fields = 1;
    }
    if (fields == 2) {
      long unit = (c == 't') ? PULSES_PER_MUD_HOUR : PASSES_PER_SEC;

      /* Saturate rather than overflow; the cap below reports it. */
      if (when > LONG_MAX / unit)
        when = LONG_MAX;
      else if (when < LONG_MIN / unit)
        when = LONG_MIN;
      else
        when *= unit;
    }
  }

  /* A wait longer than a mud year is a script error, not a schedule. */
  if (when > DG_MAX_WAIT_PULSES) {
    TB_WARN_MSG("dg", "wait-range", "process_wait",
                "trig vnum %d owner type %d uid %ld: wait %ld exceeds one mud "
                "year (%ld pulses); capped", DIAG_TRIG_VNUM(trig), type,
                dg_owner_uid(go, type), when, (long) DG_MAX_WAIT_PULSES);
    script_log("Trigger: %s, VNum %d. wait longer than one mud year "
               "shortened to one year: '%s'",
               GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cl->cmd);
    when = DG_MAX_WAIT_PULSES;
  }

  /* event_create() keys the event at when + pulse. Near the end of the
   * pulse range (a long uptime where long is 32 bits) event_key() clamps
   * that key to LONG_MAX and reports event/event-time without the trigger,
   * so the wait would end early; once pulse itself passes LONG_MAX no key
   * fits at all. Reject here, where trigger and owner are known. */
  if (pulse > (unsigned long)(LONG_MAX - when)) {
    TB_ERROR_MSG("dg", "wait-overflow", "process_wait",
                 "trig vnum %d owner type %d uid %ld: wait %ld at pulse %lu "
                 "overflows the event queue key; wait rejected",
                 DIAG_TRIG_VNUM(trig), type, dg_owner_uid(go, type), when,
                 pulse);
    script_log("Trigger: %s, VNum %d. wait cannot be scheduled at this "
               "uptime: '%s'", GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig),
               cl->cmd);
    return FALSE;
  }

  CREATE(wait_event_obj, struct wait_event_data, 1);
  wait_event_obj->trigger = trig;
  wait_event_obj->go = go;
  wait_event_obj->type = type;
  /* world[] can move before the wait ends; see trig_wait_event(). */
  wait_event_obj->owner_vnum =
    (type == WLD_TRIGGER) ? ((room_data *) go)->number : NOWHERE;

  GET_TRIG_WAIT(trig) = event_create(trig_wait_event, wait_event_obj, when);
  trig->curr_state = cl->next;
  return TRUE;
}

/* processes a script set command */
static void process_set(struct script_data *sc, trig_data *trig, char *cmd)
{
  char arg[MAX_INPUT_LENGTH], name[MAX_INPUT_LENGTH], *value;

  value = two_arguments(cmd, arg, name);

  skip_spaces(&value);

  if (!*name) {
    script_log("Trigger: %s, VNum %d. set w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  add_var(&GET_TRIG_VARS(trig), name, value, sc ? sc->context : 0);

}

/* processes a script eval command */
void process_eval(void *go, struct script_data *sc, trig_data *trig,
                 int type, char *cmd)
{
  char arg[MAX_INPUT_LENGTH], name[MAX_INPUT_LENGTH];
  char result[MAX_INPUT_LENGTH], *expr;

  expr = one_argument(cmd, arg); /* cut off 'eval' */
  expr = one_argument(expr, name); /* cut off name */

  skip_spaces(&expr);

  if (!*name) {
    script_log("Trigger: %s, VNum %d. eval w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  eval_expr(expr, result, go, sc, trig, type);
  add_var(&GET_TRIG_VARS(trig), name, result, sc ? sc->context : 0);
}

/* script attaching a trigger to something */
static void process_attach(void *go, struct script_data *sc, trig_data *trig,
                    int type, char *cmd)
{
  char arg[MAX_INPUT_LENGTH], trignum_s[MAX_INPUT_LENGTH];
  char result[MAX_INPUT_LENGTH], *id_p;
  trig_data *newtrig;
  char_data *c=NULL;
  obj_data *o=NULL;
  room_data *r=NULL;
  long trignum, id;

  id_p = two_arguments(cmd, arg, trignum_s);
  skip_spaces(&id_p);

  if (!*trignum_s) {
    script_log("Trigger: %s, VNum %d. attach w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  if (!id_p || !*id_p || PARSE_UID(id_p)==0) {
    script_log("Trigger: %s, VNum %d. attach invalid id arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  /* parse and locate the id specified */
  eval_expr(id_p, result, go, sc, trig, type);
  if (!(id = PARSE_UID(result))) {
    script_log("Trigger: %s, VNum %d. attach invalid id arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }
  c = find_char(id);
  if (!c) {
    o = find_obj(id);
    if (!o) {
      r = find_room(id);
      if (!r) {
        script_log("Trigger: %s, VNum %d. attach invalid id arg: '%s'",
                GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
        return;
      }
    }
  }

  /* Validate the target before read_trigger() allocates an instance; this
   * reject used to return after the allocation and leak it. Players carry
   * scripts only when the operator enables CONFIG_SCRIPT_PLAYERS. */
  if (c && !IS_NPC(c) && !CONFIG_SCRIPT_PLAYERS) {
    TB_ERROR_MSG("dg", "attach-target", "process_attach",
                 "trig vnum %d: cannot attach trig %s to player idnum %ld "
                 "(script_players off)", GET_TRIG_VNUM(trig), trignum_s,
                 (long)GET_IDNUM(c));
    script_log("Trigger: %s, VNum %d. attach invalid target: player idnum %ld",
               GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), (long)GET_IDNUM(c));
    return;
  }

  /* locate and load the trigger specified */
  trignum = real_trigger(dg_parse_vnum(trignum_s));
  if (trignum == NOTHING || !(newtrig=read_trigger(trignum))) {
    script_log("Trigger: %s, VNum %d. attach invalid trigger: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), trignum_s);
    return;
  }

  if (c) {
    if (!SCRIPT(c))
      CREATE(SCRIPT(c), struct script_data, 1);
    add_trigger(SCRIPT(c), newtrig, -1);
    return;
  }

  if (o) {
    if (!SCRIPT(o))
      CREATE(SCRIPT(o), struct script_data, 1);
    add_trigger(SCRIPT(o), newtrig, -1);
    return;
  }

  if (r) {
    if (!SCRIPT(r))
      CREATE(SCRIPT(r), struct script_data, 1);
    add_trigger(SCRIPT(r), newtrig, -1);
    return;
  }
}

/* process_detach() found nothing to remove. */
static void report_detach_miss(const trig_data *trig, long uid,
                               const char *trignum_s, const char *why)
{
  TB_WARN_MSG("dg", "detach-miss", "process_detach",
              "trig vnum %d: target uid %ld trigger '%s': %s",
              DIAG_TRIG_VNUM(trig), uid, trignum_s, why);
  script_log("Trigger: %s, VNum %d. detach: uid %ld trigger '%s': %s",
             GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), uid, trignum_s, why);
}

/* script detaching a trigger from something */
static void process_detach(void *go, struct script_data *sc, trig_data *trig,
    int type, char *cmd)
{
  char arg[MAX_INPUT_LENGTH], trignum_s[MAX_INPUT_LENGTH];
  char result[MAX_INPUT_LENGTH], *id_p;
  char_data *c=NULL;
  obj_data *o=NULL;
  room_data *r=NULL;
  long id;

  id_p = two_arguments(cmd, arg, trignum_s);
  skip_spaces(&id_p);

  if (!*trignum_s) {
    script_log("Trigger: %s, VNum %d. detach w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  if (!id_p || !*id_p || PARSE_UID(id_p)==0) {
    script_log("Trigger: %s, VNum %d. detach invalid id arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  /* parse and locate the id specified */
  eval_expr(id_p, result, go, sc, trig, type);
  if (!(id = PARSE_UID(result))) {
    script_log("Trigger: %s, VNum %d. detach invalid id arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }
  c = find_char(id);
  if (!c) {
    o = find_obj(id);
    if (!o) {
      r = find_room(id);
      if (!r) {
        script_log("Trigger: %s, VNum %d. detach invalid id arg: '%s'",
                GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
        return;
      }
    }
  }


  /* A detach that removes nothing used to return silently, hiding a wrong
   * trigger number or target from the builder. "all" on a target with no
   * script has nothing to do and stays quiet. trignum_s is script text. */
  if (c && SCRIPT(c)) {
    if (!strcmp(trignum_s, "all")) {
      extract_script(c, MOB_TRIGGER);
      return;
    }
    if (remove_trigger(SCRIPT(c), trignum_s, NULL, NULL, 0)) {
      if (!TRIGGERS(SCRIPT(c))) {
        extract_script(c, MOB_TRIGGER);
      }
    } else
      report_detach_miss(trig, id, trignum_s, "trigger not attached");
    return;
  }

  if (o && SCRIPT(o)) {
    if (!strcmp(trignum_s, "all")) {
      extract_script(o, OBJ_TRIGGER);
      return;
    }
    if (remove_trigger(SCRIPT(o), trignum_s, NULL, NULL, 0)) {
      if (!TRIGGERS(SCRIPT(o))) {
        extract_script(o, OBJ_TRIGGER);
      }
    } else
      report_detach_miss(trig, id, trignum_s, "trigger not attached");
    return;
  }

  if (r && SCRIPT(r)) {
    if (!strcmp(trignum_s, "all")) {
      extract_script(r, WLD_TRIGGER);
      return;
    }
    if (remove_trigger(SCRIPT(r), trignum_s, NULL, NULL, 0)) {
      if (!TRIGGERS(SCRIPT(r))) {
        extract_script(r, WLD_TRIGGER);
      }
    } else
      report_detach_miss(trig, id, trignum_s, "trigger not attached");
    return;
  }

  if (strcmp(trignum_s, "all"))
    report_detach_miss(trig, id, trignum_s, "target has no script");
}

struct room_data *dg_room_of_obj(struct obj_data *obj)
{
  if (IN_ROOM(obj) != NOWHERE)
    return &world[IN_ROOM(obj)];
  if (obj->carried_by && IN_ROOM(obj->carried_by) != NOWHERE)
    return &world[IN_ROOM(obj->carried_by)];
  if (obj->worn_by && IN_ROOM(obj->worn_by) != NOWHERE)
    return &world[IN_ROOM(obj->worn_by)];
  if (obj->in_obj)
    return dg_room_of_obj(obj->in_obj);
  return NULL;
}

/* Called through DG_OWNER_ROOM() (dg_scripts.h). Script owners are placed:
 * characters in character_list stand in a room, and a scripted object is in
 * a room or carried, worn or contained by something that is. NOWHERE (65535,
 * far past world[]) or an rnum past top_of_world therefore means a broken
 * invariant, not bad script text; it is reported here so the caller can
 * skip its world[] lookup instead of reading out of bounds. */
room_rnum dg_owner_room_at(void *go, int type, const trig_data *trig,
                           const char *what, const char *file, int line,
                           const char *func)
{
  char owner[64];
  room_rnum rnum = NOWHERE;

  switch (type) {
    case MOB_TRIGGER: {
      char_data *mob = (char_data *) go;

      rnum = IN_ROOM(mob);
      tb_snprintf(owner, sizeof(owner), DIAG_CHAR_FMT " uid %ld",
                  DIAG_CHAR(mob), mob->script_id);
      break;
    }
    case OBJ_TRIGGER: {
      obj_data *obj = (obj_data *) go;

      rnum = obj_room(obj);
      tb_snprintf(owner, sizeof(owner), "obj vnum %d uid %ld",
                  GET_OBJ_VNUM(obj), obj->script_id);
      break;
    }
    case WLD_TRIGGER:
      rnum = real_room(((room_data *) go)->number);
      tb_snprintf(owner, sizeof(owner), "room vnum %d",
                  ((room_data *) go)->number);
      break;
    default:
      tb_snprintf(owner, sizeof(owner), "owner of type %d", type);
      break;
  }
  if (VALID_ROOM_RNUM(rnum))
    return rnum;

  tb_error_log_at("ERROR", "dg", "owner-unplaced", "script", 0, file, line,
                  func, "trig vnum %d: %s needs the owner's room, but %s is "
                  "at rnum %ld (not a room); the room lookup is skipped",
                  DIAG_TRIG_VNUM(trig), what, owner, (long) rnum);
  return NOWHERE;
}

/* create a UID variable from the id number */
static void makeuid_var(void *go, struct script_data *sc, trig_data *trig,
                 int type, char *cmd)
{
  char junk[MAX_INPUT_LENGTH], varname[MAX_INPUT_LENGTH];
  char arg[MAX_INPUT_LENGTH], name[MAX_INPUT_LENGTH];
  char uid[MAX_INPUT_LENGTH + 1]; // to make room for UID_CHAR

  *uid = '\0';
  half_chop(cmd, junk, cmd);    /* makeuid */
  half_chop(cmd, varname, cmd); /* variable name */
  half_chop(cmd, arg, cmd);     /* numerical id or 'obj' 'mob' or 'room' */
  half_chop(cmd, name, cmd);    /* if the above was obj, mob or room, this is the name */

  if (!*varname) {
    script_log("Trigger: %s, VNum %d. makeuid w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);

    return;
  }

  if (!*arg) {
    script_log("Trigger: %s, VNum %d. makeuid invalid id arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  if (tb_parse_int_or_zero(arg)!=0) { /* easy, if you pass an id number */
    char result[MAX_INPUT_LENGTH];

    eval_expr(arg, result, go, sc, trig, type);
    tb_snprintf(uid, sizeof(uid), "%c%s", UID_CHAR, result);
  } else { /* a lot more work without it */
    if (!*name) {
      script_log("Trigger: %s, VNum %d. makeuid needs name: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
      return;
    }

    if (is_abbrev(arg, "mob")) {
      struct char_data *c = NULL;
      switch (type) {
        case WLD_TRIGGER:
          c = get_char_in_room((struct room_data *)go, name);
          break;
        case OBJ_TRIGGER:
          c = get_char_near_obj((struct obj_data *)go, name);
          break;
        case MOB_TRIGGER:
          /* get_char_room_vis() walks world[IN_ROOM(go)].people. */
          if (DG_OWNER_ROOM(go, type, trig, "makeuid mob") != NOWHERE)
            c = get_char_room_vis((struct char_data *)go, name, NULL);
          break;
      }
      if (c)
        tb_snprintf(uid, sizeof(uid), "%c%ld", UID_CHAR, char_script_id(c));
    } else if (is_abbrev(arg, "obj")) {
      struct obj_data *o = NULL;
      switch (type) {
        case WLD_TRIGGER:
          o = get_obj_in_room((struct room_data *)go, name);
          break;
        case OBJ_TRIGGER:
          o = get_obj_near_obj((struct obj_data *)go, name);
          break;
        case MOB_TRIGGER:
          if ((o = get_obj_in_list_vis((struct char_data *)go, name, NULL,
                    ((struct char_data *)go)->carrying)) == NULL &&
              DG_OWNER_ROOM(go, type, trig, "makeuid obj") != NOWHERE)
            o = get_obj_in_list_vis((struct char_data *)go, name, NULL,
                    world[IN_ROOM((struct char_data *)go)].contents);
          break;
      }
      if (o)
        tb_snprintf(uid, sizeof(uid), "%c%ld", UID_CHAR, obj_script_id(o));
    } else if (is_abbrev(arg, "room")) {
      room_rnum r = DG_OWNER_ROOM(go, type, trig, "makeuid room");

      if (r != NOWHERE)
        tb_snprintf(uid, sizeof(uid), "%c%ld", UID_CHAR, room_script_id(world + r));
    } else {
      script_log("Trigger: %s, VNum %d. makeuid syntax error: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);

      return;
    }
  }
  if (*uid)
    add_var(&GET_TRIG_VARS(trig), varname, uid, sc ? sc->context : 0);
}

/* Processes a script return command. Returns the new value for the script to 
 * return. */
static int process_return(trig_data *trig, char *cmd)
{
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];

  two_arguments(cmd, arg1, arg2);

  if (!*arg2) {
    script_log("Trigger: %s, VNum %d. return w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);

    return 1;
  }

  return tb_parse_int_or_zero(arg2);
}

/* Removes a variable from the global vars of sc, or the local vars of trig if 
 * not found in global list. */
static void process_unset(struct script_data *sc, trig_data *trig, char *cmd)
{
  char arg[MAX_INPUT_LENGTH], *var;

  var = any_one_arg(cmd, arg);

  skip_spaces(&var);

  if (!*var) {
    script_log("Trigger: %s, VNum %d. unset w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  if (!remove_var(&(sc->global_vars), var))
    remove_var(&GET_TRIG_VARS(trig), var);
}

/* Copy a locally owned variable to the globals of another script.
 * 'remote <variable_name> <uid>' */
static void process_remote(struct script_data *sc, trig_data *trig, char *cmd)
{
  struct trig_var_data *vd;
  struct script_data *sc_remote=NULL;
  char *line, *var, *uid_p;
  char arg[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  long uid, context;
  room_data *room;
  char_data *mob;
  obj_data *obj;

  line = any_one_arg(cmd, arg);
  two_arguments(line, buf, buf2);
  var = buf;
  uid_p = buf2;
  skip_spaces(&var);
  skip_spaces(&uid_p);


  if (!*buf || !*buf2) {
    script_log("Trigger: %s, VNum %d. remote: invalid arguments '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  /* find the locally owned variable */
  for (vd = GET_TRIG_VARS(trig); vd; vd = vd->next)
    if (!str_cmp(vd->name, buf))
      break;

  if (!vd)
    for (vd = sc->global_vars; vd; vd = vd->next)
      if (!str_cmp(vd->name, var) &&
          (vd->context==0 || vd->context==sc->context))
        break;

  if (!vd) {
    script_log("Trigger: %s, VNum %d. local var '%s' not found in remote call",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), buf);
    return;
  }
  /* find the target script from the uid number */
  uid = PARSE_UID(buf2);
  if (uid<=0) {
    script_log("Trigger: %s, VNum %d. remote: illegal uid '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), buf2);
    return;
  }

  /* For all but PC's, context comes from the existing context. For PC's, 
   * context is 0 (global) */
  context = vd->context;

  if ((room = find_room(uid))) {
    sc_remote = SCRIPT(room);
  } else if ((mob = find_char(uid))) {
    sc_remote = SCRIPT(mob);
    if (!IS_NPC(mob)) context = 0;
  } else if ((obj = find_obj(uid))) {
    sc_remote = SCRIPT(obj);
  } else {
    script_log("Trigger: %s, VNum %d. remote: uid '%ld' invalid",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), uid);
    return;
  }

  /* A target without a script cannot hold globals: the write is lost, so
   * say so. The variable name is script text, never player text. */
  if (sc_remote==NULL) {
    TB_WARN_MSG("dg", "remote-target", "process_remote",
                "trig vnum %d: target uid %ld has no script; var '%s' not "
                "stored", DIAG_TRIG_VNUM(trig), uid, vd->name);
    script_log("Trigger: %s, VNum %d. remote: uid %ld has no script; var '%s' "
               "not stored", GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), uid,
               vd->name);
    return;
  }

  add_var(&(sc_remote->global_vars), vd->name, vd->value, context);
}

/* Command-line interface to rdelete. Named vdelete so people didn't think it 
 * was to delete rooms. */
ACMD(do_vdelete)
{
  struct trig_var_data *vd, *vd_prev=NULL;
  struct script_data *sc_remote=NULL;
  char *var, *uid_p;
  char buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  long uid;
  room_data *room;
  char_data *mob = NULL;
  obj_data *obj;
  zone_rnum zone;

  two_arguments(argument, buf, buf2);
  var = buf;
  uid_p = buf2;
  skip_spaces(&var);
  skip_spaces(&uid_p);


  if (!*buf || !*buf2) {
    send_to_char(ch, "Usage: vdelete { <variablename> | * | all } <id>\r\n");
    return;
  }

  /* find the target script from the uid number */
  uid = PARSE_UID(buf2);
  if (uid<=0) {
    send_to_char(ch, "vdelete: illegal id specified.\r\n");
    return;
  }

  if ((room = find_room(uid))) {
    sc_remote = SCRIPT(room);
    zone = room->zone;
  } else if ((mob = find_char(uid))) {
    sc_remote = SCRIPT(mob);
    zone = dg_char_edit_zone(mob);
  } else if ((obj = find_obj(uid))) {
    sc_remote = SCRIPT(obj);
    zone = real_zone_by_thing(GET_OBJ_VNUM(obj));
  } else {
    send_to_char(ch, "vdelete: cannot resolve specified id.\r\n");
    return;
  }

  /* Global variables are their owner's state; a player's are saved quest
   * flags.  vdelete is a builder command, so it follows attach and detach
   * (the owner's zone must be the builder's), and a player must also rank
   * below the builder. */
  if (!can_edit_zone(ch, zone) ||
      (mob && !IS_NPC(mob) && mob != ch && GET_LEVEL(mob) >= GET_LEVEL(ch))) {
    TB_WARN_MSG("dg", "vdelete-denied", "do_vdelete",
                "builder " DIAG_CHAR_FMT ": variables of uid %ld are outside "
                "the builder's zones or rank; refused", DIAG_CHAR(ch), uid);
    send_to_char(ch, "You can only delete variables of things in your own "
                 "zone.\r\n");
    return;
  }

  if (sc_remote==NULL) {
    send_to_char(ch, "That id represents no global variables.(1)\r\n");
    return;
  }

  if (sc_remote->global_vars==NULL) {
    send_to_char(ch, "That id represents no global variables.(2)\r\n");
    return;
  }

  if (*var == '*' || is_abbrev(var, "all")) {
    struct trig_var_data *vd_next;
    for (vd = sc_remote->global_vars; vd; vd = vd_next) {
      vd_next = vd->next;
      free(vd->value);
      free(vd->name);
      free(vd);
    }
    sc_remote->global_vars = NULL;
    send_to_char(ch, "All variables deleted from that id.\r\n");
    return;
  }

  /* find the global */
  for (vd = sc_remote->global_vars; vd; vd_prev = vd, vd = vd->next)
    if (!str_cmp(vd->name, var))
      break;

  if (!vd) {
    send_to_char(ch, "That variable cannot be located.\r\n");
    return;
  }

  /* ok, delete the variable */
  if (vd_prev) vd_prev->next = vd->next;
  else sc_remote->global_vars = vd->next;

  /* and free up the space */
  free(vd->value);
  free(vd->name);
  free(vd);

  send_to_char(ch, "Deleted.\r\n");
}

/* Called from do_set - return 0 for failure, 1 for success.  ch and vict are 
 * verified. */
int perform_set_dg_var(struct char_data *ch, struct char_data *vict, char *val_arg)
{
  char var_name[MAX_INPUT_LENGTH], *var_value;

  var_value = any_one_arg(val_arg, var_name);

  if (!*var_name || var_value == NULL || !*var_value) {
    send_to_char(ch, "Usage: set <char> <varname> <value>\r\n");
    return 0;
  }
  if (!SCRIPT(vict))
    CREATE(SCRIPT(vict), struct script_data, 1);

  add_var(&(SCRIPT(vict)->global_vars), var_name, var_value, 0);
  return 1;
}

/* Delete a variable from the globals of another script.
 * 'rdelete <variable_name> <uid>' */
static void process_rdelete(struct script_data *sc, trig_data *trig, char *cmd)
{
  struct trig_var_data *vd, *vd_prev=NULL;
  struct script_data *sc_remote=NULL;
  char *line, *var, *uid_p;
  char arg[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];
  long uid;
  room_data *room;
  char_data *mob;
  obj_data *obj;

  line = any_one_arg(cmd, arg);
  two_arguments(line, buf, buf2);
  var = buf;
  uid_p = buf2;
  skip_spaces(&var);
  skip_spaces(&uid_p);

  if (!*buf || !*buf2) {
    script_log("Trigger: %s, VNum %d. rdelete: invalid arguments '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  /* find the target script from the uid number */
  uid = PARSE_UID(buf2);
  if (uid<=0) {
    script_log("Trigger: %s, VNum %d. rdelete: illegal uid '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), buf2);
    return;
  }

  if ((room = find_room(uid))) {
    sc_remote = SCRIPT(room);
  } else if ((mob = find_char(uid))) {
    sc_remote = SCRIPT(mob);
  } else if ((obj = find_obj(uid))) {
    sc_remote = SCRIPT(obj);
  } else {
    script_log("Trigger: %s, VNum %d. rdelete: uid '%ld' invalid",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), uid);
    return;
  }

  /* rdelete is "make sure it is gone": a target with no script or no such
   * global already satisfies it, and scripts routinely rdelete flags that
   * may never have been set, so those cases stay quiet by design. */
  if (sc_remote==NULL) return; /* no script to delete a trigger from */
  if (sc_remote->global_vars==NULL) return; /* no script globals */

  /* find the global */
  for (vd = sc_remote->global_vars; vd; vd_prev = vd, vd = vd->next)
    if (!str_cmp(vd->name, var) &&
        (vd->context==0 || vd->context==sc->context))
      break;

  if (!vd) return; /* the variable doesn't exist, or is the wrong context */

  /* ok, delete the variable */
  if (vd_prev) vd_prev->next = vd->next;
  else sc_remote->global_vars = vd->next;

  /* and free up the space */
  free(vd->value);
  free(vd->name);
  free(vd);
}

/* Makes a local variable into a global variable. */
static void process_global(struct script_data *sc, trig_data *trig, char *cmd, long id)
{
  struct trig_var_data *vd;
  char arg[MAX_INPUT_LENGTH], *var;

  var = any_one_arg(cmd, arg);

  skip_spaces(&var);

  if (!*var) {
    script_log("Trigger: %s, VNum %d. global w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  for (vd = GET_TRIG_VARS(trig); vd; vd = vd->next)
    if (!str_cmp(vd->name, var))
      break;

  if (!vd) {
    script_log("Trigger: %s, VNum %d. local var '%s' not found in global call",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), var);
    return;
  }

  add_var(&(sc->global_vars), vd->name, vd->value, id);
  remove_var(&GET_TRIG_VARS(trig), vd->name);
}

/* set the current context for a script */
static void process_context(struct script_data *sc, trig_data *trig, char *cmd)
{
  char arg[MAX_INPUT_LENGTH], *var;

  var = any_one_arg(cmd, arg);

  skip_spaces(&var);

  if (!*var) {
    script_log("Trigger: %s, VNum %d. context w/o an arg: '%s'",
            GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig), cmd);
    return;
  }

  sc->context = tb_parse_long_or_zero(var);
}

static void extract_value(struct script_data *sc, trig_data *trig, char *cmd)
{
  char buf[MAX_INPUT_LENGTH], buf2[MAX_INPUT_LENGTH];
  char *buf3;
  char to[128];
  int num;

  buf3 = any_one_arg(cmd, buf);
  half_chop(buf3, buf2, buf);
  strlcpy(to, buf2, sizeof(to));

  num = tb_parse_int_or_zero(buf);
  if (num < 1) {
    TB_WARN_MSG("dg", "extract-range", "extract_value",
                "trig vnum %d: word number %d < 1; variable not set",
                DIAG_TRIG_VNUM(trig), num);
    script_log("Trigger: %s, VNum %d. extract number < 1!",
               GET_TRIG_NAME(trig), GET_TRIG_VNUM(trig));
    return;
  }

  half_chop(buf, buf3, buf2);

  /* num is script-controlled (up to INT_MAX) and the string may be player
   * text. Each pass consumes one word, so stop once the words run out
   * instead of spinning num times on an empty string, which stalled the
   * game loop for seconds. A word past the end yields an empty value, as
   * before, but now says so. */
  {
    int wanted = num;

    while (num > 0) {
      if (!*buf2) {
        TB_WARN_MSG("dg", "extract-range", "extract_value",
                    "trig vnum %d: word %d requested beyond end of string; "
                    "variable set empty", DIAG_TRIG_VNUM(trig), wanted);
        *buf = '\0';
        break;
      }
      half_chop(buf2, buf, buf2);
      num--;
    }
  }

  add_var(&GET_TRIG_VARS(trig), to, buf, sc ? sc->context : 0);
}

/* Thanks to Jamie Nelson for 4 dimensions for this addition.
 * Syntax :
 * dg_letter <new varname> <letter position> <string to get from>. 
 *   set string L337-String
 *   dg_letter var1 4 %string%
 *   dg_letter var2 11 %string%
 *   now %var1% == 7 and %var2% == g
 *   Note that the index starts at 1. */
static void dg_letter_value(struct script_data *sc, trig_data *trig, char *cmd)
{
  /* Set the letter/number at position 'num' as the variable. */
  char junk[MAX_INPUT_LENGTH];
  char varname[MAX_INPUT_LENGTH];
  char num_s[MAX_INPUT_LENGTH];
  char string[MAX_INPUT_LENGTH];
  int num;

  half_chop(cmd, junk, cmd);   /* "dg_letter" */
  half_chop(cmd, varname, cmd);
  half_chop(cmd, num_s, string);

  num = tb_parse_int_or_zero(num_s);

  script_log("The use of dg_letter is deprecated");
  script_log("- Use 'set <new variable> %%<text/var>.charat(index)%%' instead.");


  if (num < 1) {
    script_log("Trigger #%d : dg_letter number < 1!", GET_TRIG_VNUM(trig));
    return;
  }

  if ((size_t)num > strlen(string)) {
    script_log("Trigger #%d : dg_letter number > strlen!", GET_TRIG_VNUM(trig));
    return;
  }

  *junk = string[num-1];
  *(junk+1) = '\0';
  add_var(&GET_TRIG_VARS(trig), varname, junk, sc->context);
}

/* What the next script_driver() run returns when its script ends without
 * 'return'. Only script_driver_default() sets it, and the run consumes it
 * as it starts, so runs nested in it and every later call return 1, which
 * most trigger handlers read as "carry on". */
static int dg_next_default_return = 1;

int script_driver_default(void *go_adress, trig_data *trig, int type,
                          int mode, int default_return)
{
  dg_next_default_return = default_return;
  return script_driver(go_adress, trig, type, mode);
}

/* This is the core driver for scripts.
 * Arguments:
 * void *go_adress
 *   A pointer to a pointer to the entity running the script. The reason for 
 *   this approcah is that we want to be able to see from the calling function,
 *   if the entity has been free'd.
 *
 * trig_data *trig
 *   A pointer to the current running trigger.
 *
 * int type
 *   MOB_TRIGGER, OBJ_TRIGGER or WLD_TRIGGER, respectively.
 *
 * int mode
     TRIG_NEW     just started from dg_triggers.c
     TRIG_RESTART restarted after a 'wait'

 * Owner lifetime: a mobile (or player) owner pending extraction is refused
 * on entry (dg/owner-extracted): its waits and random triggers can still
 * fire in the pulse it died or quit. Each frame records its own owner (an
 * object's UID) and checks it after every command with
 * dg_frame_owner_gone(). If the owner is gone (purged itself, its container
 * was purged, a damage killed it, or a nested trigger purged it) the run
 * stops: an object's run returns at once with *go_adress set to NULL so the
 * caller sees it; a character's run, whose owner stays allocated until
 * extract_pending_chars(), ends through the normal cleanup and is reported
 * as dg/owner-extracted unless the command was 'mpurge' of itself. Nested
 * frames check only their own owner, so they cannot end this run.
 *
 * Exits: only an accepted 'wait' leaves the trigger suspended (depth 1,
 * variables kept, a wait event owning a wait_event_data). Every other exit,
 * including a rejected wait and a malformed while/switch, frees var_list and
 * resets depth to 0 so TRIGGER_CHECK() accepts the trigger again. sc may be
 * retired mid-run by detach/purge; it stays readable until
 * dg_script_execution_leave(). */
/* TRUE once a command run by this frame has freed its object owner or
 * extracted its mobile (or player) owner.  Each frame checks its own owner,
 * so nested scripts can neither hide nor fake this frame's result.  Rooms
 * are never freed by scripts.  A character owner is never pending
 * extraction on entry (script_driver() refuses that run), so DEAD() here
 * means this frame's commands, or a nested trigger, extracted it. */
static int dg_frame_owner_gone(void *go, int type, long owner_uid)
{
  switch (type) {
    case OBJ_TRIGGER:
      /* Compares the registered address; go itself may be freed. */
      return !obj_uid_matches(owner_uid, (obj_data *) go);
    case MOB_TRIGGER:
      /* Extracted characters stay allocated until the end of the pulse. */
      return DEAD((char_data *) go);
  }
  return FALSE;
}

/* TRUE when a mob script's command is 'mpurge' aimed at the mob itself, the
 * one deliberate way for a mob's command to extract its own owner (the
 * counterpart of 'opurge self' in obj_command_interpreter()). The target is
 * resolved the way do_mpurge() resolves it; mob is in a valid room here. */
static int dg_mob_cmd_purges_self(char_data *mob, char *cmd)
{
  char word[MAX_INPUT_LENGTH], target[MAX_INPUT_LENGTH];
  char *rest = any_one_arg(cmd, word);
  char_data *victim;

  if (str_cmp(word, "mpurge"))
    return FALSE;
  one_argument(rest, target);
  if (!*target)
    return FALSE;
  victim = (*target == UID_CHAR) ? get_char(target)
                                 : get_char_room_vis(mob, target, NULL);
  return victim == mob;
}

/* The trigger of the innermost running script_driver() frame, for
 * diagnostics raised by commands that are not handed one: %damage% reaches
 * script_damage() through command_interpreter() and the obj/wld command
 * interpreters. NULL when no script is running (an implementor typing
 * mdamage). The trigger stays allocated while its frame runs (retired
 * triggers are freed at the outermost dg_script_execution_leave()). */
static trig_data *running_trigger;

trig_data *dg_running_trigger(void)
{
  return running_trigger;
}

/* Every return of a script_driver() frame that entered: give the outer
 * frame back its running trigger, then leave. */
static void dg_frame_leave(trig_data *outer)
{
  running_trigger = outer;
  dg_script_execution_leave();
}

int script_driver(void *go_adress, trig_data *trig, int type, int mode)
{
  static int depth = 0;
  int ret_val = dg_next_default_return;   /* 1 unless script_driver_default() */
  long owner_uid = 0;
  int purges_self = FALSE;   /* this line is 'mpurge' of the mob itself */
  struct cmdlist_element *cl;
  char cmd[MAX_INPUT_LENGTH], *p;
  struct script_data *sc = 0;
  struct cmdlist_element *temp;
  void *go = NULL;
  trig_data *outer_trigger;

  void obj_command_interpreter(obj_data *obj, char *argument);
  void wld_command_interpreter(struct room_data *room, char *argument);

  dg_next_default_return = 1;   /* consumed: nested and later runs get 1 */

  if (!go_adress || !trig) {
    script_log("script_driver called with invalid NULL argument.");
    return SCRIPT_ERROR_CODE;
  }

  switch (type) {
    case MOB_TRIGGER:
      go = *(char_data **)go_adress;
      if (!go)
        return SCRIPT_ERROR_CODE;
      sc = SCRIPT((char_data *) go);
      break;
    case OBJ_TRIGGER:
      go = *(obj_data **)go_adress;
      if (!go)
        return SCRIPT_ERROR_CODE;
      sc = SCRIPT((obj_data *) go);
      break;
    case WLD_TRIGGER:
      go = *(room_data **)go_adress;
      if (!go)
        return SCRIPT_ERROR_CODE;
      sc = SCRIPT((room_data *) go);
      break;
    default:
      script_log("script_driver called with invalid trigger type %d.", type);
      return SCRIPT_ERROR_CODE;
  }

  if (!sc) {
    script_log("Trigger %d has no owning script.", GET_TRIG_VNUM(trig));
    return SCRIPT_ERROR_CODE;
  }

  /* extract_char() only flags a character (MOB_/PLR_NOTDEADYET); its script
   * and pending waits live on until extract_pending_chars() at the end of
   * the pulse. A wait expiring, or a random trigger firing, in the pulse a
   * mob died or a scripted player quit would run commands for a character
   * that has left the game. End the run instead, as for a lost owner. */
  if (type == MOB_TRIGGER && DEAD((char_data *) go)) {
    char_data *mob = (char_data *) go;

    TB_WARN_MSG("dg", "owner-extracted", "script_driver",
                "trig vnum %d: owner " DIAG_CHAR_FMT " uid %ld is pending "
                "extraction; %s", DIAG_TRIG_VNUM(trig), DIAG_CHAR(mob),
                mob->script_id, mode == TRIG_RESTART ?
                "suspended run ended" : "trigger not run");
    free_varlist(GET_TRIG_VARS(trig));
    GET_TRIG_VARS(trig) = NULL;
    GET_TRIG_DEPTH(trig) = 0;
    return SCRIPT_ERROR_CODE;
  }

  if (depth > MAX_SCRIPT_DEPTH) {
    TB_ERROR_MSG("dg", "script-recursion", "script_driver",
                 "trigger %d exceeded MAX_SCRIPT_DEPTH=%d (type=%d); its script "
                 "is being extracted", GET_TRIG_VNUM(trig), MAX_SCRIPT_DEPTH, type);
    script_log("Trigger %d recursed beyond maximum allowed depth.", GET_TRIG_VNUM(trig));
    switch (type) {
      case MOB_TRIGGER:
        script_log("It was attached to %s [%d]",
           GET_NAME((char_data *) go), GET_MOB_VNUM((char_data *) go));
        break;
      case OBJ_TRIGGER:
        script_log("It was attached to %s [%d]",
           ((obj_data *) go)->short_description, GET_OBJ_VNUM((obj_data *) go));
        break;
      case WLD_TRIGGER:
        script_log("It was attached to %s [%d]",
           ((room_data *) go)->name, ((room_data *) go)->number);
        break;
    }

    extract_script(go, type);

    /* extract_script() works on rooms, but on mobiles and objects, it will be 
     * called again if the caller is load_mtrigger or load_otrigger if it is 
     * one of these, we must make sure the script is not just reloaded on the 
     * next mob. We make the calling code decide how to handle it, so it does
     * not get totally removed unless it's a load_xtrigger(). */
    return SCRIPT_ERROR_CODE;
  }

  /* Every diagnostic raised while this trigger runs carries this breadcrumb,
   * so an error deep in a callback names the trigger and owner behind it. */
  switch (type) {
    case MOB_TRIGGER:
      tb_breadcrumbf("dg", "run trig=%d mob vnum=%d uid=%ld depth=%d",
                     GET_TRIG_VNUM(trig), GET_MOB_VNUM((char_data *) go),
                     ((char_data *) go)->script_id, depth);
      break;
    case OBJ_TRIGGER:
      tb_breadcrumbf("dg", "run trig=%d obj vnum=%d uid=%ld depth=%d",
                     GET_TRIG_VNUM(trig), GET_OBJ_VNUM((obj_data *) go),
                     ((obj_data *) go)->script_id, depth);
      break;
    case WLD_TRIGGER:
      tb_breadcrumbf("dg", "run trig=%d room vnum=%d depth=%d",
                     GET_TRIG_VNUM(trig), ((room_data *) go)->number, depth);
      break;
  }

  /* Remember who owns this frame; its commands can free or extract it. */
  switch (type) {
    case MOB_TRIGGER:
      owner_uid = char_script_id((char_data *) go);
      break;
    case OBJ_TRIGGER:
      owner_uid = obj_script_id((obj_data *) go);
      break;
  }

  depth++;
  dg_script_execution_enter();
  outer_trigger = running_trigger;
  running_trigger = trig;

  if (mode == TRIG_NEW) {
    GET_TRIG_DEPTH(trig) = 1;
    GET_TRIG_LOOPS(trig) = 0;
    sc->context = 0;
  }

  for (cl = (mode == TRIG_NEW) ? trig->cmdlist : trig->curr_state;
      cl && GET_TRIG_DEPTH(trig); cl = cl->next) {
    for (p = cl->cmd; *p && isspace(*p); p++);

    if (*p == '*') /* comment */
      continue;

    else if (!strn_cmp(p, "if ", 3)) {
      if (process_if(p + 3, go, sc, trig, type))
        GET_TRIG_DEPTH(trig)++;
      else
        cl = find_else_end(trig, cl, go, sc, type);
    }

    else if (DG_LINE_IS_ELSEIF(p) || dg_line_is(p, "else")) {
      /* If not in an if-block, ignore the extra 'else[if]' and warn about it. */
      if (GET_TRIG_DEPTH(trig) == 1) {
        script_log("Trigger VNum %d has 'else' without 'if'.",
                   GET_TRIG_VNUM(trig));
        continue;
      }
      cl = find_end(trig, cl);
      GET_TRIG_DEPTH(trig)--;
    } else if (!strn_cmp("while ", p, 6)) {
      temp = find_done(cl);
      if (!temp) {
        /* End through the normal cleanup below: returning here left the
         * trigger at depth 1, so TRIGGER_CHECK() refused it until its owner
         * was extracted. */
        report_unterminated_block(trig, "while");
        break;
      }
      if (process_if(p + 6, go, sc, trig, type)) {
         temp->original = cl;
      } else {
         cl = temp;
      }
    } else if (!strn_cmp("switch ", p, 7)) {
      /* NULL: the switch has no 'done' (reported); end the trigger. */
      if (!(cl = find_case(trig, cl, go, sc, type, p + 7)))
        break;
    } else if (DG_LINE_IS_END(p)) {
      /* If not in an if-block, ignore the extra 'end' and warn about it. */
      if (GET_TRIG_DEPTH(trig) == 1) {
        script_log("Trigger VNum %d has 'end' without 'if'.",
                   GET_TRIG_VNUM(trig));
        continue;
      }
      GET_TRIG_DEPTH(trig)--;
    } else if (dg_line_is(p, "done")) {
      /* if in a while loop, cl->original is non-NULL */
      if (cl->original) {
      char *orig_cmd = cl->original->cmd;
      while (*orig_cmd && isspace(*orig_cmd)) orig_cmd++;
      if (cl->original && process_if(orig_cmd + 6, go, sc, trig,
          type)) {
        cl = cl->original;
        GET_TRIG_LOOPS(trig)++;
        /* Pause every 30 passes. The count must be this instance's own:
         * cl->loops lived in the command list that every instance of the
         * trigger shares, so one mob's loop advanced or reset another's. */
        if (GET_TRIG_LOOPS(trig) % 30 == 0) {
          if (process_wait(go, trig, type, "wait 1", cl)) {
            depth--;
            dg_frame_leave(outer_trigger);
            return ret_val;
          }
          TB_ERROR_MSG("dg", "wait-invalid", "script_driver",
                       "trig vnum %d owner type %d uid %ld: loop pause "
                       "rejected; trigger ended", DIAG_TRIG_VNUM(trig), type,
                       dg_owner_uid(go, type));
          break;
        }
          if (GET_TRIG_LOOPS(trig) >= 100) {
          script_log("Trigger VNum %d has looped 100 times!!!",
            GET_TRIG_VNUM(trig));
            break;
          }
        } else {
         /* if we're falling through a switch statement, this ends it. */
        }
      }
    } else if (dg_line_is(p, "break")) {
      if (!(cl = find_done(cl))) {
        report_unterminated_block(trig, "break");
        break;
      }
    } else if (dg_line_is(p, "case") || dg_line_is(p, "default")) {
       /* Do nothing, this allows multiple cases to a single instance */
    }

    else {
      purges_self = FALSE;
      var_subst(go, sc, trig, type, p, cmd);

      if (!strn_cmp(cmd, "eval ", 5))
        process_eval(go, sc, trig, type, cmd);

      else if (!strn_cmp(cmd, "nop ", 4)); /* nop: do nothing */

      else if (!strn_cmp(cmd, "extract ", 8))
        extract_value(sc, trig, cmd);

      else if (!strn_cmp(cmd, "dg_letter ", 10))
        dg_letter_value(sc, trig, cmd);

      else if (!strn_cmp(cmd, "makeuid ", 8))
        makeuid_var(go, sc, trig, type, cmd);

      else if (!strn_cmp(cmd, "halt", 4))
        break;

      else if (!strn_cmp(cmd, "dg_cast ", 8))
        do_dg_cast(go, sc, trig, type, cmd);

      else if (!strn_cmp(cmd, "dg_affect ", 10))
        do_dg_affect(go, sc, trig, type, cmd);

      else if (!strn_cmp(cmd, "global ", 7))
        process_global(sc, trig, cmd, sc->context);

      else if (!strn_cmp(cmd, "context ", 8))
        process_context(sc, trig, cmd);

      else if (!strn_cmp(cmd, "remote ", 7))
        process_remote(sc, trig, cmd);

      else if (!strn_cmp(cmd, "rdelete ", 8))
        process_rdelete(sc, trig, cmd);

      else if (!strn_cmp(cmd, "return ", 7))
        ret_val = process_return(trig, cmd);

      else if (!strn_cmp(cmd, "set ", 4))
        process_set(sc, trig, cmd);

      else if (!strn_cmp(cmd, "unset ", 6))
        process_unset(sc, trig, cmd);

      else if (!strn_cmp(cmd, "wait ", 5)) {
        /* Only a scheduled wait may leave the trigger suspended (depth and
         * variables kept for trig_wait_event()). A rejected wait ends the
         * trigger through the cleanup below: returning here instead left
         * depth at 1, so TRIGGER_CHECK() refused it until extraction. */
        if (process_wait(go, trig, type, cmd, cl)) {
          depth--;
          dg_frame_leave(outer_trigger);
          return ret_val;
        }
        TB_ERROR_MSG("dg", "wait-invalid", "script_driver",
                     "trig vnum %d owner type %d uid %ld: wait rejected; "
                     "trigger ended", DIAG_TRIG_VNUM(trig), type,
                     dg_owner_uid(go, type));
        break;
      }

      else if (!strn_cmp(cmd, "attach ", 7))
        process_attach(go, sc, trig, type, cmd);

      else if (!strn_cmp(cmd, "detach ", 7))
        process_detach(go, sc, trig, type, cmd);

      else {
        /* m-commands (masound, mpurge, mload...) and the other commands a
         * mob's script runs index world[IN_ROOM(mob)]. A mob in
         * character_list always stands in a room, so an unplaced owner is a
         * broken invariant: refuse every command and end the trigger. */
        if (type == MOB_TRIGGER &&
            !VALID_ROOM_RNUM(IN_ROOM((char_data *) go))) {
          char_data *mob = (char_data *) go;

          TB_ERROR_MSG("dg", "mob-cmd-unplaced", "script_driver",
                       "trig vnum %d line %d: " DIAG_CHAR_FMT " uid %ld is "
                       "at rnum %ld (not a room); command refused, trigger "
                       "ended", DIAG_TRIG_VNUM(trig), dg_line_number(trig, cl),
                       DIAG_CHAR(mob), mob->script_id, (long) IN_ROOM(mob));
          break;
        }
        /* The command leaves the script engine here; player text in it
         * reads as typed again. */
        dg_restore_percent(cmd);
        if (type == MOB_TRIGGER)
          purges_self = dg_mob_cmd_purges_self((char_data *) go, cmd);
        switch (type) {
          case MOB_TRIGGER:
            if (!script_command_interpreter((char_data *) go, cmd))
            command_interpreter((char_data *) go, cmd);
            break;
          case OBJ_TRIGGER:
            obj_command_interpreter((obj_data *) go, cmd);
            break;
          case WLD_TRIGGER:
            wld_command_interpreter((struct room_data *) go, cmd);
            break;
        }
      }

      /* Any command, not just a purge of self, can free the owner: purging
       * the container it is in, a damage that kills it, a nested script. */
      if (dg_frame_owner_gone(go, type, owner_uid)) {
        if (type == MOB_TRIGGER) {
          /* The mob or player is pending extraction (see the entry check),
           * still allocated and holding its script: end through the normal
           * cleanup below. 'mpurge' of itself is the deliberate way; any
           * other (a damage, a nested trigger) skips lines the builder
           * wrote, so say which. */
          char_data *mob = (char_data *) go;

          if (!purges_self)
            TB_WARN_MSG("dg", "owner-extracted", "script_driver",
                        "trig vnum %d line %d: owner " DIAG_CHAR_FMT " uid %ld "
                        "was extracted by its own command or a nested "
                        "trigger; remaining lines skipped",
                        DIAG_TRIG_VNUM(trig), dg_line_number(trig, cl),
                        DIAG_CHAR(mob), mob->script_id);
          break;
        }
        depth--;
        if (type == OBJ_TRIGGER)
          *(obj_data **)go_adress = NULL;
        dg_frame_leave(outer_trigger);
        return ret_val;
      }
    }
  }

  /* The script may have detached itself ("detach all %self.id%"), leaving
   * the owner with no script. trig itself stays allocated until
   * dg_script_execution_leave() (extract_trigger() retires it while any
   * script runs), so its variables are always ours to free here. Freeing
   * them only when the owner still had a script leaked them otherwise. */
  free_varlist(GET_TRIG_VARS(trig));
  GET_TRIG_VARS(trig) = NULL;
  GET_TRIG_DEPTH(trig) = 0;

  depth--;
  dg_frame_leave(outer_trigger);
  return ret_val;
}

/* returns the real number of the trigger with given virtual number */
trig_rnum real_trigger(trig_vnum vnum)
{
  trig_rnum bot, top, mid;

  bot = 0;
  top = top_of_trigt - 1;

  if (!top_of_trigt || trig_index[bot]->vnum > vnum || trig_index[top]->vnum < vnum)
    return (NOTHING);

  /* perform binary search on trigger-table */
  while (bot <= top) {
    mid = (bot + top) / 2;

    if (trig_index[mid]->vnum == vnum)
      return (mid);
    if (trig_index[mid]->vnum > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOTHING);
}

ACMD(do_tstat)
{
  int rnum;
  char str[MAX_INPUT_LENGTH];

  half_chop(argument, str, argument);
  if (*str) {
    rnum = real_trigger(dg_parse_vnum(str));
    if (rnum == NOTHING) {
      send_to_char(ch, "That vnum does not exist.\r\n");
      return;
    }

    do_stat_trigger(ch, trig_index[rnum]->proto);
  } else
    send_to_char(ch, "Usage: tstat <vnum>\r\n");
}

/* Reports a while/switch block that the trigger ends before closing. */
static void report_unterminated_block(trig_data *trig, const char *keyword)
{
  TB_ERROR_MSG("dg", "block-unterminated", "script_driver",
               "trigger %d: '%s' has no matching 'done'",
               GET_TRIG_VNUM(trig), keyword);
  script_log("Trigger VNum %d has '%s' without 'done'.",
             GET_TRIG_VNUM(trig), keyword);
}

/* 1-based position of cl in trig's command list, for reports that name the
 * script line; 0 when cl is not in the list. */
static int dg_line_number(const trig_data *trig, const struct cmdlist_element *cl)
{
  const struct cmdlist_element *c;
  int n = 1;

  for (c = trig->cmdlist; c; c = c->next, n++)
    if (c == cl)
      return n;
  return 0;
}

/* The last line of the trigger, at or after cl. */
static struct cmdlist_element *last_line(struct cmdlist_element *cl)
{
  while (cl->next)
    cl = cl->next;
  return cl;
}

/* Scans for a case/default instance. Returns the line containg the correct
 * case instance, the closing 'done' if none matches, or the last line of
 * the trigger if the switch is never closed. */
static struct cmdlist_element *
find_case(struct trig_data *trig, struct cmdlist_element *cl,
          void *go, struct script_data *sc, int type, char *cond)
{
  char result[MAX_INPUT_LENGTH], value[MAX_INPUT_LENGTH];
  char matched[MAX_INPUT_LENGTH];
  struct cmdlist_element *c;
  char *p;

  eval_expr(cond, result, go, sc, trig, type);

  for (c = cl->next; c; c = c->next) {
    for (p = c->cmd; *p && isspace(*p); p++);

    if (!strn_cmp("while ", p, 6) || !strn_cmp("switch ", p, 7)) {
      /* Skip the nested block; its 'done' is not ours. */
      if (!(c = find_done(c)))
        break;
    } else if (!strn_cmp("case ", p, 5)) {
      /* eval_op() trims its operands in place; the line is shared with the
       * trigger prototype, so compare a copy. */
      strlcpy(value, p + 5, sizeof(value));
      eval_op("==", result, value, matched, go, sc, trig);
      if (*matched && *matched != '0')
        return c;
    } else if (dg_line_is(p, "default"))
      return c;
    else if (dg_line_is(p, "done"))
     return c;
  }

  report_unterminated_block(trig, "switch");
  return last_line(cl);
}

/* Scans for the 'done' that closes the while/switch block opened at cl.
 * Returns that line, or NULL if the trigger ends first; callers report it.
 * Never returns a line past the end of the trigger. */
static struct cmdlist_element *find_done(struct cmdlist_element *cl)
{
  struct cmdlist_element *c;
  char *p;

  if (!cl)
    return NULL;

  for (c = cl->next; c; c = c->next) {
    for (p = c->cmd; *p && isspace(*p); p++);

    if (!strn_cmp("while ", p, 6) || !strn_cmp("switch ", p, 7)) {
      if (!(c = find_done(c)))
        return NULL;
    } else if (dg_line_is(p, "done"))
      return c;
  }

  return NULL;
}


/* load in a character's saved variables.
 * Legacy plrvars file, one "name context value" per line; get_line() caps a
 * line at READ_SIZE - 1 bytes. Nothing writes these files any more (the
 * ASCII pfile Vars section replaced them), so the input is a legacy or
 * admin-placed file. */
void read_saved_vars(struct char_data *ch)
{
  FILE *file;
  long context;
  char fn[127];
  char input_line[1024], *temp, *p;
  /* any_one_arg() writes up to MAX_INPUT_LENGTH bytes (its contract). These
   * were 32 and 16 bytes, so a legacy plrvars line with a long first or
   * second word overflowed the stack. get_line() limits lines to READ_SIZE,
   * which is smaller, so neither word can be truncated either. */
  char varname[MAX_INPUT_LENGTH];
  char context_str[MAX_INPUT_LENGTH];

  /* If getting to the menu from inside the game, the vars aren't removed. So 
   * let's not allocate them again. */
  if (SCRIPT(ch))
    return;

  /* Create the space for the script structure which holds the vars. We need to
   * do this first, because later calls to 'remote' will need. A script already 
   * assigned. */
  CREATE(SCRIPT(ch), struct script_data, 1);

  /* find the file that holds the saved variables and open it.  The name comes
   * from the player file: a damaged or crafted one can leave it empty (or, with
   * the NPC bit set in Act, make GET_NAME() a missing short description), and
   * get_filename() refuses such names and unsafe ones without writing fn.
   * Opening the unwritten fn read stack garbage as a path. */
  if (!get_filename(fn, sizeof(fn), SCRIPT_VARS_FILE, GET_NAME(ch))) {
    TB_ERROR_MSG("dg", "vars-filename", "script_vars",
                 "player idnum %ld has no usable name for a variable file; "
                 "legacy variables not read", (long)GET_IDNUM(ch));
    return;
  }
  file = fopen(fn,"r");

  /* No legacy file is the normal case: nothing writes plrvars any more. */
  if( !file ) {
    log("read_saved_vars: player idnum %ld has no legacy variable file %s",
        (long)GET_IDNUM(ch), fn);
    return;
  }
  /* walk through each line in the file parsing variables */
  do {
    if (get_line(file, input_line)>0) {
      p = temp = strdup(input_line);
      temp = any_one_arg(temp, varname);
      temp = any_one_arg(temp, context_str);
      skip_spaces(&temp); /* temp now points to the rest of the line */

      context = tb_parse_long_or_zero(context_str);
      add_var(&(SCRIPT(ch)->global_vars), varname, temp, context);
      free(p); /* plug memory hole */
    }
  } while( !feof(file) );

  /* close the file and return */
  tb_fclose(file);
}

/* save a characters variables out to disk (legacy format; no callers).
 * Variables whose name starts with '-' are never saved. */
void save_char_vars(struct char_data *ch)
{
  FILE *file;
  char fn[127];
  struct trig_var_data *vars;

  /* Immediate return if no script (and therefore no variables) structure has 
   * been created. this will happen when the player is logging in */
  if (SCRIPT(ch) == NULL) return;

  /* we should never be called for an NPC, but just in case... */
  if (IS_NPC(ch)) return;

  /* As in read_saved_vars(): an unwritten fn would be unlinked and opened
   * for writing as whatever the stack held. */
  if (!get_filename(fn, sizeof(fn), SCRIPT_VARS_FILE, GET_NAME(ch))) {
    TB_ERROR_MSG("dg", "vars-filename", "script_vars",
                 "player idnum %ld has no usable name for a variable file; "
                 "variables not saved", (long)GET_IDNUM(ch));
    return;
  }
  /* No legacy file is the normal case. Any other failure leaves a stale
   * file that read_saved_vars() would load at a later login. */
  if (unlink(fn) != 0 && errno != ENOENT)
    TB_ERROR_ERRNO("dg", "vars-unlink", "script_vars",
                   "player idnum %ld: cannot remove the old variable file %s",
                   (long)GET_IDNUM(ch), fn);

  /* make sure this char has global variables to save */
  if (ch->script->global_vars == NULL) return;
  vars = ch->script->global_vars;

  file = fopen(fn,"wt");
  if (!file) {
    TB_REPORT_ERRNO("fopen", fn);
    mudlog(NRM, LVL_GOD, TRUE,
           "SYSERR: Could not open player variable file %s for writing.", fn);
    return;
  }
  /* Note that currently, context will always be zero. This may change in the 
   * future. */
  while (vars) {
    if (*vars->name != '-') /* don't save if it begins with - */
      tb_fprintf(file, "%s %ld %s\n", vars->name, vars->context, vars->value);
    vars = vars->next;
  }

  tb_fclose(file);
}

/* load in a character's saved variables from an ASCII pfile*/
void read_saved_vars_ascii(FILE *file, struct char_data *ch, int count)
{
  long context;
  char input_line[1024], *temp, *p;
  /* Sized for any_one_arg()'s MAX_INPUT_LENGTH contract, not READ_SIZE. */
  char varname[MAX_INPUT_LENGTH];
  char context_str[MAX_INPUT_LENGTH];
  int i;

  /* If getting to the menu from inside the game, the vars aren't removed. So 
   * let's not allocate them again. */
  if (SCRIPT(ch))
    return;

  /* Create the space for the script structure which holds the vars. We need to
   * do this first, because later calls to 'remote' will need. A script already
   * assigned. */
  CREATE(SCRIPT(ch), struct script_data, 1);

  /* walk through each line in the file parsing variables */
  for (i = 0; i < count; i++)
  {
    if (get_line(file, input_line)>0) {
      p = temp = strdup(input_line);
      temp = any_one_arg(temp, varname);
      temp = any_one_arg(temp, context_str);
      skip_spaces(&temp); /* temp now points to the rest of the line */

      context = tb_parse_long_or_zero(context_str);
      add_var(&(SCRIPT(ch)->global_vars), varname, temp, context);
      free(p); /* plug memory hole */
    }
  }
}

/* TRUE if var can be saved as one line that get_line() reads back whole.
 * A longer line, or a line break in the value, would let the rest of the
 * value be parsed as pfile tags of its own. */
static int saved_var_fits(const struct trig_var_data *var)
{
  char line[READ_SIZE];
  int n = snprintf(line, sizeof(line), "%s %ld %s",
                   var->name, var->context, var->value);

  return n >= 0 && (size_t)n < sizeof(line) && !strpbrk(var->value, "\r\n");
}

/* save a characters variables out to an ASCII pfile */
void save_char_vars_ascii(FILE *file, struct char_data *ch)
{
  struct trig_var_data *vars;
  int count = 0;
  /* Immediate return if no script (and therefore no variables) structure has 
   * been created. this will happen when the player is logging in */
  if (SCRIPT(ch) == NULL) return;

  /* we should never be called for an NPC, but just in case... */
  if (IS_NPC(ch)) return;

  /* make sure this char has global variables to save */
  if (ch->script->global_vars == NULL) return;

  /* Note that currently, context will always be zero. This may change in the 
   * future */
  for (vars = ch->script->global_vars;vars;vars = vars->next) {
    if (*vars->name == '-') /* don't save if it begins with - */
      continue;
    if (saved_var_fits(vars))
      count++;
    else
      TB_ERROR_MSG("dg", "var-save", vars->name,
                   "player idnum %ld: %zu-byte value does not fit one "
                   "%d-byte pfile line or holds a line break; not saved",
                   GET_IDNUM(ch), strlen(vars->value), READ_SIZE - 1);
  }

  if (count != 0) {
	  tb_fprintf(file, "Vars: %d\n", count);

  for (vars = ch->script->global_vars;vars;vars = vars->next)
    if (*vars->name != '-' && saved_var_fits(vars))
      tb_fprintf(file, "%s %ld %s\n", vars->name, vars->context, vars->value);
  }
}

/* find_char() helpers */
/* Must be power of 2. */
#define BUCKET_COUNT 64
/* Marks a bucket head, which is a sentinel and never holds an entity. */
#define UID_OUT_OF_RANGE 1000000000

struct lookup_table_t {
  long uid;
  void * c;
  struct lookup_table_t *next;
};
static struct lookup_table_t lookup_table[BUCKET_COUNT];

void init_lookup_table(void)
{
  int i;
  for (i = 0; i < BUCKET_COUNT; i++) {
    lookup_table[i].uid  = UID_OUT_OF_RANGE;
    lookup_table[i].c    = NULL;
    lookup_table[i].next = NULL;
  }
}

static inline struct lookup_table_t *get_bucket_head(long uid)
{
  int bucket = (int) (uid & (BUCKET_COUNT - 1));
  return &lookup_table[bucket];
}

static inline struct lookup_table_t *find_element_by_uid_in_lookup_table(long uid)
{
  struct lookup_table_t *lt = get_bucket_head(uid)->next;

  for (; lt && lt->uid != uid; lt = lt->next)
    ;
  return lt;
}

static struct char_data *find_char_by_uid_in_lookup_table(long uid)
{
  struct lookup_table_t *lt = find_element_by_uid_in_lookup_table(uid);

  /* A miss is routine (a UID that left the game, a probe in the wrong
   * namespace). The plain log() that was here named no caller or trigger;
   * callers that treat a miss as an error report it with that context. */
  if (lt)
    return (struct char_data *)(lt->c);
  return NULL;
}

static struct obj_data *find_obj_by_uid_in_lookup_table(long uid)
{
  struct lookup_table_t *lt = find_element_by_uid_in_lookup_table(uid);

  /* See find_char_by_uid_in_lookup_table(): misses are reported by callers. */
  if (lt)
    return (struct obj_data *)(lt->c);
  return NULL;
}

int has_obj_by_uid_in_lookup_table(long uid)
{
  /* Only object-range UIDs name objects; see note in dg_scripts.h. */
  return UID_IS_OBJ(uid) && find_element_by_uid_in_lookup_table(uid) != NULL;
}

/* Validate both the object UID namespace and the original allocation's
 * registered address without dereferencing a potentially freed object. */
int obj_uid_matches(long uid, const obj_data *obj)
{
  struct lookup_table_t *entry;
  if (!obj || !UID_IS_OBJ(uid))
    return FALSE;
  entry = find_element_by_uid_in_lookup_table(uid);
  return entry && entry->c == (const void *)obj;
}


/* uid is already registered to bound, and c asks for it.
 *  - The same entity again changes nothing (a player who went back to the
 *    menu re-enters the game with the character still registered).
 *  - A player UID (an idnum, below MOB_ID_BASE) moves to the new character:
 *    a player logs in again while the earlier character with the same idnum
 *    awaits extraction. remove_from_lookup_table() then refuses to drop the
 *    new binding when the old character is freed.
 *  - Mobile and object UIDs are allocated unique (char_script_id() and
 *    obj_script_id() skip registered IDs), so a second entity claiming one
 *    is a bug. It is refused: lookups keep finding the registered entity,
 *    never a newcomer that may be a temporary copy.
 * Returns TRUE if the binding should move to c. */
static int lookup_rebind_allowed(long uid, const void *bound, const void *c)
{
  if (bound == c)
    return FALSE;
  if (uid < MOB_ID_BASE) {
    TB_WARN_MSG("dg", "uid-rebind", "lookup_table",
                "uid=%ld rebound from %p to %p", uid, bound, c);
    return TRUE;
  }
  TB_ERROR_MSG("dg", "uid-rebind", "lookup_table",
               "uid=%ld is bound to %p; refusing to rebind it to %p "
               "(mobile and object UIDs are unique)", uid, bound, c);
  return FALSE;
}

void add_to_lookup_table(long uid, void *c)
{
  /* The bucket head is a sentinel holding no entity (uid UID_OUT_OF_RANGE);
   * lookups start at head->next. Matching the head stored an object whose
   * UID equals the sentinel (1000000000, a valid object UID) where no
   * lookup would ever find it. */
  struct lookup_table_t *lt = get_bucket_head(uid);

  for (;lt && lt->next; lt = lt->next)
    if (lt->next->uid == uid) {
      if (lookup_rebind_allowed(uid, lt->next->c, c))
        lt->next->c = c;
      return;
    }

  CREATE(lt->next, struct lookup_table_t, 1);
  lt->next->uid = uid;
  lt->next->c = c;
}

/* Remove uid only while it still maps to c. A UID may be rebound (player
 * login re-registers the idnum) while an older holder awaits extraction;
 * freeing that holder must not drop the live entity's mapping. */
void remove_from_lookup_table(long uid, const void *c)
{
  int bucket = (int) (uid & (BUCKET_COUNT - 1));
  struct lookup_table_t *lt = &lookup_table[bucket], *flt = NULL;

  /* This is not supposed to happen. UID 0 is not used. However, while I'm 
   * debugging the issue, let's just return right away. - Welcor */
  if (uid == 0)
    return;

  flt = find_element_by_uid_in_lookup_table(uid);

  if (flt && flt->c != c) {
    TB_WARN_MSG("dg", "uid-remove", "lookup_table",
                "uid=%ld is bound to %p, not the freed entity %p; keeping the "
                "live mapping", uid, flt->c, c);
    return;
  }

  if (flt) {
    for (lt = &lookup_table[bucket];lt->next != flt;lt = lt->next)
      ;
    lt->next = flt->next;
    free(flt);
    return;
  }

  TB_ERROR_MSG("dg", "uid-remove", "lookup_table",
               "uid=%ld (entity %p) is not registered; it was never added or "
               "was already removed", uid, c);
}

bool check_flags_by_name_ar(int *array, int numflags, char *search, const char *namelist[]) 
{ 
  int i, item=-1; 

  for (i=0; i<numflags && item < 0; i++) 
    if (!strcmp(search, namelist[i])) item = i; 

  if (item < 0) return FALSE; 

  if (IS_SET_AR(array, item)) return TRUE; 

  return FALSE; 
}

int trig_is_attached(struct script_data *sc, int trig_num) 
{ 
  trig_data *t; 

  if (!sc || !TRIGGERS(sc)) return 0; 

  for (t = TRIGGERS(sc); t; t = t->next) 
    if (GET_TRIG_VNUM(t) == trig_num) 
      return 1; 

  return 0; 
}

/**
* Fetches the char's script id -- may also set it here if it's not set yet.
*
* This function was provided by EmpireMUD to help reduce how quickly DG Scripts
* runs out of id space.
*
* @param char_data *ch The character.
* @return long The unique ID.
*/
long char_script_id(char_data *ch)
{
  if (ch->script_id == 0) {
    /* Stay inside the mobile range: an ID past ROOM_ID_BASE is unreachable
     * by find_char() and eventually collides with object IDs. After wrapping,
     * skip IDs still held by live mobiles. */
    long attempts;
    for (attempts = 0; attempts < ROOM_ID_BASE - MOB_ID_BASE; attempts++) {
      if (max_mob_id < MOB_ID_BASE || max_mob_id >= ROOM_ID_BASE) {
        TB_WARN_MSG("dg", "uid-allocate", "mobile_ids",
                    "mobile id counter %ld left [%d, %d); wrapping and reusing "
                    "IDs of extracted mobiles", max_mob_id, MOB_ID_BASE, ROOM_ID_BASE);
        mudlog(CMP, LVL_BUILDER, TRUE, "SYSERR: Script IDs for mobiles wrapped; reusing IDs of extracted mobiles");
        max_mob_id = MOB_ID_BASE;
      }
      if (!find_element_by_uid_in_lookup_table(max_mob_id))
        break;
      max_mob_id++;
    }
    if (attempts == ROOM_ID_BASE - MOB_ID_BASE) {
      TB_FATAL_MSG("dg", "uid-allocate", "mobile_ids",
                   "all %d mobile script IDs are registered; aborting",
                   ROOM_ID_BASE - MOB_ID_BASE);
      abort();
    }
    ch->script_id = max_mob_id++;
    add_to_lookup_table(ch->script_id, (void *)ch);
  }
  return ch->script_id;
}

/**
* Fetches the object's script id -- may also set it here if it's not set yet.
*
* This function was provided by EmpireMUD to help reduce how quickly DG Scripts
* runs out of id space.
*
* @param obj_data *obj The object.
* @return long The unique ID.
*/
long obj_script_id(obj_data *obj)
{
  if (obj->script_id == 0) {
    /* Never wrap into the character/room ranges (long may be 32 bits), and
     * never take an ID that is still registered. */
    int wraps = 0;
    while (max_obj_id < OBJ_ID_BASE || max_obj_id == LONG_MAX ||
           find_element_by_uid_in_lookup_table(max_obj_id)) {
      if (max_obj_id < OBJ_ID_BASE || max_obj_id == LONG_MAX) {
        if (wraps++) {
          TB_FATAL_MSG("dg", "uid-allocate", "object_ids",
                       "every object script ID from %d is registered; aborting",
                       OBJ_ID_BASE);
          abort();
        }
        TB_WARN_MSG("dg", "uid-allocate", "object_ids",
                    "object id counter %ld left [%d, LONG_MAX); wrapping and "
                    "reusing IDs of extracted objects", max_obj_id, OBJ_ID_BASE);
        mudlog(CMP, LVL_BUILDER, TRUE, "SYSERR: Script IDs for objects wrapped; reusing IDs of extracted objects");
        max_obj_id = OBJ_ID_BASE;
      } else
        max_obj_id++;
    }
    obj->script_id = max_obj_id++;
    add_to_lookup_table(obj->script_id, (void *)obj);
    
    /* objs don't run out of idspace, currently
    if (max_obj_id > x && reboot_control.time > 16) {
      mudlog(CMP, LVL_BUILDER, TRUE, "SYSERR: Script IDs for objects have exceeded the limit -- reboot to fix this");
    }
    */
  }
  return obj->script_id;
}
