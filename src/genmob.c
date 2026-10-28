/**************************************************************************
*  File: genmob.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - Mobiles.                                  *
*                                                                         *
*  Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer.            *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "shop.h"
#include "handler.h"
#include "genolc.h"
#include "genmob.h"
#include "genzon.h"
#include "dg_olc.h"
#include "spells.h"

/* OWNERSHIP AND RENUMBERING
 * - mob_proto[rnum] owns its strings and proto_script.  Live instances
 *   share the prototype's string pointers until something gives them their
 *   own copy, so freeing an instance's string is only correct when it
 *   differs from the prototype's (free_mobile, extract_mobile_all).
 * - add_mobile()/copy_mobile() deep-copy strings (copy_mobile_strings) but
 *   shallow-copy everything else in the OLC working mob, including
 *   proto_script, affected and script pointers: the caller hands those
 *   over and must not free them afterwards.
 * - add_mobile() and delete_mobile() RECREATE mob_proto/mob_index, which
 *   invalidates every 'struct char_data *' into mob_proto, and shift rnums.
 *   Everything that stores a mob rnum (live mobs, zone 'M' commands, shop
 *   keepers) is renumbered here; NOBODY (the largest rnum value) is never
 *   shifted.  Instances of a deleted mob are queued for extraction and
 *   detached (rnum NOBODY) before renumbering.
 * - Who controls the input: builders, through medit. */

/* local functions */
static void extract_mobile_all(mob_vnum vnum);

int add_mobile(struct char_data *mob, mob_vnum vnum)
{
  int rnum, i, found = FALSE, shop, cmd_no;
  zone_rnum zone;
  struct char_data *live_mob;

  if ((rnum = real_mobile(vnum)) != NOBODY) {
    /* Copy over the mobile and free() the old strings. */
    copy_mobile(&mob_proto[rnum], mob);

    /* Now re-point all existing mobile strings to here. */
    for (live_mob = character_list; live_mob; live_mob = live_mob->next)
      if (rnum == live_mob->nr)
        update_mobile_strings(live_mob, &mob_proto[rnum]);

    add_to_save_list(zone_table[real_zone_by_thing(vnum)].number, SL_MOB);
    log("GenOLC: add_mobile: Updated existing mobile #%d.", vnum);
    return rnum;
  }

  RECREATE(mob_proto, struct char_data, top_of_mobt + 2);
  RECREATE(mob_index, struct index_data, top_of_mobt + 2);
  top_of_mobt++;

  for (i = top_of_mobt; i > 0; i--) {
    if (vnum > mob_index[i - 1].vnum) {
      mob_proto[i] = *mob;
      mob_proto[i].nr = i;
      copy_mobile_strings(mob_proto + i, mob);
      mob_index[i].vnum = vnum;
      mob_index[i].number = 0;
      mob_index[i].func = 0;
      found = i;
      break;
    }
    mob_index[i] = mob_index[i - 1];
    mob_proto[i] = mob_proto[i - 1];
    mob_proto[i].nr++;
  }
  if (!found) {
    mob_proto[0] = *mob;
    mob_proto[0].nr = 0;
    copy_mobile_strings(&mob_proto[0], mob);
    mob_index[0].vnum = vnum;
    mob_index[0].number = 0;
    mob_index[0].func = 0;
  }

  log("GenOLC: add_mobile: Added mobile %d at index #%d.", vnum, found);

  /* Update live mobile rnums. */
  for (live_mob = character_list; live_mob; live_mob = live_mob->next)
    GET_MOB_RNUM(live_mob) += (GET_MOB_RNUM(live_mob) != NOTHING && GET_MOB_RNUM(live_mob) >= found);

  /* Update zone table. */
  for (zone = 0; zone <= top_of_zone_table; zone++)
    for (cmd_no = 0; ZCMD(zone, cmd_no).command != 'S'; cmd_no++)
      if (ZCMD(zone, cmd_no).command == 'M')
	ZCMD(zone, cmd_no).arg1 += (ZCMD(zone, cmd_no).arg1 >= found);

  /* Update shop keepers. */
  if (shop_index)
    for (shop = 0; shop <= top_shop; shop++)
      SHOP_KEEPER(shop) += (SHOP_KEEPER(shop) != NOTHING && SHOP_KEEPER(shop) >= found);

  add_to_save_list(zone_table[real_zone_by_thing(vnum)].number, SL_MOB);
  return found;
}

