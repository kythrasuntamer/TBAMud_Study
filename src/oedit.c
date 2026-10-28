/**************************************************************************
*  File: oedit.c                                           Part of tbaMUD *
*  Usage: Oasis OLC - Objects.                                            *
*                                                                         *
* By Levork. Copyright 1996 Harvey Gilpin. 1997-2001 George Greer.        *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "spells.h"
#include "db.h"
#include "boards.h"
#include "constants.h"
#include "shop.h"
#include "genolc.h"
#include "genobj.h"
#include "genzon.h"
#include "handler.h"
#include "oasis.h"
#include "improved-edit.h"
#include "dg_olc.h"
#include "fight.h"
#include "modify.h"
#include "olc_checks.h"

/* local functions */


/* iedit only copies string fields back into the live instance in this MVP. */
#define IEDIT_STRING_NAME   0
#define IEDIT_STRING_LONG   1
#define IEDIT_STRING_SHORT  2
#define IEDIT_STRING_ACTION 3

/* Forward declarations needed because iedit is inserted before oedit's normal static prototypes. */
static void oedit_disp_menu(struct descriptor_data *d);
static int iedit_is_prototype_string(struct obj_data *obj, char *str, int field_id);
static void iedit_replace_string(struct obj_data *obj, char **field, char *new_string, int field_id);
static int iedit_save_internally(struct descriptor_data *d);
static int iedit_is_forbidden_main_menu_choice(char *arg);

ACMD(do_oasis_iedit)
{
  struct descriptor_data *d;
  struct obj_data *obj = NULL;
  struct char_data *vict = NULL;
  char arg[MAX_INPUT_LENGTH];

  one_argument(argument, arg);

  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)
    return;

  if (!*arg) {
    send_to_char(ch, "Edit which object instance?\r\n");
    return;
  }

  generic_find(arg, FIND_OBJ_EQUIP | FIND_OBJ_INV | FIND_OBJ_ROOM,
               ch, &vict, &obj);

  if (!obj) {
    send_to_char(ch, "You don't see that object here.\r\n");
    return;
  }

  d = ch->desc;

  if (d->olc) {
    mudlog(BRF, LVL_IMMORT, TRUE,
           "SYSERR: do_oasis_iedit: Player already had an OLC structure.");
    cleanup_olc(d, CLEANUP_ALL);
  }

  CREATE(d->olc, struct oasis_olc_data, 1);

  /* The live object can be extracted (and its memory reused) by any player,
   * script or zone reset while the builder edits. Keep its UID: save
   * re-validates the pointer against the UID table before touching it. */
  OLC_IEDIT_OBJ(d) = obj;
  OLC_IEDIT_UID(d) = obj_script_id(obj);
  OLC_NUM(d) = GET_OBJ_VNUM(obj);
  OLC_ZNUM(d) = NOWHERE;

  CREATE(OLC_OBJ(d), struct obj_data, 1);
  clear_object(OLC_OBJ(d));
  copy_object(OLC_OBJ(d), obj);

  /* The OLC copy is only a scratch object. Do not share scripts. */
  SCRIPT(OLC_OBJ(d)) = NULL;
  OLC_OBJ(d)->proto_script = OLC_SCRIPT(d) = NULL;
  OLC_VAL(d) = 0;
  OLC_ITEM_TYPE(d) = OBJ_TRIGGER;

  oedit_disp_menu(d);
  STATE(d) = CON_IEDIT;

  act("$n starts using OLC.", TRUE, d->character, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);

  mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
         "OLC: %s starts iediting %s",
         GET_NAME(ch), obj->short_description ? obj->short_description : "an object");
}

static int iedit_is_prototype_string(struct obj_data *obj, char *str, int field)
{
  obj_rnum rnum;

  if (!obj || !str || GET_OBJ_RNUM(obj) == NOTHING)
    return FALSE;

  rnum = GET_OBJ_RNUM(obj);

  switch (field) {
  case IEDIT_STRING_NAME:
    return str == obj_proto[rnum].name;
  case IEDIT_STRING_LONG:
    return str == obj_proto[rnum].description;
  case IEDIT_STRING_SHORT:
    return str == obj_proto[rnum].short_description;
  case IEDIT_STRING_ACTION:
    return str == obj_proto[rnum].action_description;
  default:
    return FALSE;
  }
}

static void iedit_replace_string(struct obj_data *obj, char **field,
                                 char *new_string, int field_id)
{
  if (*field && !iedit_is_prototype_string(obj, *field, field_id))
    free(*field);

  *field = new_string ? strdup(new_string) : NULL;
}

static int iedit_is_prototype_ex_description(struct obj_data *obj)
{
  obj_rnum rnum;

  if (!obj || !obj->ex_description || GET_OBJ_RNUM(obj) == NOTHING)
    return FALSE;

  rnum = GET_OBJ_RNUM(obj);

  return obj->ex_description == obj_proto[rnum].ex_description;
}

static void iedit_replace_ex_descriptions(struct obj_data *target, struct obj_data *edited)
{
  if (!target || !edited)
    return;

  if (target->ex_description && !iedit_is_prototype_ex_description(target))
    free_ex_descriptions(target->ex_description);

  target->ex_description = NULL;

  if (edited->ex_description)
    copy_ex_descriptions(&target->ex_description, edited->ex_description);
}

static void iedit_replace_values(struct obj_data *target, struct obj_data *edited)
{
  int i;

  if (!target || !edited)
    return;

  /* Values are interpreted by the object type, which iedit copies first
   * (menu '5' may change it), so they are always copied together. */
  for (i = 0; i < NUM_OBJ_VAL_POSITIONS; i++)
    GET_OBJ_VAL(target, i) = GET_OBJ_VAL(edited, i);
}

/* Copy the scratch edits back into the live object. Returns FALSE, after
 * reporting, when the target is gone.
 *
 * Invariant: OLC_IEDIT_OBJ(d) may be followed only while OLC_IEDIT_UID(d)
 * still maps to that address. extract_obj() unregisters the UID before the
 * object is freed, and UIDs are not reused while registered, so
 * obj_uid_matches() rejects both a freed target and a new object that malloc
 * placed at the same address, without dereferencing the pointer. */
