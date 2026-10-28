/**************************************************************************
*  File: objsave.c                                         Part of tbaMUD *
*  Usage: loading/saving player objects for rent and crash-save           *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/**
 * @file objsave.c
 * Rent and crash-save files (lib/plrobjs/<x>/<name>.objs): the objects a
 * player wears and carries while logged out, plus the shared object-record
 * reader/writer that house.c uses for house contents.
 *
 * File format: one rent header line "rentcode timed netcost gold account
 * nitems", then object records ("#vnum" followed by "Tag : value" lines
 * that differ from the prototype), ended by "$~". Each record's "Loc"
 * encodes where the object was (see objsave_parse_objects_from()).
 *
 * Who controls the input: rent files are written by this server but are
 * plain text that can be truncated or hand-edited, so every header field,
 * Loc value, flag word, object value and string is untrusted on read. Bad
 * values are reported (objsave operations with the owner, object line and
 * obj vnum) and the record or field is skipped, or repaired as db.c's
 * check_object() repairs world objects; reading never exits the server.
 *
 * Ownership and lifetime: objects read from a file are created with
 * read_object()/create_obj() (so they are on object_list) and handed to the
 * caller, which must place or extract each one. Writes are staged through
 * objsave_open_staged()/objsave_commit_staged(); rented objects are
 * extracted only after the file is committed, so a failed save never loses
 * them.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "db.h"
#include "interpreter.h"
#include "spells.h"
#include "act.h"
#include "class.h"
#include "config.h"
#include "modify.h"
#include "genolc.h" /* for strip_cr and sprintascii */
#include "oasis.h" /* for MAX_WEAPON_NDICE/MAX_WEAPON_SDICE, the oedit limits */

/* these factors should be unique integers */
#define RENT_FACTOR    1
#define CRYO_FACTOR    4

#define LOC_INVENTORY  0
#define MAX_BAG_ROWS   5

/* Short description for listings and diagnostics. A unique (vnum -1)
 * object loaded from a rent file without a "Shrt" tag has none, and
 * printf("%s", NULL) is undefined. Evaluates obj twice. */
#define OBJSAVE_SDESC(obj) \
  ((obj)->short_description ? (obj)->short_description : "<no short description>")

/* Appended when a rent or house listing does not fit its page buffer. */
#define OBJSAVE_LIST_TRUNCATED "*** listing truncated; see server log ***\r\n"

/* local functions */
static int Crash_save(struct obj_data *obj, FILE *fp, int location);
static void Crash_extract_norent_eq(struct char_data *ch);
static void auto_equip(struct char_data *ch, struct obj_data *obj, int location);
static int Crash_offer_rent(struct char_data *ch, struct char_data *receptionist, int display, int factor);
static int Crash_report_unrentables(struct char_data *ch, struct char_data *recep, struct obj_data *obj);
static void Crash_report_rent(struct char_data *ch, struct char_data *recep, struct obj_data *obj, long *cost, long *nitems, int display, int factor);
static int gen_receptionist(struct char_data *ch, struct char_data *recep, int cmd, char *arg, int mode);
static void Crash_rent_deadline(struct char_data *ch, struct char_data *recep, long cost);
static void Crash_restore_weight(struct obj_data *obj);
static void Crash_extract_objs(struct obj_data *obj);
static int Crash_is_unrentable(struct obj_data *obj);
static void Crash_extract_norents(struct obj_data *obj);
static void Crash_extract_expensive(struct obj_data *obj);
static void Crash_calculate_rent(struct obj_data *obj, int *cost);
static int Crash_cryosave(struct char_data *ch, int cost);
static int Crash_load_objs(struct char_data *ch);
static int handle_obj(struct obj_data *obj, struct char_data *ch, int locate, struct obj_data **cont_rows);
static int objsave_write_rentcode(FILE *fl, int rentcode, int cost_per_day, struct char_data *ch);

/* Read numeric rent headers without silent overflow or partial conversion.
 * Existing one-field and six-field disk layouts remain unchanged. */
static int objsave_parse_int_fields(const char *text, int *fields, size_t count)
{
  const char *p = text;
  char *end;
  size_t i;
  long value;

  if (!p || !fields)
    return 0;
  for (i = 0; i < count; i++) {
    while (isspace((unsigned char)*p))
      p++;
    errno = 0;
    value = strtol(p, &end, 10);
    if (p == end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
      return 0;
    fields[i] = (int)value;
    p = end;
  }
  while (isspace((unsigned char)*p))
    p++;
  return *p == '\0';
}

static int objsave_parse_rent_header(const char *text, int *rentcode,
                                    int *timed, int *netcost, int *gold,
                                    int *account, int *nitems)
{
  int fields[6];
  if (!objsave_parse_int_fields(text, fields, 6))
    return 0;
  *rentcode = fields[0];
  *timed = fields[1];
  *netcost = fields[2];
  *gold = fields[3];
  *account = fields[4];
  *nitems = fields[5];
  return 1;
}

/** Write one object record to fp (old name: Obj_to_store()).
 * Only fields that differ from the object's prototype are written. To
 * compare, a full prototype instance is created with read_object() (which
 * bumps obj_index[].number and attaches prototype triggers) and extracted
 * again before return, on every path.
 * @param locate The record's Loc: wear slot + 1, 0 for inventory, or minus
 * the container nesting depth.
 * ADes and EDes bodies are cut at MAX_STRING_LENGTH. Extra descriptions
 * with an empty or NULL keyword or text are not written (write-edesc).
 * @return 1 on success, 0 after reporting a write or prototype failure; the
 * caller must then abandon the staged file. */
int objsave_save_obj_record(struct obj_data *obj, FILE *fp, int locate)
{
  int counter2;
  struct extra_descr_data *ex_desc;
  char buf1[MAX_STRING_LENGTH +1];
  struct obj_data *temp = NULL;

#define OBJSAVE_PRINT(...) do { \
  if (fprintf(fp, __VA_ARGS__) < 0) { \
    int objsave_errno_ = errno; \
    TB_ERROR_LOG("ERROR", "objsave", "fprintf", "rent-object-record", \
                 objsave_errno_, "object_vnum=%d locate=%d", \
                 GET_OBJ_VNUM(obj), locate); \
    if (temp != NULL) \
      extract_obj(temp); \
    return 0; \
  } \
} while (0)

  if (obj == NULL || fp == NULL) {
    TB_ERROR_MSG("objsave", "write-record", "rent-object-record",
                 "invalid argument obj=%p fp=%p", (void *)obj, (void *)fp);
    return 0;
  }

  if (GET_OBJ_VNUM(obj) != NOTHING)
    temp = read_object(GET_OBJ_VNUM(obj), VIRTUAL);
  else {
    temp = create_obj();
    if (temp != NULL)
      temp->item_number = NOWHERE;
  }

  if (temp == NULL) {
    TB_ERROR_MSG("objsave", "load-prototype", "rent-object-record",
                 "unable to obtain comparison object for vnum=%d",
                 GET_OBJ_VNUM(obj));
    return 0;
  }

  if (obj->action_description) {

    strlcpy(buf1, obj->action_description, sizeof(buf1));
    strip_cr(buf1);
  } else
    *buf1 = 0;

  OBJSAVE_PRINT( "#%d\n", GET_OBJ_VNUM(obj));
  if (locate)
    OBJSAVE_PRINT( "Loc : %d\n", locate);
  if (GET_OBJ_VAL(obj, 0) != GET_OBJ_VAL(temp, 0) ||
      GET_OBJ_VAL(obj, 1) != GET_OBJ_VAL(temp, 1) ||
      GET_OBJ_VAL(obj, 2) != GET_OBJ_VAL(temp, 2) ||
      GET_OBJ_VAL(obj, 3) != GET_OBJ_VAL(temp, 3))
    OBJSAVE_PRINT(
             "Vals: %d %d %d %d\n",
             GET_OBJ_VAL(obj, 0),
             GET_OBJ_VAL(obj, 1),
             GET_OBJ_VAL(obj, 2),
             GET_OBJ_VAL(obj, 3)
             );
  if (GET_OBJ_EXTRA(obj) != GET_OBJ_EXTRA(temp))
    OBJSAVE_PRINT( "Flag: %d %d %d %d\n", GET_OBJ_EXTRA(obj)[0], GET_OBJ_EXTRA(obj)[1], GET_OBJ_EXTRA(obj)[2], GET_OBJ_EXTRA(obj)[3]);

#define TEST_OBJS(obj1, obj2, field) ((!((obj1)->field) || !((obj2)->field) || \
                                      strcmp((obj1)->field, (obj2)->field)))
