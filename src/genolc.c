/**************************************************************************
*  File: genolc.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - General.                                  *
*                                                                         *
*  Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer.            *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "comm.h"
#include "shop.h"
#include "oasis.h"
#include "genolc.h"
#include "genwld.h"
#include "genmob.h"
#include "genshp.h"
#include "genzon.h"
#include "genobj.h"
#include "dg_olc.h"
#include "constants.h"
#include "interpreter.h"
#include "act.h"        /* for the space_to_minus function */
#include "modify.h"      /* for smash_tilde */
#include "quest.h"

/* Global variables defined here, used elsewhere */
/* List of zones to be saved. */
struct save_list_data *save_list;

/* Local (file scope) variables */
/* Structure defining all known save types. */
static struct {
  int save_type;
  int (*func)(IDXTYPE rnum);
  const char *message;
} save_types[] = {
  { SL_MOB, save_mobiles , "mobile" },
  { SL_OBJ, save_objects, "object" },
  { SL_SHP, save_shops, "shop" },
  { SL_WLD, save_rooms, "room" },
  { SL_ZON, save_zone, "zone" },
  { SL_CFG, save_config, "config" },
  { SL_QST, save_quests, "quest" },
  { SL_ACT, NULL, "social" },
  { SL_HLP, NULL, "help" },
  { -1, NULL, NULL },
};
/* for Zone Export */
static int zone_exits = 0;

/* Local (file scope) functions */
/* Zone export functions */
static int export_save_shops(zone_rnum zrnum);
static int export_save_mobiles(zone_rnum rznum);
static int export_save_zone(zone_rnum zrnum);
static int export_save_objects(zone_rnum zrnum);
static int export_save_rooms(zone_rnum zrnum);
static int export_save_triggers(zone_rnum zrnum);
static int export_mobile_record(mob_vnum mvnum, struct char_data *mob, FILE *fd);
static void export_script_save_to_disk(FILE *fp, void *item, int type);
static int export_info_file(zone_rnum zrnum);

int genolc_checkstring(struct descriptor_data *d, char *arg)
{
  smash_tilde(arg);
  parse_at(arg);
  return TRUE;
}

char *str_udup(const char *txt)
{
  return strdup((txt && *txt) ? txt : "undefined");
}

char *str_udupnl(const char *txt)
{
  char *str = NULL, undef[] = "undefined";
  const char *ptr = NULL;
  size_t n;

  ptr = (txt && *txt) ? txt : undef;
  n = strlen(ptr) + 3;

  CREATE(str, char, n);
  strlcpy(str, ptr, n);
  strlcat(str, "\r\n", n);

  return str;
}

/* Original use: to be called at shutdown time. */
int save_all(void)
{
  while (save_list) {
    if (save_list->type < 0 || save_list->type > SL_MAX) {
      switch (save_list->type) {
        case SL_ACT:
          log("Actions not saved - can not autosave. Use 'aedit save'.");
          save_list = save_list->next;    /* Fatal error, skip this one. */
          break;
        case SL_HLP:
          log("Help not saved - can not autosave. Use 'hedit save'.");
          save_list = save_list->next;    /* Fatal error, skip this one. */
          break;
        default:
          log("SYSERR: GenOLC: Invalid save type %d in save list.\n", save_list->type);
          break;
        }
      } else if ((*save_types[save_list->type].func) (real_zone(save_list->zone)) < 0)
        save_list = save_list->next;      /* Fatal error, skip this one. */
    }
    return TRUE;
}

/* NOTE: This changes the buffer passed in. */
void strip_cr(char *buffer)
{
  int rpos, wpos;

  if (buffer == NULL)
    return;

  for (rpos = 0, wpos = 0; buffer[rpos]; rpos++) {
    buffer[wpos] = buffer[rpos];
    wpos += (buffer[rpos] != '\r');
  }
  buffer[wpos] = '\0';
}

void copy_ex_descriptions(struct extra_descr_data **to, struct extra_descr_data *from)
{
  struct extra_descr_data *wpos;

  CREATE(*to, struct extra_descr_data, 1);
  wpos = *to;

  for (; from; from = from->next, wpos = wpos->next) {
    wpos->keyword = str_udup(from->keyword);
    wpos->description = str_udup(from->description);
    if (from->next)
      CREATE(wpos->next, struct extra_descr_data, 1);
  }
}

void free_ex_descriptions(struct extra_descr_data *head)
{
  struct extra_descr_data *thised, *next_one;

  if (!head) {
    log("free_ex_descriptions: NULL pointer or NULL data.");
    return;
  }

  for (thised = head; thised; thised = next_one) {
    next_one = thised->next;
    if (thised->keyword)
      free(thised->keyword);
    if (thised->description)
      free(thised->description);
    free(thised);
  }
}

int remove_from_save_list(zone_vnum zone, int type)
{
  struct save_list_data *ritem, *temp;

  for (ritem = save_list; ritem; ritem = ritem->next)
    if (ritem->zone == zone && ritem->type == type)
      break;

  if (ritem == NULL) {
    log("SYSERR: remove_from_save_list: Saved item not found. (%d/%d)", zone, type);
    return FALSE;
  }
  REMOVE_FROM_LIST(ritem, save_list, next);
  free(ritem);
  return TRUE;
}

