/**************************************************************************
*  File: dg_db_scripts.c                                   Part of tbaMUD *
*  Usage: Contains routines to handle db functions for scripts and trigs. *
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
#include "db.h"
#include "handler.h"
#include "dg_event.h"
#include "comm.h"
#include "constants.h"
#include "interpreter.h" /* For half_chop */

/* local functions */
static void trig_data_init(trig_data *this_data);

static int parse_int_token(const char *text, int *out)
{
  char *end;
  long value;

  if (text == NULL || out == NULL)
    return 0;

  errno = 0;
  value = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || *end != '\0' ||
      value < INT_MIN || value > INT_MAX)
    return 0;

  *out = (int)value;
  return 1;
}

void parse_trigger(FILE *trig_f, int nr)
{
    int k, attach_type, narg = 0;
    char line[256], *cmds, *s, flags[256], errors[MAX_INPUT_LENGTH];
    char attach_text[32], narg_text[32];
    struct cmdlist_element *cle;
    struct index_data *t_index;
    struct trig_data *trig;

    CREATE(trig, trig_data, 1);
    CREATE(t_index, index_data, 1);

    t_index->vnum = nr;
    t_index->number = 0;
    t_index->func = NULL;
    t_index->proto = trig;

    tb_snprintf(errors, sizeof(errors), "trig vnum %d", nr);

    trig->nr = top_of_trigt;
    trig->name = fread_string(trig_f, errors);

    get_line(trig_f, line);
    k = sscanf(line, "%31s %255s %31s", attach_text, flags, narg_text);
    if (k < 2 || !parse_int_token(attach_text, &attach_type) ||
        (k == 3 && !parse_int_token(narg_text, &narg))) {
      log("SYSERR: Malformed trigger header for vnum %d: %s", nr, line);
      exit(1);
    }
    trig->attach_type = (byte)attach_type;
    trig->trigger_type = (long)asciiflag_conv(flags);
    trig->narg = (k == 3) ? narg : 0;

    trig->arglist = fread_string(trig_f, errors);

    cmds = fread_string(trig_f, errors);

    /* The .trg file is hand-editable.  fread_string() returns NULL for an
     * empty '~' body, and a body of only line breaks has no first token;
     * either way the trigger still needs one command line, because every
     * instance shares this cmdlist. */
    CREATE(trig->cmdlist, struct cmdlist_element, 1);
    s = cmds ? strtok(cmds, "\n\r") : NULL;
    if (s == NULL) {
      TB_WARN_MSG("dg", "trigger-parse-empty", "trigger",
                  "trig vnum %d has an empty command body; using '* No script'", nr);
      trig->cmdlist->cmd = strdup("* No script");
    } else {
      trig->cmdlist->cmd = strdup(s);
      cle = trig->cmdlist;

      while ((s = strtok(NULL, "\n\r"))) {
	CREATE(cle->next, struct cmdlist_element, 1);
	cle = cle->next;
	cle->cmd = strdup(s);
      }
    }

    free(cmds);

    trig_index[top_of_trigt++] = t_index;
}

/* Create a new trigger from a prototype. nr is the real number of the trigger. */
trig_data *read_trigger(int nr)
{
    index_data *t_index;
    trig_data *trig;

    if (nr >= top_of_trigt) return NULL;
    if ((t_index = trig_index[nr]) == NULL)
	return NULL;

    CREATE(trig, trig_data, 1);
    trig_data_copy(trig, t_index->proto);

    t_index->number++;

    return trig;
}

static void trig_data_init(trig_data *this_data)
{
    /* Never reused while the server runs; 0 is left for unserialized data. */
    static unsigned long next_serial = 0;

    if (++next_serial == 0)
      ++next_serial;
    this_data->serial = next_serial;
    this_data->nr = NOTHING;
    this_data->data_type = 0;
    this_data->name = NULL;
    this_data->trigger_type = 0;
    this_data->cmdlist = NULL;
    this_data->curr_state = NULL;
    this_data->narg = 0;
    this_data->arglist = NULL;
    this_data->depth = 0;
    this_data->wait_event = NULL;
    this_data->purged = FALSE;
    this_data->var_list = NULL;

    this_data->next = NULL;
}

void trig_data_copy(trig_data *this_data, const trig_data *trg)
{
    trig_data_init(this_data);

    this_data->nr = trg->nr;
    this_data->attach_type = trg->attach_type;
    this_data->data_type = trg->data_type;
    if (trg->name)
      this_data->name = strdup(trg->name);
    else {
      this_data->name = strdup("unnamed trigger");
      log("Trigger with no name! (%d)", trg->nr);
    }
    this_data->trigger_type = trg->trigger_type;
    this_data->cmdlist = trg->cmdlist;
    this_data->narg = trg->narg;
    if (trg->arglist) this_data->arglist = strdup(trg->arglist);
}

/* Append trigger vnum to the end of a prototype's trigger list. */
static void append_proto_trigger(struct trig_proto_list **head, int vnum)
{
  struct trig_proto_list *new_trg;

  CREATE(new_trg, struct trig_proto_list, 1);
  new_trg->vnum = vnum;
  new_trg->next = NULL;
  while (*head)
    head = &(*head)->next;
  *head = new_trg;
}

/* A prototype names a trigger vnum that has no trigger: tell builders in
 * game and leave a structured record naming the owner. */