int copy_mobile(struct char_data *to, struct char_data *from)
{
  free_mobile_strings(to);
  *to = *from;
  check_mobile_strings(from);
  copy_mobile_strings(to, from);
  return TRUE;
}

static void extract_mobile_all(mob_vnum vnum)
{
  struct char_data *next, *ch;
  int i;

  for (ch = character_list; ch; ch = next) {
    next = ch->next;
    if (GET_MOB_VNUM(ch) == vnum) {
			if ((i = GET_MOB_RNUM(ch)) != NOBODY) {
	    if (ch->player.name && ch->player.name != mob_proto[i].player.name)
          free(ch->player.name);
				ch->player.name = NULL;
				
        if (ch->player.title && ch->player.title != mob_proto[i].player.title)
          free(ch->player.title);
				ch->player.title = NULL;
				
        if (ch->player.short_descr && ch->player.short_descr != mob_proto[i].player.short_descr)
          free(ch->player.short_descr);
				ch->player.short_descr = NULL;
				
        if (ch->player.long_descr && ch->player.long_descr != mob_proto[i].player.long_descr)
          free(ch->player.long_descr);
				ch->player.long_descr = NULL;
				
        if (ch->player.description && ch->player.description != mob_proto[i].player.description)
          free(ch->player.description);
				ch->player.description = NULL;
    
        /* free script proto list if it's not the prototype */
        if (ch->proto_script && ch->proto_script != mob_proto[i].proto_script)
          free_proto_script(ch, MOB_TRIGGER);			
				ch->proto_script = NULL;
			}
      extract_char(ch);
		}
  }
}

