/**************************************************************************
*  File: house.c                                           Part of tbaMUD *
*  Usage: Handling of player houses.                                      *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/**
 * @file house.c
 * Player houses: the house control table (lib/etc/hcontrol), each house's
 * object file (lib/house/<vnum>.house), and the hcontrol/house commands.
 *
 * Who controls the input: hcontrol is a format header and a raw binary
 * array of struct house_control_rec (see house.h and hcontrol_header
 * below), and every field in it is untrusted on read; House_boot() checks
 * the header and validates each record before it is loaded. House
 * object files use the rent object-record format and go through
 * objsave_parse_objects_from(). Guest names come from immortals and house
 * owners and are resolved through the player index.
 *
 * Lifetime: house_control[] is a fixed static table of num_of_houses
 * records, owned here. Both hcontrol and the object files are written
 * through staged files and committed only after a complete write.
 * Objects are restored flat into the house room: container nesting is not
 * kept across a reload (House_save() writes every record with Loc 0).
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "db.h"
#include "interpreter.h"
#include "house.h"
#include "constants.h"
#include "modify.h"
#include <stdint.h>	/* uint32_t: the hcontrol header's fixed-width fields */

/* local (file scope only) globals */
static struct house_control_rec house_control[MAX_HOUSES];
static int num_of_houses = 0;

/* hcontrol file format (B2-23). Format 2 is a struct hcontrol_header and
 * then record_count records. A record is still an in-memory image of
 * struct house_control_rec, whose layout depends on the compiler ABI
 * (house.h), so the header says which layout wrote it: a build with
 * another record size or format version refuses the file (hcontrol-format)
 * instead of misreading every field. The header's numbers are in host
 * byte order, so a file from a host of the other byte order fails the
 * version check as well. Format 1 (legacy) is the bare record array with
 * no header; it is still read, and written back as format 2 at the next
 * save. Bump HCONTROL_FORMAT_VERSION whenever house_control_rec changes. */
#define HCONTROL_FORMAT_VERSION	2
#define HCONTROL_MAGIC_LEN	8

struct hcontrol_header {
  unsigned char magic[HCONTROL_MAGIC_LEN];	/* hcontrol_magic */
  uint32_t version;		/* HCONTROL_FORMAT_VERSION */
  uint32_t record_size;		/* sizeof(struct house_control_rec) */
  uint32_t record_count;	/* records after the header */
};

/* A format 1 file starts with a record's room_vnum. 0xFFFF is NOWHERE,
 * never a house vnum, and these bytes would also make that record's
 * exit_num 'H','C' (over 17000, far outside DIR_COUNT), so no format 1
 * file House_boot() could load starts with this magic. */
static const unsigned char hcontrol_magic[HCONTROL_MAGIC_LEN] =
  { 0xFF, 0xFF, 'T', 'B', 'H', 'C', '\r', '\n' };

/* hcontrol_read_header() results. */
#define HCONTROL_LEGACY		0	/* format 1: no header; stream rewound */
#define HCONTROL_HEADER		1	/* format 2: header read and checked */
#define HCONTROL_UNREADABLE	2	/* already reported; load nothing */

/* TRUE when House_boot() could not read HCONTROL_FILE (it could not be
 * opened, or its header is one this build cannot use). The file may hold
 * houses that were never loaded, so House_save_control() must not replace
 * it with the in-memory table. */
static int hcontrol_unread = FALSE;

/* local functions */
static int House_get_filename(room_vnum vnum, char *filename, size_t maxlen);
static int House_load(room_vnum vnum);
static void House_restore_weight(struct obj_data *obj);
static void House_delete_file(room_vnum vnum);
static int find_house(room_vnum vnum);
static void House_save_control(void);
static void hcontrol_build_house(struct char_data *ch, char *arg);
static void hcontrol_destroy_house(struct char_data *ch, char *arg);
static void hcontrol_pay_house(struct char_data *ch, char *arg);
static void House_listrent(struct char_data *ch, room_vnum vnum);
/* CONVERSION code starts here -- see comment below. */
static int ascii_convert_house(struct char_data *ch, obj_vnum vnum);
static void hcontrol_convert_houses(struct char_data *ch);
static struct obj_data *Obj_from_store(struct obj_file_elem object, int *location);
/* CONVERSION code ends here -- see comment below. */

/* First, the basics: finding the filename; loading/saving objects */
/* Return a filename given a house vnum */
static int House_get_filename(room_vnum vnum, char *filename, size_t maxlen)
{
  if (vnum == NOWHERE)
    return (0);

  tb_snprintf(filename, maxlen, LIB_HOUSE"%d.house", vnum);
  return (1);
}

/* Load all objects for a house */
static int House_load(room_vnum vnum)
{
  FILE *fl;
  char filename[MAX_STRING_LENGTH];
	obj_save_data *loaded, *current;
  room_rnum rnum;

  if ((rnum = real_room(vnum)) == NOWHERE)
    return (0);
  if (!House_get_filename(vnum, filename, sizeof(filename)))
    return (0);
  if (!(fl = fopen(filename, "r"))) {
    int saved_errno = errno;
    if (saved_errno != ENOENT)
      log("SYSERR: House_load could not open %s for house #%d: %s (errno=%d)",
          filename, vnum, strerror(saved_errno), saved_errno);
    return (0);
  }

	{
	  char owner[32];
	  tb_snprintf(owner, sizeof(owner), "house vnum %d", vnum);
	  loaded = objsave_parse_objects_from(fl, filename, owner);
	}

	for (current = loaded; current != NULL; current = current->next)
    obj_to_room(current->obj, rnum);

	/* now it's safe to free the obj_save_data list - all members of it
	 * have been put in the correct lists by obj_to_room()
	 */
	while (loaded != NULL) {
		current = loaded;
		loaded = loaded->next;
		free(current);
	}

  tb_fclose(fl);

  return (1);
}