#define TEST_OBJN(field) (obj->obj_flags.field != temp->obj_flags.field)

  if (TEST_OBJS(obj, temp, name))
    OBJSAVE_PRINT( "Name: %s\n", obj->name ? obj->name : "Undefined");
  if (TEST_OBJS(obj, temp, short_description))
    OBJSAVE_PRINT( "Shrt: %s\n", obj->short_description ? obj->short_description : "Undefined");

  /* These two could be a pain on the read... we'll see... */
  if (TEST_OBJS(obj, temp, description))
    OBJSAVE_PRINT( "Desc: %s\n", obj->description ? obj->description : "Undefined");

  /* Only even try to process this if an action desc exists */
  if (obj->action_description || temp->action_description)
    if (TEST_OBJS(obj, temp, action_description))
      OBJSAVE_PRINT( "ADes:\n%s~\n", buf1);

  if (TEST_OBJN(type_flag))
    OBJSAVE_PRINT( "Type: %d\n", GET_OBJ_TYPE(obj));
  if (TEST_OBJN(weight))
    OBJSAVE_PRINT( "Wght: %d\n", GET_OBJ_WEIGHT(obj));
  if (TEST_OBJN(cost))
    OBJSAVE_PRINT( "Cost: %d\n", GET_OBJ_COST(obj));
  if (TEST_OBJN(cost_per_day))
    OBJSAVE_PRINT( "Rent: %d\n", GET_OBJ_RENT(obj));
  if (TEST_OBJN(bitvector))
    OBJSAVE_PRINT( "Perm: %d %d %d %d\n", GET_OBJ_AFFECT(obj)[0], GET_OBJ_AFFECT(obj)[1], GET_OBJ_AFFECT(obj)[2], GET_OBJ_AFFECT(obj)[3]);
  if (TEST_OBJN(wear_flags))
    OBJSAVE_PRINT( "Wear: %d %d %d %d\n", GET_OBJ_WEAR(obj)[0], GET_OBJ_WEAR(obj)[1], GET_OBJ_WEAR(obj)[2], GET_OBJ_WEAR(obj)[3]);

  /* Do we have affects? */
  for (counter2 = 0; counter2 < MAX_OBJ_AFFECT; counter2++)
    if (obj->affected[counter2].modifier != temp->affected[counter2].modifier)
      OBJSAVE_PRINT( "Aff : %d %d %d\n",
               counter2,
               obj->affected[counter2].location,
               obj->affected[counter2].modifier
               );

  /* Do we have extra descriptions? */
  if (obj->ex_description || temp->ex_description) {
    /* To be reimplemented.  Need to handle this case in loading as
       well */
    if ((obj->ex_description && temp->ex_description &&
         obj->ex_description != temp->ex_description) ||
        !obj->ex_description || !temp->ex_description) {
      for (ex_desc = obj->ex_description; ex_desc; ex_desc = ex_desc->next) {
        /* A keyword or description can be NULL: fread_string() and the rent
         * loader return NULL for an empty "~" string, and OLC can leave
         * either empty. The old check dereferenced them. An entry without
         * both cannot be written in the EDes format and is dropped, loudly. */
        if (!ex_desc->keyword || !*ex_desc->keyword ||
            !ex_desc->description || !*ex_desc->description) {
          TB_WARN_MSG("objsave", "write-edesc", "rent-object-record",
                      "obj vnum %d extra description with empty keyword or "
                      "text not saved", GET_OBJ_VNUM(obj));
          continue;
        }
        strlcpy(buf1, ex_desc->description, sizeof(buf1));
        strip_cr(buf1);
        OBJSAVE_PRINT( "EDes:\n"
                 "%s~\n"
                 "%s~\n",
                 ex_desc->keyword,
                 buf1
                 );
      }
    }
  }

  OBJSAVE_PRINT( "\n");

  extract_obj(temp);

#undef OBJSAVE_PRINT
  return 1;
}

#undef TEST_OBJS
#undef TEST_OBJN

/** Put a freshly loaded object where its rent record says (AutoEQ by
 * Burkhard Knopf).
 * @param location The record's Loc. Above 0 asks for wear slot location-1;
 * the object is equipped only if the slot is in range and free, the object
 * can be worn there, and the wearer passes the alignment/class checks.
 * Otherwise, and for every location <= 0, the object goes to inventory.
 * Callers must therefore not assume the object ended up equipped: check
 * GET_EQ(ch, location-1) == obj (handle_obj() does). */
static void auto_equip(struct char_data *ch, struct obj_data *obj, int location)
{
  int j;

  /* Lots of checks... */
  if (location > 0) {  /* Was wearing it. */
    switch (j = (location - 1)) {
    case WEAR_LIGHT:
      break;
    case WEAR_FINGER_R:
    case WEAR_FINGER_L:
      if (!CAN_WEAR(obj, ITEM_WEAR_FINGER)) /* not fitting :( */
        location = LOC_INVENTORY;
      break;
    case WEAR_NECK_1:
    case WEAR_NECK_2:
      if (!CAN_WEAR(obj, ITEM_WEAR_NECK))
        location = LOC_INVENTORY;
      break;
    case WEAR_BODY:
      if (!CAN_WEAR(obj, ITEM_WEAR_BODY))
        location = LOC_INVENTORY;
      break;
    case WEAR_HEAD:
      if (!CAN_WEAR(obj, ITEM_WEAR_HEAD))
        location = LOC_INVENTORY;
      break;
    case WEAR_LEGS:
      if (!CAN_WEAR(obj, ITEM_WEAR_LEGS))
        location = LOC_INVENTORY;
      break;
    case WEAR_FEET:
      if (!CAN_WEAR(obj, ITEM_WEAR_FEET))
        location = LOC_INVENTORY;
      break;
    case WEAR_HANDS:
      if (!CAN_WEAR(obj, ITEM_WEAR_HANDS))
        location = LOC_INVENTORY;
      break;
    case WEAR_ARMS:
      if (!CAN_WEAR(obj, ITEM_WEAR_ARMS))
        location = LOC_INVENTORY;
      break;
    case WEAR_SHIELD:
      if (!CAN_WEAR(obj, ITEM_WEAR_SHIELD))
        location = LOC_INVENTORY;
      break;
    case WEAR_ABOUT:
      if (!CAN_WEAR(obj, ITEM_WEAR_ABOUT))
        location = LOC_INVENTORY;
      break;
    case WEAR_WAIST:
      if (!CAN_WEAR(obj, ITEM_WEAR_WAIST))
        location = LOC_INVENTORY;
      break;
    case WEAR_WRIST_R:
    case WEAR_WRIST_L:
      if (!CAN_WEAR(obj, ITEM_WEAR_WRIST))
        location = LOC_INVENTORY;
      break;
    case WEAR_WIELD:
      if (!CAN_WEAR(obj, ITEM_WEAR_WIELD))
        location = LOC_INVENTORY;
      break;
    case WEAR_HOLD:
      if (CAN_WEAR(obj, ITEM_WEAR_HOLD))
        break;
      if (IS_WARRIOR(ch) && CAN_WEAR(obj, ITEM_WEAR_WIELD) && GET_OBJ_TYPE(obj) == ITEM_WEAPON)
        break;
      location = LOC_INVENTORY;
      break;
    default:
      location = LOC_INVENTORY;
    }

    if (location > 0) {      /* Wearable. */
      if (!GET_EQ(ch,j)) {
        /* Check the characters's alignment to prevent them from being zapped
	 * through the auto-equipping. */
        if (invalid_align(ch, obj) || invalid_class(ch, obj))
          location = LOC_INVENTORY;
        else
          equip_char(ch, obj, j);
      } else {  /* Oops, saved a player with double equipment? */
        mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
               "SYSERR: autoeq: '%s' already equipped in position %d.", GET_NAME(ch), location);
        location = LOC_INVENTORY;
      }
    }
  }
  if (location <= 0)  /* Inventory */
    obj_to_char(obj, ch);
}

int Crash_delete_file(char *name)
{
  char filename[MAX_INPUT_LENGTH];
  FILE *fl;

  if (!get_filename(filename, sizeof(filename), CRASH_FILE, name))
    return FALSE;

  if (!(fl = fopen(filename, "r"))) {
    if (errno != ENOENT)  /* if it fails but NOT because of no file */
      TB_REPORT_ERRNO("fopen", filename);
    return FALSE;
  }
  tb_fclose(fl);

  /* if it fails, NOT because of no file */
  if (remove(filename) < 0 && errno != ENOENT)
    TB_REPORT_ERRNO("remove", filename);

  return TRUE;
}

int Crash_delete_crashfile(struct char_data *ch)
{
  char filename[MAX_INPUT_LENGTH];
  int numread;
  FILE *fl;
  int rentcode;
  char line[READ_SIZE];

  if (!get_filename(filename, sizeof(filename), CRASH_FILE, GET_NAME(ch)))
    return FALSE;

  if (!(fl = fopen(filename, "r"))) {
    if (errno != ENOENT)  /* if it fails, NOT because of no file */
      TB_REPORT_ERRNO("fopen", filename);
    return FALSE;
  }
  numread = get_line(fl,line);
  tb_fclose(fl);

  if (numread == FALSE) {
    TB_ERROR_MSG("objsave", "read-header", filename,
                 "player=%s empty or unreadable crash header", GET_NAME(ch));
    return FALSE;
  }
  /* Crash files use the same six-field header as rented object files.
   * Parse the complete record rather than rejecting its remaining fields. */
  {
    int timed, netcost, gold, account, nitems;
    if (!objsave_parse_rent_header(line, &rentcode, &timed, &netcost,
                                  &gold, &account, &nitems)) {
      TB_ERROR_MSG("objsave", "parse-header", filename,
                   "player=%s malformed crash header=\"%s\"", GET_NAME(ch), line);
      return FALSE;
    }
  }

  if (rentcode == RENT_CRASH)
    Crash_delete_file(GET_NAME(ch));

  return TRUE;
}