static void report_missing_trigger(int vnum, const char *owner_type, int owner_vnum)
{
  TB_WARN_MSG("dg", "trigger-attach-missing", "trigger",
              "trig vnum %d missing for %s vnum %d; kept in proto list, not attached",
              vnum, owner_type, owner_vnum);
  mudlog(BRF, LVL_BUILDER, TRUE,
         "SYSERR: Trigger vnum #%d asked for but non-existant! (%s %d)",
         vnum, owner_type, owner_vnum);
}

/* for mobs and rooms: */
void dg_read_trigger(FILE *fp, void *proto, int type)
{
  char line[READ_SIZE];
  char junk[8], vnum_text[32];
  int vnum, rnum, count;
  char_data *mob;
  room_data *room;

  get_line(fp, line);
  count = sscanf(line, "%7s %31s", junk, vnum_text);

  if (count != 2 || !parse_int_token(vnum_text, &vnum)) {
    mudlog(BRF, LVL_BUILDER, TRUE,
           "SYSERR: Error assigning trigger! - Line was\n  %s", line);
    return;
  }

  /* A 'T' line naming a trigger that does not exist (yet) is kept in the
   * proto list, as assign_triggers() does, and only not instantiated:
   * dropping it would make the next OLC save, which writes proto_script,
   * delete the line from disk for good. */
  rnum = real_trigger(vnum);

  switch(type) {
    case MOB_TRIGGER:
      mob = (char_data *)proto;
      append_proto_trigger(&mob->proto_script, vnum);
      if (rnum == NOTHING)
        report_missing_trigger(vnum, "mob", GET_MOB_VNUM(mob));
      break;
    case WLD_TRIGGER:
      room = (room_data *)proto;
      append_proto_trigger(&room->proto_script, vnum);
      if (rnum != NOTHING) {
        if (!(room->script))
          CREATE(room->script, struct script_data, 1);
        add_trigger(SCRIPT(room), read_trigger(rnum), -1);
      } else
        report_missing_trigger(vnum, "room", room->number);
      break;
    default:
      TB_ERROR_MSG("dg", "trigger-attach-type", "trigger",
                   "trig vnum %d: owner type %d is not mob or room; not attached",
                   vnum, type);
      mudlog(BRF, LVL_BUILDER, TRUE,
             "SYSERR: Trigger vnum #%d assigned to non-mob/obj/room", vnum);
  }
}

void dg_obj_trigger(char *line, struct obj_data *obj)
{
  char junk[8], vnum_text[32];
  int vnum, count;

  count = sscanf(line, "%7s %31s", junk, vnum_text);

  if (count != 2 || !parse_int_token(vnum_text, &vnum)) {
    mudlog(BRF, LVL_BUILDER, TRUE,
           "SYSERR: dg_obj_trigger() : Error assigning trigger! - Line was:\n  %s", line);
    return;
  }

  /* Kept even when missing; see dg_read_trigger(). */
  append_proto_trigger(&obj->proto_script, vnum);
  if (real_trigger(vnum) == NOTHING)
    report_missing_trigger(vnum, "obj", GET_OBJ_VNUM(obj));
}

void assign_triggers(void *i, int type)
{
  struct char_data *mob = NULL;
  struct obj_data *obj = NULL;
  struct room_data *room = NULL;
  int rnum;
  struct trig_proto_list *trg_proto;

  switch (type)
  {
    case MOB_TRIGGER:
      mob = (char_data *)i;
      trg_proto = mob->proto_script;
      while (trg_proto) {
        rnum = real_trigger(trg_proto->vnum);
        if (rnum==NOTHING) {
          mudlog(BRF, LVL_BUILDER, TRUE,
                 "SYSERR: trigger #%d non-existant, for mob #%d",
                 trg_proto->vnum, mob_index[mob->nr].vnum);
        } else {
          if (!SCRIPT(mob))
            CREATE(SCRIPT(mob), struct script_data, 1);
          add_trigger(SCRIPT(mob), read_trigger(rnum), -1);
        }
        trg_proto = trg_proto->next;
      }
      break;
    case OBJ_TRIGGER:
      obj = (obj_data *)i;
      trg_proto = obj->proto_script;
      while (trg_proto) {
        rnum = real_trigger(trg_proto->vnum);
        if (rnum==NOTHING) {
          log("SYSERR: trigger #%d non-existant, for obj #%d",
            trg_proto->vnum, obj_index[obj->item_number].vnum);
        } else {
          if (!SCRIPT(obj))
            CREATE(SCRIPT(obj), struct script_data, 1);
          add_trigger(SCRIPT(obj), read_trigger(rnum), -1);
        }
        trg_proto = trg_proto->next;
      }
      break;
    case WLD_TRIGGER:
      room = (struct room_data *)i;
      trg_proto = room->proto_script;
      while (trg_proto) {
        rnum = real_trigger(trg_proto->vnum);
        if (rnum==NOTHING) {
          mudlog(BRF, LVL_BUILDER, TRUE,
                 "SYSERR: trigger #%d non-existant, for room #%d",
                 trg_proto->vnum, room->number);
        } else {
          if (!SCRIPT(room))
            CREATE(SCRIPT(room), struct script_data, 1);
          add_trigger(SCRIPT(room), read_trigger(rnum), -1);
        }
        trg_proto = trg_proto->next;
      }
      break;
    default:
      mudlog(BRF, LVL_BUILDER, TRUE,
             "SYSERR: unknown type for assign_triggers()");
      break;
  }
}