static int iedit_save_internally(struct descriptor_data *d)
{
  struct obj_data *target = OLC_IEDIT_OBJ(d);
  struct obj_data *edited = OLC_OBJ(d);
  struct obj_data *top_obj, *container;
  struct char_data *owner = NULL, *wearer, *carrier;
  struct char_data *builder = d->character;
  int wear_pos;

  if (!edited || !obj_uid_matches(OLC_IEDIT_UID(d), target)) {
    TB_ERROR_MSG("olc", "iedit-target-gone", "object",
                 "builder " DIAG_CHAR_FMT " obj uid %ld vnum %d extracted during iedit; edits discarded",
                 DIAG_CHAR(builder), OLC_IEDIT_UID(d), OLC_NUM(d));
    write_to_output(d, "The object you were editing no longer exists; nothing was saved.\r\n");
    OLC_IEDIT_OBJ(d) = NULL;
    return FALSE;
  }

  /* Invariant: carry weight, container weight, character affects and room
   * light were accounted for from the object's current type, weight,
   * affected[] and AFF bits when it was equipped, picked up or put in a
   * container. Writing new values in place would leave that accounting
   * wrong (a +10 HIT ring edited to +50 removes 50 on unequip). Take the
   * object out through the handler first and put it back afterwards, so
   * the handler subtracts the old values and adds the new ones. */
  wearer = target->worn_by;
  wear_pos = target->worn_on;
  carrier = target->carried_by;
  container = target->in_obj;
  if (wearer)
    unequip_char(wearer, wear_pos);
  else if (carrier)
    obj_from_char(target);
  else if (container)
    obj_from_obj(target);

  iedit_replace_string(target, &target->name, edited->name, IEDIT_STRING_NAME);
  iedit_replace_string(target, &target->short_description, edited->short_description, IEDIT_STRING_SHORT);
  iedit_replace_string(target, &target->description, edited->description, IEDIT_STRING_LONG);
  iedit_replace_string(target, &target->action_description, edited->action_description, IEDIT_STRING_ACTION);
  iedit_replace_ex_descriptions(target, edited);

  /* Instance-only changes: never write to obj_proto or mutate the prototype. */
  GET_OBJ_TYPE(target) = GET_OBJ_TYPE(edited);
  GET_OBJ_WEIGHT(target) = GET_OBJ_WEIGHT(edited);
  GET_OBJ_COST(target) = GET_OBJ_COST(edited);
  GET_OBJ_RENT(target) = GET_OBJ_RENT(edited);
  GET_OBJ_TIMER(target) = GET_OBJ_TIMER(edited);
  GET_OBJ_LEVEL(target) = GET_OBJ_LEVEL(edited);
  {
    int i;
    for (i = 0; i < EF_ARRAY_MAX; i++)
      GET_OBJ_EXTRA(target)[i] = GET_OBJ_EXTRA(edited)[i];
    for (i = 0; i < TW_ARRAY_MAX; i++)
      GET_OBJ_WEAR(target)[i] = GET_OBJ_WEAR(edited)[i];
    for (i = 0; i < AF_ARRAY_MAX; i++)
      GET_OBJ_AFFECT(target)[i] = GET_OBJ_AFFECT(edited)[i];
    for (i = 0; i < MAX_OBJ_AFFECT; i++)
      target->affected[i] = edited->affected[i];
  }
  iedit_replace_values(target, edited);

  /* Re-insert with the new values (equip_char may refuse a now-invalid
   * item and leave it in the wearer's inventory, with its usual message). */
  if (wearer)
    equip_char(wearer, target, wear_pos);
  else if (carrier)
    obj_to_char(target, carrier);
  else if (container)
    obj_to_obj(target, container);

  /* If the object, or its top-level container, belongs to a player, make sure
   * the crash/rent save code notices the change. */
  for (top_obj = target; top_obj->in_obj; top_obj = top_obj->in_obj)
    ;

  if (top_obj->carried_by)
    owner = top_obj->carried_by;
  else if (top_obj->worn_by)
    owner = top_obj->worn_by;

  if (owner && !IS_NPC(owner))
    SET_BIT_AR(PLR_FLAGS(owner), PLR_CRASH);

  return TRUE;
}

static int iedit_is_forbidden_main_menu_choice(char *arg)
{
  switch (*arg) {
  case 's': case 'S': /* Scripts need separate instance ownership handling. */
  case 'w': case 'W': /* Copying prototypes cannot be applied to live objects. */
  case 'x': case 'X': /* Deletion needs an explicit live-object lifecycle path. */
    return TRUE;
  default:
    return FALSE;
  }
}

static void oedit_setup_new(struct descriptor_data *d);
static void oedit_disp_container_flags_menu(struct descriptor_data *d);
static void oedit_disp_extradesc_menu(struct descriptor_data *d);
static void oedit_disp_prompt_apply_menu(struct descriptor_data *d);
static void oedit_liquid_type(struct descriptor_data *d);
static void oedit_disp_apply_menu(struct descriptor_data *d);
static void oedit_disp_weapon_menu(struct descriptor_data *d);
static void oedit_disp_spells_menu(struct descriptor_data *d);
static void oedit_disp_val1_menu(struct descriptor_data *d);
static void oedit_disp_val2_menu(struct descriptor_data *d);
static void oedit_disp_val3_menu(struct descriptor_data *d);
static void oedit_disp_val4_menu(struct descriptor_data *d);
static void oedit_disp_type_menu(struct descriptor_data *d);
static void oedit_disp_extra_menu(struct descriptor_data *d);
static void oedit_disp_wear_menu(struct descriptor_data *d);
static void oedit_disp_menu(struct descriptor_data *d);
static void oedit_disp_perm_menu(struct descriptor_data *d);
static void oedit_save_to_disk(int zone_num);
static int iedit_is_prototype_string(struct obj_data *obj, char *str, int field);
static int iedit_is_prototype_ex_description(struct obj_data *obj);
static void iedit_replace_string(struct obj_data *obj, char **field, char *new_string, int field_id);
static void iedit_replace_ex_descriptions(struct obj_data *target, struct obj_data *edited);
static void iedit_replace_values(struct obj_data *target, struct obj_data *edited);
static int iedit_save_internally(struct descriptor_data *d);
static int iedit_is_forbidden_main_menu_choice(char *arg);


/* Utility and exported functions */
ACMD(do_oasis_oedit)
{
  int number = NOWHERE, save = 0, real_num;
  struct descriptor_data *d;
  char buf1[MAX_STRING_LENGTH];
  char buf2[MAX_STRING_LENGTH];

  /* No building as a mob or while being forced. */
  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)
    return;

  /* Parse any arguments. */
  two_arguments(argument, buf1, buf2);

  /* If there aren't any arguments they can't modify anything. */
  if (!*buf1) {
    send_to_char(ch, "Specify an object VNUM to edit.\r\n");
    return;
  } else if (!isdigit(*buf1)) {
    if (str_cmp("save", buf1) != 0) {
      send_to_char(ch, "Yikes!  Stop that, someone will get hurt!\r\n");
      return;
    }

    save = TRUE;

    if (is_number(buf2))
      number = tb_parse_int_or_zero(buf2);
    else if (GET_OLC_ZONE(ch) > 0) {
      zone_rnum zlok;

      if ((zlok = real_zone(GET_OLC_ZONE(ch))) == NOWHERE)
        number = NOWHERE;
      else
        number = genolc_zone_bottom(zlok);
    }

    if (number == NOWHERE) {
      send_to_char(ch, "Save which zone?\r\n");
      return;
    }
  }

  /* If a numeric argument was given, get it. */
  if (number == NOWHERE)
    number = tb_parse_int_or_zero(buf1);

  if (number < IDXTYPE_MIN || number > IDXTYPE_MAX) {
    send_to_char(ch, "That object VNUM can't exist.\r\n");
    return;
  }

  /* Check that whatever it is isn't already being edited. */
  for (d = descriptor_list; d; d = d->next) {
    if (STATE(d) == CON_OEDIT) {
      if (d->olc && OLC_NUM(d) == number) {
        send_to_char(ch, "That object is currently being edited by %s.\r\n",
          PERS(d->character, ch));
        return;
      }
    }
  }

  /* Point d to the builder's descriptor (for easier typing later). */
  d = ch->desc;

  /* Give the descriptor an OLC structure. */
  if (d->olc) {
    mudlog(BRF, LVL_IMMORT, TRUE,
      "SYSERR: do_oasis: Player already had olc structure.");
    free(d->olc);
  }

  CREATE(d->olc, struct oasis_olc_data, 1);

  /* Find the zone. */
  OLC_ZNUM(d) = save ? real_zone(number) : real_zone_by_thing(number);
  if (OLC_ZNUM(d) == NOWHERE) {
    send_to_char(ch, "Sorry, there is no zone for that number!\r\n");

    /* Free the descriptor's OLC structure. */
    free(d->olc);
    d->olc = NULL;
    return;
  }

  /* Everyone but IMPLs can only edit zones they have been assigned. */
  if (!can_edit_zone(ch, OLC_ZNUM(d))) {
    send_cannot_edit(ch, zone_table[OLC_ZNUM(d)].number);
    /* Free the OLC structure. */
    free(d->olc);
    d->olc = NULL;
    return;
  }

  /* If we need to save, save the objects. */
  if (save) {
    send_to_char(ch, "Saving all objects in zone %d.\r\n",
      zone_table[OLC_ZNUM(d)].number);
    mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE,
      "OLC: %s saves object info for zone %d.", GET_NAME(ch),
      zone_table[OLC_ZNUM(d)].number);

    /* Save the objects in this zone. */
    save_objects(OLC_ZNUM(d));

    /* Free the descriptor's OLC structure. */
    free(d->olc);
    d->olc = NULL;
    return;
  }

  OLC_NUM(d) = number;

  /* If a new object, setup new, otherwise setup the existing object. */
  if ((real_num = real_object(number)) != NOTHING)
    oedit_setup_existing(d, real_num);
  else
    oedit_setup_new(d);

  oedit_disp_menu(d);
  STATE(d) = CON_OEDIT;

  /* Send the OLC message to the players in the same room as the builder. */
  act("$n starts using OLC.", TRUE, d->character, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);

  /* Log the OLC message. */
  mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "OLC: %s starts editing zone %d allowed zone %d",
    GET_NAME(ch), zone_table[OLC_ZNUM(d)].number, GET_OLC_ZONE(ch));
}