int Crash_clean_file(char *name)
{
  char filename[MAX_INPUT_LENGTH], filetype[20];
  int numread;
  FILE *fl;
  int rentcode, timed, netcost, gold, account, nitems;
  char line[READ_SIZE];

  if (!get_filename(filename, sizeof(filename), CRASH_FILE, name))
    return FALSE;

  /* Open so that permission problems will be flagged now, at boot time. */
  if (!(fl = fopen(filename, "r"))) {
    if (errno != ENOENT)  /* if it fails, NOT because of no file */
      TB_REPORT_ERRNO("fopen", filename);
    return FALSE;
  }

  numread = get_line(fl,line);
  tb_fclose(fl);
  if (numread == FALSE)
    return FALSE;

  if (!objsave_parse_rent_header(line, &rentcode, &timed, &netcost,
                                 &gold, &account, &nitems)) {
    TB_ERROR_MSG("objsave", "parse-header", filename,
                 "player=%s malformed clean-file header=\"%s\"", name, line);
    return FALSE;
  }

  if ((rentcode == RENT_CRASH) ||
      (rentcode == RENT_FORCED) ||
      (rentcode == RENT_TIMEDOUT) ) {
    if (timed < time(0) - (CONFIG_CRASH_TIMEOUT * SECS_PER_REAL_DAY)) {
      Crash_delete_file(name);
      switch (rentcode) {
      case RENT_CRASH:
        strlcpy(filetype, "crash", sizeof(filetype));
        break;
      case RENT_FORCED:
        strlcpy(filetype, "forced rent", sizeof(filetype));
        break;
      case RENT_TIMEDOUT:
        strlcpy(filetype, "idlesave", sizeof(filetype));
        break;
      default:
        strlcpy(filetype, "UNKNOWN!", sizeof(filetype));
        break;
      }
      log("    Deleting %s's %s file.", name, filetype);
      return TRUE;
    }
    /* Must retrieve rented items w/in 30 days */
  } else if (rentcode == RENT_RENTED)
    if (timed < time(0) - (CONFIG_RENT_TIMEOUT * SECS_PER_REAL_DAY)) {
      Crash_delete_file(name);
      log("    Deleting %s's rent file.", name);
      return TRUE;
    }
  return FALSE;
}

void update_obj_file(void)
{
  int i;

  for (i = 0; i <= top_of_p_table; i++)
    if (*player_table[i].name)
      Crash_clean_file(player_table[i].name);
}

void Crash_listrent(struct char_data *ch, char *name)
{
  FILE *fl;
  char filename[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH], line[READ_SIZE];
  obj_save_data *loaded, *current;
  int rentcode = RENT_UNDEF, timed, netcost, gold, account, nitems, numread;
  int listed = 0, total = 0;
  struct tb_buf out;
  
  if (!get_filename(filename, sizeof(filename), CRASH_FILE, name))
    return;

  if (!(fl = fopen(filename, "r"))) {
    send_to_char(ch, "%s has no rent file.\r\n", name);
    return;
  }
  /* The listing is bounded by buf; a rent file may hold any number of
   * objects with builder- or restring-length descriptions. Keep room for the
   * truncation marker. */
  tb_buf_init(&out, buf, sizeof(buf) - sizeof(OBJSAVE_LIST_TRUNCATED));
  tb_buf_appendf(&out, "%s\r\n", filename);

  numread = get_line(fl, line);

  /* Oops, can't get the data, punt. */
  if (numread == FALSE) {
    send_to_char(ch, "Error reading rent information.\r\n");
    tb_fclose(fl);
    return;
  }

  if (!objsave_parse_rent_header(line, &rentcode, &timed, &netcost,
                                 &gold, &account, &nitems)) {
    TB_ERROR_MSG("objsave", "parse-header", filename,
                 "player=%s malformed rent listing header", name);
    send_to_char(ch, "Error reading rent information.\r\n");
    tb_fclose(fl);
    return;
  }

  switch (rentcode) {
  case RENT_RENTED:
    tb_buf_append(&out, "Rent\r\n");
    break;
  case RENT_CRASH:
    tb_buf_append(&out, "Crash\r\n");
    break;
  case RENT_CRYO:
    tb_buf_append(&out, "Cryo\r\n");
    break;
  case RENT_TIMEDOUT:
  case RENT_FORCED:
    tb_buf_append(&out, "TimedOut\r\n");
    break;
  default:
    tb_buf_append(&out, "Undef\r\n");
    break;
  }

	{
	  char owner[MAX_NAME_LENGTH + 16];
	  tb_snprintf(owner, sizeof(owner), "player %s", name);
	  loaded = objsave_parse_objects_from(fl, filename, owner);
	}

	for (current = loaded; current != NULL; current=current->next) {
	  total++;
	  if (!tb_buf_truncated(&out) &&
	      tb_buf_appendf(&out, "[%5d] (%5dau) %-20s\r\n",
                GET_OBJ_VNUM(current->obj),
                GET_OBJ_RENT(current->obj),
                OBJSAVE_SDESC(current->obj)))
	    listed++;
	}
	if (tb_buf_truncated(&out)) {
	  TB_WARN_MSG("objsave", "listrent-truncate", filename,
	              "%d objects listed of %d; listing exceeds %zu bytes",
	              listed, total, sizeof(buf));
	  strlcat(buf, OBJSAVE_LIST_TRUNCATED, sizeof(buf));
	}

	/* Now it's safe to free the obj_save_data list and the objects on it. */
	while (loaded != NULL) {
		current = loaded;
		loaded = loaded->next;
		extract_obj(current->obj);
		free(current);
	}

  page_string(ch->desc,buf,0);
  tb_fclose(fl);
}

/* Return values:
 *  0 - successful load, keep char in rent room.
 *  1 - load failure or load of crash items -- put char in temple.
 *  2 - rented equipment lost (no $) */
int Crash_load(struct char_data *ch)
{
  return (Crash_load_objs(ch));
}

/** Write obj, its later siblings and all their contents to fp.
 * Order: the sibling chain first, then the contents (Loc one level deeper,
 * negative), then obj itself, so on reload contents precede their
 * container, as handle_obj() expects. Each object's weight is subtracted
 * from the containers holding it, so a reloaded container is not counted
 * twice; the caller MUST run Crash_restore_weight() on the same tree
 * afterwards, on failure too. Every object is visited even after a failed
 * record, so the restore pass stays exact.
 * @return TRUE only when every record in the tree was written (failures
 * deeper in the tree used to be ignored and the file committed without
 * them). objsave_save_obj_record() reports each failed record. */
static int Crash_save(struct obj_data *obj, FILE *fp, int location)
{
  struct obj_data *tmp;
  int ok = TRUE;

  if (obj) {
    if (!Crash_save(obj->next_content, fp, location))
      ok = FALSE;
    if (!Crash_save(obj->contains, fp, MIN(0, location) - 1))
      ok = FALSE;

    if (!objsave_save_obj_record(obj, fp, location))
      ok = FALSE;

    for (tmp = obj->in_obj; tmp; tmp = tmp->in_obj)
      GET_OBJ_WEIGHT(tmp) -= GET_OBJ_WEIGHT(obj);
  }
  return ok;
}

static void Crash_restore_weight(struct obj_data *obj)
{
  if (obj) {
    Crash_restore_weight(obj->contains);
    Crash_restore_weight(obj->next_content);
    if (obj->in_obj)
      GET_OBJ_WEIGHT(obj->in_obj) += GET_OBJ_WEIGHT(obj);
  }
}

/* Get !RENT items from equipment to inventory and extract !RENT out of worn
 * containers. */
static void Crash_extract_norent_eq(struct char_data *ch)
{
  int j;

  for (j = 0; j < NUM_WEARS; j++) {
    if (GET_EQ(ch, j) == NULL)
      continue;

    if (Crash_is_unrentable(GET_EQ(ch, j)))
      obj_to_char(unequip_char(ch, j), ch);
    else
      Crash_extract_norents(GET_EQ(ch, j));
  }
}

static void Crash_extract_objs(struct obj_data *obj)
{
  if (obj) {
    Crash_extract_objs(obj->contains);
    Crash_extract_objs(obj->next_content);
    extract_obj(obj);
  }
}

static int Crash_is_unrentable(struct obj_data *obj)
{
  if (!obj)
    return FALSE;

  if (OBJ_FLAGGED(obj, ITEM_NORENT) ||
      GET_OBJ_RENT(obj) < 0 ||
      GET_OBJ_RNUM(obj) == NOTHING ||
      GET_OBJ_TYPE(obj) == ITEM_KEY) {
 log("Crash_is_unrentable: removing object %s", obj->short_description);
    return TRUE;
  }

  return FALSE;
}

static void Crash_extract_norents(struct obj_data *obj)
{
  if (obj) {
    Crash_extract_norents(obj->contains);
    Crash_extract_norents(obj->next_content);
    if (Crash_is_unrentable(obj))
      extract_obj(obj);
  }
}

static void Crash_extract_expensive(struct obj_data *obj)
{
  struct obj_data *tobj, *max;

  max = obj;
  for (tobj = obj; tobj; tobj = tobj->next_content)
    if (GET_OBJ_RENT(tobj) > GET_OBJ_RENT(max))
      max = tobj;
  extract_obj(max);
}

static void Crash_calculate_rent(struct obj_data *obj, int *cost)
{
  if (obj) {
    *cost += MAX(0, GET_OBJ_RENT(obj));
    Crash_calculate_rent(obj->contains, cost);
    Crash_calculate_rent(obj->next_content, cost);
  }
}

/** Open a staged replacement for a rent or house object file.
 * The live file is never truncated in place: the new contents go to a
 * mkstemp() file beside it, and objsave_commit_staged() replaces the live
 * file only after a complete, flushed write.
 * @param dest Live file path.
 * @param[out] temp Receives the temporary path (pass it to commit/abort).
 * @return The stream, or NULL after reporting stage-open. */
