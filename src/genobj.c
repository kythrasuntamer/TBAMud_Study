/**************************************************************************
*  File: genobj.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - Objects.                                  *
*                                                                         *
*  Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer.            *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "shop.h"
#include "constants.h"
#include "genolc.h"
#include "genobj.h"
#include "genzon.h"
#include "genshp.h"
#include "dg_olc.h"
#include "handler.h"
#include "interpreter.h"
#include "boards.h" /* for board_info */


/* local functions */
static int update_all_objects(struct obj_data *obj);
static int release_instance_state(obj_rnum rnum);
static void copy_object_strings(struct obj_data *to, struct obj_data *from);

obj_rnum add_object(struct obj_data *newobj, obj_vnum ovnum)
{
  int found = NOTHING;
  zone_rnum rznum = real_zone_by_thing(ovnum);

  /* Write object to internal tables. */
  if ((newobj->item_number = real_object(ovnum)) != NOTHING) {
    obj_rnum rnum = newobj->item_number;
    /* Must run before copy_object() frees the old prototype strings: it
     * tells instance-owned strings from shared ones by comparing them. */
    int reset = release_instance_state(rnum);

    copy_object(&obj_proto[rnum], newobj);
    update_all_objects(&obj_proto[rnum]);
    if (reset > 0)
      TB_WARN_MSG("olc", "obj-proto-update", "object",
                  "proto vnum %d: %d live instances lost restrung text or "
                  "running scripts on prototype update", ovnum, reset);
    add_to_save_list(zone_table[rznum].number, SL_OBJ);
    return rnum;
  }

  found = insert_object(newobj, ovnum);
  adjust_objects(found);
  add_to_save_list(zone_table[rznum].number, SL_OBJ);
  return found;
}

/* Free what each live instance of prototype rnum owns privately and that
 * update_all_objects() is about to overwrite: its running script (which
 * also cancels its wait events), its private proto_script copy, and any
 * restrung text (strings that are not the prototype's).  Returns how many
 * instances lost a restring or a running script. */
static int release_instance_state(obj_rnum rnum)
{
  struct obj_data *obj;
  int lost = 0;

  for (obj = object_list; obj; obj = obj->next) {
    bool restrung;

    if (obj->item_number != rnum)
      continue;
    restrung = (obj->name != obj_proto[rnum].name ||
                obj->description != obj_proto[rnum].description ||
                obj->short_description != obj_proto[rnum].short_description ||
                obj->action_description != obj_proto[rnum].action_description ||
                obj->ex_description != obj_proto[rnum].ex_description);
    if (restrung || SCRIPT(obj))
      lost++;
    if (SCRIPT(obj))
      extract_script(obj, OBJ_TRIGGER);
    if (obj->proto_script && obj->proto_script != obj_proto[rnum].proto_script)
      free_proto_script(obj, OBJ_TRIGGER);
    if (restrung)
      free_object_strings_proto(obj);
  }
  return lost;
}

/* Fix all existing objects to have these values. We need to run through each
 * and every object currently in the game to see which ones are pointing to
 * this prototype. If object is pointing to this prototype, then we need to
 * replace it with the new one.  release_instance_state() must have run
 * first: everything not restored below is replaced by the prototype's value.
 * Location, list links, UID and events stay with the instance; triggers are
 * left empty for the caller to re-attach (oedit_save_internally()). */
static int update_all_objects(struct obj_data *refobj)
{
  struct obj_data *obj, swap;
  int count = 0;

  for (obj = object_list; obj; obj = obj->next) {
    if (obj->item_number != refobj->item_number)
      continue;

    count++;

    /* Update the existing object but save a copy for private information. */
    swap = *obj;
    *obj = *refobj;

    /* Copy game-time dependent variables over. */
    obj->script_id = swap.script_id;
    IN_ROOM(obj) = swap.in_room;
    obj->carried_by = swap.carried_by;
    obj->worn_by = swap.worn_by;
    obj->worn_on = swap.worn_on;
    obj->in_obj = swap.in_obj;
    obj->contains = swap.contains;
    obj->next_content = swap.next_content;
    obj->next = swap.next;
    obj->sitting_here = swap.sitting_here;
    /* Pending events point at this instance; losing the list would leave
     * them unreachable by extract_obj() and firing after the free. */
    obj->events = swap.events;
    /* Never share the prototype's runtime script or trigger list. */
    obj->script = NULL;
    obj->proto_script = NULL;
  }

  return count;
}