static void oedit_setup_new(struct descriptor_data *d)
{
  CREATE(OLC_OBJ(d), struct obj_data, 1);

  clear_object(OLC_OBJ(d));
  OLC_OBJ(d)->name = strdup("unfinished object");
  OLC_OBJ(d)->description = strdup("An unfinished object is lying here.");
  OLC_OBJ(d)->short_description = strdup("an unfinished object");
  SET_BIT_AR(GET_OBJ_WEAR(OLC_OBJ(d)), ITEM_WEAR_TAKE);
  OLC_VAL(d) = 0;
  OLC_ITEM_TYPE(d) = OBJ_TRIGGER;

  SCRIPT(OLC_OBJ(d)) = NULL;
  OLC_OBJ(d)->proto_script = OLC_SCRIPT(d) = NULL;
}

void oedit_setup_existing(struct descriptor_data *d, int real_num)
{
  struct obj_data *obj;

  /* Allocate object in memory. */
  CREATE(obj, struct obj_data, 1);
  copy_object(obj, &obj_proto[real_num]);

  /* Attach new object to player's descriptor. */
  OLC_OBJ(d) = obj;
  OLC_VAL(d) = 0;
  OLC_ITEM_TYPE(d) = OBJ_TRIGGER;
  dg_olc_script_copy(d);
  /* The edited obj must not have a script. It will be assigned to the updated
   * obj later, after editing. */
  SCRIPT(obj) = NULL;
  OLC_OBJ(d)->proto_script = NULL;
}

static int oedit_save_checked(struct descriptor_data *d)
{
  int i;
  obj_rnum robj_num, existing;
  struct obj_data *obj;
  struct trig_proto_list *old_proto;

  existing = real_object(OLC_NUM(d));
  i = (existing == NOTHING);
  /* add_object() struct-copies the scratch object (proto_script NULL, see
   * oedit_setup_existing) over the prototype, so after it returns the old
   * trigger list is reachable only through this pointer. */
  old_proto = i ? NULL : obj_proto[existing].proto_script;

  /* add_object() -> update_all_objects() struct-copies the prototype over
   * every live instance and keeps only location and list fields, so each
   * instance's script (triggers, globals, pending waits) and its private
   * proto_script list would be overwritten with the prototype's NULLs and
   * leaked, leaving its triggers orphaned on trigger_list. Tear them down
   * properly first; the refresh loop below rebuilds them from the new
   * prototype. */
  if (!i)
    for (obj = object_list; obj; obj = obj->next) {
      if (obj->item_number != existing)
        continue;
      if (SCRIPT(obj))
        extract_script(obj, OBJ_TRIGGER);
      free_proto_script(obj, OBJ_TRIGGER);
    }

  if ((robj_num = add_object(OLC_OBJ(d), OLC_NUM(d))) == NOTHING) {
    struct char_data *builder = d->character;
    TB_ERROR_MSG("olc", "oedit-save", "object",
                 "builder " DIAG_CHAR_FMT " obj vnum %d: add_object returned NOTHING; edits not saved",
                 DIAG_CHAR(builder), OLC_NUM(d));
    return FALSE;
  }

  /* Update triggers and free the old proto list. */
  if (old_proto && old_proto != OLC_SCRIPT(d)) {
    obj_proto[robj_num].proto_script = old_proto;
    free_proto_script(&obj_proto[robj_num], OBJ_TRIGGER);
  }
  /* this will handle new instances of the object. The prototype now owns
   * the list; clear the session's handle so cleanup_olc() does not free it. */
  obj_proto[robj_num].proto_script = OLC_SCRIPT(d);
  OLC_SCRIPT(d) = NULL;

  /* this takes care of the objects currently in-game */
  for (obj = object_list; obj; obj = obj->next) {
    if (obj->item_number != robj_num)
      continue;
    /* Invariant: the teardown above released every instance's script, and
     * a prototype never carries one, so anything here was struct-copied
     * from the prototype and is shared. Detach it without freeing. */
    if (SCRIPT(obj) || obj->proto_script) {
      TB_ERROR_MSG("olc", "oedit-script-refresh", "object",
                   "obj vnum %d instance uid %ld still holds %s%s after add_object; detached unfreed",
                   OLC_NUM(d), obj->script_id,
                   SCRIPT(obj) ? "a script " : "",
                   obj->proto_script ? "a proto_script" : "");
      SCRIPT(obj) = NULL;
      obj->proto_script = NULL;
    }
    copy_proto_script(&obj_proto[robj_num], obj, OBJ_TRIGGER);
    assign_triggers(obj, OBJ_TRIGGER);
  }
  /* end trigger update */

  if (!i)	/* If it's not a new object, don't renumber. */
    return TRUE;

  /* insert_object() shifted obj_proto up from robj_num. Move every other
   * open session's object rnums with it: zedit commands, sedit products
   * and the scratch identity of other oedit/iedit sessions (a stale one
   * would make their delete or save act on a neighbouring object). */
  olc_renumber_sessions(OLC_RNUM_OBJ, robj_num, +1, d);
  return TRUE;
}

/* Save the scratch object into obj_proto[]; FALSE (reported) when nothing was saved.
 * Entry point for oasis_copy.c's copy table, which has no failure path;
 * the failure was already reported by oedit_save_checked(). */
void oedit_save_internally(struct descriptor_data *d)
{
  (void)oedit_save_checked(d);
}

static void oedit_save_to_disk(int zone_num)
{
  save_objects(zone_num);
}

/* Menu functions */
/* For container flags. */
static void oedit_disp_container_flags_menu(struct descriptor_data *d)
{
  char bits[MAX_STRING_LENGTH];
  get_char_colors(d->character);
  clear_screen(d);

  sprintbit(GET_OBJ_VAL(OLC_OBJ(d), 1), container_bits, bits, sizeof(bits));
  write_to_output(d,
	  "%s1%s) CLOSEABLE\r\n"
	  "%s2%s) PICKPROOF\r\n"
	  "%s3%s) CLOSED\r\n"
	  "%s4%s) LOCKED\r\n"
	  "Container flags: %s%s%s\r\n"
	  "Enter flag, 0 to quit : ",
	  grn, nrm, grn, nrm, grn, nrm, grn, nrm, cyn, bits, nrm);
}