FILE *objsave_open_staged(const char *dest, char *temp, size_t temp_size)
{
  int fd;
  FILE *fp;

  if (snprintf(temp, temp_size, "%s.tmpXXXXXX", dest) >= (int)temp_size) {
    TB_ERROR_MSG("objsave", "stage-open", dest, "temporary path too long");
    return NULL;
  }
  if ((fd = mkstemp(temp)) < 0) {
    TB_ERROR_ERRNO("objsave", "stage-open", dest, "cannot create staged file");
    return NULL;
  }
  if (!(fp = fdopen(fd, "w"))) {
    TB_ERROR_ERRNO("objsave", "stage-open", temp, "cannot open staged file");
    close(fd);
    unlink(temp);
    return NULL;
  }
  return fp;
}

/** Discard a staged file: close the stream and remove the temporary path. */
void objsave_abort_staged(FILE *fp, const char *temp)
{
  if (fp)
    (void) fclose(fp);	/* discarding the staged file; nothing to keep */
  (void) unlink(temp);
}

/** Flush, close and commit a staged file over dest. Always closes fp.
 * @return TRUE when dest now holds the new contents; FALSE after reporting
 * stage-commit, with dest unchanged and the temporary file removed. */
int objsave_commit_staged(FILE *fp, const char *temp, const char *dest)
{
  int failed = FALSE;

  if (ferror(fp) || fflush(fp) != 0)
    failed = TRUE;
  if (fclose(fp) != 0)
    failed = TRUE;
  if (!failed && !tb_replace_file_checked(temp, dest))
    failed = TRUE;
  if (failed) {
    TB_ERROR_ERRNO("objsave", "stage-commit", dest,
                   "write or rename failed; live file kept");
    unlink(temp);
    return FALSE;
  }
  return TRUE;
}

/* Open the staged rent file for ch, reporting rent-open with the player
 * and rent code on failure. */
static FILE *Crash_open_rentfile(struct char_data *ch, int rentcode,
                                 char *filename, size_t filename_size,
                                 char *temp, size_t temp_size)
{
  FILE *fp;

  if (!get_filename(filename, filename_size, CRASH_FILE, GET_NAME(ch))) {
    TB_ERROR_MSG("objsave", "rent-open", "rent-file",
                 "player idnum %ld rentcode %d: no rent file name",
                 GET_IDNUM(ch), rentcode);
    return NULL;
  }
  if (!(fp = objsave_open_staged(filename, temp, temp_size)))
    TB_ERROR_MSG("objsave", "rent-open", filename,
                 "player idnum %ld rentcode %d: rent file not written",
                 GET_IDNUM(ch), rentcode);
  return fp;
}

/* Report a rent save that did not reach the disk. */
#define CRASH_SAVE_FAILED(ch, filename, rentcode) \
  TB_ERROR_MSG("objsave", "rent-write", (filename), \
               "player idnum %ld rentcode %d: rent file not written; " \
               "objects kept on the character", GET_IDNUM(ch), (rentcode))

static int objsave_write_rentcode(FILE *fl, int rentcode, int cost_per_day, struct char_data *ch)
{
  if (fprintf(fl, "%d %ld %d %d %d %d\r\n",
          rentcode,
          (long) time(0),
          cost_per_day,
          GET_GOLD(ch),
          GET_BANK_GOLD(ch),
          0)
       < 1)
    {
       TB_REPORT_ERRNO("fprintf", "rent code");
       return FALSE;
    }
  return TRUE;

}

/* Write the rent header, every equipped object (Loc = slot + 1, with its
 * contents) and the inventory, then the "$~" terminator. Container weights
 * are always restored. Nothing is extracted here: the caller extracts only
 * after objsave_commit_staged() succeeds, so a failed write never leaves
 * the player with objects that exist neither in the game nor on disk.
 * @return TRUE when every record was written; FALSE after reporting
 * rent-write with the slot and obj vnum. */
static int Crash_write_char_objs(struct char_data *ch, FILE *fp,
                                 const char *filename, int rentcode, int cost)
{
  int j;

  if (!objsave_write_rentcode(fp, rentcode, cost, ch))
    return FALSE;
  for (j = 0; j < NUM_WEARS; j++)
    if (GET_EQ(ch, j)) {
      int ok = Crash_save(GET_EQ(ch, j), fp, j + 1);
      Crash_restore_weight(GET_EQ(ch, j));
      if (!ok) {
        TB_ERROR_MSG("objsave", "rent-write", filename,
                     "player idnum %ld failed at slot %d obj vnum %d",
                     GET_IDNUM(ch), j, GET_OBJ_VNUM(GET_EQ(ch, j)));
        return FALSE;
      }
    }
  if (ch->carrying) {
    int ok = Crash_save(ch->carrying, fp, 0);
    Crash_restore_weight(ch->carrying);
    if (!ok) {
      TB_ERROR_MSG("objsave", "rent-write", filename,
                   "player idnum %ld failed in inventory (first obj vnum %d)",
                   GET_IDNUM(ch), GET_OBJ_VNUM(ch->carrying));
      return FALSE;
    }
  }
  return fprintf(fp, "$~\n") >= 0;
}

/* Extract everything a character wears and carries (after a committed
 * rent save). */
static void Crash_extract_char_objs(struct char_data *ch)
{
  int j;

  for (j = 0; j < NUM_WEARS; j++)
    if (GET_EQ(ch, j))
      Crash_extract_objs(GET_EQ(ch, j));
  Crash_extract_objs(ch->carrying);
}

/* Stage, write and commit ch's rent file. @return TRUE when committed. */
static int Crash_save_char_file(struct char_data *ch, int rentcode, int cost)
{
  char filename[MAX_INPUT_LENGTH], temp[MAX_INPUT_LENGTH + 16];
  FILE *fp;

  if (!(fp = Crash_open_rentfile(ch, rentcode, filename, sizeof(filename),
                                 temp, sizeof(temp))))
    return FALSE;
  if (!Crash_write_char_objs(ch, fp, filename, rentcode, cost)) {
    objsave_abort_staged(fp, temp);
    CRASH_SAVE_FAILED(ch, filename, rentcode);
    return FALSE;
  }
  if (!objsave_commit_staged(fp, temp, filename)) {
    CRASH_SAVE_FAILED(ch, filename, rentcode);
    return FALSE;
  }
  return TRUE;
}

/** Crash-save a player's equipment and inventory (rent code RENT_CRASH).
 * Objects stay on the character. PLR_CRASH is cleared only on success, so a
 * failed save is retried. @return TRUE when the file was written. */
int Crash_crashsave(struct char_data *ch)
{
  if (IS_NPC(ch))
    return FALSE;
  if (!Crash_save_char_file(ch, RENT_CRASH, 0))
    return FALSE;
  REMOVE_BIT_AR(PLR_FLAGS(ch), PLR_CRASH);
  return TRUE;
}

/** Force-rent an idle player (RENT_TIMEDOUT, twice the normal rent).
 * !RENT objects are extracted first; if gold is short the most expensive
 * objects are extracted until the rent is affordable. The saved objects are
 * extracted only after the file is committed; on failure they stay on the
 * character. @return TRUE when the file was written (or nothing was left
 * to save). */
int Crash_idlesave(struct char_data *ch)
{
  int j;
  int cost, cost_eq;

  if (IS_NPC(ch))
    return FALSE;

  Crash_extract_norent_eq(ch);
  Crash_extract_norents(ch->carrying);

  cost = 0;
  Crash_calculate_rent(ch->carrying, &cost);

  cost_eq = 0;
  for (j = 0; j < NUM_WEARS; j++)
    Crash_calculate_rent(GET_EQ(ch, j), &cost_eq);

  cost += cost_eq;
  cost *= 2;    /* forcerent cost is 2x normal rent */

  if (cost > GET_GOLD(ch) + GET_BANK_GOLD(ch)) {
    for (j = 0; j < NUM_WEARS; j++)  /* Unequip players with low gold. */
      if (GET_EQ(ch, j))
        obj_to_char(unequip_char(ch, j), ch);

    while ((cost > GET_GOLD(ch) + GET_BANK_GOLD(ch)) && ch->carrying) {
      Crash_extract_expensive(ch->carrying);
      cost = 0;
      Crash_calculate_rent(ch->carrying, &cost);
      cost *= 2;
    }
  }

  if (ch->carrying == NULL) {
    for (j = 0; j < NUM_WEARS && GET_EQ(ch, j) == NULL; j++) /* Nothing */ ;
    if (j == NUM_WEARS) {  /* No equipment or inventory. */
      Crash_delete_file(GET_NAME(ch));
      return TRUE;
    }
  }

  if (!Crash_save_char_file(ch, RENT_TIMEDOUT, cost))
    return FALSE;
  Crash_extract_char_objs(ch);
  return TRUE;
}

/** Rent a player out (RENT_RENTED) at cost per day. !RENT objects are
 * extracted first; the saved objects are extracted only after the file is
 * committed. @return TRUE when the file was written. */
int Crash_rentsave(struct char_data *ch, int cost)
{
  if (IS_NPC(ch))
    return FALSE;

  Crash_extract_norent_eq(ch);
  Crash_extract_norents(ch->carrying);

  if (!Crash_save_char_file(ch, RENT_RENTED, cost))
    return FALSE;
  Crash_extract_char_objs(ch);
  return TRUE;
}

/** Cryo-rent a player (RENT_CRYO): cost is taken from gold on hand (the
 * header records the reduced gold), the saved objects are extracted and
 * PLR_CRYO is set, all only after the file is committed. On failure the
 * gold is given back and the objects stay. @return TRUE when written. */
