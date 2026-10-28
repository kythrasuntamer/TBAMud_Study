/**************************************************************************
*  File: genzon.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - Zones.                                    *
*                                                                         *
*  Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer.            *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "genolc.h"
#include "genzon.h"
#include "dg_scripts.h"

/* local functions */
static int remove_cmd_from_list(struct reset_com **list, int pos, zone_vnum znum);

/* Only the temporary index is removed on failure; preserve the live index. */
static void cleanup_new_zone_index(const char *path)
{
  if (remove(path) != 0 && errno != ENOENT)
    TB_REPORT_ERRNO("remove", path);
}

/* Index entries start with a decimal zone number followed by a file suffix. */
static int parse_zone_index_number(const char *text, int *number)
{
  char *end;
  long value;
  if (!text || !number)
    return 0;
  while (isspace((unsigned char)*text))
    text++;
  errno = 0;
  value = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || value < 0 || value > INT_MAX ||
      (*end != '.' && !isspace((unsigned char)*end) && *end != '\0'))
    return 0;
  *number = (int)value;
  return 1;
}

/* real zone of room/mobile/object/shop given */
zone_rnum real_zone_by_thing(room_vnum vznum)
{
  zone_rnum bot, top, mid;
  int low, high;

  bot = 0;
  top = top_of_zone_table;

  if (genolc_zone_bottom(bot) > vznum || zone_table[top].top < vznum)
    return (NOWHERE);

  /* perform binary search on zone-table */
  while (bot <= top) {
    mid = (bot + top) / 2;

    /* Upper/lower bounds of the zone. */
    low = genolc_zone_bottom(mid);
    high = zone_table[mid].top;

    if (low <= vznum && vznum <= high)
      return mid;
    if (low > vznum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOWHERE);
}

/* Create one file of a brand-new zone.  "x" makes the open fail if the file
 * exists: a file already on disk belongs to some zone (perhaps one that is
 * not loaded), and zedit new must never truncate it. */
static FILE *open_new_zone_file(const char *path, zone_vnum vzone_num)
{
  FILE *fp = fopen(path, "wx");

  if (fp == NULL) {
    if (errno == EEXIST)
      TB_ERROR_ERRNO("olc", "zone-create-exists", path,
                     "zone %d: file already exists; not overwritten", vzone_num);
    else
      TB_ERROR_ERRNO("olc", "zone-create-file", path,
                     "zone %d: cannot create file", vzone_num);
  }
  return fp;
}

zone_rnum create_new_zone(zone_vnum vzone_num, room_vnum bottom, room_vnum top, const char **error)
{
  FILE *fp;
  struct zone_data *zone;
  int i, max_zone;
  zone_rnum rznum;
  char buf[MAX_STRING_LENGTH];

#if CIRCLE_UNSIGNED_INDEX
  max_zone = 655;
  if (vzone_num == NOWHERE) {
#else
  max_zone = 327;
  if (vzone_num < 0) {
#endif
    *error = "You can't make negative zones.\r\n";
    return NOWHERE;
   } else if (vzone_num > max_zone) {
#if CIRCLE_UNSIGNED_INDEX
    *error = "New zone cannot be higher than 655.\r\n";
#else
    *error = "New zone cannot be higher than 327.\r\n";
#endif
    return NOWHERE;
  } else if (bottom > top) {
    *error = "Bottom room cannot be greater than top room.\r\n";
    return NOWHERE;
  } else if (bottom <= 0) {
    *error = "Bottom room cannot be less then 0.\r\n";
    return NOWHERE;
  } else if (top >= IDXTYPE_MAX) {
    *error = "Top greater than IDXTYPE_MAX. (Commonly 65535)\r\n";
    return NOWHERE;
  }

  /* Check every loaded zone, the last one included.  zone_table must stay
   * free of duplicate numbers and overlapping vnum ranges: real_zone_by_thing()
   * binary-searches the ranges, and the files below are named by number. */
  for (i = 0; i <= top_of_zone_table; i++) {
    if (zone_table[i].number == vzone_num) {
      TB_WARN_MSG("olc", "zone-create-duplicate", "zone",
                  "zone %d already loaded at rnum %d; not created", vzone_num, i);
      *error = "That virtual zone already exists.\r\n";
      return NOWHERE;
    }
    if (bottom <= zone_table[i].top && top >= genolc_zone_bottom(i)) {
      TB_WARN_MSG("olc", "zone-create-overlap", "zone",
                  "zone %d range %d-%d overlaps zone %d (%d-%d); not created",
                  vzone_num, bottom, top, zone_table[i].number,
                  genolc_zone_bottom(i), zone_table[i].top);
      *error = "That room range overlaps an existing zone.\r\n";
      return NOWHERE;
    }
  }

  /* Create the zone file. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.zon", ZON_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new zone file.");
    *error = "Could not write zone file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "#%d\nNone~\nNew Zone~\n%d %d 30 2\nS\n$\n", vzone_num, bottom, top);
  tb_fclose(fp);

  /* Create the room file. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.wld", WLD_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new world file.");
    *error = "Could not write world file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "#%d\nThe Beginning~\nNot much here.\n~\n%d 0 0\nS\n$\n", bottom, vzone_num);
  tb_fclose(fp);

  /* Create the mobile file. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.mob", MOB_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new mob file.");
    *error = "Could not write mobile file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "$\n");
  tb_fclose(fp);

  /* Create the object file. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.obj", OBJ_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new obj file.");
    *error = "Could not write object file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "$\n");
  tb_fclose(fp);

  /* Create the shop file. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.shp", SHP_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new shop file.");
    *error = "Could not write shop file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "$~\n");
  tb_fclose(fp);

  /* Create the quests file */
  tb_snprintf(buf, sizeof(buf), "%s/%d.qst", QST_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new quest file");
    *error = "Could not write quest file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "$~\n");
  tb_fclose(fp);

  /* Create the trigger file. */
  tb_snprintf(buf, sizeof(buf), "%s/%d.trg", TRG_PREFIX, vzone_num);
  if (!(fp = open_new_zone_file(buf, vzone_num))) {
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Can't write new trigger file");
    *error = "Could not write trigger file.\r\n";
    return NOWHERE;
  }
  tb_fprintf(fp, "$~\n");
  tb_fclose(fp);

  /* Update index files.  The zone files exist by now, so the zone is still
   * created in memory if an index update fails; each failure was reported
   * by create_world_index(), and the implementor is told the zone will not
   * load at the next boot until the index is fixed. */
  {
    static const char *const index_types[] = { "qst", "zon", "wld", "mob", "obj", "shp", "trg" };
    int failed = 0;

    for (i = 0; i < (int)(sizeof(index_types) / sizeof(index_types[0])); i++)
      if (!create_world_index(vzone_num, index_types[i]))
        failed++;
    if (failed) {
      TB_ERROR_MSG("olc", "zone-create-index", "zone",
                   "zone %d: %d of 7 world index updates failed; the zone "
                   "will not fully load at the next boot", vzone_num, failed);
      mudlog(BRF, LVL_IMPL, TRUE,
             "SYSERR: OLC: zone %d created, but %d world index update(s) failed.",
             vzone_num, failed);
    }
  }

  /* Make a new zone in memory. This was the source of all the zedit new
   * crashes. It was happily overwriting the stack.  This new loop by Andrew
   * Helm fixes that problem and is more understandable at the same time.
   *
   * The variable is 'top_of_zone_table_table + 2' because we need record 0
   * through top_of_zone (top_of_zone_table + 1 items) and a new one which
   * makes it top_of_zone_table + 2 elements large. */
  RECREATE(zone_table, struct zone_data, top_of_zone_table + 2);
  zone_table[top_of_zone_table + 1].number = 32000;

  if (vzone_num > zone_table[top_of_zone_table].number)
    rznum = top_of_zone_table + 1;
  else {
    int j, room;
    for (i = top_of_zone_table + 1; i > 0 && vzone_num < zone_table[i - 1].number; i--) {
      zone_table[i] = zone_table[i - 1];
      for (j = zone_table[i].bot; j <= zone_table[i].top; j++)
        if ((room = real_room(j)) != NOWHERE)
          world[room].zone++;
    }
rznum = i;
  }
  zone = &zone_table[rznum];

  /* Ok, insert the new zone here. */
  zone->name = strdup("New Zone");
  zone->number = vzone_num;
  zone->builders = strdup("None");
  zone->bot = bottom;
  zone->top = top;
  zone->lifespan = 30;
  zone->age = 0;
  zone->reset_mode = 2;
  zone->min_level = -1;
  zone->max_level = -1;

  for (i=0; i<ZN_ARRAY_MAX; i++)  zone->zone_flags[i] = 0;

  /* No zone commands, just terminate it with an 'S' */
  CREATE(zone->cmd, struct reset_com, 1);
  zone->cmd[0].command = 'S';

  top_of_zone_table++;

  add_to_save_list(zone->number, SL_ZON);
  return rznum;
}

int create_world_index(int znum, const char *type)
{
  FILE *newfile, *oldfile;
  char new_name[32], old_name[32], *prefix;
  int num, found = FALSE, terminated = FALSE;
  char buf[MAX_STRING_LENGTH];
  char buf1[MAX_STRING_LENGTH];

  switch (*type) {
  case 'z':
    prefix = ZON_PREFIX;
    break;
  case 'w':
    prefix = WLD_PREFIX;
    break;
  case 'o':
    prefix = OBJ_PREFIX;
    break;
  case 'm':
    prefix = MOB_PREFIX;
    break;
  case 's':
    prefix = SHP_PREFIX;
    break;
  case 't':
    prefix = TRG_PREFIX;
    break;
  case 'q':
    prefix = QST_PREFIX;
    break;
  default:
    /* Caller messed up. */
    TB_ERROR_MSG("olc", "world-index-type", type,
                 "zone %d: unknown index type; no index updated", znum);
    return FALSE;
  }

  tb_snprintf(old_name, sizeof(old_name), "%s/index", prefix);
  tb_snprintf(new_name, sizeof(new_name), "%s/newindex", prefix);

  if (!(oldfile = fopen(old_name, "r"))) {
    TB_ERROR_ERRNO("olc", "world-index-open", old_name,
                   "zone %d type %s: index not updated", znum, type);
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Failed to open %s.", old_name);
    return FALSE;
  } else if (!(newfile = fopen(new_name, "w"))) {
    TB_ERROR_ERRNO("olc", "world-index-open", new_name,
                   "zone %d type %s: index not updated", znum, type);
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: OLC: Failed to open %s.", new_name);
    tb_fclose(oldfile);
    return FALSE;
  }

  /* Index contents must be in order: search through the old file for the right
   * place, insert the new file, then copy the rest over. */
  tb_snprintf(buf1, sizeof(buf1), "%d.%s", znum, type);
  while (get_line(oldfile, buf)) {
    if (*buf == '$') {
      /* The following used to add a blank line, thanks to Brian Taylor for the fix. */
      tb_fprintf(newfile, "%s", (!found ? strncat(buf1, "\n$\n", sizeof(buf1) - strlen(buf1) - 1) : "$\n"));
      terminated = TRUE;
      break;
    } else if (!found) {
      if (!parse_zone_index_number(buf, &num)) {
        TB_REPORT_ERROR("world-index-parse", old_name);
        tb_fclose(oldfile);
        tb_fclose(newfile);
        cleanup_new_zone_index(new_name);
        return FALSE;
      }
      if (num > znum) {
	found = TRUE;
	tb_fprintf(newfile, "%s\n", buf1);
      } else if (num == znum) {
        /* index file already had an entry for this zone: nothing to do. */
        tb_fclose(oldfile);
        tb_fclose(newfile);
        cleanup_new_zone_index(new_name);
        return TRUE;
      }
    }
    tb_fprintf(newfile, "%s\n", buf);
  }

  /* The boot loader stops at '$'.  Without one the new entry was never
   * written (it is only emitted at the terminator or before a larger
   * number), so committing would report success for an unchanged index. */
  if (!terminated) {
    TB_ERROR_MSG("olc", "world-index-unterminated", old_name,
                 "zone %d type %s: no '$' terminator; index unchanged", znum, type);
    tb_fclose(newfile);
    tb_fclose(oldfile);
    cleanup_new_zone_index(new_name);
    return FALSE;
  }

  /* Verify both streams before replacing the last good index. */
  if (ferror(oldfile) || ferror(newfile)) {
    TB_REPORT_ERROR("world-index-io", old_name);
    tb_fclose(newfile);
    tb_fclose(oldfile);
    cleanup_new_zone_index(new_name);
    return FALSE;
  }

  if (fclose(newfile) == EOF) {
    TB_REPORT_ERRNO("world-index-close", new_name);
    tb_fclose(oldfile);
    cleanup_new_zone_index(new_name);
    return FALSE;
  }
  if (fclose(oldfile) == EOF) {
    TB_REPORT_ERRNO("world-index-close", old_name);
    cleanup_new_zone_index(new_name);
    return FALSE;
  }

  /* Never unlink the destination before a successful replacement. */
  if (!tb_replace_file_checked(new_name, old_name)) {
    TB_REPORT_ERROR("world-index-commit", old_name);
    return FALSE;
  }
  return TRUE;
}

void remove_room_zone_commands(zone_rnum zone, room_rnum room_num)
{
  int subcmd = 0, cmd_room = -2;

  /* Delete all entries in zone_table that relate to this room so we can add
   * all the ones we have in their place. */
  while (zone_table[zone].cmd[subcmd].command != 'S') {
    switch (zone_table[zone].cmd[subcmd].command) {
    case 'M':
    case 'O':
    case 'T':
    case 'V':
      cmd_room = zone_table[zone].cmd[subcmd].arg3;
      break;
    case 'D':
    case 'R':
      cmd_room = zone_table[zone].cmd[subcmd].arg1;
      break;
    default:
      break;
    }
    /* A failed removal is already reported; step past it so the scan
     * still terminates. */
    if (cmd_room != room_num ||
        !remove_cmd_from_list(&zone_table[zone].cmd, subcmd, zone_table[zone].number))
      subcmd++;
  }
}

/* Save all the zone_table for this zone to disk.  This function now writes
 * simple comments in the form of (<name>) to each record.  A header for each
 * field is also there. */
int save_zone(zone_rnum zone_num)
{
  int subcmd, arg1 = -1, arg2 = -1, arg3 = -1, flag_tot=0, i;
  char fname[128], oldname[128];
  const char *comment = NULL;
  FILE *zfile;
  char zbuf1[MAX_STRING_LENGTH], zbuf2[MAX_STRING_LENGTH];
  char zbuf3[MAX_STRING_LENGTH], zbuf4[MAX_STRING_LENGTH];

#if CIRCLE_UNSIGNED_INDEX
  if (zone_num == NOWHERE || zone_num > top_of_zone_table) {
#else
  if (zone_num < 0 || zone_num > top_of_zone_table) {
#endif
    log("SYSERR: GenOLC: save_zone: Invalid real zone number %d. (0-%d)", zone_num, top_of_zone_table);
    return FALSE;
  }

  tb_snprintf(fname, sizeof(fname), "%s/%d.new", ZON_PREFIX, zone_table[zone_num].number);
  if (!(zfile = fopen(fname, "w"))) {
    TB_ERROR_ERRNO("olc", "zone-save-open", fname, "zone %d not saved",
                   zone_table[zone_num].number);
    mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: OLC: save_zones:  Can't write zone %d.", zone_table[zone_num].number);
    return FALSE;
  }

  for (i=0; i<ZN_ARRAY_MAX; i++)
    flag_tot += zone_table[zone_num].zone_flags[(i)];

  /* If zone flags or levels aren't set, there is no reason to save them! */
  if (flag_tot == 0 && zone_table[zone_num].min_level == -1 && zone_table[zone_num].max_level == -1)
  {
    /* Print zone header to file. */
    tb_fprintf(zfile, "#%d\n"
                   "%s~\n"
                   "%s~\n"
                   "%d %d %d %d\n",
           zone_table[zone_num].number,
          (zone_table[zone_num].builders && *zone_table[zone_num].builders)
                ? zone_table[zone_num].builders : "None.",
          (zone_table[zone_num].name && *zone_table[zone_num].name)
                ? convert_from_tabs(zone_table[zone_num].name) : "undefined",
          genolc_zone_bottom(zone_num),
          zone_table[zone_num].top,
          zone_table[zone_num].lifespan,
          zone_table[zone_num].reset_mode
          );
  } else {
    sprintascii(zbuf1, zone_table[zone_num].zone_flags[0]);
    sprintascii(zbuf2, zone_table[zone_num].zone_flags[1]);
    sprintascii(zbuf3, zone_table[zone_num].zone_flags[2]);
    sprintascii(zbuf4, zone_table[zone_num].zone_flags[3]);

    /* Print zone header to file. */
    tb_fprintf(zfile, "#%d\n"
                   "%s~\n"
                   "%s~\n"
                   "%d %d %d %d %s %s %s %s %d %d\n",       /* New tbaMUD data line */
           zone_table[zone_num].number,
          (zone_table[zone_num].builders && *zone_table[zone_num].builders)
                ? zone_table[zone_num].builders : "None.",
          (zone_table[zone_num].name && *zone_table[zone_num].name)
                ? convert_from_tabs(zone_table[zone_num].name) : "undefined",
          genolc_zone_bottom(zone_num),
          zone_table[zone_num].top,
          zone_table[zone_num].lifespan,
          zone_table[zone_num].reset_mode,
          zbuf1, zbuf2, zbuf3, zbuf4,
          zone_table[zone_num].min_level,
          zone_table[zone_num].max_level
          );
  }

	/* Handy Quick Reference Chart for Zone Values.
	 *
	 * Field #1    Field #3   Field #4  Field #5
	 * -------------------------------------------------
	 * M (Mobile)  Mob-Vnum   Wld-Max   Room-Vnum
	 * O (Object)  Obj-Vnum   Wld-Max   Room-Vnum
	 * G (Give)    Obj-Vnum   Wld-Max   Unused
	 * E (Equip)   Obj-Vnum   Wld-Max   EQ-Position
	 * P (Put)     Obj-Vnum   Wld-Max   Target-Obj-Vnum
	 * D (Door)    Room-Vnum  Door-Dir  Door-State
	 * R (Remove)  Room-Vnum  Obj-Vnum  Unused
         * T (Trigger) Trig-type  Trig-Vnum Room-Vnum
         * V (var)     Trig-type  Context   Room-Vnum Varname Value
	 * ------------------------------------------------- */

  for (subcmd = 0; ZCMD(zone_num, subcmd).command != 'S'; subcmd++) {
    const struct reset_com *zc = &ZCMD(zone_num, subcmd);

    /* Every stored rnum is checked against its table before it is turned
     * back into a vnum.  A stale one (from a renumbering bug) would read
     * past the table and write another thing's vnum, so the whole save is
     * abandoned instead and the previous .zon stays in place. */
#define ZSAVE_CHECK(argno, val, top) do { \
      if ((val) < 0 || (val) > (int)(top)) { \
        TB_ERROR_MSG("olc", "zone-save-rnum", fname, \
                     "zone %d cmd %d '%c' (line %d) arg%d=%d out of range 0..%d; " \
                     "zone not saved", zone_table[zone_num].number, subcmd, \
                     zc->command, zc->line, (argno), (val), (int)(top)); \
        goto abort_save; \
      } \
    } while (0)

    switch (zc->command) {
    case 'M':
      ZSAVE_CHECK(1, zc->arg1, top_of_mobt);
      ZSAVE_CHECK(3, zc->arg3, top_of_world);
      arg1 = mob_index[zc->arg1].vnum;
      arg2 = zc->arg2;
      arg3 = world[zc->arg3].number;
      comment = mob_proto[zc->arg1].player.short_descr;
      break;
    case 'O':
      ZSAVE_CHECK(1, zc->arg1, top_of_objt);
      ZSAVE_CHECK(3, zc->arg3, top_of_world);
      arg1 = obj_index[zc->arg1].vnum;
      arg2 = zc->arg2;
      arg3 = world[zc->arg3].number;
      comment = obj_proto[zc->arg1].short_description;
      break;
    case 'G':
      ZSAVE_CHECK(1, zc->arg1, top_of_objt);
      arg1 = obj_index[zc->arg1].vnum;
      arg2 = zc->arg2;
      arg3 = -1;
      comment = obj_proto[zc->arg1].short_description;
      break;
    case 'E':
      ZSAVE_CHECK(1, zc->arg1, top_of_objt);
      arg1 = obj_index[zc->arg1].vnum;
      arg2 = zc->arg2;
      arg3 = zc->arg3;
      comment = obj_proto[zc->arg1].short_description;
      break;
    case 'P':
      ZSAVE_CHECK(1, zc->arg1, top_of_objt);
      ZSAVE_CHECK(3, zc->arg3, top_of_objt);
      arg1 = obj_index[zc->arg1].vnum;
      arg2 = zc->arg2;
      arg3 = obj_index[zc->arg3].vnum;
      comment = obj_proto[zc->arg1].short_description;
      break;
    case 'D':
      ZSAVE_CHECK(1, zc->arg1, top_of_world);
      arg1 = world[zc->arg1].number;
      arg2 = zc->arg2;
      arg3 = zc->arg3;
      comment = world[zc->arg1].name;
      break;
    case 'R':
      ZSAVE_CHECK(1, zc->arg1, top_of_world);
      ZSAVE_CHECK(2, zc->arg2, top_of_objt);
      arg1 = world[zc->arg1].number;
      arg2 = obj_index[zc->arg2].vnum;
      comment = obj_proto[zc->arg2].short_description;
      arg3 = -1;
      break;
    case 'T':
      /* trig_index[] holds top_of_trigt entries (a count, not a top). */
      ZSAVE_CHECK(2, zc->arg2, top_of_trigt - 1);
      ZSAVE_CHECK(3, zc->arg3, top_of_world);
      arg1 = zc->arg1; /* trigger type */
      arg2 = trig_index[zc->arg2]->vnum; /* trigger vnum */
      arg3 = world[zc->arg3].number; /* room num */
      comment = GET_TRIG_NAME(trig_index[zc->arg2]->proto);
      break;
    case 'V':
      ZSAVE_CHECK(3, zc->arg3, top_of_world);
      if (!zc->sarg1 || !zc->sarg2) {
        TB_ERROR_MSG("olc", "zone-save-sarg", fname,
                     "zone %d cmd %d 'V' (line %d) has NULL %s; zone not saved",
                     zone_table[zone_num].number, subcmd, zc->line,
                     !zc->sarg1 ? "variable name" : "value");
        goto abort_save;
      }
      arg1 = zc->arg1; /* trigger type */
      arg2 = zc->arg2; /* context */
      arg3 = world[zc->arg3].number;
      break;
    case '*':
      /* Commands disabled at boot (an unresolved vnum in renum_zone_table())
       * or by delete_room() hold no vnums to write back, so saving drops
       * the reset line from disk for good.  Say so, per line. */
      TB_WARN_MSG("olc", "zone-save-drop-cmd", fname,
                  "zone %d cmd %d (line %d) disabled ('*'); not saved",
                  zone_table[zone_num].number, subcmd, zc->line);
      continue;
    default:
      TB_ERROR_MSG("olc", "zone-save-drop-cmd", fname,
                   "zone %d cmd %d (line %d) unknown command '%c'; not saved",
                   zone_table[zone_num].number, subcmd, zc->line, zc->command);
      mudlog(BRF, LVL_BUILDER, TRUE,
             "SYSERR: OLC: save_zone: zone %d cmd %d: unknown command '%c' - NOT saving",
             zone_table[zone_num].number, subcmd, zc->command);
      continue;
    }
    if (ZCMD(zone_num, subcmd).command != 'V')
      tb_fprintf(zfile, "%c %d %d %d %d \t(%s)\n",
		ZCMD(zone_num, subcmd).command, ZCMD(zone_num, subcmd).if_flag, arg1, arg2, arg3,
		comment ? comment : "");
    else
      tb_fprintf(zfile, "%c %d %d %d %d %s %s\n",
              ZCMD(zone_num, subcmd).command, ZCMD(zone_num, subcmd).if_flag, arg1, arg2, arg3,
              ZCMD(zone_num, subcmd).sarg1, ZCMD(zone_num, subcmd).sarg2);
  }
#undef ZSAVE_CHECK
  tb_fputs("S\n$\n", zfile);
  {
    int flush_ok = tb_fflush_checked(zfile, fname);
    int close_ok = tb_fclose_checked(zfile, fname);
    if (!flush_ok || !close_ok) {
      if (remove(fname) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", fname);
      return FALSE;
    }
  }
  tb_snprintf(oldname, sizeof(oldname), "%s/%d.zon", ZON_PREFIX, zone_table[zone_num].number);
  if (!tb_replace_file_checked(fname, oldname))
    return FALSE;

  if (in_save_list(zone_table[zone_num].number, SL_ZON))
    remove_from_save_list(zone_table[zone_num].number, SL_ZON);
  return TRUE;

abort_save:
  /* Already reported at the failing command; keep the previous .zon. */
  tb_fclose(zfile);
  if (remove(fname) != 0 && errno != ENOENT)
    TB_REPORT_ERRNO("remove", fname);
  return FALSE;
}

/* Some common code to count the number of comands in the list. */
int count_commands(struct reset_com *list)
{
  int count = 0;

  while (list[count].command != 'S')
    count++;

  return count;
}

/* Adds a new reset command into a list.  Takes a pointer to the list so that
 * it may play with the memory locations. */
void add_cmd_to_list(struct reset_com **list, struct reset_com *newcmd, int pos)
{
  int count, i, l;
  struct reset_com *newlist;

  /* Count number of commands (not including terminator). */
  count = count_commands(*list);

  /* Value is +2 for the terminator and new field to add. */
  CREATE(newlist, struct reset_com, count + 2);

  /* Even tighter loop to copy the old list and insert a new command. */
  for (i = 0, l = 0; i <= count; i++) {
    newlist[i] = ((i == pos) ? *newcmd : (*list)[l++]);
  }

  /* Add terminator, then insert new list. */
  newlist[count + 1].command = 'S';
  free(*list);
  *list = newlist;
}

/**
 * Remove the reset command at pos from a zone command list.
 *
 * The list is reallocated to one slot fewer and every later command slides
 * down, so a caller scanning by index must re-examine pos afterwards.
 *
 * Ownership: the sarg1/sarg2 strings of a removed 'V' command are NOT
 * freed.  zedit_setup() copies the live list by struct value, so the zedit
 * working copy shares those pointers, and zedit_save_internally() removes
 * the room's live commands (here) before re-adding the working copy's;
 * freeing them here would hand the re-added commands freed memory.  Until
 * zedit deep-copies its working list, a 'V' dropped by delete_room() leaks
 * its two strings (finding C2-27, reported as skipped).
 *
 * @param list Address of the zone's command list (zone_table[z].cmd).
 * @param pos  Index to remove, 0..count-1; callers derive it from their own
 *             scan or from builder input already range-checked.
 * @param znum Zone vnum, for diagnostics only.
 * @return TRUE when a command was removed; FALSE, reported, when the list
 *         is missing or pos is out of range (the list is unchanged).
 */
static int remove_cmd_from_list(struct reset_com **list, int pos, zone_vnum znum)
{
  int count, i, l;
  struct reset_com *newlist;

  if (!list || !*list) {
    TB_ERROR_MSG("olc", "zone-cmd-remove", "zone",
                 "zone %d: no command list; pos %d not removed", znum, pos);
    return FALSE;
  }

  /* Count number of commands (not including terminator). */
  count = count_commands(*list);
  if (count <= 0 || pos < 0 || pos >= count) {
    TB_ERROR_MSG("olc", "zone-cmd-remove", "zone",
                 "zone %d: pos %d out of range 0..%d; not removed",
                 znum, pos, count - 1);
    return FALSE;
  }

  /* One command is removed, so count slots are enough for the remaining
   * commands plus the terminating S command. */
  CREATE(newlist, struct reset_com, count);

  /* Even tighter loop to copy old list and skip unwanted command. */
  for (i = 0, l = 0; i < count; i++) {
    if (i != pos) {
      newlist[l++] = (*list)[i]; /* NOLINT(clang-analyzer-security.ArrayBound): one of count entries is skipped. */
    }
  }
  /* Add the terminator, then insert the new list. */
  newlist[count - 1].command = 'S';
  free(*list);
  *list = newlist;
  return TRUE;
}

/* Error check user input and then add new (blank) command. */
int new_command(struct zone_data *zone, int pos)
{
  int subcmd = 0;
  /* Every field is copied into the list, including if_flag, line and the
   * 'V' string pointers, so the blank command must be fully zeroed. */
  struct reset_com new_com = { 0 };

  /* Error check to ensure users hasn't given too large an index. */
  while (zone->cmd[subcmd].command != 'S')
    subcmd++;

  if (pos < 0 || pos > subcmd) {
    TB_WARN_MSG("olc", "zone-cmd-index", "zone",
                "zone %d insert pos %d out of range 0..%d",
                zone->number, pos, subcmd);
    return 0;
  }

  /* Ok, let's add a new (blank) command. */
  new_com.command = 'N';
  add_cmd_to_list(&zone->cmd, &new_com, pos);
  return 1;
}

/* Error check user input and then remove command.  Returns TRUE when the
 * command at pos was removed.  The commands after it slide down one slot, so a
 * caller scanning by index must re-examine pos rather than advance past it. */
int delete_zone_command(struct zone_data *zone, int pos)
{
  int subcmd = 0;

  /* Error check to ensure users hasn't given too large an index. */
  while (zone->cmd[subcmd].command != 'S')
    subcmd++;

  if (pos < 0 || pos >= subcmd) {
    TB_WARN_MSG("olc", "zone-cmd-index", "zone",
                "zone %d delete pos %d out of range 0..%d",
                zone->number, pos, subcmd - 1);
    return FALSE;
  }

  /* Ok, let's zap it. */
  return remove_cmd_from_list(&zone->cmd, pos, zone->number);
}