/** Save all objects for a house (recursive). Assumes fp is open.
 * Each object's weight is subtracted from its containers while saving, so
 * the initial call MUST be followed by House_restore_weight() on the same
 * tree, on failure too. Every object is visited even after a failed record
 * so that pass stays exact. Records are written flat (Loc 0): container
 * nesting is not kept across a house reload.
 * @return 1 only when every record in the tree was written (failures below
 * the head used to be ignored); objsave_save_obj_record() reports each. */
int House_save(struct obj_data *obj, FILE *fp)
{
  struct obj_data *tmp;
  int ok = 1;

  if (obj) {
    if (!House_save(obj->contains, fp))
      ok = 0;
    if (!House_save(obj->next_content, fp))
      ok = 0;
    if (!objsave_save_obj_record(obj, fp, 0))
      ok = 0;

    for (tmp = obj->in_obj; tmp; tmp = tmp->in_obj)
      GET_OBJ_WEIGHT(tmp) -= GET_OBJ_WEIGHT(obj);
  }
  return ok;
}

/* restore weight of containers after House_save has changed them for saving */
static void House_restore_weight(struct obj_data *obj)
{
  if (obj) {
    House_restore_weight(obj->contains);
    House_restore_weight(obj->next_content);
    if (obj->in_obj)
      GET_OBJ_WEIGHT(obj->in_obj) += GET_OBJ_WEIGHT(obj);
  }
}

/* Save all objects in a house. The object file is staged beside the live
 * one and replaced only after a complete write, so a failed save keeps the
 * previous contents. ROOM_HOUSE_CRASH stays set on failure, so
 * House_save_all() retries. */
void House_crashsave(room_vnum vnum)
{
  int rnum;
  char buf[MAX_STRING_LENGTH], temp[MAX_STRING_LENGTH + 16];
  FILE *fp;

  if ((rnum = real_room(vnum)) == NOWHERE) {
    TB_ERROR_MSG("house", "crashsave", "house-file",
                 "house vnum %d has no room; objects not saved", vnum);
    return;
  }
  if (!House_get_filename(vnum, buf, sizeof(buf)))
    return;
  if (!(fp = objsave_open_staged(buf, temp, sizeof(temp)))) {
    TB_ERROR_MSG("house", "crashsave", buf,
                 "house vnum %d objects not saved; live file kept", vnum);
    return;
  }
  if (!House_save(world[rnum].contents, fp)) {
    House_restore_weight(world[rnum].contents);
    objsave_abort_staged(fp, temp);
    TB_ERROR_MSG("house", "crashsave", buf,
                 "house vnum %d object write failed; live file kept", vnum);
    return;
  }
  if (!objsave_commit_staged(fp, temp, buf)) {
    House_restore_weight(world[rnum].contents);
    TB_ERROR_MSG("house", "crashsave", buf,
                 "house vnum %d objects not committed; live file kept", vnum);
    return;
  }
  House_restore_weight(world[rnum].contents);
  REMOVE_BIT_AR(ROOM_FLAGS(rnum), ROOM_HOUSE_CRASH);
}


/* Delete a house save file */
static void House_delete_file(room_vnum vnum)
{
  char filename[MAX_INPUT_LENGTH];
  FILE *fl;

  if (!House_get_filename(vnum, filename, sizeof(filename)))
    return;
  if (!(fl = fopen(filename, "rb"))) {
    if (errno != ENOENT)
      { int saved_errno = errno; log("SYSERR: house %d open-for-delete failed for %s: %s (errno=%d)", vnum, filename, strerror(saved_errno), saved_errno); }
    return;
  }
  tb_fclose(fl);
  if (remove(filename) < 0) {
    int saved_errno = errno;
    log("SYSERR: house %d remove failed for %s: %s (errno=%d)", vnum, filename, strerror(saved_errno), saved_errno);
  }
}