int delete_mobile(mob_rnum refpt)
{
  struct char_data *live_mob;
  struct char_data *proto;
  int counter, cmd_no;
  mob_vnum vnum;
  zone_rnum zone;

#if CIRCLE_UNSIGNED_INDEX
  if (refpt == NOBODY || refpt > top_of_mobt) {
#else
  if (refpt < 0 || refpt > top_of_mobt) {
#endif
    log("SYSERR: GenOLC: delete_mobile: Invalid rnum %d.", refpt);
    return NOBODY;
  }

  vnum = mob_index[refpt].vnum;
  proto = &mob_proto[refpt];
  
  extract_mobile_all(vnum);
  /* The prototype is not a live character: extract_char() would queue it
   * for extraction it can never get (it is not in character_list), and its
   * strings and trigger list would be overwritten by the shift below.
   * Release what it owns directly.  Live instances no longer share these
   * strings: extract_mobile_all() cleared their copies of the pointers. */
  free_mobile_strings(proto);
  free_proto_script(proto, MOB_TRIGGER);

  for (counter = refpt; counter < top_of_mobt; counter++) {
    mob_index[counter] = mob_index[counter + 1];
    mob_proto[counter] = mob_proto[counter + 1];
    mob_proto[counter].nr--;
  }

  top_of_mobt--;
  RECREATE(mob_index, struct index_data, top_of_mobt + 1);
  RECREATE(mob_proto, struct char_data, top_of_mobt + 1);

  /* Update live mobile rnums.  Players (and unprototyped mobs) carry
   * NOBODY, the largest value of the unsigned rnum type: it must not be
   * shifted, or it becomes a valid-looking rnum (65534). */
  for (live_mob = character_list; live_mob; live_mob = live_mob->next) {
    if (GET_MOB_RNUM(live_mob) == NOBODY || GET_MOB_RNUM(live_mob) < refpt)
      continue;
    if (GET_MOB_RNUM(live_mob) > refpt) {
      GET_MOB_RNUM(live_mob)--;
      continue;
    }
    /* Instances of the deleted mob were queued for extraction above and are
     * freed at the end of the pulse.  Detach them from the prototype table
     * now: renumbered to refpt - 1 they would decrement another mob's live
     * count (mob_index[].number) when extract_char_final() runs. */
    if (!IS_NPC(live_mob) || !MOB_FLAGGED(live_mob, MOB_NOTDEADYET))
      TB_ERROR_MSG("genolc", "mob-delete", "mob",
                   TB_ERRID_INVARIANT " live character still references deleted mob vnum %d (rnum %d) and is not pending extraction",
                   vnum, refpt);
    GET_MOB_RNUM(live_mob) = NOBODY;
  }

  /* Update zone table.  Deleting a command shifts the rest of the list
   * (reallocated to exactly fit) down into cmd_no, so cmd_no only advances
   * when nothing was deleted: advancing past a deletion skipped the next
   * command and, when the deleted one was last, stepped over the 'S'
   * terminator and read past the end of the list. */
  for (zone = 0; zone <= top_of_zone_table; zone++) {
    cmd_no = 0;
    while (ZCMD(zone, cmd_no).command != 'S') {
      if (ZCMD(zone, cmd_no).command == 'M') {
        if (ZCMD(zone, cmd_no).arg1 == refpt) {
          TB_WARN_MSG("genolc", "zcmd-delete", "zone",
                      "zone %d cmd %d removed: it loads mob vnum %d, which was deleted",
                      zone_table[zone].number, cmd_no, vnum);
          if (!delete_zone_command(&zone_table[zone], cmd_no)) {
            /* delete_zone_command() reported why.  The command is still
             * here, so revisiting cmd_no would never end: step past it. */
            TB_ERROR_MSG("genolc", "zone-cmd-remove", "zone",
                         "zone %d cmd %d still loads deleted mob vnum %d",
                         zone_table[zone].number, cmd_no, vnum);
            cmd_no++;
            continue;
          }
          /* The reset list changed: write the zone file at the next save,
           * or the deleted mob's load comes back at reboot. */
          add_to_save_list(zone_table[zone].number, SL_ZON);
          continue;
        }
        ZCMD(zone, cmd_no).arg1 -= (ZCMD(zone, cmd_no).arg1 > refpt);
      }
      cmd_no++;
    }
  }

  /* Update shop keepers.  A shop whose keeper was the deleted mob loses its
   * keeper (it used to be silently handed to the mob at refpt - 1); a shop
   * with no keeper (NOBODY) keeps none instead of wrapping to 65534. */
  if (shop_index)
    for (counter = 0; counter <= top_shop; counter++) {
      if (SHOP_KEEPER(counter) == refpt) {
        TB_WARN_MSG("genolc", "shop-keeper-deleted", "shop",
                    "shop %d keeper mob vnum %d deleted; shop now has no keeper",
                    SHOP_NUM(counter), vnum);
        SHOP_KEEPER(counter) = NOBODY;
      } else if (SHOP_KEEPER(counter) != NOBODY && SHOP_KEEPER(counter) > refpt)
        SHOP_KEEPER(counter)--;
      if (SHOP_KEEPER(counter) != NOBODY && SHOP_KEEPER(counter) > top_of_mobt)
        TB_ERROR_MSG("genolc", "mob-delete", "shop",
                     TB_ERRID_INVARIANT " shop %d keeper rnum %d out of range (top %d) after deleting mob vnum %d",
                     SHOP_NUM(counter), SHOP_KEEPER(counter), top_of_mobt, vnum);
    }

  save_mobiles(real_zone_by_thing(vnum));

  return refpt;
}

int copy_mobile_strings(struct char_data *t, struct char_data *f)
{
  if (f->player.name)
    t->player.name = strdup(f->player.name);
  if (f->player.title)
    t->player.title = strdup(f->player.title);
  if (f->player.short_descr)
    t->player.short_descr = strdup(f->player.short_descr);
  if (f->player.long_descr)
    t->player.long_descr = strdup(f->player.long_descr);
  if (f->player.description)
    t->player.description = strdup(f->player.description);
  return TRUE;
}

int update_mobile_strings(struct char_data *t, struct char_data *f)
{
  if (f->player.name)
    t->player.name = f->player.name;
  if (f->player.title)
    t->player.title = f->player.title;
  if (f->player.short_descr)
    t->player.short_descr = f->player.short_descr;
  if (f->player.long_descr)
    t->player.long_descr = f->player.long_descr;
  if (f->player.description)
    t->player.description = f->player.description;
  return TRUE;
}

int free_mobile_strings(struct char_data *mob)
{
  if (mob->player.name)
    free(mob->player.name);
  if (mob->player.title)
    free(mob->player.title);
  if (mob->player.short_descr)
    free(mob->player.short_descr);
  if (mob->player.long_descr)
    free(mob->player.long_descr);
  if (mob->player.description)
    free(mob->player.description);
  return TRUE;
}

/* Free a mobile structure that has been edited. Take care of existing mobiles
 * and their mob_proto! */
int free_mobile(struct char_data *mob)
{
  mob_rnum i;

  if (mob == NULL)
    return FALSE;

  /* Non-prototyped mobile.  Also known as new mobiles. */
  if ((i = GET_MOB_RNUM(mob)) == NOBODY) {
    free_mobile_strings(mob);
    /* free script proto list */
    free_proto_script(mob, MOB_TRIGGER);
   } else {	/* Prototyped mobile. */
    if (mob->player.name && mob->player.name != mob_proto[i].player.name)
      free(mob->player.name);
    if (mob->player.title && mob->player.title != mob_proto[i].player.title)
      free(mob->player.title);
    if (mob->player.short_descr && mob->player.short_descr != mob_proto[i].player.short_descr)
      free(mob->player.short_descr);
    if (mob->player.long_descr && mob->player.long_descr != mob_proto[i].player.long_descr)
      free(mob->player.long_descr);
    if (mob->player.description && mob->player.description != mob_proto[i].player.description)
      free(mob->player.description);
    /* free script proto list if it's not the prototype */
    if (mob->proto_script && mob->proto_script != mob_proto[i].proto_script)
      free_proto_script(mob, MOB_TRIGGER);
  }
  while (mob->affected)
    affect_remove(mob, mob->affected);

  /* free any assigned scripts */
  if (SCRIPT(mob))
    extract_script(mob, MOB_TRIGGER);

  free(mob);
  return TRUE;
}

int save_mobiles(zone_rnum rznum)
{
  zone_vnum vznum;
  FILE *mobfd;
  room_vnum i;
  mob_rnum rmob;
  int written;
  char mobfname[64], usedfname[64];

#if CIRCLE_UNSIGNED_INDEX
  if (rznum == NOWHERE || rznum > top_of_zone_table) {
#else
  if (rznum < 0 || rznum > top_of_zone_table) {
#endif
    log("SYSERR: GenOLC: save_mobiles: Invalid real zone number %d. (0-%d)", rznum, top_of_zone_table);
    return FALSE;
  }

  vznum = zone_table[rznum].number;
  tb_snprintf(mobfname, sizeof(mobfname), "%s%d.new", MOB_PREFIX, vznum);
  if ((mobfd = fopen(mobfname, "w")) == NULL) {
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: GenOLC: Cannot open mob file for writing.");
    return FALSE;
  }

  for (i = genolc_zone_bottom(rznum); i <= zone_table[rznum].top; i++) {
    if ((rmob = real_mobile(i)) == NOBODY)
      continue;
    check_mobile_strings(&mob_proto[rmob]);
    if (!write_mobile_record(i, &mob_proto[rmob], mobfd)) {
      tb_fclose(mobfd);
        if (remove(mobfname) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", mobfname);
      return FALSE;
    }
  }
  tb_fputs("$\n", mobfd);
  written = ftell(mobfd);
  {
    int flush_ok = tb_fflush_checked(mobfd, mobfname);
    int close_ok = tb_fclose_checked(mobfd, mobfname);
    if (!flush_ok || !close_ok) {
      if (remove(mobfname) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", mobfname);
      return FALSE;
    }
  }
  tb_snprintf(usedfname, sizeof(usedfname), "%s%d.mob", MOB_PREFIX, vznum);
  if (!tb_replace_file_checked(mobfname, usedfname))
    return FALSE;

  if (in_save_list(vznum, SL_MOB))
    remove_from_save_list(vznum, SL_MOB);
  log("GenOLC: '%s' saved, %d bytes written.", usedfname, written);
  return written;
}

int write_mobile_espec(mob_vnum mvnum, struct char_data *mob, FILE *fd)
{
  if (GET_ATTACK(mob) != 0)
    tb_fprintf(fd, "BareHandAttack: %d\n", GET_ATTACK(mob));
  if (GET_STR(mob) != 11)
    tb_fprintf(fd, "Str: %d\n", GET_STR(mob));
  if (GET_ADD(mob) != 0)
    tb_fprintf(fd, "StrAdd: %d\n", GET_ADD(mob));
  if (GET_DEX(mob) != 11)
    tb_fprintf(fd, "Dex: %d\n", GET_DEX(mob));
  if (GET_INT(mob) != 11)
    tb_fprintf(fd, "Int: %d\n", GET_INT(mob));
  if (GET_WIS(mob) != 11)
    tb_fprintf(fd, "Wis: %d\n", GET_WIS(mob));
  if (GET_CON(mob) != 11)
    tb_fprintf(fd, "Con: %d\n", GET_CON(mob));
  if (GET_CHA(mob) != 11)
    tb_fprintf(fd, "Cha: %d\n", GET_CHA(mob));
  if (GET_SAVE(mob, SAVING_PARA) != 0)
    tb_fprintf(fd, "SavingPara: %d\n", GET_SAVE(mob, SAVING_PARA));
  if (GET_SAVE(mob, SAVING_ROD) != 0)
    tb_fprintf(fd, "SavingRod: %d\n", GET_SAVE(mob, SAVING_ROD));
  if (GET_SAVE(mob, SAVING_PETRI) != 0)
    tb_fprintf(fd, "SavingPetri: %d\n", GET_SAVE(mob, SAVING_PETRI));
  if (GET_SAVE(mob, SAVING_BREATH) != 0)
    tb_fprintf(fd, "SavingBreath: %d\n", GET_SAVE(mob, SAVING_BREATH));
  if (GET_SAVE(mob, SAVING_SPELL) != 0)
    tb_fprintf(fd, "SavingSpell: %d\n", GET_SAVE(mob, SAVING_SPELL));
  tb_fputs("E\n", fd);
  return TRUE;
}

int write_mobile_record(mob_vnum mvnum, struct char_data *mob, FILE *fd)
{
  char ldesc[MAX_STRING_LENGTH];
  char ddesc[MAX_STRING_LENGTH];
  char buf[MAX_STRING_LENGTH];
  int n;

  ldesc[MAX_STRING_LENGTH - 1] = '\0';
  ddesc[MAX_STRING_LENGTH - 1] = '\0';
  strip_cr(strncpy(ldesc, GET_LDESC(mob), MAX_STRING_LENGTH - 1));
  strip_cr(strncpy(ddesc, GET_DDESC(mob), MAX_STRING_LENGTH - 1));

  n = snprintf(buf, MAX_STRING_LENGTH, "#%d\n"
        "%s%c\n"
        "%s%c\n"
        "%s%c\n"
        "%s%c\n",
        mvnum,
        GET_ALIAS(mob), STRING_TERMINATOR,
        GET_SDESC(mob), STRING_TERMINATOR,
        ldesc, STRING_TERMINATOR,
        ddesc, STRING_TERMINATOR);

  if (n < 0 || n >= MAX_STRING_LENGTH) {
    if (n < 0)
      mudlog(BRF, LVL_BUILDER, TRUE,
             "SYSERR: Could not format mobile #%d for saving.",
             mvnum);
    else
      mudlog(BRF, LVL_BUILDER, TRUE,
             "SYSERR: Could not save mobile #%d due to size (%d >= maximum of %d).",
             mvnum, n, MAX_STRING_LENGTH);
    return FALSE;
  }

  tb_fprintf(fd, "%s", convert_from_tabs(buf));

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

  tb_fprintf(fd, "%d %d\n"
      "%d %d %d\n",
      GET_GOLD(mob), GET_EXP(mob),
      GET_POS(mob), GET_DEFAULT_POS(mob), GET_SEX(mob));

  write_mobile_espec(mvnum, mob, fd);
  script_save_to_disk(fd, mob, MOB_TRIGGER);

#if CONFIG_GENOLC_MOBPROG
  if (write_mobile_mobprog(mvnum, mob, fd) < 0)
    log("SYSERR: GenOLC: Error writing MobProgs for mobile #%d.", mvnum);
#endif

  return TRUE;
}

/* Replace missing mob strings with placeholders (check_mobile_string).
 * mob->nr is used only to name the mob in the report; an OLC working copy
 * of a new mob carries NOBODY there, which must not index mob_index. */
void check_mobile_strings(struct char_data *mob)
{
  mob_vnum mvnum = (mob->nr != NOBODY && mob->nr <= top_of_mobt) ?
                   mob_index[mob->nr].vnum : NOBODY;
  check_mobile_string(mvnum, &GET_LDESC(mob), "long description");
  check_mobile_string(mvnum, &GET_DDESC(mob), "detailed description");
  check_mobile_string(mvnum, &GET_ALIAS(mob), "alias list");
  check_mobile_string(mvnum, &GET_SDESC(mob), "short description");
}

void check_mobile_string(mob_vnum i, char **string, const char *desc)
{
  if (*string == NULL || **string == '\0') {
    char smbuf[128];
    tb_snprintf(smbuf, sizeof(smbuf), "GenOLC: Mob #%d has an invalid %s.", i, desc);
    mudlog(BRF, LVL_GOD, TRUE, "%s", smbuf);
    if (*string)
      free(*string);
    *string = strdup("An undefined string.\n");
  }
}