/* For extra descriptions. */
static void oedit_disp_extradesc_menu(struct descriptor_data *d)
{
  struct extra_descr_data *extra_desc = OLC_DESC(d);

  get_char_colors(d->character);
  clear_screen(d);
  write_to_output(d,
	  "Extra desc menu\r\n"
	  "%s1%s) Keywords: %s%s\r\n"
	  "%s2%s) Description:\r\n%s%s\r\n"
	  "%s3%s) Goto next description: %s\r\n"
	  "%s0%s) Quit\r\n"
	  "Enter choice : ",

     	  grn, nrm, yel, (extra_desc->keyword && *extra_desc->keyword) ? extra_desc->keyword : "<NONE>",
	  grn, nrm, yel, (extra_desc->description && *extra_desc->description) ? extra_desc->description : "<NONE>",
	  grn, nrm, !extra_desc->next ? "Not set." : "Set.", grn, nrm);
  OLC_MODE(d) = OEDIT_EXTRADESC_MENU;
}

/* Ask for *which* apply to edit. */
static void oedit_disp_prompt_apply_menu(struct descriptor_data *d)
{
  char apply_buf[MAX_STRING_LENGTH];
  int counter;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 0; counter < MAX_OBJ_AFFECT; counter++) {
    if (OLC_OBJ(d)->affected[counter].modifier) {
      sprinttype(OLC_OBJ(d)->affected[counter].location, apply_types, apply_buf, sizeof(apply_buf));
      write_to_output(d, " %s%d%s) %+d to %s\r\n", grn, counter + 1, nrm,
	      OLC_OBJ(d)->affected[counter].modifier, apply_buf);
    } else {
      write_to_output(d, " %s%d%s) None.\r\n", grn, counter + 1, nrm);
    }
  }
  write_to_output(d, "\r\nEnter affection to modify (0 to quit) : ");
  OLC_MODE(d) = OEDIT_PROMPT_APPLY;
}

/* Ask for liquid type. */
static void oedit_liquid_type(struct descriptor_data *d)
{
  get_char_colors(d->character);
  clear_screen(d);
  column_list(d->character, 0, drinks, NUM_LIQ_TYPES, TRUE);
  write_to_output(d, "\r\n%sEnter drink type : ", nrm);
  OLC_MODE(d) = OEDIT_VALUE_3;
}

/* The actual apply to set. */
static void oedit_disp_apply_menu(struct descriptor_data *d)
{
  get_char_colors(d->character);
  clear_screen(d);
  column_list(d->character, 0, apply_types, NUM_APPLIES, TRUE);
  write_to_output(d, "\r\nEnter apply type (0 is no apply) : ");
  OLC_MODE(d) = OEDIT_APPLY;
}

/* Weapon type. */
static void oedit_disp_weapon_menu(struct descriptor_data *d)
{
  int counter, columns = 0;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 0; counter < NUM_ATTACK_TYPES; counter++) {
    write_to_output(d, "%s%2d%s) %-20.20s %s", grn, counter, nrm,
		attack_hit_text[counter].singular,
		!(++columns % 2) ? "\r\n" : "");
  }
  write_to_output(d, "\r\nEnter weapon type : ");
}

/* Spell type. */
static void oedit_disp_spells_menu(struct descriptor_data *d)
{
  int counter, columns = 0;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 1; counter <= NUM_SPELLS; counter++) {
    write_to_output(d, "%s%2d%s) %s%-20.20s %s", grn, counter, nrm, yel,
		spell_info[counter].name, !(++columns % 3) ? "\r\n" : "");
  }
  write_to_output(d, "\r\n%sEnter spell choice (-1 for none) : ", nrm);
}

/* Object value #1 */
static void oedit_disp_val1_menu(struct descriptor_data *d)
{
  OLC_MODE(d) = OEDIT_VALUE_1;
  switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
  case ITEM_LIGHT:
    /* values 0 and 1 are unused.. jump to 2 */
    oedit_disp_val3_menu(d);
    break;
  case ITEM_SCROLL:
  case ITEM_WAND:
  case ITEM_STAFF:
  case ITEM_POTION:
    write_to_output(d, "Spell level : ");
    break;
  case ITEM_WEAPON:
    /* This doesn't seem to be used if I remember right. */
    write_to_output(d, "Modifier to Hitroll : ");
    break;
  case ITEM_ARMOR:
    write_to_output(d, "Apply to AC : ");
    break;
  case ITEM_CONTAINER:
    write_to_output(d, "Max weight to contain (-1 for unlimited) : ");
    break;
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
    write_to_output(d, "Max drink units (-1 for unlimited) : ");
    break;
  case ITEM_FOOD:
    write_to_output(d, "Hours to fill stomach : ");
    break;
  case ITEM_MONEY:
    write_to_output(d, "Number of gold coins : ");
    break;
  case ITEM_FURNITURE:
    write_to_output(d, "Number of people it can hold : ");
    break;
  case ITEM_NOTE:  // These object types have no 'values' so go back to menu
  case ITEM_OTHER:
  case ITEM_WORN:
  case ITEM_TREASURE:
  case ITEM_TRASH:
  case ITEM_KEY:
  case ITEM_PEN:
  case ITEM_BOAT:
  case ITEM_FREE:   /* Not implemented, but should be handled here */
  case ITEM_FREE2:  /* Not implemented, but should be handled here */
    oedit_disp_menu(d);
    break;
  default:
    mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: OLC: Reached default case in oedit_disp_val1_menu()!");
    break;
  }
}

/* Object value #2 */
static void oedit_disp_val2_menu(struct descriptor_data *d)
{
  OLC_MODE(d) = OEDIT_VALUE_2;
  switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
  case ITEM_SCROLL:
  case ITEM_POTION:
    oedit_disp_spells_menu(d);
    break;
  case ITEM_WAND:
  case ITEM_STAFF:
    write_to_output(d, "Max number of charges : ");
    break;
  case ITEM_WEAPON:
    write_to_output(d, "Number of damage dice : ");
    break;
  case ITEM_FOOD:
    /* Values 2 and 3 are unused, jump to 4...Odd. */
    oedit_disp_val4_menu(d);
    break;
  case ITEM_CONTAINER:
    /* These are flags, needs a bit of special handling. */
    oedit_disp_container_flags_menu(d);
    break;
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
    write_to_output(d, "Initial drink units : ");
    break;
  default:
    oedit_disp_menu(d);
  }
}

/* Object value #3 */
static void oedit_disp_val3_menu(struct descriptor_data *d)
{
  OLC_MODE(d) = OEDIT_VALUE_3;
  switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
  case ITEM_LIGHT:
    write_to_output(d, "Number of hours (0 = burnt, -1 is infinite) : ");
    break;
  case ITEM_SCROLL:
  case ITEM_POTION:
    oedit_disp_spells_menu(d);
    break;
  case ITEM_WAND:
  case ITEM_STAFF:
    write_to_output(d, "Number of charges remaining : ");
    break;
  case ITEM_WEAPON:
    write_to_output(d, "Size of damage dice : ");
    break;
  case ITEM_CONTAINER:
    write_to_output(d, "Vnum of key to open container (-1 for no key) : ");
    break;
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
    oedit_liquid_type(d);
    break;
  default:
    oedit_disp_menu(d);
  }
}

/* Object value #4 */
static void oedit_disp_val4_menu(struct descriptor_data *d)
{
  OLC_MODE(d) = OEDIT_VALUE_4;
  switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
  case ITEM_SCROLL:
  case ITEM_POTION:
  case ITEM_WAND:
  case ITEM_STAFF:
    oedit_disp_spells_menu(d);
    break;
  case ITEM_WEAPON:
    oedit_disp_weapon_menu(d);
    break;
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
  case ITEM_FOOD:
    write_to_output(d, "Poisoned (0 = not poison) : ");
    break;
  default:
    oedit_disp_menu(d);
  }
}

/* Object type. */
static void oedit_disp_type_menu(struct descriptor_data *d)
{
  int counter, columns = 0;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 0; counter < NUM_ITEM_TYPES; counter++) {
    write_to_output(d, "%s%2d%s) %-20.20s %s", grn, counter, nrm,
		item_types[counter], !(++columns % 2) ? "\r\n" : "");
  }
  write_to_output(d, "\r\nEnter object type : ");
}