/* List all objects in a house file */
static void House_listrent(struct char_data *ch, room_vnum vnum)
{
  FILE *fl;
  char filename[MAX_STRING_LENGTH];
  char buf[MAX_STRING_LENGTH];
	obj_save_data *loaded, *current;
	int listed = 0, total = 0;
	struct tb_buf out;
	static const char truncated[] = "*** listing truncated; see server log ***\r\n";

  if (!House_get_filename(vnum, filename, sizeof(filename)))
    return;
  if (!(fl = fopen(filename, "rb"))) {
    int saved_errno = errno;
    if (saved_errno == ENOENT)
      send_to_char(ch, "No objects on file for house #%d.\r\n", vnum);
    else {
      log("SYSERR: House_listrent could not open %s for house #%d: %s (errno=%d)",
          filename, vnum, strerror(saved_errno), saved_errno);
      send_to_char(ch, "House object file exists but could not be opened; see server log.\r\n");
    }
    return;
  }
  /* Bounded by buf: a house may hold any number of objects with
   * builder- or restring-length descriptions. */
  tb_buf_init(&out, buf, sizeof(buf) - sizeof(truncated));
	tb_buf_appendf(&out, "filename: %s\r\n", filename);

	{
	  char owner[32];
	  tb_snprintf(owner, sizeof(owner), "house vnum %d", vnum);
	  loaded = objsave_parse_objects_from(fl, filename, owner);
	}

	for (current = loaded; current != NULL; current = current->next) {
	  total++;
	  if (!tb_buf_truncated(&out) &&
	      tb_buf_appendf(&out, " [%5d] (%5dau) %s\r\n",
	        GET_OBJ_VNUM(current->obj), GET_OBJ_RENT(current->obj),
	        current->obj->short_description ? current->obj->short_description
	                                        : "<no short description>"))
	    listed++;
	}
	if (tb_buf_truncated(&out)) {
	  TB_WARN_MSG("house", "listrent-truncate", filename,
	              "house vnum %d: %d objects listed of %d; listing exceeds %zu bytes",
	              vnum, listed, total, sizeof(buf));
	  strlcat(buf, truncated, sizeof(buf));
	}

	/* now it's safe to free the obj_save_data list - all members of it
	 * have been put in the correct lists by obj_to_room()
	 */
	while (loaded != NULL) {
		current = loaded;
		loaded = loaded->next;
		extract_obj(current->obj);
		free(current);
	}

	/* buf lives in this frame, but a listing longer than one page is shown
	 * after we return (the immortal pages on), so the pager must keep its
	 * own copy; without keep_internal it pointed into the dead frame. */
	page_string(ch->desc, buf, TRUE);
  tb_fclose(fl);
}

/* Functions for house administration (creation, deletion, etc. */
static int find_house(room_vnum vnum)
{
  int i;

  for (i = 0; i < num_of_houses; i++)
    if (house_control[i].vnum == vnum)
      return (i);

  return (NOWHERE);
}

/** Save the house control records to HCONTROL_FILE.
 * The file used to be truncated in place with fopen("wb"): a failed or
 * short write left it empty or partial, and the next boot then lost every
 * house. It is now staged beside the live file and committed only after a
 * complete write; on failure the live file is kept and the error reported
 * (hcontrol-save). The in-memory table is unchanged either way, so the
 * next successful save writes the current state. The file is written in
 * format 2 (hcontrol_header, then the records). If boot could not read
 * the live file (hcontrol_unread), it is not replaced at all. */
static void House_save_control(void)
{
  FILE *fl;
  char temp[MAX_INPUT_LENGTH];
  struct hcontrol_header hdr;

  /* Replacing a file boot could not read would silently destroy every
   * house in it; an admin repairs or moves it, then reboots. */
  if (hcontrol_unread) {
    TB_ERROR_MSG("house", "hcontrol-save", HCONTROL_FILE,
                 "%d house records not saved: boot could not read this file, "
                 "which may hold houses that were not loaded; live file kept "
                 "for repair", num_of_houses);
    return;
  }
  if (!(fl = objsave_open_staged(HCONTROL_FILE, temp, sizeof(temp)))) {
    TB_ERROR_MSG("house", "hcontrol-save", HCONTROL_FILE,
                 "%d house records not saved; live file kept", num_of_houses);
    return;
  }
  /* Zeroed first, so no undefined byte reaches the file. */
  memset(&hdr, 0, sizeof(hdr));
  memcpy(hdr.magic, hcontrol_magic, sizeof(hdr.magic));
  hdr.version = HCONTROL_FORMAT_VERSION;
  hdr.record_size = (uint32_t) sizeof(struct house_control_rec);
  hdr.record_count = (uint32_t) num_of_houses;
  /* write the header, then all the house control recs in one fell swoop. */
  if (fwrite(&hdr, sizeof(hdr), 1, fl) != 1 ||
      fwrite(house_control, sizeof(struct house_control_rec), num_of_houses, fl) != (size_t)num_of_houses) {
    TB_ERROR_ERRNO("house", "hcontrol-save", HCONTROL_FILE,
                   "short write of %d house records; live file kept",
                   num_of_houses);
    objsave_abort_staged(fl, temp);
    return;
  }

  if (!objsave_commit_staged(fl, temp, HCONTROL_FILE))
    TB_ERROR_MSG("house", "hcontrol-save", HCONTROL_FILE,
                 "%d house records not committed; live file kept",
                 num_of_houses);
}

/* Report one hcontrol record that House_boot() will not load. */
#define HOUSE_BOOT_SKIP(rec, reason) \
  TB_ERROR_MSG("house", "boot-skip", HCONTROL_FILE, \
               "house vnum %d atrium %d owner idnum %ld: %s; record skipped", \
               (rec).vnum, (rec).atrium, (rec).owner, (reason))

/** Read and check the format 2 header at the start of hcontrol.
 * The file is untrusted. A file that does not start with hcontrol_magic
 * is format 1 (reported once as hcontrol-legacy) and fl is rewound to its
 * first record. A header from another format version or record layout,
 * or a damaged one, is reported (hcontrol-format) and nothing may load.
 * @param fl hcontrol, opened for reading and positioned at its start.
 * @param[out] count Records the header promises (HCONTROL_HEADER only).
 * @return HCONTROL_HEADER, HCONTROL_LEGACY or HCONTROL_UNREADABLE. */