static int Crash_cryosave(struct char_data *ch, int cost)
{
  int old_gold;

  if (IS_NPC(ch))
    return FALSE;

  Crash_extract_norent_eq(ch);
  Crash_extract_norents(ch->carrying);

  old_gold = GET_GOLD(ch);
  GET_GOLD(ch) = MAX(0, GET_GOLD(ch) - cost);

  if (!Crash_save_char_file(ch, RENT_CRYO, 0)) {
    GET_GOLD(ch) = old_gold;
    return FALSE;
  }
  Crash_extract_char_objs(ch);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_CRYO);
  return TRUE;
}

/* Routines used for the receptionist. */
static void Crash_rent_deadline(struct char_data *ch, struct char_data *recep,
                         long cost)
{
  long rent_deadline;
  char buf[MAX_STRING_LENGTH];

  if (!cost)
    return;

  rent_deadline = ((GET_GOLD(ch) + GET_BANK_GOLD(ch)) / cost);
  tb_snprintf(buf, sizeof(buf), "$n tells you, 'You can rent for %ld day%s with the gold you have\r\n"
         "on hand and in the bank.'\r\n", rent_deadline, rent_deadline != 1 ? "s" : "");
act(buf, FALSE, recep, 0, ch, TO_VICT);
}

static int Crash_report_unrentables(struct char_data *ch, struct char_data *recep,
                             struct obj_data *obj)
{
  int has_norents = 0;

  if (obj) {
    if (Crash_is_unrentable(obj)) {
      has_norents = 1;
      /* The object is act()'s $p, never part of the format: a short
       * description is builder- or iedit-controlled text, and a '$' in it
       * was read as an act() code (B2-32). */
      act("$n tells you, 'You cannot store $p.'", FALSE, recep, obj, ch, TO_VICT);
    }
    has_norents += Crash_report_unrentables(ch, recep, obj->contains);
    has_norents += Crash_report_unrentables(ch, recep, obj->next_content);
  }
  return (has_norents);
}

static void Crash_report_rent(struct char_data *ch, struct char_data *recep, struct
    obj_data *obj, long *cost, long *nitems, int display, int factor)
{
  static char buf[256];

  if (obj) {
    if (!Crash_is_unrentable(obj)) {
      (*nitems)++;
      *cost += MAX(0, (GET_OBJ_RENT(obj) * factor));
      if (display) {
        /* Only the number is formatted in; the object is $p (B2-32). */
        tb_snprintf(buf, sizeof(buf), "$n tells you, '%5d coins for $p..'",
                (GET_OBJ_RENT(obj) * factor));
        act(buf, FALSE, recep, obj, ch, TO_VICT);
      }
    }
    Crash_report_rent(ch, recep, obj->contains, cost, nitems, display, factor);
    Crash_report_rent(ch, recep, obj->next_content, cost, nitems, display, factor);
  }
}

static int Crash_offer_rent(struct char_data *ch, struct char_data *recep,
                     int display, int factor)
{
  char buf[MAX_INPUT_LENGTH];
  int i;
  long totalcost = 0, numitems = 0, norent;

  norent = Crash_report_unrentables(ch, recep, ch->carrying);
  for (i = 0; i < NUM_WEARS; i++)
    norent += Crash_report_unrentables(ch, recep, GET_EQ(ch, i));

  if (norent)
    return FALSE;

  totalcost = CONFIG_MIN_RENT_COST * factor;

  Crash_report_rent(ch, recep, ch->carrying, &totalcost, &numitems, display, factor);

  for (i = 0; i < NUM_WEARS; i++)
    Crash_report_rent(ch, recep, GET_EQ(ch, i), &totalcost, &numitems, display, factor);

  if (!numitems) {
    act("$n tells you, 'But you are not carrying anything!  Just quit!'",
        FALSE, recep, 0, ch, TO_VICT);
    return FALSE;
  }
  if (numitems > CONFIG_MAX_OBJ_SAVE) {
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'Sorry, but I cannot store more than %d items.'",
        CONFIG_MAX_OBJ_SAVE);
    act(buf, FALSE, recep, 0, ch, TO_VICT);
    return FALSE;
  }
  if (display) {
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'Plus, my %d coin fee..'",
        CONFIG_MIN_RENT_COST * factor);
    act(buf, FALSE, recep, 0, ch, TO_VICT);
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'For a total of %ld coins%s.'",
            totalcost, (factor == RENT_FACTOR ? " per day" : ""));
    act(buf, FALSE, recep, 0, ch, TO_VICT);
    if (totalcost > GET_GOLD(ch) + GET_BANK_GOLD(ch)) {
      act("$n tells you, '...which I see you can't afford.'",
          FALSE, recep, 0, ch, TO_VICT);
      return FALSE;
    } else if (factor == RENT_FACTOR)
      Crash_rent_deadline(ch, recep, totalcost);
  }
  return (totalcost);
}

static int gen_receptionist(struct char_data *ch, struct char_data *recep, int cmd,
    char *arg, int mode)
{
  int cost;
  char buf[128];
  const char *action_table[] = { "smile", "dance", "sigh", "blush", "burp",
	  "cough", "fart", "twiddle", "yawn" };

  if (!cmd && !rand_number(0, 5)) {
    do_action(recep, NULL, find_command(action_table[rand_number(0, 8)]), 0);
    return (FALSE);
  }

  if (!ch->desc || IS_NPC(ch))
    return (FALSE);

  if (!CMD_IS("offer") && !CMD_IS("rent"))
    return (FALSE);

  if (!AWAKE(recep)) {
    send_to_char(ch, "%s is unable to talk to you...\r\n", HSSH(recep));
    return (TRUE);
  }

  if (!CAN_SEE(recep, ch)) {
    act("$n says, 'I don't deal with people I can't see!'", FALSE, recep, 0, 0, TO_ROOM);
    return (TRUE);
  }

  if (CONFIG_FREE_RENT) {
    act("$n tells you, 'Rent is free here.  Just quit, and your objects will be saved!'",
	FALSE, recep, 0, ch, TO_VICT);
    return TRUE;
  }

  if (CMD_IS("rent")) {

    if (!(cost = Crash_offer_rent(ch, recep, FALSE, mode)))
      return (TRUE);
    if (mode == RENT_FACTOR)
      tb_snprintf(buf, sizeof(buf), "$n tells you, 'Rent will cost you %d gold coins per day.'", cost);
    else if (mode == CRYO_FACTOR)
      tb_snprintf(buf, sizeof(buf), "$n tells you, 'It will cost you %d gold coins to be frozen.'", cost);
    act(buf, FALSE, recep, 0, ch, TO_VICT);

    if (cost > GET_GOLD(ch) + GET_BANK_GOLD(ch)) {
      act("$n tells you, '...which I see you can't afford.'",
	  FALSE, recep, 0, ch, TO_VICT);
      return (TRUE);
    }
    if (cost && (mode == RENT_FACTOR))
      Crash_rent_deadline(ch, recep, cost);

    /* The player is extracted below, so a rent that did not reach the disk
     * must stop here: logging "has rented" and extracting would lose
     * everything the player carries. The saver has reported the cause. */
    if (mode == RENT_FACTOR) {
      if (!Crash_rentsave(ch, cost)) {
        act("$n tells you, 'I'm sorry, I could not store your belongings. "
            "Please try again later.'", FALSE, recep, 0, ch, TO_VICT);
        return (TRUE);
      }
      act("$n stores your belongings and helps you into your private chamber.", FALSE, recep, 0, ch, TO_VICT);
      mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "%s has rented (%d/day, %d tot.)",
		GET_NAME(ch), cost, GET_GOLD(ch) + GET_BANK_GOLD(ch));
    } else {			/* cryo */
      if (!Crash_cryosave(ch, cost)) {
        act("$n tells you, 'I'm sorry, I could not store your belongings. "
            "Please try again later.'", FALSE, recep, 0, ch, TO_VICT);
        return (TRUE);
      }
      act("$n stores your belongings and helps you into your private chamber.\r\n"
	  "A white mist appears in the room, chilling you to the bone...\r\n"
	  "You begin to lose consciousness...",
	  FALSE, recep, 0, ch, TO_VICT);
      mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "%s has cryo-rented.", GET_NAME(ch));
      SET_BIT_AR(PLR_FLAGS(ch), PLR_CRYO);
    }

    act("$n helps $N into $S private chamber.", FALSE, recep, 0, ch, TO_NOTVICT);

    GET_LOADROOM(ch) = GET_ROOM_VNUM(IN_ROOM(ch));
    extract_char(ch);	/* It saves. */
  } else {
    Crash_offer_rent(ch, recep, TRUE, mode);
    act("$N gives $n an offer.", FALSE, ch, 0, recep, TO_ROOM);
  }
  return (TRUE);
}

SPECIAL(receptionist)
{
  return (gen_receptionist(ch, (struct char_data *)me, cmd, argument, RENT_FACTOR));
}

SPECIAL(cryogenicist)
{
  return (gen_receptionist(ch, (struct char_data *)me, cmd, argument, CRYO_FACTOR));
}

void Crash_save_all(void)
{
  struct descriptor_data *d;
  for (d = descriptor_list; d; d = d->next) {
    if ((STATE(d) == CON_PLAYING) && !IS_NPC(d->character)) {
      if (PLR_FLAGGED(d->character, PLR_CRASH)) {
        /* Crash_crashsave() clears PLR_CRASH only after a committed write,
         * so a failed save is retried on the next pass. */
        Crash_crashsave(d->character);
        save_char(d->character);
      }
    }
  }
}