int add_to_save_list(zone_vnum zone, int type)
{
  struct save_list_data *nitem;
  zone_rnum rznum;

  if (type == SL_CFG)
    return FALSE;

  rznum = real_zone(zone);
  if (rznum == NOWHERE || rznum > top_of_zone_table) {
    if (zone != AEDIT_PERMISSION && zone != HEDIT_PERMISSION) {
      log("SYSERR: add_to_save_list: Invalid zone number passed. (%d => %d, 0-%d)", zone, rznum, top_of_zone_table);
      return FALSE;
    }
  }

  for (nitem = save_list; nitem; nitem = nitem->next)
    if (nitem->zone == zone && nitem->type == type)
      return FALSE;

  CREATE(nitem, struct save_list_data, 1);
  nitem->zone = zone;
  nitem->type = type;
  nitem->next = save_list;
  save_list = nitem;
  return TRUE;
}

int in_save_list(zone_vnum zone, int type)
{
  struct save_list_data *nitem;

  for (nitem = save_list; nitem; nitem = nitem->next)
    if (nitem->zone == zone && nitem->type == type)
      return TRUE;

  return FALSE;
}

void free_save_list(void)
{
  struct save_list_data *sld, *next_sld;

  for (sld = save_list; sld; sld = next_sld) {
    next_sld = sld->next;
    free(sld);
  }
}

/* Used from do_show(), ideally. */
ACMD(do_show_save_list)
{
  if (save_list == NULL)
    send_to_char(ch, "All world files are up to date.\r\n");
  else {
    struct save_list_data *item;

    send_to_char(ch, "The following files need saving:\r\n");
    for (item = save_list; item; item = item->next) {
      if (item->type != SL_CFG)
        send_to_char(ch, " - %s data for zone %d.\r\n", save_types[item->type].message, item->zone);
      else
        send_to_char(ch, " - Game configuration data.\r\n");
    }
  }
}

room_vnum genolc_zonep_bottom(struct zone_data *zone)
{
  return zone->bot;
}

zone_vnum genolc_zone_bottom(zone_rnum rznum)
{
  return zone_table[rznum].bot;
}

int sprintascii(char *out, bitvector_t bits)
{
  int i, j = 0;
  /* 32 bits, don't just add letters to try to get more unless your bitvector_t is also as large. */
  char *flags = "abcdefghijklmnopqrstuvwxyzABCDEF";

  for (i = 0; flags[i] != '\0'; i++)
    if (bits & ((bitvector_t)1 << i))
      out[j++] = flags[i];

  if (j == 0) /* Didn't write anything. */
    out[j++] = '0';

  /* NUL terminate the output string. */
  out[j++] = '\0';
  return j;
}

/* converts illegal filename chars into appropriate equivalents */ 
static void fix_filename(const char *str, char *outbuf, size_t maxlen)
{ 
  const char *in = str;
  char *out = outbuf;
  size_t count = 0;

  while (*in) {
    switch(*in) {
      case ' ': *out = '_'; out++; break;
      case '(': *out = '{'; out++; break;
      case ')': *out = '}'; out++; break;
 
      /* skip the following */ 
      case '\'':             break; 
      case '"':              break; 
 
      /* Legal character */ 
      default: *out = *in;  out++;break;
    } 
    in++;
    count++;
    if (count == maxlen - 1) break;
  }
  *out = '\0';
}

#ifndef CIRCLE_WINDOWS
static int run_program_argv(char *const argv[])
{
  pid_t pid;
  int status;

  pid = fork();
  if (pid < 0) {
    TB_REPORT_ERRNO("fork", argv && argv[0] ? argv[0] : "zone export helper");
    return -1;
  }

  if (pid == 0) {
    execvp(argv[0], argv);
    TB_REPORT_ERRNO("execvp", argv && argv[0] ? argv[0] : "zone export helper");
    _exit(127);
  }

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      TB_REPORT_ERRNO("waitpid", argv && argv[0] ? argv[0] : "zone export helper");
      return -1;
    }
  }

  if (!WIFEXITED(status)) {
    TB_REPORT_ERROR("waitpid", "child terminated abnormally");
    return -1;
  }

  return WEXITSTATUS(status);
}
#endif