static int hcontrol_read_header(FILE *fl, int *count)
{
  struct hcontrol_header hdr;
  size_t got;

  memset(&hdr, 0, sizeof(hdr));
  got = fread(&hdr, 1, sizeof(hdr), fl);
  if (ferror(fl)) {
    TB_ERROR_ERRNO("house", "hcontrol-read", HCONTROL_FILE,
                   "header read failed; no houses loaded");
    return HCONTROL_UNREADABLE;
  }
  if (got < HCONTROL_MAGIC_LEN ||
      memcmp(hdr.magic, hcontrol_magic, HCONTROL_MAGIC_LEN) != 0) {
    if (fseek(fl, 0L, SEEK_SET) != 0) {
      TB_ERROR_ERRNO("house", "hcontrol-read", HCONTROL_FILE,
                     "cannot rewind format 1 file; no houses loaded");
      return HCONTROL_UNREADABLE;
    }
    TB_WARN_MSG("house", "hcontrol-legacy", HCONTROL_FILE,
                "no format header: format 1, read as %zu-byte records; "
                "written as format %d at the next save",
                sizeof(struct house_control_rec), HCONTROL_FORMAT_VERSION);
    return HCONTROL_LEGACY;
  }
  if (got != sizeof(hdr)) {
    TB_ERROR_MSG("house", "hcontrol-format", HCONTROL_FILE,
                 "header truncated (%zu of %zu bytes); no houses loaded",
                 got, sizeof(hdr));
    return HCONTROL_UNREADABLE;
  }
  if (hdr.version != HCONTROL_FORMAT_VERSION ||
      hdr.record_size != sizeof(struct house_control_rec)) {
    TB_ERROR_MSG("house", "hcontrol-format", HCONTROL_FILE,
                 "format %lu with %lu-byte records, but this build reads "
                 "format %d with %zu-byte records (written on another "
                 "platform or struct layout?); no houses loaded",
                 (unsigned long) hdr.version, (unsigned long) hdr.record_size,
                 HCONTROL_FORMAT_VERSION, sizeof(struct house_control_rec));
    return HCONTROL_UNREADABLE;
  }
  if (hdr.record_count > INT_MAX) {
    TB_ERROR_MSG("house", "hcontrol-format", HCONTROL_FILE,
                 "header promises %lu records; no houses loaded",
                 (unsigned long) hdr.record_count);
    return HCONTROL_UNREADABLE;
  }
  *count = (int) hdr.record_count;
  return HCONTROL_HEADER;
}

/** Load the house control records, their object files and room flags.
 * Called once from boot_db(). HCONTROL_FILE holds raw images of struct
 * house_control_rec (see house.h) after a format header (see
 * hcontrol_read_header(); a headerless format 1 file is still read). A
 * header this build cannot use loads nothing and leaves the file alone,
 * and every later save refuses to replace it (hcontrol_unread). Each
 * record is validated:
 * owner, house and atrium rooms, uniqueness, exit, guest count and mode.
 * A record that fails is reported (boot-skip / hcontrol-record) and not
 * loaded. If any record was skipped, or the file could not be read
 * completely, hcontrol is NOT rewritten: a transient problem (a truncated
 * player index, a zone file that failed to load) must not delete houses
 * permanently. The file is rewritten only after a clean boot, which
 * normalises it to the records that were loaded. */