/** Parse the object records in fl (positioned after any rent header).
 * @return A malloc'd obj_save_data list in file order, or NULL if there
 * are no records. Each node's obj is a live object (on object_list) that is
 * in no room, inventory or container yet: the caller must place it
 * (obj_to_char, obj_to_room, handle_obj ...) or extract_obj() it, and must
 * free() every node.
 * Each node's locate is the record's Loc: 1..NUM_WEARS = worn in slot
 * locate-1, 0 = inventory, negative = inside a container, -locate levels
 * deep (contents are written before their container). A Loc above
 * NUM_WEARS is reported and becomes inventory. A record whose prototype no
 * longer exists is skipped (missing-proto) together with its tags. Each
 * completed record's values get check_object()'s per-type range checks
 * (objsave_check_values(): rent-object-check).
 *
 * Callers that know which file and owner they are reading should use
 * objsave_parse_objects_from(), so that diagnostics name them. */
obj_save_data *objsave_parse_objects(FILE *fl)
{
  return objsave_parse_objects_from(fl, "rent-object-record", "unknown owner");
}

/* Every record diagnostic carries the file (resource), the owner ("player
 * idnum N" / "house vnum N"), the "object line" (lines read by this parser,
 * counting from the line after any rent header the caller consumed, so it is
 * the file line minus one for rent files and the file line for house files)
 * and, once known, the record's object vnum. The file is untrusted: a corrupted or hand-edited
 * rent or house file must point an admin at the exact record. */
#define OBJSAVE_REC_FMT "%s object line %d obj vnum %d: "
#define OBJSAVE_REC_ARGS(vnum) owner, lineno, (int) (vnum)

/* A spell number an object may carry: -1 or 0 is "none", otherwise
 * 1..TOP_SPELL_DEFINE but not a skill (MAX_SPELLS+1..MAX_SKILLS), as in
 * db.c's check_object_spell_number(). Evaluates s several times. */
#define OBJSAVE_SPELL_OK(s) \
  ((s) == -1 || (s) == 0 || ((s) > 0 && (s) <= TOP_SPELL_DEFINE && \
                             ((s) <= MAX_SPELLS || (s) > MAX_SKILLS)))

/** Range-check the values a rent or house record gave obj (its Type and
 * Vals tags, or a unique object's whole record). The file is untrusted
 * (hand-edited or damaged) and db.c's check_object() only ever sees world
 * prototypes, so the same per-type checks run here. A value that indexes a
 * table or drives a loop is repaired to check_object()'s default and
 * reported as an error: liquid (drinks[], drink_aff[], drinknames[]) to
 * LIQ_WATER, weapon attack type (attack_hit_text[]) to 0 (hit), weapon
 * dice clamped to the oedit limit MAX_WEAPON_NDICE d MAX_WEAPON_SDICE.
 * check_object()'s other per-type checks only report, and so do these
 * (warnings; the value is kept, as on a world object): spell level, spell
 * numbers, charges and contents above their maximum. Every report is
 * rent-object-check with the owner, the record's line and the obj vnum. */
static void objsave_check_values(struct obj_data *obj, const char *resource,
                                 const char *owner, int lineno)
{
  int i, first_spell = 1;

  switch (GET_OBJ_TYPE(obj)) {
  case ITEM_DRINKCON:
  case ITEM_FOUNTAIN:
    if (GET_OBJ_VAL(obj, 2) < 0 || GET_OBJ_VAL(obj, 2) >= NUM_LIQ_TYPES) {
      TB_ERROR_MSG("objsave", "rent-object-check", resource,
                   OBJSAVE_REC_FMT "liquid %d out of range 0..%d; using LIQ_WATER",
                   OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 2),
                   NUM_LIQ_TYPES - 1);
      GET_OBJ_VAL(obj, 2) = LIQ_WATER;
    }
    if (GET_OBJ_VAL(obj, 0) > 0 && GET_OBJ_VAL(obj, 1) > GET_OBJ_VAL(obj, 0))
      TB_WARN_MSG("objsave", "rent-object-check", resource,
                  OBJSAVE_REC_FMT "contains %d, more than its capacity %d",
                  OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 1),
                  GET_OBJ_VAL(obj, 0));
    break;
  case ITEM_WEAPON:
    if (GET_OBJ_VAL(obj, 3) < 0 || GET_OBJ_VAL(obj, 3) >= NUM_ATTACK_TYPES) {
      TB_ERROR_MSG("objsave", "rent-object-check", resource,
                   OBJSAVE_REC_FMT "attack type %d out of range 0..%d; using 0 (hit)",
                   OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 3),
                   NUM_ATTACK_TYPES - 1);
      GET_OBJ_VAL(obj, 3) = 0;
    }
    /* dice() loops value 1 times and sums into an int on every blow. */
    if (GET_OBJ_VAL(obj, 1) < 0 || GET_OBJ_VAL(obj, 1) > MAX_WEAPON_NDICE ||
        GET_OBJ_VAL(obj, 2) < 0 || GET_OBJ_VAL(obj, 2) > MAX_WEAPON_SDICE) {
      int ndice = LIMIT(GET_OBJ_VAL(obj, 1), 0, MAX_WEAPON_NDICE);
      int sdice = LIMIT(GET_OBJ_VAL(obj, 2), 0, MAX_WEAPON_SDICE);

      TB_ERROR_MSG("objsave", "rent-object-check", resource,
                   OBJSAVE_REC_FMT "damage dice %dd%d outside 0..%dd0..%d; using %dd%d",
                   OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 1),
                   GET_OBJ_VAL(obj, 2), MAX_WEAPON_NDICE, MAX_WEAPON_SDICE,
                   ndice, sdice);
      GET_OBJ_VAL(obj, 1) = ndice;
      GET_OBJ_VAL(obj, 2) = sdice;
    }
    break;
  case ITEM_WAND:
  case ITEM_STAFF:
    first_spell = 3;
    if (GET_OBJ_VAL(obj, 2) > GET_OBJ_VAL(obj, 1))
      TB_WARN_MSG("objsave", "rent-object-check", resource,
                  OBJSAVE_REC_FMT "has %d charges, more than its maximum %d",
                  OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 2),
                  GET_OBJ_VAL(obj, 1));
    /* The level and spell checks below apply to wands and staves too. */
    /* Fall through. */
  case ITEM_SCROLL:
  case ITEM_POTION:
    if (GET_OBJ_VAL(obj, 0) < 0 || GET_OBJ_VAL(obj, 0) > LVL_IMPL)
      TB_WARN_MSG("objsave", "rent-object-check", resource,
                  OBJSAVE_REC_FMT "spell level %d out of range 0..%d",
                  OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 0), LVL_IMPL);
    for (i = first_spell; i <= 3; i++)
      if (!OBJSAVE_SPELL_OK(GET_OBJ_VAL(obj, i)))
        TB_WARN_MSG("objsave", "rent-object-check", resource,
                    OBJSAVE_REC_FMT "value %d spell %d out of range (1..%d, not a skill)",
                    OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), i, GET_OBJ_VAL(obj, i),
                    TOP_SPELL_DEFINE);
    break;
  case ITEM_FURNITURE:
    if (GET_OBJ_VAL(obj, 1) > GET_OBJ_VAL(obj, 0))
      TB_WARN_MSG("objsave", "rent-object-check", resource,
                  OBJSAVE_REC_FMT "seats %d, more than its capacity %d",
                  OBJSAVE_REC_ARGS(GET_OBJ_VNUM(obj)), GET_OBJ_VAL(obj, 1),
                  GET_OBJ_VAL(obj, 0));
    break;
  default:
    break;
  }
}
#undef OBJSAVE_SPELL_OK