/* Adjust the internal values of other objects as if something was inserted at
 * the given array index. Might also be useful to make 'holes' in the array
 * for some reason. */
obj_rnum adjust_objects(obj_rnum refpt)
{
  int shop, i, zone, cmd_no;
  struct obj_data *obj;

#if CIRCLE_UNSIGNED_INDEX
  if (refpt == NOTHING || refpt > top_of_objt)
#else
  if (refpt < 0 || refpt > top_of_objt)
#endif
    return NOTHING;

  /* Renumber live objects. */
  for (obj = object_list; obj; obj = obj->next)
    GET_OBJ_RNUM(obj) += (GET_OBJ_RNUM(obj) != NOTHING && GET_OBJ_RNUM(obj) >= refpt);

  /* Renumber zone table. */
  for (zone = 0; zone <= top_of_zone_table; zone++) {
    for (cmd_no = 0; ZCMD(zone, cmd_no).command != 'S'; cmd_no++) {
      switch (ZCMD(zone, cmd_no).command) {
      case 'P':
        ZCMD(zone, cmd_no).arg3 += (ZCMD(zone, cmd_no).arg3 >= refpt);
         /* fall through */
      case 'O':
      case 'G':
      case 'E':
        ZCMD(zone, cmd_no).arg1 += (ZCMD(zone, cmd_no).arg1 >= refpt);
        break;
      case 'R':
        ZCMD(zone, cmd_no).arg2 += (ZCMD(zone, cmd_no).arg2 >= refpt);
        break;
      }
    }
  }

  /* Renumber notice boards. */
  for (i = 0; i < NUM_OF_BOARDS; i++)
    BOARD_RNUM(i) += (BOARD_RNUM(i) >= refpt);

  /* Renumber shop produce. */
  for (shop = 0; shop <= top_shop; shop++)
    for (i = 0; SHOP_PRODUCT(shop, i) != NOTHING; i++)
      SHOP_PRODUCT(shop, i) += (SHOP_PRODUCT(shop, i) >= refpt);

  return refpt;
}

/* Function handle the insertion of an object within the prototype framework.
 * Note that this does not adjust internal values of other objects, use
 * add_object() for that. */
obj_rnum insert_object(struct obj_data *obj, obj_vnum ovnum)
{
  obj_rnum i;

  top_of_objt++;
  RECREATE(obj_index, struct index_data, top_of_objt + 1);
  RECREATE(obj_proto, struct obj_data, top_of_objt + 1);

  /* Start counting through both tables. */
  for (i = top_of_objt; i > 0; i--) {
    /* Check if current virtual is bigger than our virtual number. */
    if (ovnum > obj_index[i - 1].vnum)
      return index_object(obj, ovnum, i);

    /* Copy over the object that should be here. */
    obj_index[i] = obj_index[i - 1];
    obj_proto[i] = obj_proto[i - 1];
    obj_proto[i].item_number = i;
  }

  /* Not found, place at 0. */
  return index_object(obj, ovnum, 0);
}

obj_rnum index_object(struct obj_data *obj, obj_vnum ovnum, obj_rnum ornum)
{
#if CIRCLE_UNSIGNED_INDEX
  if (obj == NULL || ornum == NOTHING || ornum > top_of_objt)
#else
  if (obj == NULL || ovnum < 0 || ornum < 0 || ornum > top_of_objt)
#endif
    return NOWHERE;

  obj->item_number = ornum;
  obj_index[ornum].vnum = ovnum;
  obj_index[ornum].number = 0;
  obj_index[ornum].func = NULL;

  copy_object_preserve(&obj_proto[ornum], obj);
  obj_proto[ornum].in_room = NOWHERE;

  return ornum;
}