void House_boot(void)
{
  struct house_control_rec temp_house;
  room_rnum real_house, real_atrium;
  FILE *fl;
  int record = 0, skipped = 0;
  int expected = -1;	/* records the header promises; -1: format 1, to EOF */
  size_t got;

  memset((char *)house_control,0,sizeof(struct house_control_rec)*MAX_HOUSES);
  num_of_houses = 0;
  hcontrol_unread = FALSE;

  if (!(fl = fopen(HCONTROL_FILE, "rb"))) {
    if (errno == ENOENT)
      log("   No houses to load. File '%s' does not exist.", HCONTROL_FILE);
    else {
      TB_ERROR_ERRNO("house", "hcontrol-open", HCONTROL_FILE,
                     "house control file unreadable; no houses loaded");
      hcontrol_unread = TRUE;
    }
    return;
  }
  if (hcontrol_read_header(fl, &expected) == HCONTROL_UNREADABLE) {
    hcontrol_unread = TRUE;
    tb_fclose(fl);
    return;
  }
  for (;;) {
    /* Format 2 ends after the records its header promises; more bytes
     * mean the header or the file is damaged. */
    if (expected >= 0 && record == expected) {
      if (fgetc(fl) != EOF) {
        TB_ERROR_MSG("house", "hcontrol-read", HCONTROL_FILE,
                     "data after the %d records the header promises",
                     expected);
        skipped++;
      } else if (ferror(fl)) {
        TB_ERROR_ERRNO("house", "hcontrol-read", HCONTROL_FILE,
                       "read failed after %d records", record);
        skipped++;
      }
      break;
    }
    got = fread(&temp_house, 1, sizeof(struct house_control_rec), fl);
    if (got != sizeof(struct house_control_rec)) {
      if (ferror(fl)) {
        TB_ERROR_ERRNO("house", "hcontrol-read", HCONTROL_FILE,
                       "read failed after %d records", record);
        skipped++;
      } else if (got != 0) {
        TB_ERROR_MSG("house", "hcontrol-read", HCONTROL_FILE,
                     "trailing partial record (%zu of %zu bytes) after %d records",
                     got, sizeof(struct house_control_rec), record);
        skipped++;
      } else if (expected >= 0) {
        TB_ERROR_MSG("house", "hcontrol-read", HCONTROL_FILE,
                     "file ends after %d of the %d records the header promises",
                     record, expected);
        skipped++;
      }
      break;
    }
    record++;

    if (num_of_houses >= MAX_HOUSES) {
      TB_ERROR_MSG("house", "hcontrol-read", HCONTROL_FILE,
                   "more than MAX_HOUSES (%d) records; record %d and later ignored",
                   MAX_HOUSES, record);
      skipped++;
      break;
    }

    if (get_name_by_id(temp_house.owner) == NULL) {
      HOUSE_BOOT_SKIP(temp_house, "owner not in player index");
      skipped++;
      continue;
    }

    if ((real_house = real_room(temp_house.vnum)) == NOWHERE) {
      HOUSE_BOOT_SKIP(temp_house, "house room does not exist");
      skipped++;
      continue;
    }

    if (find_house(temp_house.vnum) != NOWHERE) {
      HOUSE_BOOT_SKIP(temp_house, "duplicate house vnum");
      skipped++;
      continue;
    }

    if ((real_atrium = real_room(temp_house.atrium)) == NOWHERE) {
      HOUSE_BOOT_SKIP(temp_house, "atrium room does not exist");
      skipped++;
      continue;
    }

    if (temp_house.exit_num < 0 || temp_house.exit_num >= DIR_COUNT) {
      HOUSE_BOOT_SKIP(temp_house, "exit direction out of range");
      skipped++;
      continue;
    }

    if (TOROOM(real_house, temp_house.exit_num) != real_atrium) {
      HOUSE_BOOT_SKIP(temp_house, "exit does not lead to the atrium");
      skipped++;
      continue;
    }

    /* num_of_guests comes straight from the binary hcontrol file and bounds
     * every guests[] loop (House_can_enter, House_list_guests, do_house);
     * do_house also appends at guests[num_of_guests]. HOUSE_PRIVATE is the
     * only ownership mode the code implements. */
    if (!VALID_HOUSE_GUESTS(&temp_house) || temp_house.mode != HOUSE_PRIVATE) {
      TB_ERROR_MSG("house", "hcontrol-record", HCONTROL_FILE,
                   "house vnum %d owner idnum %ld num_of_guests %d (max %d) "
                   "mode %d; record skipped", temp_house.vnum, temp_house.owner,
                   temp_house.num_of_guests, MAX_GUESTS, temp_house.mode);
      skipped++;
      continue;
    }

    house_control[num_of_houses++] = temp_house;

    SET_BIT_AR(ROOM_FLAGS(real_house), ROOM_HOUSE);
    SET_BIT_AR(ROOM_FLAGS(real_house), ROOM_PRIVATE);
    SET_BIT_AR(ROOM_FLAGS(real_atrium), ROOM_ATRIUM);
    House_load(temp_house.vnum);
  }

  tb_fclose(fl);
  if (skipped) {
    TB_ERROR_MSG("house", "hcontrol-keep", HCONTROL_FILE,
                 "%d of %d records not loaded; file left unchanged at boot "
                 "(the next hcontrol or house change rewrites it without them)",
                 skipped, record);
    return;
  }
  House_save_control();
}

#undef HOUSE_BOOT_SKIP


/* "House Control" functions */
static const char *HCONTROL_FORMAT =
"Usage: hcontrol build <house vnum> <exit direction> <player name>\r\n"
"       hcontrol destroy <house vnum>\r\n"
"       hcontrol pay <house vnum>\r\n"
"       hcontrol show [house vnum | .]\r\n";

void hcontrol_list_houses(struct char_data *ch, char *arg)
{
  int i;
  char *temp;
  char built_on[128], last_pay[128], own_name[MAX_NAME_LENGTH + 1];

	if (arg && *arg) {
		room_vnum toshow;

		if (*arg == '.')
			toshow = GET_ROOM_VNUM(IN_ROOM(ch));
		else
			toshow = tb_parse_int_or_zero(arg);

	  if ((i = find_house(toshow)) == NOWHERE) {
  	  send_to_char(ch, "Unknown house, \"%s\".\r\n", arg);
	    return;
	  }
		House_listrent(ch, toshow);
		return;
	}

  if (!num_of_houses) {
    send_to_char(ch, "No houses have been defined.\r\n");
    return;
  }
  send_to_char(ch,
	"Address  Atrium  Build Date       Guests  Owner        Last Paymt\r\n"
	"-------  ------  ---------------  ------  ------------ ---------------\r\n");

  for (i = 0; i < num_of_houses; i++) {
    /* Avoid seeing <UNDEF> entries from self-deleted people. -gg 6/21/98 */
    if ((temp = get_name_by_id(house_control[i].owner)) == NULL)
      continue;

    if (house_control[i].built_on) {
      tb_strftime(built_on, sizeof(built_on), "%a %b %d %Y", localtime(&(house_control[i].built_on)));
    } else
      strlcpy(built_on, "Unknown", sizeof(built_on)); /* strcpy: OK */

    if (house_control[i].last_payment) {
      tb_strftime(last_pay, sizeof(last_pay), "%a %b %d %Y", localtime(&(house_control[i].last_payment)));
    } else
      strlcpy(last_pay, "None", sizeof(last_pay));	/* strcpy: OK */

    /* Now we need a copy of the owner's name to capitalize. -gg 6/21/98 */
    strlcpy(own_name, temp, sizeof(own_name));	/* strcpy: OK (names guaranteed <= MAX_NAME_LENGTH+1) */
    send_to_char(ch, "%7d %7d  %-15s    %2d    %-12s %s\r\n",
	    house_control[i].vnum, house_control[i].atrium, built_on,
	    house_control[i].num_of_guests, CAP(own_name), last_pay);

    House_list_guests(ch, i, TRUE);
  }
}