obj_save_data *objsave_parse_objects_from(FILE *fl, const char *resource,
                                          const char *owner)
{
  obj_save_data *head, *current, *tempsave;
  char f1[128], f2[128], f3[128], f4[128], line[READ_SIZE];
  int t[4],i, nr, lineno = 0, got;
  int record_line = 0;	/* object line of the current record's '#' */
  struct obj_data *temp;

  if (resource == NULL)
    resource = "rent-object-record";
  if (owner == NULL)
    owner = "unknown owner";

  CREATE(current, obj_save_data, 1);
  head = current;
  current->locate = 0;

  temp = NULL;
  while (TRUE) {
    char tag[6];
    int num;

    /* if the file is done, wrap it all up */
    got = get_line(fl, line);
    lineno += got;	/* get_line() returns the lines it consumed */
    if(got == FALSE || (*line == '$' && line[1] == '~')) {
      if (temp == NULL && current->obj == NULL) {
        /* Remove current from list. */
        tempsave = head;
        if (tempsave == current) {
          free(current);
          head = NULL;
        } else {
          while (tempsave) {
            if (tempsave->next == current)
              tempsave->next = NULL;
            tempsave = tempsave->next;
          }
          free(current);
        }
      }
    else if (temp != NULL && current->obj == NULL) {
      objsave_check_values(temp, resource, owner, record_line);
      current->obj = temp;
    } else if (temp == NULL && current->obj != NULL) {
      /* Do nothing. */
  } else if (temp != NULL && current->obj != NULL) {
      if (temp != current->obj)
        TB_ERROR_MSG("objsave", "parse-state", resource,
                     "%s object line %d: inconsistent object pointers %p/%p",
                     owner, lineno, (void *)temp, (void *)current->obj);
    }

    break;
  }

    /* A '#' line starts a new record. Wrap up the previous object FIRST:
     * if this record is then skipped (malformed line, missing prototype),
     * temp must be NULL so the skipped record's tags (Loc, Vals, Name...)
     * are ignored instead of being applied to the previous object. */
    if (*line == '#') {
      if (temp) {
        /* The record is complete: its Type and Vals are known. */
        objsave_check_values(temp, resource, owner, record_line);
        current->obj = temp;
        CREATE(current->next, obj_save_data, 1);
        current=current->next;

        current->locate = 0;
        temp = NULL;
      }
      record_line = lineno;

      if (!objsave_parse_int_fields(line + 1, &nr, 1)) {
        TB_WARN_MSG("objsave", "rent-record", resource,
                    "%s object line %d: malformed '#' record line; record skipped",
                    owner, lineno);
        continue;
      }

      /* If we attempt to load an object with a legal VNUM 0-65534, that
       * does not exist, skip it. If the object has a VNUM of NOTHING or
       * 65535, then we assume it doesn't exist on purpose. (Custom Item,
       * Coins, Corpse, etc...) */
      if (nr == NOTHING) {   /* then it is unique */
        temp = create_obj();
        temp->item_number=NOTHING;
      } else if (nr < 0 || real_object(nr) == NOTHING) {
        TB_WARN_MSG("objsave", "missing-proto", resource,
                    "%s object line %d: rent record vnum %d has no prototype; "
                    "record skipped", owner, lineno, nr);
      } else
        temp=read_object(nr,VIRTUAL);
      /* go read next line - nothing more to see here. */
      continue;
    }

    /* If "temp" is NULL, we are most likely progressing through
     * a non-existant object, so just keep continuing till we find 
     * the next object */
    if (temp == NULL)
      continue;

    tag_argument(line, tag);
    if (!*tag)
      continue;	/* short line, already reported by tag_argument() */
    num = tb_parse_int_or_zero(line);

    switch(*tag) {
    case 'A':
      if (!strcmp(tag, "ADes")) {
        /* fread_string() exit(1)s the server at EOF, so one truncated rent
         * or house file would take the game down at the owner's login. */
        char *text;
        if (fread_string_checked(fl, resource, "ADes", &text))
          temp->action_description = text;
        else
          TB_ERROR_MSG("objsave", "rent-string", resource,
                       OBJSAVE_REC_FMT "unterminated or oversize ADes; "
                       "prototype text kept", OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
      } else if (!strcmp(tag, "Aff ")) {
        if (!objsave_parse_int_fields(line, t, 3)) {
          TB_ERROR_MSG("objsave", "parse-affect", resource,
                       OBJSAVE_REC_FMT "malformed saved object affect",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
          break;
        }
        if (t[0] >= 0 && t[0] < MAX_OBJ_AFFECT) {
          temp->affected[t[0]].location = t[1];
          temp->affected[t[0]].modifier = t[2];
        }
      }
      break;
    case 'C':
      if (!strcmp(tag, "Cost"))
        GET_OBJ_COST(temp) = num;
      break;
    case 'D':
      if (!strcmp(tag, "Desc"))
        temp->description = strdup(line);
      break;
    case 'E':
      if(!strcmp(tag, "EDes")) {
        struct extra_descr_data *new_desc;
        char *keyword = NULL, *description = NULL;

        /* Read both strings before touching the object: a truncated file
         * (no closing '~') must neither exit(1) via fread_string() nor
         * leave half an extra description behind. */
        if (!fread_string_checked(fl, resource, "EDes keyword", &keyword) ||
            !fread_string_checked(fl, resource, "EDes description", &description)) {
          free(keyword);
          TB_ERROR_MSG("objsave", "rent-string", resource,
                       OBJSAVE_REC_FMT "unterminated or oversize EDes; "
                       "extra description dropped",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
          break;
        }
        if (temp->item_number != NOTHING && temp->ex_description) {
          obj_rnum proto_rnum = real_object(temp->item_number);

          if (proto_rnum != NOTHING &&
              temp->ex_description == obj_proto[proto_rnum].ex_description)
            temp->ex_description = NULL;
        }
        CREATE(new_desc, struct extra_descr_data, 1);
        new_desc->keyword = keyword;
        new_desc->description = description;
        new_desc->next = temp->ex_description;
        temp->ex_description = new_desc;
      }
      break;
    case 'F':
      if (!strcmp(tag, "Flag")) {
        if (sscanf(line, "%127s %127s %127s %127s", f1, f2, f3, f4) != 4) {
          TB_ERROR_MSG("objsave", "parse-flags", resource,
                       OBJSAVE_REC_FMT "malformed saved object extra flags",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
          break;
        }
        GET_OBJ_EXTRA(temp)[0] = asciiflag_conv(f1);
        GET_OBJ_EXTRA(temp)[1] = asciiflag_conv(f2);
        GET_OBJ_EXTRA(temp)[2] = asciiflag_conv(f3);
        GET_OBJ_EXTRA(temp)[3] = asciiflag_conv(f4);
      }
      break;
    case 'L':
      if(!strcmp(tag, "Loc ")) {
        /* Loc is 1..NUM_WEARS for a worn slot, 0 for inventory, and negative
         * for the nesting depth inside a container (Crash_save writes depths
         * past MAX_BAG_ROWS, which handle_obj flattens). A worn slot past
         * NUM_WEARS would index ch->equipment out of bounds. */
        if (num > NUM_WEARS) {
          TB_ERROR_MSG("objsave", "rent-loc-range", resource,
                       OBJSAVE_REC_FMT "Loc %d outside 1..%d; loading into inventory",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)), num, NUM_WEARS);
          num = LOC_INVENTORY;
        }
        current->locate = num;
      }
      break;
    case 'N':
      if (!strcmp(tag, "Name"))
        temp->name = strdup(line);
      break;
    case 'P':
      if (!strcmp(tag, "Perm")) {
        if (sscanf(line, "%127s %127s %127s %127s", f1, f2, f3, f4) != 4) {
          TB_ERROR_MSG("objsave", "parse-flags", resource,
                       OBJSAVE_REC_FMT "malformed saved object permanent flags",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
          break;
        }
        GET_OBJ_AFFECT(temp)[0] = asciiflag_conv(f1);
        GET_OBJ_AFFECT(temp)[1] = asciiflag_conv(f2);
        GET_OBJ_AFFECT(temp)[2] = asciiflag_conv(f3);
        GET_OBJ_AFFECT(temp)[3] = asciiflag_conv(f4);
      }
      break;
    case 'R':
      if (!strcmp(tag, "Rent"))
        GET_OBJ_RENT(temp) = num;
      break;
    case 'S':
      if (!strcmp(tag, "Shrt"))
        temp->short_description = strdup(line);
      break;
    case 'T':
      if (!strcmp(tag, "Type"))
        GET_OBJ_TYPE(temp) = num;
      break;
    case 'W':
      if (!strcmp(tag, "Wear")) {
        if (sscanf(line, "%127s %127s %127s %127s", f1, f2, f3, f4) != 4) {
          TB_ERROR_MSG("objsave", "parse-flags", resource,
                       OBJSAVE_REC_FMT "malformed saved object wear flags",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
          break;
        }
        GET_OBJ_WEAR(temp)[0] = asciiflag_conv(f1);
        GET_OBJ_WEAR(temp)[1] = asciiflag_conv(f2);
        GET_OBJ_WEAR(temp)[2] = asciiflag_conv(f3);
        GET_OBJ_WEAR(temp)[3] = asciiflag_conv(f4);
      }
      else if (!strcmp(tag, "Wght"))
        GET_OBJ_WEIGHT(temp) = num;
      break;
    case 'V':
      if (!strcmp(tag, "Vals")) {
        if (!objsave_parse_int_fields(line, t, 4)) {
          TB_ERROR_MSG("objsave", "parse-values", resource,
                       OBJSAVE_REC_FMT "malformed saved object values",
                       OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)));
          break;
        }
        for (i = 0; i < NUM_OBJ_VAL_POSITIONS; i++)
          GET_OBJ_VAL(temp, i) = t[i];
      }
      break;
    default:
      TB_WARN_MSG("objsave", "rent-tag", resource,
                  OBJSAVE_REC_FMT "unknown tag %.4s ignored",
                  OBJSAVE_REC_ARGS(GET_OBJ_VNUM(temp)), tag);
    }
  }

  return head;
}
#undef OBJSAVE_REC_FMT
#undef OBJSAVE_REC_ARGS

static int Crash_load_objs(struct char_data *ch) {
  FILE *fl;
  char filename[PATH_MAX];
  char line[READ_SIZE];
  char buf[MAX_STRING_LENGTH];
  int i, orig_rent_code, num_objs=0;
  struct obj_data *cont_row[MAX_BAG_ROWS];
  int rentcode = RENT_UNDEF;
  int timed=0,netcost=0,gold,account,nitems;
	obj_save_data *loaded, *current;

  if (!get_filename(filename, sizeof(filename), CRASH_FILE, GET_NAME(ch)))
    return 1;

  for (i = 0; i < MAX_BAG_ROWS; i++)
    cont_row[i] = NULL;

  if (!(fl = fopen(filename, "r"))) {
    if (errno != ENOENT) { /* if it fails, NOT because of no file */
      tb_snprintf(buf, MAX_STRING_LENGTH, "SYSERR: READING OBJECT FILE %s (5)", filename);
      TB_REPORT_ERRNO("fopen", buf);
      send_to_char(ch, "\r\n********************* NOTICE *********************\r\n"
                       "There was a problem loading your objects from disk.\r\n"
                       "Contact a God for assistance.\r\n");
    }
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "%s entering game with no equipment.", GET_NAME(ch));
    return 1;
  }
 
  if (!get_line(fl, line)) {
    TB_ERROR_MSG("objsave", "read-header", filename,
                 "player=%s rent header missing", GET_NAME(ch));
    tb_fclose(fl);
    return 1;
  }
  if (!objsave_parse_rent_header(line, &rentcode, &timed, &netcost,
                                 &gold, &account, &nitems)) {
    TB_ERROR_MSG("objsave", "parse-header", filename,
                 "player=%s malformed rent header=\"%s\"", GET_NAME(ch), line);
    tb_fclose(fl);
    return 1;
  }

  if (rentcode == RENT_RENTED || rentcode == RENT_TIMEDOUT) {
    /* 'timed' and 'netcost' come from the rent file (written by the server,
     * but hand-editable and exposed to clock changes). A timestamp in the
     * future made the day count negative, and the old unsigned cost then
     * wrapped to a huge value and destroyed the rented equipment. The old
     * unsigned deduction also wrapped when gold on hand exceeded the cost
     * and credited the bank. All arithmetic is now signed 64-bit. */
    long long elapsed = (long long) time(0) - (long long) timed;
    long long days, cost, on_hand = GET_GOLD(ch), in_bank = GET_BANK_GOLD(ch);

    if (elapsed < 0) {
      TB_ERROR_MSG("objsave", "rent-clock", filename,
                   "player idnum %ld rent timed %d is %lld s in the future; "
                   "charging 0 days", GET_IDNUM(ch), timed, -elapsed);
      elapsed = 0;
    }
    if (netcost < 0) {
      TB_ERROR_MSG("objsave", "rent-clock", filename,
                   "player idnum %ld negative rent netcost %d; charging 0",
                   GET_IDNUM(ch), netcost);
      netcost = 0;
    }
    days = elapsed / SECS_PER_REAL_DAY;
    cost = (long long) netcost * days;
    if (cost > on_hand + in_bank) {
      tb_fclose(fl);
      mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
             "%s entering game, rented equipment lost (no $).", GET_NAME(ch));
      Crash_crashsave(ch);
      return 2;
    } else {
      if (cost > on_hand) {
        GET_BANK_GOLD(ch) = (int) (in_bank - (cost - on_hand));
        GET_GOLD(ch) = 0;
      } else
        GET_GOLD(ch) = (int) (on_hand - cost);
      save_char(ch);
    }
  }
  switch (orig_rent_code = rentcode) {
  case RENT_RENTED:
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
           "%s un-renting and entering game.", GET_NAME(ch));
    break;
  case RENT_CRASH:

    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
           "%s retrieving crash-saved items and entering game.", GET_NAME(ch));
    break;
  case RENT_CRYO:
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
           "%s un-cryo'ing and entering game.", GET_NAME(ch));
    break;
  case RENT_FORCED:
  case RENT_TIMEDOUT:
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
           "%s retrieving force-saved items and entering game.", GET_NAME(ch));
    break;
  default:
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
           "WARNING: %s entering game with undefined rent code.", GET_NAME(ch));
    break;
  }

	{
	  char owner[64];
	  tb_snprintf(owner, sizeof(owner), "player idnum %ld", GET_IDNUM(ch));
	  loaded = objsave_parse_objects_from(fl, filename, owner);
	}
	for (current = loaded; current != NULL; current=current->next)
	  num_objs += handle_obj(current->obj, ch, current->locate, cont_row);

	/* now it's safe to free the obj_save_data list - all members of it
	 * have been put in the correct lists by handle_obj() */
	while (loaded != NULL) {
		current = loaded;
		loaded = loaded->next;
		free(current);
	}

  /* Little hoarding check. -gg 3/1/98 */
 mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(ch)), TRUE, "%s (level %d) has %d object%s (max %d).",
         GET_NAME(ch), GET_LEVEL(ch), num_objs, num_objs != 1 ? "s" : "", CONFIG_MAX_OBJ_SAVE);

  tb_fclose(fl);

  if ((orig_rent_code == RENT_RENTED) || (orig_rent_code == RENT_CRYO))
    return 0;
  else
    return 1;
}