int save_objects(zone_rnum zone_num)
{
  char filename[128], buf[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];
  char ebuf1[MAX_STRING_LENGTH], ebuf2[MAX_STRING_LENGTH], ebuf3[MAX_STRING_LENGTH], ebuf4[MAX_STRING_LENGTH];
  char wbuf1[MAX_STRING_LENGTH], wbuf2[MAX_STRING_LENGTH], wbuf3[MAX_STRING_LENGTH], wbuf4[MAX_STRING_LENGTH];
  char pbuf1[MAX_STRING_LENGTH], pbuf2[MAX_STRING_LENGTH], pbuf3[MAX_STRING_LENGTH], pbuf4[MAX_STRING_LENGTH];
  int counter, counter2, realcounter;
  FILE *fp;
  struct obj_data *obj;
  struct extra_descr_data *ex_desc;

#if CIRCLE_UNSIGNED_INDEX
  if (zone_num == NOWHERE || zone_num > top_of_zone_table) {
#else
  if (zone_num < 0 || zone_num > top_of_zone_table) {
#endif
    log("SYSERR: GenOLC: save_objects: Invalid real zone number %d. (0-%d)", zone_num, top_of_zone_table);
    return FALSE;
  }

  tb_snprintf(filename, sizeof(filename), "%s/%d.new", OBJ_PREFIX, zone_table[zone_num].number);
  if (!(fp = fopen(filename, "w+"))) {
    mudlog(BRF, LVL_IMMORT, TRUE, "SYSERR: OLC: Cannot open objects file %s!", filename);
    return FALSE;
  }
  /* Start running through all objects in this zone. */
  for (counter = genolc_zone_bottom(zone_num); counter <= zone_table[zone_num].top; counter++) {
    if ((realcounter = real_object(counter)) != NOTHING) {
      if ((obj = &obj_proto[realcounter])->action_description) {
        strlcpy(buf, obj->action_description, sizeof(buf));
        strip_cr(buf);
      } else
        *buf = '\0';

      int n = snprintf(buf2, MAX_STRING_LENGTH,
	      "#%d\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n",

	      GET_OBJ_VNUM(obj),
	      (obj->name && *obj->name) ? obj->name : "undefined",
	      (obj->short_description && *obj->short_description) ? obj->short_description : "undefined",
	      (obj->description && *obj->description) ?	obj->description : "undefined",
	      buf);
        
      if (n < 0) {
        mudlog(BRF, LVL_BUILDER, TRUE,
               "SYSERR: Could not format object #%d for saving; zone %d not saved.",
               GET_OBJ_VNUM(obj), zone_table[zone_num].number);
        tb_fclose(fp);
        if (remove(filename) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", filename);
        return FALSE;
      }

      if (n >= MAX_STRING_LENGTH) {
        mudlog(BRF, LVL_BUILDER, TRUE,
               "SYSERR: Could not save object #%d due to size (%d >= maximum of %d); "
               "zone %d not saved.",
               GET_OBJ_VNUM(obj), n, MAX_STRING_LENGTH, zone_table[zone_num].number);
        tb_fclose(fp);
        if (remove(filename) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", filename);
        return FALSE;
      }
      
      tb_fprintf(fp, "%s", convert_from_tabs(buf2));

      sprintascii(ebuf1, GET_OBJ_EXTRA(obj)[0]);
      sprintascii(ebuf2, GET_OBJ_EXTRA(obj)[1]);
      sprintascii(ebuf3, GET_OBJ_EXTRA(obj)[2]);
      sprintascii(ebuf4, GET_OBJ_EXTRA(obj)[3]);
      sprintascii(wbuf1, GET_OBJ_WEAR(obj)[0]);
      sprintascii(wbuf2, GET_OBJ_WEAR(obj)[1]);
      sprintascii(wbuf3, GET_OBJ_WEAR(obj)[2]);
      sprintascii(wbuf4, GET_OBJ_WEAR(obj)[3]);
      sprintascii(pbuf1, GET_OBJ_AFFECT(obj)[0]);
      sprintascii(pbuf2, GET_OBJ_AFFECT(obj)[1]);
      sprintascii(pbuf3, GET_OBJ_AFFECT(obj)[2]);
      sprintascii(pbuf4, GET_OBJ_AFFECT(obj)[3]);

      tb_fprintf(fp, "%d %s %s %s %s %s %s %s %s %s %s %s %s\n"
          "%d %d %d %d\n"
          "%d %d %d %d %d\n",

	  GET_OBJ_TYPE(obj),
          ebuf1, ebuf2, ebuf3, ebuf4,
          wbuf1, wbuf2, wbuf3, wbuf4,
          pbuf1, pbuf2, pbuf3, pbuf4,
          GET_OBJ_VAL(obj, 0), GET_OBJ_VAL(obj, 1),
          GET_OBJ_VAL(obj, 2), GET_OBJ_VAL(obj, 3),
          GET_OBJ_WEIGHT(obj), GET_OBJ_COST(obj),
          GET_OBJ_RENT(obj), GET_OBJ_LEVEL(obj), GET_OBJ_TIMER(obj)
      );

      /* Do we have script(s) attached? */
      script_save_to_disk(fp, obj, OBJ_TRIGGER);

      /* Do we have extra descriptions? */
      if (obj->ex_description) {	/* Yes, save them too. */
	for (ex_desc = obj->ex_description; ex_desc; ex_desc = ex_desc->next) {
	  /* Sanity check to prevent nasty protection faults. */
	  if (!ex_desc->keyword || !ex_desc->description || !*ex_desc->keyword || !*ex_desc->description) {
	    mudlog(BRF, LVL_IMMORT, TRUE, "SYSERR: OLC: oedit_save_to_disk: Corrupt ex_desc!");
	    continue;
	  }
	  strlcpy(buf, ex_desc->description, sizeof(buf));
	  strip_cr(buf);
	  tb_fprintf(fp, "E\n"
		  "%s~\n"
		  "%s~\n", ex_desc->keyword, buf);
	}
      }
      /* Do we have affects? */
      for (counter2 = 0; counter2 < MAX_OBJ_AFFECT; counter2++)
	if (obj->affected[counter2].modifier)
	  tb_fprintf(fp, "A\n"
		  "%d %d\n", obj->affected[counter2].location,
		  obj->affected[counter2].modifier);
    }
  }

  /* Write the final line and commit only a fully flushed temporary file. */
  tb_fprintf(fp, "$~\n");
  {
    int flush_ok = tb_fflush_checked(fp, filename);
    int close_ok = tb_fclose_checked(fp, filename);
    if (!flush_ok || !close_ok) {
      if (remove(filename) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", filename);
      return FALSE;
    }
  }
  tb_snprintf(buf, sizeof(buf), "%s/%d.obj", OBJ_PREFIX, zone_table[zone_num].number);
  if (!tb_replace_file_checked(filename, buf))
    return FALSE;

  if (in_save_list(zone_table[zone_num].number, SL_OBJ))
    remove_from_save_list(zone_table[zone_num].number, SL_OBJ);
  return TRUE;
}

/* Free all, unconditionally. */
void free_object_strings(struct obj_data *obj)
{
  if (obj->name)
    free(obj->name);
  if (obj->description)
    free(obj->description);
  if (obj->short_description)
    free(obj->short_description);
  if (obj->action_description)
    free(obj->action_description);
  if (obj->ex_description)
    free_ex_descriptions(obj->ex_description);
}

/* For object instances that are not the prototype. */
void free_object_strings_proto(struct obj_data *obj)
{
  obj_rnum robj_num = GET_OBJ_RNUM(obj);

  /* The strings are compared against the prototype's to find which ones the
   * instance owns, so the rnum must name a prototype.  free_obj() only calls
   * this with a valid rnum; a NOTHING or stale rnum here means a renumbering
   * bug.  Freeing would risk a double free of strings a prototype still
   * owns, so leak them and report instead. */
  if (robj_num == NOTHING || robj_num > top_of_objt) {
    TB_ERROR_MSG("olc", "obj-rnum-range", "object",
                 "obj uid %ld rnum %d outside 0..%d; strings not freed",
                 obj->script_id, (int)robj_num, (int)top_of_objt);
    return;
  }

  if (obj->name && obj->name != obj_proto[robj_num].name)
    free(obj->name);
  if (obj->description && obj->description != obj_proto[robj_num].description)
    free(obj->description);
  if (obj->short_description && obj->short_description != obj_proto[robj_num].short_description)
    free(obj->short_description);
  if (obj->action_description && obj->action_description != obj_proto[robj_num].action_description)
    free(obj->action_description);
  if (obj->ex_description) {
    struct extra_descr_data *thised, *plist, *next_one; /* O(horrible) */
    int ok_key, ok_desc, ok_item;
    for (thised = obj->ex_description; thised; thised = next_one) {
      next_one = thised->next;
      for (ok_item = ok_key = ok_desc = 1, plist = obj_proto[robj_num].ex_description; plist; plist = plist->next) {
        if (plist->keyword == thised->keyword)
          ok_key = 0;
        if (plist->description == thised->description)
          ok_desc = 0;
        if (plist == thised)
          ok_item = 0;
      }
      if (thised->keyword && ok_key)
        free(thised->keyword);
      if (thised->description && ok_desc)
        free(thised->description);
      if (ok_item)
        free(thised);
    }
  }
}

static void copy_object_strings(struct obj_data *to, struct obj_data *from)
{
  to->name = from->name ? strdup(from->name) : NULL;
  to->description = from->description ? strdup(from->description) : NULL;
  to->short_description = from->short_description ? strdup(from->short_description) : NULL;
  to->action_description = from->action_description ? strdup(from->action_description) : NULL;

  if (from->ex_description)
    copy_ex_descriptions(&to->ex_description, from->ex_description);
  else
    to->ex_description = NULL;
}

int copy_object(struct obj_data *to, struct obj_data *from)
{
  free_object_strings(to);
  return copy_object_main(to, from, TRUE);
}

int copy_object_preserve(struct obj_data *to, struct obj_data *from)
{
  return copy_object_main(to, from, FALSE);
}

int copy_object_main(struct obj_data *to, struct obj_data *from, int free_object)
{
  struct script_data *to_script = to->script;
  struct list_data *to_events = to->events;

  /* copy_object() (free_object TRUE) replaces data `to` owns.  Its
   * proto_script is then dropped unless `from` shares the same list, as
   * when oedit re-saves a prototype whose trigger list it re-attaches.
   * copy_object_preserve() is for slots whose old pointers belong to a
   * neighbour after a table shift, so it frees nothing. */
  if (free_object && to->proto_script && to->proto_script != from->proto_script)
    free_proto_script(to, OBJ_TRIGGER);
  *to = *from;
  copy_object_strings(to, from);
  /* A running script and pending events belong to one object; they are
   * never copied, and `to` keeps its own. */
  to->script = to_script;
  to->events = to_events;
  return TRUE;
}

int delete_object(obj_rnum rnum)
{
  obj_rnum i;
  zone_rnum zrnum, shop_zone;
  struct obj_data *obj, *tmp;
  int shop, j, zone, cmd_no;
  obj_vnum deleted_vnum;

  /* oedit can confirm 'delete' on an object that was never saved, whose
   * rnum is NOTHING. */
  if (rnum == NOTHING || rnum > top_of_objt) {
    TB_WARN_MSG("olc", "obj-delete-range", "object",
                "rnum %d out of range 0..%d; nothing deleted",
                (int)rnum, (int)top_of_objt);
    return NOTHING;
  }

  obj = &obj_proto[rnum];
  /* Captured before the prototype table shifts down over this slot. */
  deleted_vnum = GET_OBJ_VNUM(obj);

  /* A board's messages are bound to its object; boards.c has no way to
   * detach one at run time, so refuse rather than leave the board pointing
   * at whichever prototype slides into this rnum. */
  for (j = 0; j < NUM_OF_BOARDS; j++)
    if (BOARD_RNUM(j) == rnum) {
      TB_ERROR_MSG("olc", "obj-delete-board", "object",
                   "obj vnum %d is the object of board %d; not deleted",
                   deleted_vnum, j);
      return NOTHING;
    }

  zrnum = real_zone_by_thing(GET_OBJ_VNUM(obj));

  /* This is something you might want to read about in the logs. */
  log("GenOLC: delete_object: Deleting object #%d (%s).", GET_OBJ_VNUM(obj), obj->short_description);

  /* Extract every live instance.  extract_obj() would destroy the contents
   * too, so each content first moves to where the container is: its room
   * (IN_ROOM != NOWHERE; rnum 0 is a real room), the character carrying or
   * wearing it, or the container holding it.  extract_obj() can free objects
   * other than tmp (an orphaned content below), and any of them may be the
   * next element of object_list, so the scan restarts from the head after
   * each extraction instead of keeping a saved next pointer. */
  for (tmp = object_list; tmp; ) {
    if (tmp->item_number != rnum) {
      tmp = tmp->next;
      continue;
    }

    while (tmp->contains) {
      struct obj_data *content = tmp->contains;

      obj_from_obj(content);
      if (IN_ROOM(tmp) != NOWHERE)
        obj_to_room(content, IN_ROOM(tmp));
      else if (tmp->carried_by)
        obj_to_char(content, tmp->carried_by);
      else if (tmp->worn_by)
        obj_to_char(content, tmp->worn_by);
      else if (tmp->in_obj)
        obj_to_obj(content, tmp->in_obj);
      else {
        TB_ERROR_MSG("olc", "obj-delete-orphan", "object",
                     "container vnum %d uid %ld has no location; content "
                     "vnum %d uid %ld extracted", deleted_vnum, tmp->script_id,
                     GET_OBJ_VNUM(content), content->script_id);
        extract_obj(content);
      }
    }
    /* Remove from object_list, etc. - handles weightchanges, and similar. */
    extract_obj(tmp);
    tmp = object_list;
  }

  /* Every instance was extracted above, so the live count must be zero.
   * This used to be an assert(), which NDEBUG builds compile out.  A
   * non-zero count means an instance escaped object_list (or the count was
   * corrupted); shifting the table now would leave that instance naming the
   * next prototype, so refuse the delete and keep the prototype. */
  if (obj_index[rnum].number != 0) {
    TB_ERROR_MSG("olc", "obj-delete-instances", "object",
                 "obj vnum %d still counts %d live instances after "
                 "extraction; not deleted", deleted_vnum,
                 obj_index[rnum].number);
    return NOTHING;
  }

  /* The prototype owns its strings, extra descriptions and trigger list
   * (no instance shares them any more), and the slot is overwritten by the
   * shift below, so release them now or they leak. */
  free_object_strings(obj);
  free_proto_script(obj, OBJ_TRIGGER);

  /* Adjust rnums of all other objects.  obj_rnum is unsigned and NOTHING is
   * its maximum, so prototype-less objects (corpses, money) compare greater
   * than any rnum and must be skipped, or they would point at a prototype. */
  for (tmp = object_list; tmp; tmp = tmp->next) {
    if (GET_OBJ_RNUM(tmp) != NOTHING && GET_OBJ_RNUM(tmp) > rnum)
      GET_OBJ_RNUM(tmp)--;
  }

  for (i = rnum; i < top_of_objt; i++) {
    obj_index[i] = obj_index[i + 1];
    obj_proto[i] = obj_proto[i + 1];
    obj_proto[i].item_number = i;
  }

  top_of_objt--;
  RECREATE(obj_index, struct index_data, top_of_objt + 1);
  RECREATE(obj_proto, struct obj_data, top_of_objt + 1);

  /* Renumber notice boards.  No board names rnum (checked above); a board
   * whose object never loaded keeps NOTHING. */
  for (j = 0; j < NUM_OF_BOARDS; j++)
    if (BOARD_RNUM(j) != NOTHING && BOARD_RNUM(j) > rnum)
      BOARD_RNUM(j)--;

  /* Renumber shop produce.  A product equal to rnum would otherwise name
   * the next prototype, or top_of_objt + 1 when the last one was deleted, so
   * it is removed from the shop (the list then slides down over j). */
  for (shop = 0; shop <= top_shop; shop++)
    for (j = 0; SHOP_PRODUCT(shop, j) != NOTHING; ) {
      if (SHOP_PRODUCT(shop, j) != rnum) {
        SHOP_PRODUCT(shop, j) -= (SHOP_PRODUCT(shop, j) > rnum);
        j++;
        continue;
      }
      TB_WARN_MSG("olc", "shop-product-drop", "shop",
                  "shop vnum %d lost product obj vnum %d (deleted)",
                  SHOP_NUM(shop), deleted_vnum);
      remove_shop_from_int_list(&shop_index[shop].producing, j);
      if ((shop_zone = real_zone_by_thing(SHOP_NUM(shop))) != NOWHERE)
        add_to_save_list(zone_table[shop_zone].number, SL_SHP);
    }

  /* Renumber zone table.  A command that names the deleted prototype is
   * removed, which slides the next command into cmd_no and reallocates the
   * list: cmd_no must not advance after a removal, and the removed slot must
   * not be read again (so 'P' never falls through once it is dropped).  Each
   * dropped command is reported, because the builder loses a reset line. */
  for (zone = 0; zone <= top_of_zone_table; zone++) {
    for (cmd_no = 0; ZCMD(zone, cmd_no).command != 'S'; ) {
      struct reset_com *zcmd = &ZCMD(zone, cmd_no);
      bool drop = FALSE;

      switch (zcmd->command) {
      case 'P':
        if (zcmd->arg3 == rnum) {
          drop = TRUE;
          break;
        }
        zcmd->arg3 -= (zcmd->arg3 > rnum);
        /* fall through: arg1 of 'P' is an object rnum as well. */
      case 'O':
      case 'G':
      case 'E':
        if (zcmd->arg1 == rnum)
          drop = TRUE;
        else
          zcmd->arg1 -= (zcmd->arg1 > rnum);
        break;
      case 'R':
        if (zcmd->arg2 == rnum)
          drop = TRUE;
        else
          zcmd->arg2 -= (zcmd->arg2 > rnum);
        break;
      }

      if (!drop) {
        cmd_no++;
        continue;
      }
      TB_WARN_MSG("olc", "zone-cmd-drop", "zone",
                  "zone %d cmd %d '%c' (line %d) dropped: obj vnum %d deleted",
                  zone_table[zone].number, cmd_no, zcmd->command, zcmd->line,
                  deleted_vnum);
      if (!delete_zone_command(&zone_table[zone], cmd_no)) {
        /* Already reported by delete_zone_command(); step past the command
         * so the scan still terminates. */
        TB_ERROR_MSG("olc", "zone-cmd-remove", "zone",
                     "zone %d cmd %d still names deleted obj vnum %d",
                     zone_table[zone].number, cmd_no, deleted_vnum);
        cmd_no++;
        continue;
      }
      add_to_save_list(zone_table[zone].number, SL_ZON);
    }
  }

  save_objects(zrnum);

  return rnum;
}

/* oset handling, this location should be temporary */
bool oset_alias(struct obj_data *obj, char * argument)
{ 
  static size_t max_len = 64;
  int i = GET_OBJ_RNUM(obj);
  
  skip_spaces(&argument);
  
  if (strlen(argument) > max_len)
    return FALSE;
  
  if (i != NOWHERE && obj->name && obj->name != obj_proto[i].name)
    free(obj->name);  
		   	   
  obj->name = strdup(argument);  
  
  return TRUE;
}

bool oset_apply(struct obj_data *obj, char * argument)
{ 
  int i = 0, apply = -1, location = -1, mod = 0, empty = -1, value;
  char arg[MAX_INPUT_LENGTH], *end;
  long parsed;

  argument = one_argument(argument, arg);

  skip_spaces(&argument);

  /* The amount comes from a DG script or the immortal oset command.  Parse
   * it strictly: tb_parse_int_or_zero() cannot tell "0" from "abc". */
  errno = 0;
  parsed = strtol(argument, &end, 10);
  if (end == argument || (*end && !isspace((unsigned char)*end)) ||
      errno == ERANGE || parsed == 0 || parsed < INT_MIN || parsed > INT_MAX) {
    TB_WARN_MSG("dg", "oset-reject", "object",
                "obj vnum %d uid %ld: apply amount is not a non-zero integer "
                "(%zu bytes)", GET_OBJ_VNUM(obj), obj->script_id, strlen(argument));
    return FALSE;
  }
  value = (int)parsed;

  while (*apply_types[i] != '\n') {
    if (is_abbrev(apply_types[i], arg)) {
      apply = i;
      break;
    }
    i++;
  }

  if (apply == -1) {
    TB_WARN_MSG("dg", "oset-reject", "object",
                "obj vnum %d uid %ld: unknown apply name (%zu bytes)",
                GET_OBJ_VNUM(obj), obj->script_id, strlen(arg));
    return FALSE;
  }

  for (i = 0; i < MAX_OBJ_AFFECT; i++) {
    if (obj->affected[i].location == apply) {
      location = i;
      mod = (int)obj->affected[i].modifier;
      break;
    } else if (obj->affected[i].location == APPLY_NONE && empty == -1) {
      empty = i;
    }
  }
  
  /* No slot already using APPLY_XXX, so use an empty one... if available */
  if (location == -1)
    location = empty;  
  
  /* There is no slot already using our APPLY_XXX type, and no empty slots either */
  if (location == -1) {
    TB_WARN_MSG("dg", "oset-reject", "object",
                "obj vnum %d uid %ld: apply %s: all %d affect slots in use",
                GET_OBJ_VNUM(obj), obj->script_id, apply_types[apply], MAX_OBJ_AFFECT);
    return FALSE;
  }

  /* modifier is an sbyte: a sum outside its range would wrap (+200 became
   * -56).  value is bounded by INT_MAX and mod by sbyte, so compare in long. */
  if ((long)mod + value < SCHAR_MIN || (long)mod + value > SCHAR_MAX) {
    TB_WARN_MSG("dg", "oset-reject", "object",
                "obj vnum %d uid %ld: apply %s %ld out of range %d..%d",
                GET_OBJ_VNUM(obj), obj->script_id, apply_types[apply],
                (long)mod + value, SCHAR_MIN, SCHAR_MAX);
    return FALSE;
  }
  obj->affected[location].modifier = (sbyte)(mod + value);
  
  /* Our modifier is set at 0, so lets just clear the apply location so that it may
   * be reused at a later point */
  if (obj->affected[location].modifier != 0)
    obj->affected[location].location = apply;
  else
    obj->affected[location].location = APPLY_NONE;
  
  return TRUE;
}

bool oset_short_description(struct obj_data *obj, char * argument)
{ 
  static size_t max_len = 64;
  int i = GET_OBJ_RNUM(obj);
  
  skip_spaces(&argument);
  
  if (strlen(argument) > max_len)
    return FALSE;
  
  if (i != NOWHERE && obj->short_description && obj->short_description != obj_proto[i].short_description)
    free(obj->short_description);  
		   	   
  obj->short_description = strdup(argument);  
  
  return TRUE;
}

bool oset_long_description(struct obj_data *obj, char * argument)
{
  static size_t max_len = 128;
  int i = GET_OBJ_RNUM(obj);  
  
  skip_spaces(&argument);
  
  if (strlen(argument) > max_len)
    return FALSE;  
  
  if (i != NOWHERE && obj->description && obj->description != obj_proto[i].description)
    free(obj->description);  
		   	   
  obj->description = strdup(argument);  
  
  return TRUE;
}