/* Export command by Kyle */ 
ACMD(do_export_zone) 
{ 
#ifdef CIRCLE_WINDOWS
   /* tar and gzip are usually not available */
    send_to_char(ch, "Sorry, that is not available in the windows port.\r\n");
#else /* all other configurations */
  zone_rnum zrnum; 
  zone_vnum zvnum;
  int parsed_zone; 
  char zone_name[READ_SIZE], fixed_file_name[READ_SIZE];
  char archive[READ_SIZE * 2], old_archive[READ_SIZE * 2];
  char info_path[READ_SIZE * 2], wld_path[READ_SIZE * 2], zon_path[READ_SIZE * 2];
  char mob_path[READ_SIZE * 2], obj_path[READ_SIZE * 2], trg_path[READ_SIZE * 2];
  char shp_path[READ_SIZE * 2];
  int success, errorcode = 0;

  /* system command locations are relative to where the binary IS, not where it
   * was run from, thus we act like we are in the bin folder, because we are*/ 
  char *path = "../lib/world/export/"; 

  if (IS_NPC(ch) || GET_LEVEL(ch) < LVL_IMPL) 
    return; 

  skip_spaces(&argument); 
  if (!*argument){ 
    send_to_char(ch, "Syntax: export <zone vnum>"); 
    return; 
  } 

  if (!tb_parse_int_checked(argument, &parsed_zone) || parsed_zone < 0) {
    send_to_char(ch, "Zone vnum must be a valid non-negative integer.\r\n");
    log("SYSERR: rejected malformed export zone argument '%s' from %s",
        argument, GET_NAME(ch));
    return;
  }

  zvnum = (zone_vnum)parsed_zone;
  if ((int)zvnum != parsed_zone) {
    send_to_char(ch, "Zone vnum is outside the range supported by this build.\r\n");
    log("SYSERR: rejected out-of-range export zone argument '%s' from %s",
        argument, GET_NAME(ch));
    return;
  }
  zrnum = real_zone(zvnum); 

  if (zrnum == NOWHERE) { 
    send_to_char(ch, "Export which zone?\r\n"); 
    return; 
  } 

  /* If we fail, it might just be because the directory didn't exist.  Can't 
   * hurt to try again. Do it silently though ( no logs ). */ 
  if (!export_info_file(zrnum)) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
      errorcode = -1;
  }
  if (errorcode) {
    TB_REPORT_ERRNO("mkdir", path);
    send_to_char(ch, "Failed to create export directory; see server log for the originating error.\r\n");
    return;
  }

  success = TRUE;
  if (!export_info_file(zrnum)) {
    log("SYSERR: zone export failed in export_info_file for zone %d", zvnum);
    send_to_char(ch, "Info file not saved!\r\n");
    success = FALSE;
  }
  if (!export_save_shops(zrnum)) {
    log("SYSERR: zone export failed in export_save_shops for zone %d", zvnum);
    send_to_char(ch, "Shops not saved!\r\n");
    success = FALSE;
  }
  if (!export_save_mobiles(zrnum)) {
    log("SYSERR: zone export failed in export_save_mobiles for zone %d", zvnum);
    send_to_char(ch, "Mobiles not saved!\r\n");
    success = FALSE;
  }
  if (!export_save_objects(zrnum)) {
    log("SYSERR: zone export failed in export_save_objects for zone %d", zvnum);
    send_to_char(ch, "Objects not saved!\r\n");
    success = FALSE;
  }
  if (!export_save_zone(zrnum)) {
    log("SYSERR: zone export failed in export_save_zone for zone %d", zvnum);
    send_to_char(ch, "Zone info not saved!\r\n");
    success = FALSE;
  }
  if (!export_save_rooms(zrnum)) {
    log("SYSERR: zone export failed in export_save_rooms for zone %d", zvnum);
    send_to_char(ch, "Rooms not saved!\r\n");
    success = FALSE;
  }
  if (!export_save_triggers(zrnum)) {
    log("SYSERR: zone export failed in export_save_triggers for zone %d", zvnum);
    send_to_char(ch, "Triggers not saved!\r\n");
    success = FALSE;
  }

  /* If anything went wrong, don't try to tar the files. */ 
  if (success) { 
    send_to_char(ch, "Individual files saved to /lib/world/export.\r\n"); 
    tb_snprintf(zone_name, sizeof(zone_name), "%s", zone_table[zrnum].name); 
  } else { 
    send_to_char(ch, "Ran into problems writing to files.\r\n"); 
    return; 
  }
  /* Make sure the name of the zone doesn't make the filename illegal. */ 
  fix_filename(zone_name, fixed_file_name, sizeof(fixed_file_name));

  if (snprintf(archive, sizeof(archive), "%s%s.tar", path, fixed_file_name) >= (int)sizeof(archive) ||
      snprintf(old_archive, sizeof(old_archive), "%s%s.tar.gz", path, fixed_file_name) >= (int)sizeof(old_archive) ||
      snprintf(info_path, sizeof(info_path), "%sqq.info", path) >= (int)sizeof(info_path) ||
      snprintf(wld_path, sizeof(wld_path), "%sqq.wld", path) >= (int)sizeof(wld_path) ||
      snprintf(zon_path, sizeof(zon_path), "%sqq.zon", path) >= (int)sizeof(zon_path) ||
      snprintf(mob_path, sizeof(mob_path), "%sqq.mob", path) >= (int)sizeof(mob_path) ||
      snprintf(obj_path, sizeof(obj_path), "%sqq.obj", path) >= (int)sizeof(obj_path) ||
      snprintf(trg_path, sizeof(trg_path), "%sqq.trg", path) >= (int)sizeof(trg_path) ||
      snprintf(shp_path, sizeof(shp_path), "%sqq.shp", path) >= (int)sizeof(shp_path)) {
    send_to_char(ch, "Export path is too long.\r\n");
    return;
  }

  if (remove(old_archive) != 0 && errno != ENOENT) {
    TB_REPORT_ERRNO("remove", old_archive);
    send_to_char(ch, "Failed to delete previous zip file; see server log for details.\r\n");
  }

  {
    char *tar_argv[] = { "tar", "-cf", archive, info_path, wld_path, zon_path,
                         mob_path, obj_path, trg_path, shp_path, NULL };
    errorcode = run_program_argv(tar_argv);
  }
  if (errorcode != 0) {
    log("SYSERR: zone export tar failed for zone %d with exit status %d", zvnum, errorcode);
    send_to_char(ch, "Failed to tar files; see server log for the exact failure site/status.\r\n");
    return;
  }

  {
    char *gzip_argv[] = { "gzip", archive, NULL };
    errorcode = run_program_argv(gzip_argv);
  }
  if (errorcode != 0) {
    log("SYSERR: zone export gzip failed for zone %d with exit status %d", zvnum, errorcode);
    send_to_char(ch, "Failed to gzip tar file; see server log for the exact failure site/status.\r\n");
    return;
  }

  send_to_char(ch, "Files tar'ed to \"%s%s.tar.gz\"\r\n", path, fixed_file_name);