/** Place one parsed rent object on ch and rebuild container nesting.
 * cont_row[MAX_BAG_ROWS] is per-load state, NULL-initialised by the
 * caller: cont_row[d] holds the objects read so far at nesting depth d+1
 * whose container has not been seen yet (contents are written before their
 * container). When a container arrives at depth d, it takes cont_row[d] as
 * its contents. A row whose container never arrives is spilled into ch's
 * inventory. An object nested deeper than MAX_BAG_ROWS stays loose in
 * inventory (reported as rent-nesting).
 * A worn container is filled only if auto_equip() really equipped it;
 * otherwise it is filled in inventory and reported (autoeq-container).
 * @return TRUE if temp was placed (counts toward the hoarding total). */
static int handle_obj(struct obj_data *temp, struct char_data *ch, int locate, struct obj_data **cont_row)
{
  int j;
  struct obj_data *obj1;

  if (!temp)  /* this should never happen, but.... */
    return FALSE;

  auto_equip(ch, temp, locate);

  /* What to do with a new loaded item:
   * If there's a list with <locate> less than 1 below this: (equipped items
   * are assumed to have <locate>==0 here) then its container has disappeared
   * from the file   *gasp* -> put all the list back to ch's inventory if
   * there's a list of contents with <locate> 1 below this: check if it's a
   * container - if so: get it from ch, fill it, and give it back to ch (this
   * way the container has its correct weight before modifying ch) - if not:
   * the container is missing -> put all the list to ch's inventory. For items
   * with negative <locate>: If there's already a list of contents with the
   * same <locate> put obj to it if not, start a new list. Since <locate> for
   * contents is < 0 the list indices are switched to non-negative. */
  if (locate > 0) { /* item equipped */

    for (j = MAX_BAG_ROWS-1;j > 0;j--)
      if (cont_row[j]) { /* no container -> back to ch's inventory */
        for (;cont_row[j];cont_row[j] = obj1) {
          obj1 = cont_row[j]->next_content;
          obj_to_char(cont_row[j], ch);
        }
        cont_row[j] = NULL;
      }
    if (cont_row[0]) { /* content list existing */
      /* auto_equip() may have refused the slot recorded in the rent file
       * (wrong wear flags, alignment/class zap, slot already taken, slot out
       * of range) and put the container in inventory instead. The file's
       * Loc value is player-file data, so never assume the object is worn:
       * unequip_char() on an empty slot returns NULL, and on an occupied slot
       * it returns a different object. Fill the container where it really is. */
      if (GET_OBJ_TYPE(temp) == ITEM_CONTAINER &&
          locate - 1 < NUM_WEARS && GET_EQ(ch, locate - 1) == temp) {
        /* rem item ; fill ; equip again */
        temp = unequip_char(ch, locate-1);
        temp->contains = NULL; /* should be empty - but who knows */
        for (;cont_row[0];cont_row[0] = obj1) {
          obj1 = cont_row[0]->next_content;
          obj_to_obj(cont_row[0], temp);
        }
        equip_char(ch, temp, locate-1);
      } else if (GET_OBJ_TYPE(temp) == ITEM_CONTAINER) {
        TB_ERROR_MSG("objsave", "autoeq-container", "rent-object-record",
                     "player idnum %ld obj vnum %d locate %d not equipped; "
                     "contents restored into the container in inventory",
                     GET_IDNUM(ch), GET_OBJ_VNUM(temp), locate);
        obj_from_char(temp);
        temp->contains = NULL;
        for (;cont_row[0];cont_row[0] = obj1) {
          obj1 = cont_row[0]->next_content;
          obj_to_obj(cont_row[0], temp);
        }
        obj_to_char(temp, ch);
      } else { /* object isn't container -> empty content list */
        for (;cont_row[0];cont_row[0] = obj1) {
          obj1 = cont_row[0]->next_content;
          obj_to_char(cont_row[0], ch);
        }
        cont_row[0] = NULL;
      }
    }
  } else { /* locate <= 0 */
    for (j = MAX_BAG_ROWS-1;j > -locate;j--)
      if (cont_row[j]) { /* no container -> back to ch's inventory */
        for (;cont_row[j];cont_row[j] = obj1) {
          obj1 = cont_row[j]->next_content;
          obj_to_char(cont_row[j], ch);
        }
        cont_row[j] = NULL;
      }

    if (j == -locate && cont_row[j]) { /* content list existing */
      if (GET_OBJ_TYPE(temp) == ITEM_CONTAINER) {
        /* take item ; fill ; give to char again */
        obj_from_char(temp);
        temp->contains = NULL;
        for (;cont_row[j];cont_row[j] = obj1) {
          obj1 = cont_row[j]->next_content;
          obj_to_obj(cont_row[j], temp);
        }
        obj_to_char(temp, ch); /* add to inv first ... */
      } else { /* object isn't container -> empty content list */
        for (;cont_row[j];cont_row[j] = obj1) {
          obj1 = cont_row[j]->next_content;
          obj_to_char(cont_row[j], ch);
        }
        cont_row[j] = NULL;
      }
    }

    if (locate < 0 && locate >= -MAX_BAG_ROWS) {
      /* let obj be part of content list
         but put it at the list's end thus having the items
         in the same order as before renting */
      obj_from_char(temp);
      if ((obj1 = cont_row[-locate-1])) {
        while (obj1->next_content)
          obj1 = obj1->next_content;
        obj1->next_content = temp;
      } else
        cont_row[-locate-1] = temp;
    } else if (locate < -MAX_BAG_ROWS) {
      /* Crash_save() writes any depth, but only MAX_BAG_ROWS levels are
       * rebuilt: this object stays loose in inventory. */
      TB_WARN_MSG("objsave", "rent-nesting", "rent-object-record",
                  "player idnum %ld obj vnum %d nested %d deep (max %d); "
                  "left in inventory", GET_IDNUM(ch), GET_OBJ_VNUM(temp),
                  -locate, MAX_BAG_ROWS);
    }
  } /* locate less than zero */

  return TRUE;
}