/* Object extra flags. */
static void oedit_disp_extra_menu(struct descriptor_data *d)
{
  char bits[MAX_STRING_LENGTH];
  int counter, columns = 0;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 0; counter < NUM_ITEM_FLAGS; counter++) {
    write_to_output(d, "%s%2d%s) %-20.20s %s", grn, counter + 1, nrm,
		extra_bits[counter], !(++columns % 2) ? "\r\n" : "");
  }
  sprintbitarray(GET_OBJ_EXTRA(OLC_OBJ(d)), extra_bits, EF_ARRAY_MAX, bits, sizeof(bits));
  write_to_output(d, "\r\nObject flags: %s%s%s\r\n"
	  "Enter object extra flag (0 to quit) : ",
	  cyn, bits, nrm);
}

/* Object perm flags. */
static void oedit_disp_perm_menu(struct descriptor_data *d)
{
  char bits[MAX_STRING_LENGTH];
  int counter, columns = 0;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 1; counter < NUM_AFF_FLAGS; counter++) {
    write_to_output(d, "%s%2d%s) %-20.20s %s", grn, counter, nrm, affected_bits[counter], !(++columns % 2) ? "\r\n" : "");
  }
  sprintbitarray(GET_OBJ_AFFECT(OLC_OBJ(d)), affected_bits, EF_ARRAY_MAX, bits, sizeof(bits));
  write_to_output(d, "\r\nObject permanent flags: %s%s%s\r\n"
          "Enter object perm flag (0 to quit) : ", cyn, bits, nrm);
}

/* Object wear flags. */
static void oedit_disp_wear_menu(struct descriptor_data *d)
{
  char bits[MAX_STRING_LENGTH];
  int counter, columns = 0;

  get_char_colors(d->character);
  clear_screen(d);

  for (counter = 0; counter < NUM_ITEM_WEARS; counter++) {
    write_to_output(d, "%s%2d%s) %-20.20s %s", grn, counter + 1, nrm,
		wear_bits[counter], !(++columns % 2) ? "\r\n" : "");
  }
  sprintbitarray(GET_OBJ_WEAR(OLC_OBJ(d)), wear_bits, TW_ARRAY_MAX, bits, sizeof(bits));
  write_to_output(d, "\r\nWear flags: %s%s%s\r\n"
	  "Enter wear flag, 0 to quit : ", cyn, bits, nrm);
}

/* Display main menu. */
static void oedit_disp_menu(struct descriptor_data *d)
{
  char buf1[MAX_STRING_LENGTH];
  char buf2[MAX_STRING_LENGTH];
  struct obj_data *obj;

  obj = OLC_OBJ(d);
  get_char_colors(d->character);
  clear_screen(d);

  /* Build buffers for first part of menu. */
  sprinttype(GET_OBJ_TYPE(obj), item_types, buf1, sizeof(buf1));
  sprintbitarray(GET_OBJ_EXTRA(obj), extra_bits, EF_ARRAY_MAX, buf2, sizeof(buf2));

  /* Build first half of menu. */
  write_to_output(d,
	  "-- Item number : [%s%d%s]\r\n"
	  "%s1%s) Keywords : %s%s\r\n"
	  "%s2%s) S-Desc   : %s%s\r\n"
	  "%s3%s) L-Desc   :-\r\n%s%s\r\n"
	  "%s4%s) A-Desc   :-\r\n%s%s"
	  "%s5%s) Type        : %s%s\r\n"
	  "%s6%s) Extra flags : %s%s\r\n",

	  cyn, OLC_NUM(d), nrm,
	  grn, nrm, yel, (obj->name && *obj->name) ? obj->name : "undefined",
	  grn, nrm, yel, (obj->short_description && *obj->short_description) ? obj->short_description : "undefined",
	  grn, nrm, yel, (obj->description && *obj->description) ? obj->description : "undefined",
	  grn, nrm, yel, (obj->action_description && *obj->action_description) ? obj->action_description : "Not Set.\r\n",
	  grn, nrm, cyn, buf1,
	  grn, nrm, cyn, buf2
	  );
  /* Send first half then build second half of menu. */
  sprintbitarray(GET_OBJ_WEAR(OLC_OBJ(d)), wear_bits, EF_ARRAY_MAX, buf1, sizeof(buf1));
  sprintbitarray(GET_OBJ_AFFECT(OLC_OBJ(d)), affected_bits, EF_ARRAY_MAX, buf2, sizeof(buf2));

  write_to_output(d,
	  "%s7%s) Wear flags  : %s%s\r\n"
	  "%s8%s) Weight      : %s%d\r\n"
	  "%s9%s) Cost        : %s%d\r\n"
	  "%sA%s) Cost/Day    : %s%d\r\n"
	  "%sB%s) Timer       : %s%d\r\n"
	  "%sC%s) Values      : %s%d %d %d %d\r\n"
	  "%sD%s) Applies menu\r\n"
	  "%sE%s) Extra descriptions menu: %s%s%s\r\n"
          "%sM%s) Min Level   : %s%d\r\n"
          "%sP%s) Perm Affects: %s%s\r\n"
	  "%sS%s) Script      : %s%s\r\n"
          "%sW%s) Copy object\r\n"
          "%sX%s) Delete object\r\n"
	  "%sQ%s) Quit\r\n"
	  "Enter choice : ",

	  grn, nrm, cyn, buf1,
	  grn, nrm, cyn, GET_OBJ_WEIGHT(obj),
	  grn, nrm, cyn, GET_OBJ_COST(obj),
	  grn, nrm, cyn, GET_OBJ_RENT(obj),
	  grn, nrm, cyn, GET_OBJ_TIMER(obj),
	  grn, nrm, cyn, GET_OBJ_VAL(obj, 0),
	  GET_OBJ_VAL(obj, 1),
	  GET_OBJ_VAL(obj, 2),
	  GET_OBJ_VAL(obj, 3),
	  grn, nrm, grn, nrm, cyn, obj->ex_description ? "Set." : "Not Set.", grn,
          grn, nrm, cyn, GET_OBJ_LEVEL(obj),
          grn, nrm, cyn, buf2,
          grn, nrm, cyn, OLC_SCRIPT(d) ? "Set." : "Not Set.",
	  grn, nrm,
	  grn, nrm,
          grn, nrm
  );
  OLC_MODE(d) = OEDIT_MAIN_MENU;
}