static void hcontrol_build_house(struct char_data *ch, char *arg)
{
  char arg1[MAX_INPUT_LENGTH];
  struct house_control_rec temp_house;
  room_vnum virt_house, virt_atrium;
  room_rnum real_house, real_atrium;
  sh_int exit_num;
  long owner;

  if (num_of_houses >= MAX_HOUSES) {
    send_to_char(ch, "Max houses already defined.\r\n");
    return;
  }

  /* first arg: house's vnum */
  arg = one_argument(arg, arg1);
  if (!*arg1) {
    send_to_char(ch, "%s", HCONTROL_FORMAT);
    return;
  }
  virt_house = tb_parse_int_or_zero(arg1);
  if ((real_house = real_room(virt_house)) == NOWHERE) {
    send_to_char(ch, "No such room exists.\r\n");
    return;
  }
  if ((find_house(virt_house)) != NOWHERE) {
    send_to_char(ch, "House already exists.\r\n");
    return;
  }

  /* second arg: direction of house's exit */
  arg = one_argument(arg, arg1);
  if (!*arg1) {
    send_to_char(ch, "%s", HCONTROL_FORMAT);
    return;
  }
  if ((exit_num = search_block(arg1, dirs, FALSE)) < 0) {
    send_to_char(ch, "'%s' is not a valid direction.\r\n", arg1);
    return;
  }
  if (TOROOM(real_house, exit_num) == NOWHERE) {
    send_to_char(ch, "There is no exit %s from room %d.\r\n", dirs[exit_num], virt_house);
    return;
  }

  real_atrium = TOROOM(real_house, exit_num);
  virt_atrium = GET_ROOM_VNUM(real_atrium);

  if (TOROOM(real_atrium, rev_dir[exit_num]) != real_house) {
    send_to_char(ch, "A house's exit must be a two-way door.\r\n");
    return;
  }

  /* third arg: player's name */
  one_argument(arg, arg1);
  if (!*arg1) {
    send_to_char(ch, "%s", HCONTROL_FORMAT);
    return;
  }
  if ((owner = get_id_by_name(arg1)) < 0) {
    send_to_char(ch, "Unknown player '%s'.\r\n", arg1);
    return;
  }

  /* House_save_control() fwrite()s the whole record, so the spare fields
   * and struct padding must be defined: zero the record before filling it
   * rather than writing stack garbage into hcontrol. */
  memset(&temp_house, 0, sizeof(temp_house));
  temp_house.mode = HOUSE_PRIVATE;
  temp_house.vnum = virt_house;
  temp_house.atrium = virt_atrium;
  temp_house.exit_num = exit_num;
  temp_house.built_on = time(0);
  temp_house.last_payment = 0;
  temp_house.owner = owner;
  temp_house.num_of_guests = 0;

  house_control[num_of_houses++] = temp_house;

  SET_BIT_AR(ROOM_FLAGS(real_house), ROOM_HOUSE);
  SET_BIT_AR(ROOM_FLAGS(real_house), ROOM_PRIVATE);
  SET_BIT_AR(ROOM_FLAGS(real_atrium), ROOM_ATRIUM);
  House_crashsave(virt_house);

  send_to_char(ch, "House built.  Mazel tov!\r\n");
  House_save_control();
}

static void hcontrol_destroy_house(struct char_data *ch, char *arg)
{
  int i, j;
  room_rnum real_atrium, real_house;

  if (!*arg) {
    send_to_char(ch, "%s", HCONTROL_FORMAT);
    return;
  }
  if ((i = find_house(tb_parse_int_or_zero(arg))) == NOWHERE) {
    send_to_char(ch, "Unknown house.\r\n");
    return;
  }
  if ((real_atrium = real_room(house_control[i].atrium)) == NOWHERE)
    log("SYSERR: House %d had invalid atrium %d!", tb_parse_int_or_zero(arg), house_control[i].atrium);
  else
    REMOVE_BIT_AR(ROOM_FLAGS(real_atrium), ROOM_ATRIUM);

  if ((real_house = real_room(house_control[i].vnum)) == NOWHERE)
    log("SYSERR: House %d had invalid vnum %d!", tb_parse_int_or_zero(arg), house_control[i].vnum);
  else {
    REMOVE_BIT_AR(ROOM_FLAGS(real_house), ROOM_HOUSE);
    REMOVE_BIT_AR(ROOM_FLAGS(real_house), ROOM_PRIVATE);
    REMOVE_BIT_AR(ROOM_FLAGS(real_house), ROOM_HOUSE_CRASH);
  }
  House_delete_file(house_control[i].vnum);

  for (j = i; j < num_of_houses - 1; j++)
    house_control[j] = house_control[j + 1];

  num_of_houses--;

  send_to_char(ch, "House deleted.\r\n");
  House_save_control();

  /* Now, reset the ROOM_ATRIUM flag on all existing houses' atriums, just in 
   * case the house we just deleted shared an atrium with another house. -JE */
  for (i = 0; i < num_of_houses; i++)
    if ((real_atrium = real_room(house_control[i].atrium)) != NOWHERE)
      SET_BIT_AR(ROOM_FLAGS(real_atrium), ROOM_ATRIUM);
}

static void hcontrol_pay_house(struct char_data *ch, char *arg)
{
  int i;

  if (!*arg)
    send_to_char(ch, "%s", HCONTROL_FORMAT);
  else if ((i = find_house(tb_parse_int_or_zero(arg))) == NOWHERE)
    send_to_char(ch, "Unknown house.\r\n");
  else {
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE, "Payment for house %s collected by %s.", arg, GET_NAME(ch));

    house_control[i].last_payment = time(0);
    House_save_control();
    send_to_char(ch, "Payment recorded.\r\n");
  }
}