#endif /* platform specific part */
}

static int export_info_file(zone_rnum zrnum)
{
  int i;
  FILE *info_file;

  if (!(info_file = fopen("world/export/qq.info", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.info");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_info_file: cannot open world/export/qq.info");
    return FALSE;
  } else if (fprintf(info_file, "tbaMUD Area file.\n") < 0) {
    TB_REPORT_ERRNO("fprintf", "world/export/qq.info");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_info_file: cannot write world/export/qq.info");
    tb_fclose(info_file);
    return FALSE;
  }

  tb_fprintf(info_file, "The files accompanying this info file contain the area: %s\n", zone_table[zrnum].name);
  tb_fprintf(info_file, "It was written by: %s.\n\n", zone_table[zrnum].builders);
  tb_fprintf(info_file, "The author has given permission to distribute the area, provided credit is\n");
  tb_fprintf(info_file, "given. The area may be modified as you see fit, except you are not allowed to\n");
  tb_fprintf(info_file, "remove the builder name or credits.\n\n");
  tb_fprintf(info_file, "Implementation:\n");
  tb_fprintf(info_file, "1. All the files have been QQ'ed. This means all occurences of the zone number\n");
  tb_fprintf(info_file, "   have been changed to QQ. In other words, if you decide to have this zone as\n");
  tb_fprintf(info_file, "   zone 123, replace all occurences of QQ with 123 and rename the qq.zon file\n");
  tb_fprintf(info_file, "   to 123.zon (etc.). And of course add 123.zon to the respective index file.\n");
  if (zone_exits) {
    tb_fprintf(info_file, "2. Exits out of this zone have been ZZ'd. So all doors leading out have ZZ??\n");
    tb_fprintf(info_file, "   instead of the room vnum (?? are numbers 00 - 99).\n");
    tb_fprintf(info_file, "   In this zone, the exit rooms in question are:\n");

    for (i = genolc_zone_bottom(zrnum); i <= zone_table[zrnum].top; i++) {
      room_rnum rnum = real_room(i);
      struct room_data *room;
      int j;

      if (rnum == NOWHERE)
        continue;

      room = &world[rnum];

      for (j = 0; j < DIR_COUNT; j++) {
        if (!R_EXIT(room, j))
          continue;

        if (R_EXIT(room, j)->to_room == NOWHERE || world[R_EXIT(room, j)->to_room].zone == zrnum)
          continue;

        tb_fprintf(info_file, "      Room QQ%02d : Exit to the %s\n",
                           room->number%100, dirs[j]);
      }
    }
    zone_exits = 0;
  } else {
    tb_fprintf(info_file, "2. This area doesn't have any exits _out_ of the zone.\n");
    tb_fprintf(info_file, "   More info on connections can be found in the zone description room (QQ00).\n");
  }

  tb_fprintf(info_file, "\nAdditional zone information is available in the zone description room QQ00.\n");
  tb_fprintf(info_file, "The Builder's Academy is maintaining and improving these zones. Any typo or\n");
  tb_fprintf(info_file, "bug reports should be reported to rumble@tbamud.com or stop by The Builder Academy\n");
  tb_fprintf(info_file, "port telnet://tbamud.com:9091\n");
  tb_fprintf(info_file, "\nAnyone interested in submitting areas or helping improve the existing ones\n");
  tb_fprintf(info_file, "please stop by TBA and talk to Rumble.\n\n");
  tb_fprintf(info_file, "We at The Builder's Academy hope you will enjoy using the area.\n\n");

  tb_fprintf(info_file, "Rumble - Admin of TBA\n");
  tb_fprintf(info_file, "Welcor - Coder of TBA\n");
  tb_fprintf(info_file, "\ntelnet://tbamud.com:9091/\n");

  tb_fclose(info_file);
  return TRUE;
}

static int export_save_shops(zone_rnum zrnum)
{
  int i, j, rshop;
  FILE *shop_file;
  struct shop_data *shop;

  if (!(shop_file = fopen("world/export/qq.shp", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.shp");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_shops : Cannot open shop file!");
    return FALSE;
  } else if (fprintf(shop_file, "CircleMUD v3.0 Shop File~\n") < 0) {
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_shops: Cannot write to shop file!");
    tb_fclose(shop_file);
    return FALSE;
  }
  /* Search database for shops in this zone. */
  for (i = genolc_zone_bottom(zrnum); i <= zone_table[zrnum].top; i++) {
    if ((rshop = real_shop(i)) != NOWHERE) {
      tb_fprintf(shop_file, "#QQ%02d~\n", i%100);
      shop = &shop_index[rshop];

      /* Save the products. */
      for (j = 0; S_PRODUCT(shop, j) != NOTHING; j++) {
        if (obj_index[S_PRODUCT(shop, j)].vnum < genolc_zone_bottom(zrnum) ||
            obj_index[S_PRODUCT(shop, j)].vnum > zone_table[zrnum].top)
          continue;

	tb_fprintf(shop_file, "QQ%02d\n", obj_index[S_PRODUCT(shop, j)].vnum%100);
      }
      tb_fprintf(shop_file, "-1\n");

      /* Save the rates. */
      tb_fprintf(shop_file, "%1.2f\n"
                         "%1.2f\n",
                         S_BUYPROFIT(shop),
                         S_SELLPROFIT(shop));

      /* Save the buy types and namelists. */
      for (j = 0;S_BUYTYPE(shop, j) != NOTHING; j++)
        tb_fprintf(shop_file, "%d%s\n",
                S_BUYTYPE(shop, j),
		S_BUYWORD(shop, j) ? S_BUYWORD(shop, j) : "");
      tb_fprintf(shop_file, "-1\n");

      /* Save messages. Added some defaults as sanity checks. */
      tb_fprintf(shop_file,
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%d\n"
	      "%ld\n"
	      "QQ%02d\n"
	      "%d\n",
	      S_NOITEM1(shop) ? S_NOITEM1(shop) : "%s Ke?!",
	      S_NOITEM2(shop) ? S_NOITEM2(shop) : "%s Ke?!",
	      S_NOBUY(shop) ? S_NOBUY(shop) : "%s Ke?!",
	      S_NOCASH1(shop) ? S_NOCASH1(shop) : "%s Ke?!",
	      S_NOCASH2(shop) ? S_NOCASH2(shop) : "%s Ke?!",
	      S_BUY(shop) ? S_BUY(shop) : "%s Ke?! %d?",
	      S_SELL(shop) ? S_SELL(shop) : "%s Ke?! %d?",
	      S_BROKE_TEMPER(shop),
	      S_BITVECTOR(shop),
	      mob_index[S_KEEPER(shop)].vnum%100,
	      S_NOTRADE(shop)
	      );

      /* Save the rooms. */
      for (j = 0;S_ROOM(shop, j) != NOWHERE; j++) {
        if (S_ROOM(shop, j) < genolc_zone_bottom(zrnum) ||
            S_ROOM(shop, j) > zone_table[zrnum].top)
          continue;

        tb_fprintf(shop_file, "QQ%02d\n", S_ROOM(shop, j)%100);
      }
      tb_fprintf(shop_file, "-1\n");

      /* Save open/closing times. */
      tb_fprintf(shop_file, "%d\n%d\n%d\n%d\n", S_OPEN1(shop), S_CLOSE1(shop),
		S_OPEN2(shop), S_CLOSE2(shop));
    }
  }
  tb_fprintf(shop_file, "$~\n");
  tb_fclose(shop_file);

  return TRUE;
}

static int export_save_mobiles(zone_rnum rznum)
{
  FILE *mob_file;
  mob_vnum i;
  mob_rnum rmob;

  if (!(mob_file = fopen("world/export/qq.mob", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.mob");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_mobiles : Cannot open file!");
    return FALSE;
  }

  for (i = genolc_zone_bottom(rznum); i <= zone_table[rznum].top; i++) {
    if ((rmob = real_mobile(i)) == NOBODY)
      continue;
    check_mobile_strings(&mob_proto[rmob]);
    if (export_mobile_record(i, &mob_proto[rmob], mob_file) < 0)
      log("SYSERR: export_save_mobiles: Error writing mobile #%d.", i);
  }
  tb_fputs("$\n", mob_file);
  tb_fclose(mob_file);

  return TRUE;
}

static int export_mobile_record(mob_vnum mvnum, struct char_data *mob, FILE *fd)
{

  char ldesc[MAX_STRING_LENGTH];
  char ddesc[MAX_STRING_LENGTH];

  ldesc[MAX_STRING_LENGTH - 1] = '\0';
  ddesc[MAX_STRING_LENGTH - 1] = '\0';
  strip_cr(strncpy(ldesc, GET_LDESC(mob), MAX_STRING_LENGTH - 1));
  strip_cr(strncpy(ddesc, GET_DDESC(mob), MAX_STRING_LENGTH - 1));

  tb_fprintf(fd,	"#QQ%02d\n"
		"%s%c\n"
		"%s%c\n"
		"%s%c\n"
		"%s%c\n",
	mvnum%100,
	GET_ALIAS(mob), STRING_TERMINATOR,
	GET_SDESC(mob), STRING_TERMINATOR,
	ldesc, STRING_TERMINATOR,
	ddesc, STRING_TERMINATOR
  );

  tb_fprintf(fd, "%d %d %d %d %d %d %d %d %d E\n"
      "%d %d %d %dd%d+%d %dd%d+%d\n",
      MOB_FLAGS(mob)[0], MOB_FLAGS(mob)[1],
      MOB_FLAGS(mob)[2], MOB_FLAGS(mob)[3],
      AFF_FLAGS(mob)[0], AFF_FLAGS(mob)[1],
      AFF_FLAGS(mob)[2], AFF_FLAGS(mob)[3],
      GET_ALIGNMENT(mob),
      GET_LEVEL(mob), 20 - GET_HITROLL(mob), GET_AC(mob) / 10, GET_HIT(mob),
      GET_MANA(mob), GET_MOVE(mob), GET_NDD(mob), GET_SDD(mob),
      GET_DAMROLL(mob));

  tb_fprintf(fd, 	"%d %d\n"
		"%d %d %d\n",
		GET_GOLD(mob), GET_EXP(mob),
		GET_POS(mob), GET_DEFAULT_POS(mob), GET_SEX(mob)
  );

  if (write_mobile_espec(mvnum, mob, fd) < 0)
    log("SYSERR: GenOLC: Error writing E-specs for mobile #%d.", mvnum);

  export_script_save_to_disk(fd, mob, MOB_TRIGGER);

  return TRUE;
}

static int export_save_zone(zone_rnum zrnum)
{
  int subcmd;
  FILE *zone_file;

  if (!(zone_file = fopen("world/export/qq.zon", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.zon");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_zone : Cannot open file!");
    return FALSE;
  }

  /* Print zone header to file. */
  tb_fprintf(zone_file, "#QQ\n"
                 "%s~\n"
                 "%s~\n"
                 "QQ%02d QQ%02d %d %d\n",
	  (zone_table[zrnum].builders && *zone_table[zrnum].builders)
		? zone_table[zrnum].builders : "None.",
	  (zone_table[zrnum].name && *zone_table[zrnum].name)
		? zone_table[zrnum].name : "undefined",
          genolc_zone_bottom(zrnum)%100,
	  zone_table[zrnum].top%100,
	  zone_table[zrnum].lifespan,
	  zone_table[zrnum].reset_mode
	  );

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

  for (subcmd = 0; ZCMD(zrnum, subcmd).command != 'S'; subcmd++) {
    switch (ZCMD(zrnum, subcmd).command) {
    case 'M':
      tb_fprintf(zone_file, "M %d QQ%02d %d QQ%02d \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		mob_index[ZCMD(zrnum, subcmd).arg1].vnum%100,
		ZCMD(zrnum, subcmd).arg2,
		world[ZCMD(zrnum, subcmd).arg3].number%100,
		mob_proto[ZCMD(zrnum, subcmd).arg1].player.short_descr);
      break;
    case 'O':
      tb_fprintf(zone_file, "O %d QQ%02d %d QQ%02d \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		obj_index[ZCMD(zrnum, subcmd).arg1].vnum%100,
		ZCMD(zrnum, subcmd).arg2,
		world[ZCMD(zrnum, subcmd).arg3].number%100,
		obj_proto[ZCMD(zrnum, subcmd).arg1].short_description);
      break;
    case 'G':
      tb_fprintf(zone_file, "G %d QQ%02d %d -1 \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
                obj_index[ZCMD(zrnum, subcmd).arg1].vnum%100,
                ZCMD(zrnum, subcmd).arg2,
                obj_proto[ZCMD(zrnum, subcmd).arg1].short_description);
      break;
    case 'E':
      tb_fprintf(zone_file, "E %d QQ%02d %d %d \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		 obj_index[ZCMD(zrnum, subcmd).arg1].vnum%100,
		 ZCMD(zrnum, subcmd).arg2,
		 ZCMD(zrnum, subcmd).arg3,
		 obj_proto[ZCMD(zrnum, subcmd).arg1].short_description);
      break;
    case 'P':
      tb_fprintf(zone_file, "P %d QQ%02d %d QQ%02d \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		obj_index[ZCMD(zrnum, subcmd).arg1].vnum%100,
		ZCMD(zrnum, subcmd).arg2,
		obj_index[ZCMD(zrnum, subcmd).arg3].vnum%100,
		obj_proto[ZCMD(zrnum, subcmd).arg1].short_description);
      break;
    case 'D':
      tb_fprintf(zone_file, "D %d QQ%02d %d %d \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		world[ZCMD(zrnum, subcmd).arg1].number%100,
		ZCMD(zrnum, subcmd).arg2,
		ZCMD(zrnum, subcmd).arg3,
		world[ZCMD(zrnum, subcmd).arg1].name);
      break;
    case 'R':
      tb_fprintf(zone_file, "R %d QQ%02d QQ%02d -1 \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		world[ZCMD(zrnum, subcmd).arg1].number%100,
		obj_index[ZCMD(zrnum, subcmd).arg2].vnum%100,
		obj_proto[ZCMD(zrnum, subcmd).arg2].short_description);
      break;
    case 'T':
      tb_fprintf(zone_file, "T %d %d QQ%02d QQ%02d \t(%s)\n",
		ZCMD(zrnum, subcmd).if_flag,
		ZCMD(zrnum, subcmd).arg1,
		trig_index[ZCMD(zrnum, subcmd).arg2]->vnum%100,
		world[ZCMD(zrnum, subcmd).arg3].number%100,
		GET_TRIG_NAME(trig_index[ZCMD(zrnum, subcmd).arg2]->proto));
      break;
    case 'V':
      tb_fprintf(zone_file, "V %d %d %d QQ%02d %s %s\n",
              ZCMD(zrnum, subcmd).if_flag,
              ZCMD(zrnum, subcmd).arg1,
              ZCMD(zrnum, subcmd).arg2,
              world[ZCMD(zrnum, subcmd).arg3].number%100,
              ZCMD(zrnum, subcmd).sarg1,
              ZCMD(zrnum, subcmd).sarg2);
      break;
    case '*':
      /* Invalid commands are replaced with '*' - Ignore them. */
      continue;
    default:
      mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: export_save_zone(): Unknown cmd '%c' - NOT saving", ZCMD(zrnum, subcmd).command);
      continue;
    }
  }
  tb_fputs("S\n$\n", zone_file);
  tb_fclose(zone_file);

  return TRUE;
}

static int export_save_objects(zone_rnum zrnum)
{
  char buf[MAX_STRING_LENGTH];
  char ebuf1[MAX_STRING_LENGTH], ebuf2[MAX_STRING_LENGTH], ebuf3[MAX_STRING_LENGTH], ebuf4[MAX_STRING_LENGTH];
  char wbuf1[MAX_STRING_LENGTH], wbuf2[MAX_STRING_LENGTH], wbuf3[MAX_STRING_LENGTH], wbuf4[MAX_STRING_LENGTH];
  char pbuf1[MAX_STRING_LENGTH], pbuf2[MAX_STRING_LENGTH], pbuf3[MAX_STRING_LENGTH], pbuf4[MAX_STRING_LENGTH];
  obj_rnum ornum;
  obj_vnum ovnum;
  int i;
  FILE *obj_file;
  struct obj_data *obj;
  struct extra_descr_data *ex_desc;

  if (!(obj_file = fopen("world/export/qq.obj", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.obj");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_objects : Cannot open file!");
    return FALSE;
  }
  /* Start running through all objects in this zone. */
  for (ovnum = genolc_zone_bottom(zrnum); ovnum <= zone_table[zrnum].top; ovnum++) {
    if ((ornum = real_object(ovnum)) != NOTHING) {
      if ((obj = &obj_proto[ornum])->action_description) {
	strncpy(buf, obj->action_description, sizeof(buf) - 1);
	strip_cr(buf);
      } else
	*buf = '\0';

      tb_fprintf(obj_file,
	      "#QQ%02d\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n"
	      "%s~\n",

	      GET_OBJ_VNUM(obj)%100,
	      (obj->name && *obj->name) ? obj->name : "undefined",
	      (obj->short_description && *obj->short_description) ? obj->short_description : "undefined",
	      (obj->description && *obj->description) ?	obj->description : "undefined",
	      buf);

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

      tb_fprintf(obj_file,
          "%d %s %s %s %s %s %s %s %s %s %s %s %s\n",
          GET_OBJ_TYPE(obj),
          ebuf1, ebuf2, ebuf3, ebuf4,
          wbuf1, wbuf2, wbuf3, wbuf4,
          pbuf1, pbuf2, pbuf3, pbuf4);

      if (GET_OBJ_TYPE(obj) != ITEM_CONTAINER)
        tb_fprintf(obj_file,
                "%d %d %d %d\n",
	        GET_OBJ_VAL(obj, 0), GET_OBJ_VAL(obj, 1), GET_OBJ_VAL(obj, 2), GET_OBJ_VAL(obj, 3));
      else
        tb_fprintf(obj_file,
                "%d %d %s%02d %d\n",
	        GET_OBJ_VAL(obj, 0),
	        GET_OBJ_VAL(obj, 1),
	        GET_OBJ_VAL(obj, 2) == -1 ? "" : "QQ", /* key */
	        GET_OBJ_VAL(obj, 2) == -1 ? -1 : GET_OBJ_VAL(obj, 2)%100,
	        GET_OBJ_VAL(obj, 3));

      tb_fprintf(obj_file,
	      "%d %d %d %d\n",
	      GET_OBJ_WEIGHT(obj), GET_OBJ_COST(obj), GET_OBJ_RENT(obj), GET_OBJ_LEVEL(obj));

      /* Do we have script(s) attached? */
      export_script_save_to_disk(obj_file, obj, OBJ_TRIGGER);

      /* Do we have extra descriptions? */
      if (obj->ex_description) {	/* Yes, save them too. */
	for (ex_desc = obj->ex_description; ex_desc; ex_desc = ex_desc->next) {
	  /* Sanity check to prevent nasty protection faults. */
	  if (!ex_desc->keyword || !ex_desc->description || !*ex_desc->keyword || !*ex_desc->description) {
	    mudlog(BRF, LVL_IMMORT, TRUE, "SYSERR: OLC: export_save_objects: Corrupt ex_desc!");
	    continue;
	  }
	  strncpy(buf, ex_desc->description, sizeof(buf) - 1);
	  strip_cr(buf);
	  tb_fprintf(obj_file, "E\n"
		  "%s~\n"
		  "%s~\n", ex_desc->keyword, buf);
	}
      }
      /* Do we have affects? */
      for (i = 0; i < MAX_OBJ_AFFECT; i++)
	if (obj->affected[i].modifier)
	  tb_fprintf(obj_file, "A\n"
		            "%d %d\n",
		  obj->affected[i].location,
		  obj->affected[i].modifier);
    }
  }

  /* Write the final line, close the file. */
  tb_fprintf(obj_file, "$~\n");
  tb_fclose(obj_file);

  return TRUE;
}

static int export_save_rooms(zone_rnum zrnum)
{
  int i;
  struct room_data *room;
  FILE *room_file;
  char buf[MAX_STRING_LENGTH];
  char buf1[MAX_STRING_LENGTH];

  if (!(room_file = fopen("world/export/qq.wld", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.wld");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_rooms : Cannot open file!");
    return FALSE;
  }

  for (i = genolc_zone_bottom(zrnum); i <= zone_table[zrnum].top; i++) {
    room_rnum rnum;

    if ((rnum = real_room(i)) != NOWHERE) {
      int j;

      room = &world[rnum];

      /* Copy the description and strip off trailing newlines. */
      strncpy(buf, room->description ? room->description : "Empty room.", sizeof(buf)-1 );
      strip_cr(buf);

      /* Save the numeric and string section of the file. */
      tb_fprintf(room_file, 	"#QQ%02d\n"
			"%s%c\n"
			"%s%c\n"
			"QQ %d %d %d %d %d\n",
		room->number%100,
		room->name ? room->name : "Untitled", STRING_TERMINATOR,
		buf, STRING_TERMINATOR,
		room->room_flags[0], room->room_flags[1],
                room->room_flags[2], room->room_flags[3], room->sector_type
      );

      /* Now you write out the exits for the room. */
      for (j = 0; j < DIR_COUNT; j++) {
	if (R_EXIT(room, j)) {
	  int dflag;
	  if (R_EXIT(room, j)->general_description) {
	    strncpy(buf, R_EXIT(room, j)->general_description, sizeof(buf)-1);
	    strip_cr(buf);
	  } else
	    *buf = '\0';

	  /* Figure out door flag. */
	  if (IS_SET(R_EXIT(room, j)->exit_info, EX_ISDOOR)) {
	    if (IS_SET(R_EXIT(room, j)->exit_info, EX_PICKPROOF))
	      dflag = 2;
	    else
	      dflag = 1;
	  } else
	    dflag = 0;

	  if (R_EXIT(room, j)->keyword)
	    strncpy(buf1, R_EXIT(room, j)->keyword, sizeof(buf1)-1 );
	  else
	    *buf1 = '\0';

	  /* Now write the exit to the file. */
          if (R_EXIT(room, j)->to_room == NOWHERE || world[R_EXIT(room, j)->to_room].zone == zrnum)
	    tb_fprintf(room_file,"D%d\n"
		              "%s~\n"
			      "%s~\n"
        		      "%d %s%02d %s%02d\n",
			      j,
			      buf,
			      buf1,
			      dflag,
			      R_EXIT(room, j)->key == NOTHING ? "" : "QQ",
			      R_EXIT(room, j)->key == NOTHING ? -1 : R_EXIT(room, j)->key % 100 ,
			      R_EXIT(room, j)->to_room == NOTHING ? "" : "QQ",
			      R_EXIT(room, j)->to_room != NOTHING ? (world[R_EXIT(room, j)->to_room].number%100) : -1);
          else {
	    tb_fprintf(room_file,"D%d\n"
		              "%s~\n"
			      "%s~\n"
        		      "%d %s%02d ZZ%02d\n",
			      j,
			      buf,
			      buf1,
			      dflag,
			      R_EXIT(room, j)->key == NOTHING ? "" : "QQ",
			      R_EXIT(room, j)->key == NOTHING ? -1 : R_EXIT(room, j)->key % 100 ,
			      world[R_EXIT(room, j)->to_room].number%100);
            zone_exits++;
          }
	}
      }

      if (room->ex_description) {
        struct extra_descr_data *xdesc;

	for (xdesc = room->ex_description; xdesc; xdesc = xdesc->next) {
	  strncpy(buf, xdesc->description, sizeof(buf) - 1);
	  buf[sizeof(buf) - 1] = '\0';
	  strip_cr(buf);
	  tb_fprintf(room_file,	"E\n"
			"%s~\n"
			"%s~\n", xdesc->keyword, buf);
	}
      }
      tb_fprintf(room_file, "S\n");
      export_script_save_to_disk(room_file, room, WLD_TRIGGER);
    }
  }

  /* Write the final line and close it. */
  tb_fprintf(room_file, "$~\n");
  tb_fclose(room_file);

  return TRUE;
}

static void export_script_save_to_disk(FILE *fp, void *item, int type)
{
  struct trig_proto_list *t;

  if (type==MOB_TRIGGER)
    t = ((struct char_data *)item)->proto_script;
  else if (type==OBJ_TRIGGER)
    t = ((struct obj_data *)item)->proto_script;
  else if (type==WLD_TRIGGER)
    t = ((struct room_data *)item)->proto_script;
  else {
    log("SYSERR: Invalid type passed to export_script_save_to_disk()");
    return;
  }

  while (t)
  {
    tb_fprintf(fp, "T QQ%02d\n", t->vnum%100);
    t = t->next;
  }
}

/* save the zone's triggers to internal memory and to disk */
static int export_save_triggers(zone_rnum zrnum)
{
  int i;
  trig_data *trig;
  struct cmdlist_element *cmd;
  FILE *trig_file;
  char bitBuf[MAX_INPUT_LENGTH];

  if (!(trig_file = fopen("world/export/qq.trg", "w"))) {
    TB_REPORT_ERRNO("fopen", "world/export/qq.trg");
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: export_save_triggers : Cannot open file!");
    return FALSE;
  }

  for (i = genolc_zone_bottom(zrnum); i <= zone_table[zrnum].top; i++) {
    trig_rnum rnum;

    if ((rnum = real_trigger(i)) != NOTHING) {
      trig = trig_index[rnum]->proto;

      tb_fprintf(trig_file, "#QQ%02d\n", i%100);

      sprintascii(bitBuf, GET_TRIG_TYPE(trig));
      tb_fprintf(trig_file,      "%s%c\n"
                              "%d %s %d\n"
                              "%s%c\n",
           (GET_TRIG_NAME(trig)) ? (GET_TRIG_NAME(trig)) : "unknown trigger", STRING_TERMINATOR,
           trig->attach_type,
           *bitBuf ? bitBuf : "0", GET_TRIG_NARG(trig),
           GET_TRIG_ARG(trig) ? GET_TRIG_ARG(trig) : "", STRING_TERMINATOR);

      tb_fprintf(trig_file, "* This trigger has been exported 'as is'. This means that vnums\n"
                         "* in this file are not changed, and will have to be edited by hand.\n"
                         "* This zone was number %d on The Builder Academy, so you\n"
                         "* should be looking for %dxx, where xx is 00-99.\n",
                         zone_table[zrnum].number, zone_table[zrnum].number);
        for (cmd = trig->cmdlist; cmd; cmd = cmd->next) {
          tb_fprintf(trig_file, "%s\n", cmd->cmd);
        }
      tb_fprintf(trig_file, "%c\n", STRING_TERMINATOR);
    }
  }

  tb_fprintf(trig_file, "$%c\n", STRING_TERMINATOR);
  tb_fclose(trig_file);
  return TRUE;
}