/* main loop (of sorts).. basically interpreter throws all input to here. */
void oedit_parse(struct descriptor_data *d, char *arg)
{
  int number, max_val, min_val;
  char *oldtext = NULL;

  switch (OLC_MODE(d)) {

  case OEDIT_CONFIRM_SAVESTRING:
    switch (*arg) {
    case 'y':
    case 'Y':
      if (!oedit_save_checked(d)) {
        write_to_output(d, "Internal error: object %d could not be saved; nothing was changed.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), TRUE,
              "OLC: %s edits obj %d", GET_NAME(d->character), OLC_NUM(d));
      if (CONFIG_OLC_SAVE) {
        oedit_save_to_disk(real_zone_by_thing(OLC_NUM(d)));
        write_to_output(d, "Object saved to disk.\r\n");
      } else
        write_to_output(d, "Object saved to memory.\r\n");
      cleanup_olc(d, CLEANUP_ALL);
      return;
    case 'n':
    case 'N':
      /* Not saving: cleanup_olc() frees the unsaved OLC_SCRIPT copy. */
      cleanup_olc(d, CLEANUP_ALL);
      return;
    case 'a': /* abort quit */
    case 'A':
      oedit_disp_menu(d);
      return;
    default:
      write_to_output(d, "Invalid choice!\r\n");
      write_to_output(d, "Do you wish to save your changes? : \r\n");
      return;
    }

  case OEDIT_MAIN_MENU:
    /* Throw us out to whichever edit mode based on user input. */
    switch (*arg) {
    case 'q':
    case 'Q':
      if (OLC_VAL(d)) {	/* Something has been modified. */
	write_to_output(d, "Do you wish to save your changes? : ");
	OLC_MODE(d) = OEDIT_CONFIRM_SAVESTRING;
      } else
	cleanup_olc(d, CLEANUP_ALL);
      return;
    case '1':
      write_to_output(d, "Enter keywords : ");
      OLC_MODE(d) = OEDIT_KEYWORD;
      break;
    case '2':
      write_to_output(d, "Enter short desc : ");
      OLC_MODE(d) = OEDIT_SHORTDESC;
      break;
    case '3':
      write_to_output(d, "Enter long desc :-\r\n| ");
      OLC_MODE(d) = OEDIT_LONGDESC;
      break;
    case '4':
      OLC_MODE(d) = OEDIT_ACTDESC;
      send_editor_help(d);
      write_to_output(d, "Enter action description:\r\n\r\n");
      if (OLC_OBJ(d)->action_description) {
	write_to_output(d, "%s", OLC_OBJ(d)->action_description);
	oldtext = strdup(OLC_OBJ(d)->action_description);
      }
      string_write(d, &OLC_OBJ(d)->action_description, MAX_MESSAGE_LENGTH, 0, oldtext);
      OLC_VAL(d) = 1;
      break;
    case '5':
      oedit_disp_type_menu(d);
      OLC_MODE(d) = OEDIT_TYPE;
      break;
    case '6':
      oedit_disp_extra_menu(d);
      OLC_MODE(d) = OEDIT_EXTRAS;
      break;
    case '7':
      oedit_disp_wear_menu(d);
      OLC_MODE(d) = OEDIT_WEAR;
      break;
    case '8':
      write_to_output(d, "Enter weight : ");
      OLC_MODE(d) = OEDIT_WEIGHT;
      break;
    case '9':
      write_to_output(d, "Enter cost : ");
      OLC_MODE(d) = OEDIT_COST;
      break;
    case 'a':
    case 'A':
      write_to_output(d, "Enter cost per day : ");
      OLC_MODE(d) = OEDIT_COSTPERDAY;
      break;
    case 'b':
    case 'B':
      write_to_output(d, "Enter timer : ");
      OLC_MODE(d) = OEDIT_TIMER;
      break;
    case 'c':
    case 'C':
      /* Clear any old values */
      GET_OBJ_VAL(OLC_OBJ(d), 0) = 0;
      GET_OBJ_VAL(OLC_OBJ(d), 1) = 0;
      GET_OBJ_VAL(OLC_OBJ(d), 2) = 0;
      GET_OBJ_VAL(OLC_OBJ(d), 3) = 0;
      OLC_VAL(d) = 1;
      oedit_disp_val1_menu(d);
      break;
    case 'd':
    case 'D':
      oedit_disp_prompt_apply_menu(d);
      break;
    case 'e':
    case 'E':
      /* If extra descriptions don't exist. */
      if (OLC_OBJ(d)->ex_description == NULL) {
	CREATE(OLC_OBJ(d)->ex_description, struct extra_descr_data, 1);
	OLC_OBJ(d)->ex_description->next = NULL;
      }
      OLC_DESC(d) = OLC_OBJ(d)->ex_description;
      oedit_disp_extradesc_menu(d);
      break;
    case 'm':
    case 'M':
      write_to_output(d, "Enter new minimum level: ");
      OLC_MODE(d) = OEDIT_LEVEL;
      break;
    case 'p':
    case 'P':
      oedit_disp_perm_menu(d);
      OLC_MODE(d) = OEDIT_PERM;
      break;
    case 's':
    case 'S':
      OLC_SCRIPT_EDIT_MODE(d) = SCRIPT_MAIN_MENU;
      dg_script_menu(d);
      return;
    case 'w':
    case 'W':
      write_to_output(d, "Copy what object? ");
      OLC_MODE(d) = OEDIT_COPY;
      break;
    case 'x':
    case 'X':
      write_to_output(d, "Are you sure you want to delete this object? ");
      OLC_MODE(d) = OEDIT_DELETE;
      break;
    default:
      oedit_disp_menu(d);
      break;
    }
    return; /* end of OEDIT_MAIN_MENU */

  case OLC_SCRIPT_EDIT:
    if (dg_script_edit_parse(d, arg)) return;
    break;

  case OEDIT_KEYWORD:
    if (!genolc_checkstring(d, arg))
      break;
    if (OLC_OBJ(d)->name)
      free(OLC_OBJ(d)->name);
    OLC_OBJ(d)->name = str_udup(arg);
    break;

  case OEDIT_SHORTDESC:
    if (!genolc_checkstring(d, arg))
      break;
    if (OLC_OBJ(d)->short_description)
      free(OLC_OBJ(d)->short_description);
    OLC_OBJ(d)->short_description = str_udup(arg);
    break;

  case OEDIT_LONGDESC:
    if (!genolc_checkstring(d, arg))
      break;
    if (OLC_OBJ(d)->description)
      free(OLC_OBJ(d)->description);
    OLC_OBJ(d)->description = str_udup(arg);
    break;

  case OEDIT_TYPE:
    number = tb_parse_int_or_zero(arg);
    if ((number < 0) || (number >= NUM_ITEM_TYPES)) {
      write_to_output(d, "Invalid choice, try again : ");
      return;
    } else
      GET_OBJ_TYPE(OLC_OBJ(d)) = number;
    /* what's the boundschecking worth if we don't do this ? -- Welcor */
    GET_OBJ_VAL(OLC_OBJ(d), 0) = GET_OBJ_VAL(OLC_OBJ(d), 1) =
    GET_OBJ_VAL(OLC_OBJ(d), 2) = GET_OBJ_VAL(OLC_OBJ(d), 3) = 0;
    break;

  case OEDIT_EXTRAS:
    number = tb_parse_int_or_zero(arg);
    if ((number < 0) || (number > NUM_ITEM_FLAGS)) {
      oedit_disp_extra_menu(d);
      return;
    } else if (number == 0)
      break;
    else {
      TOGGLE_BIT_AR(GET_OBJ_EXTRA(OLC_OBJ(d)), (number - 1));
      oedit_disp_extra_menu(d);
      return;
    }

  case OEDIT_WEAR:
    number = tb_parse_int_or_zero(arg);
    if ((number < 0) || (number > NUM_ITEM_WEARS)) {
      write_to_output(d, "That's not a valid choice!\r\n");
      oedit_disp_wear_menu(d);
      return;
    } else if (number == 0)	/* Quit. */
      break;
    else {
      TOGGLE_BIT_AR(GET_OBJ_WEAR(OLC_OBJ(d)), (number - 1));
      oedit_disp_wear_menu(d);
      return;
    }

  case OEDIT_WEIGHT:
    OLC_SET_CLAMPED(d, GET_OBJ_WEIGHT(OLC_OBJ(d)), tb_parse_int_or_zero(arg), 0, MAX_OBJ_WEIGHT, "Weight");
    break;

  case OEDIT_COST:
    OLC_SET_CLAMPED(d, GET_OBJ_COST(OLC_OBJ(d)), tb_parse_int_or_zero(arg), 0, MAX_OBJ_COST, "Cost");
    break;

  case OEDIT_COSTPERDAY:
    OLC_SET_CLAMPED(d, GET_OBJ_RENT(OLC_OBJ(d)), tb_parse_int_or_zero(arg), 0, MAX_OBJ_RENT, "Rent");
    break;

  case OEDIT_TIMER:
    OLC_SET_CLAMPED(d, GET_OBJ_TIMER(OLC_OBJ(d)), tb_parse_int_or_zero(arg), 0, MAX_OBJ_TIMER, "Timer");
    break;

  case OEDIT_LEVEL:
    OLC_SET_CLAMPED(d, GET_OBJ_LEVEL(OLC_OBJ(d)), tb_parse_int_or_zero(arg), 0, LVL_IMPL, "Level");
    break;

  case OEDIT_PERM:
    if ((number = tb_parse_int_or_zero(arg)) == 0)
      break;
    if (number > 0 && number < NUM_AFF_FLAGS) {
      /* Setting AFF_CHARM on objects like this is dangerous: refuse it,
       * and say so. */
      if (number != AFF_CHARM) {
        TOGGLE_BIT_AR(GET_OBJ_AFFECT(OLC_OBJ(d)), number);
      } else {
        write_to_output(d, "CHARM cannot be set on an object.\r\n");
        return;
      }
    }
    oedit_disp_perm_menu(d);
    return;

  case OEDIT_VALUE_1:
    number = tb_parse_int_or_zero(arg);
    switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
    case ITEM_FURNITURE:
      /* Builder input. Re-prompt for value 1 on a bad seat count; both
       * paths return, so the common "proceed to menu 2" below neither
       * skips the bad value nor shows menu 2 twice. */
      if (number < 0 || number > MAX_PEOPLE) {
        write_to_output(d, "Seats must be 0 to %d.\r\n", MAX_PEOPLE);
        oedit_disp_val1_menu(d);
      } else {
        GET_OBJ_VAL(OLC_OBJ(d), 0) = number;
        oedit_disp_val2_menu(d);
      }
      return;
    case ITEM_WEAPON:
      OLC_SET_CLAMPED(d, GET_OBJ_VAL(OLC_OBJ(d), 0), number, -50, 50, "Weapon value 0");
      break;
    case ITEM_CONTAINER:
      OLC_SET_CLAMPED(d, GET_OBJ_VAL(OLC_OBJ(d), 0), number, -1, MAX_CONTAINER_SIZE, "Container capacity");
      break;
    default:
      GET_OBJ_VAL(OLC_OBJ(d), 0) = tb_parse_int_or_zero(arg);
    }
    /* proceed to menu 2 */
    oedit_disp_val2_menu(d);
    return;
  case OEDIT_VALUE_2:
    /* Here, I do need to check for out of range values. */
    number = tb_parse_int_or_zero(arg);
    switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
    case ITEM_SCROLL:
    case ITEM_POTION:
      if (number == 0 || number == -1)
	GET_OBJ_VAL(OLC_OBJ(d), 1) = -1;
      else
	OLC_SET_CLAMPED(d, GET_OBJ_VAL(OLC_OBJ(d), 1), number, 1, NUM_SPELLS, "Spell");

      oedit_disp_val3_menu(d);
      break;
    case ITEM_CONTAINER:
      /* Needs some special handling since we are dealing with flag values here. */
      if (number < 0 || number > 4)
	oedit_disp_container_flags_menu(d);
      else if (number != 0) {
        TOGGLE_BIT(GET_OBJ_VAL(OLC_OBJ(d), 1), 1 << (number - 1));
        OLC_VAL(d) = 1;
	oedit_disp_val2_menu(d);
      } else
	oedit_disp_val3_menu(d);
      break;
    case ITEM_WEAPON:
      OLC_SET_CLAMPED(d, GET_OBJ_VAL(OLC_OBJ(d), 1), number, 1, MAX_WEAPON_NDICE, "Number of damage dice");
      oedit_disp_val3_menu(d);
      break;

    default:
      GET_OBJ_VAL(OLC_OBJ(d), 1) = number;
      oedit_disp_val3_menu(d);
    }
    return;

  case OEDIT_VALUE_3:
    number = tb_parse_int_or_zero(arg);
    /* Quick'n'easy error checking. */
    switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
    case ITEM_SCROLL:
    case ITEM_POTION:
      if (number == 0 || number == -1) {
	GET_OBJ_VAL(OLC_OBJ(d), 2) = -1;
	oedit_disp_val4_menu(d);
	return;
      }
      min_val = 1;
      max_val = NUM_SPELLS;
      break;
    case ITEM_WEAPON:
      min_val = 1;
      max_val = MAX_WEAPON_SDICE;
      break;
    case ITEM_WAND:
    case ITEM_STAFF:
      min_val = 0;
      max_val = 20;
      break;
    case ITEM_DRINKCON:
    case ITEM_FOUNTAIN:
      min_val = 0;
      max_val = NUM_LIQ_TYPES - 1;
      number--;
      break;
    case ITEM_KEY:
      min_val = 0;
      max_val = 65099;
      break;
    default:
      min_val = -65000;
      max_val = 65000;
    }
    OLC_SET_CLAMPED(d, GET_OBJ_VAL(OLC_OBJ(d), 2), number, min_val, max_val, "Value 3");
    oedit_disp_val4_menu(d);
    return;

  case OEDIT_VALUE_4:
    number = tb_parse_int_or_zero(arg);
    switch (GET_OBJ_TYPE(OLC_OBJ(d))) {
    case ITEM_SCROLL:
    case ITEM_POTION:
      if (number == 0 || number == -1) {
	GET_OBJ_VAL(OLC_OBJ(d), 3) = -1;
        oedit_disp_menu(d);
	return;
      }
      min_val = 1;
      max_val = NUM_SPELLS;
      break;
    case ITEM_WAND:
    case ITEM_STAFF:
      min_val = 1;
      max_val = NUM_SPELLS;
      break;
    case ITEM_WEAPON:
      min_val = 0;
      max_val = NUM_ATTACK_TYPES - 1;
      break;
    default:
      min_val = -65000;
      max_val = 65000;
      break;
    }
    OLC_SET_CLAMPED(d, GET_OBJ_VAL(OLC_OBJ(d), 3), number, min_val, max_val, "Value 4");
    break;

  case OEDIT_PROMPT_APPLY:
    if ((number = tb_parse_int_or_zero(arg)) == 0)
      break;
    else if (number < 0 || number > MAX_OBJ_AFFECT) {
      oedit_disp_prompt_apply_menu(d);
      return;
    }
    OLC_VAL(d) = number - 1;
    OLC_MODE(d) = OEDIT_APPLY;
    oedit_disp_apply_menu(d);
    return;

  case OEDIT_APPLY:
    if (((number = tb_parse_int_or_zero(arg)) == 0) || ((number = tb_parse_int_or_zero(arg)) == 1)) {
      OLC_OBJ(d)->affected[OLC_VAL(d)].location = 0;
      OLC_OBJ(d)->affected[OLC_VAL(d)].modifier = 0;
      oedit_disp_prompt_apply_menu(d);
    } else if (number < 0 || number > NUM_APPLIES)
      oedit_disp_apply_menu(d);
    else {
      int counter;

      /* Below LVL_IMPL an apply may appear only once. Menu choice number
       * stores location number - 1 (below), so compare against that, and
       * ignore the slot being replaced. */
      if (GET_LEVEL(d->character) < LVL_IMPL) {
        for (counter = 0; counter < MAX_OBJ_AFFECT; counter++) {
          if (counter != OLC_VAL(d) &&
              OLC_OBJ(d)->affected[counter].modifier &&
              OLC_OBJ(d)->affected[counter].location == number - 1) {
            write_to_output(d, "Object already has that apply in slot %d; only an implementor may add it twice.\r\n",
                            counter + 1);
            oedit_disp_apply_menu(d);
            return;
          }
        }
      }

      OLC_OBJ(d)->affected[OLC_VAL(d)].location = number - 1;
      write_to_output(d, "Modifier : ");
      OLC_MODE(d) = OEDIT_APPLYMOD;
    }
    return;

  case OEDIT_APPLYMOD:
    OLC_OBJ(d)->affected[OLC_VAL(d)].modifier = tb_parse_int_or_zero(arg);
    oedit_disp_prompt_apply_menu(d);
    return;

  case OEDIT_EXTRADESC_KEY:
    if (genolc_checkstring(d, arg)) {
      if (OLC_DESC(d)->keyword)
        free(OLC_DESC(d)->keyword);
      OLC_DESC(d)->keyword = str_udup(arg);
    }
    oedit_disp_extradesc_menu(d);
    return;

  case OEDIT_EXTRADESC_MENU:
    switch ((number = tb_parse_int_or_zero(arg))) {
    case 0:
      if (!OLC_DESC(d)->keyword || !OLC_DESC(d)->description) {
        struct extra_descr_data *temp;

	if (OLC_DESC(d)->keyword)
	  free(OLC_DESC(d)->keyword);
	if (OLC_DESC(d)->description)
	  free(OLC_DESC(d)->description);

	/* Clean up pointers */
	REMOVE_FROM_LIST(OLC_DESC(d), OLC_OBJ(d)->ex_description, next);
	free(OLC_DESC(d));
	OLC_DESC(d) = NULL;
      }
    break;

    case 1:
      OLC_MODE(d) = OEDIT_EXTRADESC_KEY;
      write_to_output(d, "Enter keywords, separated by spaces :-\r\n| ");
      return;

    case 2:
      OLC_MODE(d) = OEDIT_EXTRADESC_DESCRIPTION;
      send_editor_help(d);
      write_to_output(d, "Enter the extra description:\r\n\r\n");
      if (OLC_DESC(d)->description) {
	write_to_output(d, "%s", OLC_DESC(d)->description);
	oldtext = strdup(OLC_DESC(d)->description);
      }
      string_write(d, &OLC_DESC(d)->description, MAX_MESSAGE_LENGTH, 0, oldtext);
      OLC_VAL(d) = 1;
      return;

    case 3:
      /* Only go to the next description if this one is finished. */
      if (OLC_DESC(d)->keyword && OLC_DESC(d)->description) {
	struct extra_descr_data *new_extra;

	if (OLC_DESC(d)->next)
	  OLC_DESC(d) = OLC_DESC(d)->next;
	else {	/* Make new extra description and attach at end. */
	  CREATE(new_extra, struct extra_descr_data, 1);
	  OLC_DESC(d)->next = new_extra;
	  OLC_DESC(d) = OLC_DESC(d)->next;
	}
      }
      oedit_disp_extradesc_menu(d);
      return;

    default:
      oedit_disp_extradesc_menu(d);
      return;
    }
    break;

  case OEDIT_COPY:
    if ((number = real_object(tb_parse_int_or_zero(arg))) != NOTHING) {
      /* setup allocates a fresh scratch object and trigger list: release
       * the ones this session holds first, as cleanup_olc() would. */
      olc_free_script_copy(d);
      free_object_strings(OLC_OBJ(d));
      free(OLC_OBJ(d));
      OLC_OBJ(d) = NULL;
      oedit_setup_existing(d, number);
      /* copy_object() kept the source's rnum, but this session still edits
       * OLC_NUM(d): delete and save must never act on the copy source. */
      GET_OBJ_RNUM(OLC_OBJ(d)) = real_object(OLC_NUM(d));
    } else
      write_to_output(d, "That object does not exist.\r\n");
    break;

  case OEDIT_DELETE:
    if (*arg == 'y' || *arg == 'Y') {
      /* Delete the prototype this session edits (OLC_NUM), never whatever
       * rnum the scratch copy holds: after 'W' or a missed renumber that
       * names another object, possibly in a zone the builder cannot edit. */
      obj_rnum doomed = real_object(OLC_NUM(d));
      struct char_data *builder = d->character;

      if (doomed == NOTHING) {
        TB_WARN_MSG("olc", "delete-target-missing", "object",
                    "builder " DIAG_CHAR_FMT " obj vnum %d has no prototype to delete",
                    DIAG_CHAR(builder), OLC_NUM(d));
        write_to_output(d, "Object %d has never been saved; there is nothing to delete.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      if (!can_edit_zone(builder, real_zone_by_thing(OLC_NUM(d)))) {
        TB_WARN_MSG("olc", "delete-permission", "object",
                    "builder " DIAG_CHAR_FMT " obj vnum %d delete refused: zone not editable",
                    DIAG_CHAR(builder), OLC_NUM(d));
        write_to_output(d, "You do not have permission to delete object %d.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      if (GET_OBJ_RNUM(OLC_OBJ(d)) != doomed) {
        TB_ERROR_MSG("olc", "delete-target-mismatch", "object",
                     "builder " DIAG_CHAR_FMT " editing obj vnum %d (rnum %d) but scratch rnum is %d; delete refused",
                     DIAG_CHAR(builder), OLC_NUM(d), doomed, GET_OBJ_RNUM(OLC_OBJ(d)));
        write_to_output(d, "Internal error: this editor lost track of object %d; nothing was deleted.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }

      if (delete_object(doomed) != NOTHING) {
        /* Other builders' scratch copies hold object rnums that just moved. */
        olc_renumber_sessions(OLC_RNUM_OBJ, doomed, -1, d);
        write_to_output(d, "Object deleted.\r\n");
      } else
        write_to_output(d, "Couldn't delete the object!\r\n");

      cleanup_olc(d, CLEANUP_ALL);
    } else if (*arg == 'n' || *arg == 'N') {
      oedit_disp_menu(d);
      OLC_MODE(d) = OEDIT_MAIN_MENU;
    } else
      write_to_output(d, "Please answer 'Y' or 'N': ");
    return;
  default:
    mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: OLC: Reached default case in oedit_parse()!");
    write_to_output(d, "Oops...\r\n");
    break;
  }

  /* If we get here, we have changed something. */
  OLC_VAL(d) = 1;
  oedit_disp_menu(d);
}


void iedit_parse(struct descriptor_data *d, char *arg)
{
  switch (OLC_MODE(d)) {
  case OEDIT_CONFIRM_SAVESTRING:
    switch (*arg) {
    case 'y':
    case 'Y':
      if (iedit_save_internally(d)) {
        /* Name the instance from the scratch copy: it holds the same text and,
         * unlike the live object, is owned by this session. */
        mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), TRUE,
               "OLC: %s iedits object instance %s (uid %ld)",
               GET_NAME(d->character),
               OLC_OBJ(d)->short_description ? OLC_OBJ(d)->short_description : "unknown",
               OLC_IEDIT_UID(d));
        write_to_output(d, "Object instance saved.\r\n");
      }
      cleanup_olc(d, CLEANUP_ALL);
      return;
    case 'n':
    case 'N':
      cleanup_olc(d, CLEANUP_ALL);
      return;
    case 'a':
    case 'A':
      oedit_disp_menu(d);
      return;
    default:
      write_to_output(d, "Invalid choice!\r\n");
      write_to_output(d, "Do you wish to save your changes? : ");
      return;
    }

  case OEDIT_MAIN_MENU:
    if (iedit_is_forbidden_main_menu_choice(arg)) {
      write_to_output(d,
        "That field is disabled in iedit.\r\n"
        "Scripts, copying, and deleting live objects are not yet supported here.\r\n"
        "Prototype/state fields must still be edited with oedit.\r\n");
      oedit_disp_menu(d);
      return;
    }
    break;
  }

  oedit_parse(d, arg);
}

void oedit_string_cleanup(struct descriptor_data *d, int terminator)
{
  switch (OLC_MODE(d)) {
  case OEDIT_ACTDESC:
    oedit_disp_menu(d);
    break;
  case OEDIT_EXTRADESC_DESCRIPTION:
    oedit_disp_extradesc_menu(d);
    break;
  }
}