/* The hcontrol command itself, used by imms to create/destroy houses */
ACMD(do_hcontrol)
{
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];

  half_chop(argument, arg1, arg2);

  if (is_abbrev(arg1, "build"))
    hcontrol_build_house(ch, arg2);
  else if (is_abbrev(arg1, "destroy"))
    hcontrol_destroy_house(ch, arg2);
  else if (is_abbrev(arg1, "pay"))
    hcontrol_pay_house(ch, arg2);
  else if (is_abbrev(arg1, "show"))
    hcontrol_list_houses(ch, arg2);
/* CONVERSION code starts here -- see comment below not in hcontrol_format. */
	else if (!str_cmp(arg1, "asciiconvert"))
    hcontrol_convert_houses(ch);
/* CONVERSION ends here -- read more below. */
  else
    send_to_char(ch, "%s", HCONTROL_FORMAT);
}

/* The house command, used by mortal house owners to assign guests */
ACMD(do_house)
{
  char arg[MAX_INPUT_LENGTH];
  int i, j, id;

  one_argument(argument, arg);

  if (!ROOM_FLAGGED(IN_ROOM(ch), ROOM_HOUSE))
    send_to_char(ch, "You must be in your house to set guests.\r\n");
  else if ((i = find_house(GET_ROOM_VNUM(IN_ROOM(ch)))) == NOWHERE)
    send_to_char(ch, "Um.. this house seems to be screwed up.\r\n");
  else if (GET_IDNUM(ch) != house_control[i].owner)
    send_to_char(ch, "Only the primary owner can set guests.\r\n");
  else if (!*arg)
    House_list_guests(ch, i, FALSE);
  else if ((id = get_id_by_name(arg)) < 0)
    send_to_char(ch, "No such player.\r\n");
  else if (id == GET_IDNUM(ch))
    send_to_char(ch, "It's your house!\r\n");
  else {
    for (j = 0; j < house_control[i].num_of_guests; j++)
      if (house_control[i].guests[j] == id) {
	/* Shift the later guests down one slot. Stop one short of the
	 * count: guests[num_of_guests] is past the list (and past guests[]
	 * itself when the list is full). Zero the vacated last slot so the
	 * spare entry written to hcontrol is not a stale idnum. */
	for (; j < house_control[i].num_of_guests - 1; j++)
	  house_control[i].guests[j] = house_control[i].guests[j + 1];
	house_control[i].num_of_guests--;
	house_control[i].guests[house_control[i].num_of_guests] = 0;
	House_save_control();
	send_to_char(ch, "Guest deleted.\r\n");
	return;
      }
    if (house_control[i].num_of_guests >= MAX_GUESTS) {
      send_to_char(ch, "You have too many guests.\r\n");
      return;
    }
    j = house_control[i].num_of_guests++;
    house_control[i].guests[j] = id;
    House_save_control();
    send_to_char(ch, "Guest added.\r\n");
  }
}

/* Misc. administrative functions */
/* crash-save all the houses */
void House_save_all(void)
{
  int i;
  room_rnum real_house;

  for (i = 0; i < num_of_houses; i++)
    if ((real_house = real_room(house_control[i].vnum)) != NOWHERE)
      if (ROOM_FLAGGED(real_house, ROOM_HOUSE_CRASH))
	House_crashsave(house_control[i].vnum);
}

/* note: arg passed must be house vnum, so there. */
int House_can_enter(struct char_data *ch, room_vnum house)
{
  int i, j;

  if (GET_LEVEL(ch) >= LVL_GRGOD || (i = find_house(house)) == NOWHERE)
    return (1);

  switch (house_control[i].mode) {
  case HOUSE_PRIVATE:
    if (GET_IDNUM(ch) == house_control[i].owner)
      return (1);
    for (j = 0; j < house_control[i].num_of_guests; j++)
      if (GET_IDNUM(ch) == house_control[i].guests[j])
	return (1);
  }

  return (0);
}

void House_list_guests(struct char_data *ch, int i, int quiet)
{
  int j, num_printed;
  char *temp;

  if (house_control[i].num_of_guests == 0) {
    if (!quiet)
      send_to_char(ch, "  Guests: None\r\n");
    return;
  }

  send_to_char(ch, "  Guests: ");

  for (num_printed = j = 0; j < house_control[i].num_of_guests; j++) {
    /* Avoid <UNDEF>. -gg 6/21/98 */
    if ((temp = get_name_by_id(house_control[i].guests[j])) == NULL)
      continue;

    num_printed++;
    send_to_char(ch, "%c%s ", UPPER(*temp), temp + 1);
  }

  if (num_printed == 0)
    send_to_char(ch, "all dead");

  send_to_char(ch, "\r\n");
}

/*************************************************************************
 * All code below this point and the code above, marked "CONVERSION"     *
 * can be removed after you have converted your house rent files using   *
 * the command                                                           *
 *   hcontrol asciiconvert                                               *
 *                                                                       *
 * You can only use this command as implementor.                         *
 * After you have converted your house files, I suggest a reboot, which  *
 * will let your house files load on the next bootup. -Welcor            *
 ************************************************************************/
/* Code for conversion to ascii house rent files. */
static void hcontrol_convert_houses(struct char_data *ch)
{
  int i;

	if (GET_LEVEL(ch) < LVL_IMPL)
		{
			send_to_char(ch, "Sorry, but you are not powerful enough to do that.\r\n");
			return;
		}


  if (!num_of_houses) {
    send_to_char(ch, "No houses have been defined.\r\n");
    return;
  }

	send_to_char(ch, "Converting houses:\r\n");

  for (i = 0; i < num_of_houses; i++) {
	  send_to_char(ch, "  %d", house_control[i].vnum);

	  if (!ascii_convert_house(ch, house_control[i].vnum))
	  {
	  	/* Let ascii_convert_house() tell about the error. */
	  	return;
	  }
	  else
	  {
	  	send_to_char(ch, "...done\r\n");
	  }
  }
	send_to_char(ch, "All done.\r\n");
}

static int ascii_convert_house(struct char_data *ch, obj_vnum vnum)
{
	FILE *in, *out;
	char infile[MAX_INPUT_LENGTH], *outfile;
	struct obj_data *tmp;
	int i, j=0, skipped = 0, saved;

  House_get_filename(vnum, infile, sizeof(infile));

	{
	  size_t outfile_size = strlen(infile) + 7;
	  CREATE(outfile, char, outfile_size);
	  tb_snprintf(outfile, outfile_size, "%s.ascii", infile);
	}

  if (!(in = fopen(infile, "r+b"))) {
    int saved_errno = errno;
    if (saved_errno == ENOENT)
      send_to_char(ch, "...no object file found\r\n");
    else {
      log("SYSERR: house conversion could not open %s: %s (errno=%d)",
          infile, strerror(saved_errno), saved_errno);
      send_to_char(ch, "...object file exists but could not be opened; see server log\r\n");
    }
    free(outfile);
    return (0);
  }

  if (!(out = fopen(outfile, "w"))) {
    int saved_errno = errno;
    log("SYSERR: house conversion could not create %s from %s: %s (errno=%d)",
        outfile, infile, strerror(saved_errno), saved_errno);
    send_to_char(ch, "...cannot open output file; see server log\r\n");
    free(outfile);
    tb_fclose(in);
    return (0);
  }

  for (;;) {
    struct obj_file_elem object;
    size_t nread = fread(&object, sizeof(struct obj_file_elem), 1, in);

    if (nread != 1) {
      if (ferror(in)) {
        int saved_errno = errno;
        log("SYSERR: house conversion read failed in %s at record %d: %s (errno=%d)",
            infile, j, strerror(saved_errno), saved_errno);
        send_to_char(ch, "...read error in house rent file; see server log.\r\n");
        free(outfile);
        tb_fclose(in);
        tb_fclose(out);
        return 0;
      }
      break;
    }

    /* The binary record's vnum comes from an old house file; its prototype
     * may have been deleted since. That is not a write error: skip and
     * report the record, as the ASCII loader does. */
    tmp = Obj_from_store(object, &i);
    if (!tmp) {
      TB_WARN_MSG("house", "convert-missing-proto", infile,
                  "house vnum %d record %d obj vnum %d has no prototype; skipped",
                  vnum, j + skipped, object.item_number);
      skipped++;
      continue;
    }
    /* Obj_from_store() built a live object (read_object() put it on
     * object_list) only to serialise it; release it whether or not the
     * write worked, or every converted object leaks into the world. */
    saved = objsave_save_obj_record(tmp, out, i);
    extract_obj(tmp);
    if (!saved) {
      TB_ERROR_MSG("house", "convert-write", outfile,
                   "house vnum %d record %d obj vnum %d not written; "
                   "conversion aborted", vnum, j + skipped, object.item_number);
      send_to_char(ch, "...write error in house rent file.\r\n");
      tb_fclose(in);
      tb_fclose(out);
      (void) remove(outfile);
      free(outfile);
      return 0;
    }
    j++;
  }

	tb_fprintf(out, "$~\n");

	tb_fclose(in);
	tb_fclose(out);

	free(outfile);

	send_to_char(ch, "...%d items", j);
	if (skipped)
	  send_to_char(ch, " (%d skipped, no prototype; see server log)", skipped);
	return 1;
}

/* The circle 3.1 function for reading rent files. No longer used by the rent system. */
static struct obj_data *Obj_from_store(struct obj_file_elem object, int *location)
{
  struct obj_data *obj;
  obj_rnum itemnum;
  int j, taeller;

  *location = 0;
  if ((itemnum = real_object(object.item_number)) == NOTHING)
    return (NULL);

  if ((obj = read_object(itemnum, REAL)) == NULL) {
    TB_ERROR_MSG("house", "convert-read-object", "Obj_from_store",
                 "obj vnum %d rnum %d could not be instantiated",
                 object.item_number, itemnum);
    return (NULL);
  }
#if USE_AUTOEQ
  *location = object.location;
#endif
  GET_OBJ_VAL(obj, 0) = object.value[0];
  GET_OBJ_VAL(obj, 1) = object.value[1];
  GET_OBJ_VAL(obj, 2) = object.value[2];
  GET_OBJ_VAL(obj, 3) = object.value[3];
  for(taeller = 0; taeller < EF_ARRAY_MAX; taeller++)
    GET_OBJ_EXTRA(obj)[taeller] = object.extra_flags[taeller];
  GET_OBJ_WEIGHT(obj) = object.weight;
  GET_OBJ_TIMER(obj) = object.timer;
  for(taeller = 0; taeller < AF_ARRAY_MAX; taeller++)
    GET_OBJ_AFFECT(obj)[taeller] = object.bitvector[taeller];

  for (j = 0; j < MAX_OBJ_AFFECT; j++)
    obj->affected[j] = object.affected[j];

  return (obj);
}
