/* ***********************************************************************
*    File:   genqst.c                                 Part of CircleMUD  *
* Version:   2.0 (November 2005) Written for CircleMud CWG / Suntzu      *
* Purpose:   To provide special quest-related code.                      *
* Copyright: Kenneth Ray                                                 *
* Original Version Details:                                              *
* Copyright 1996 by Harvey Gilpin           *
* Copyright 1997-2001 by George Greer (greerga@circlemud.org)     *
************************************************************************ */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "comm.h"
#include "quest.h"
#include "genolc.h"
#include "genzon.h" /* for create_world_index */

/**
 * @file genqst.c
 * Quest table maintenance for OLC (qedit) and the quest file writer.
 *
 * Input: quests come from builders through qedit (string lengths capped by
 * qedit: MAX_QUEST_NAME/DESC/MSG) or from hand-edited .qst files at boot.
 *
 * Table: aquest_table[0..total_quests-1] is sorted by vnum; qst_rnum is
 * unsigned and NOTHING marks "no quest".  QST_MASTER is a mob VNUM, not an
 * rnum: resolve it with real_mobile() before indexing mob_index.
 *
 * Questmaster spec procs: a mob that is some quest's questmaster runs
 * questmaster().  If it already had another spec proc, that one is kept in
 * the quest's QST_FUNC and questmaster() chains to it; delete_quest()
 * restores it when the mob is no longer a questmaster of any quest.
 * Changing a quest's questmaster in qedit does not restore the previous
 * mob's spec proc (it stays questmaster() until reboot).
 */


/*-------------------------------------------------------------------*/

int copy_quest(struct aq_data *to, struct aq_data *from, int free_old_strings)
{
  int i;
  if (free_old_strings)
    free_quest_strings(to);
  to->vnum       = from->vnum;
  to->flags      = from->flags;
  to->type       = from->type;
  to->qm         = from->qm;
  to->target     = from->target;
  to->prereq     = from->prereq;
  to->prev_quest = from->prev_quest;
  to->next_quest = from->next_quest;
  for (i = 0; i < 7; i++){
    to->value[i] = from->value[i];
  }
  to->gold_reward = from->gold_reward;
  to->exp_reward  = from->exp_reward;
  to->obj_reward  = from->obj_reward;
  to->func        = from->func;
  return copy_quest_strings(to, from);
}

int copy_quest_strings(struct aq_data *to, struct aq_data *from)
{
  if (from == NULL || to == NULL) {
    log("SYSERR: GenQST: copy_quest_strings: Null values passed.");
    return FALSE;
  }
  to->name = str_udup(from->name);
  to->desc = str_udup(from->desc);
  to->info = str_udup(from->info);
  to->done = str_udup(from->done);
  to->quit = str_udup(from->quit);
  return TRUE;
}

void free_quest_strings(struct aq_data *quest)
{
  if (quest->name)
    free(quest->name);
  if (quest->desc)
    free(quest->desc);
  if (quest->info)
    free(quest->info);
  if (quest->done)
    free(quest->done);
  if (quest->quit)
    free(quest->quit);
}

void free_quest(struct aq_data *quest)
{
  free_quest_strings(quest);
  free(quest);
}

/*-------------------------------------------------------------------*/

/**
 * Add a quest, or update the quest with the same vnum, from qedit's copy.
 *
 * Strings are deep-copied; nqst keeps its own.  Inserting shifts later
 * quests up one slot (their string pointers move with them, so the new
 * slot is filled without freeing).  The questmaster mob (if it has a
 * prototype) gets questmaster() as its spec proc, its previous spec proc
 * being saved in QST_FUNC.
 *
 * @return The quest's rnum.
 */
int add_quest(struct aq_data *nqst)
{
  qst_rnum rnum;
  mob_rnum qmrnum;
  zone_rnum rznum = real_zone_by_thing(nqst->vnum);

  /* The quest already exists, just update it.  */
  if ((rnum = real_quest(nqst->vnum)) != NOWHERE) {
    copy_quest(&aquest_table[rnum], nqst, TRUE);
  } else {
    /* increase the number of quest table entries */
    total_quests++;
    RECREATE(aquest_table, struct aq_data, total_quests );
    /* Initialise top quest strings to null */
    QST_NAME(total_quests - 1) = NULL;
    QST_DESC(total_quests - 1) = NULL;
    QST_INFO(total_quests - 1) = NULL;
    QST_DONE(total_quests - 1) = NULL;
    QST_QUIT(total_quests - 1) = NULL;
    /* Now process enties from the top down to see where the new one goes */
    for (rnum = total_quests - 1; rnum > 0; rnum--) {
      if (nqst->vnum > QST_NUM(rnum - 1))
        break; //found the place
      aquest_table[rnum] = aquest_table[rnum - 1]; //shift quest up one
    }
    copy_quest(&aquest_table[rnum], nqst, FALSE);
  }
  qmrnum = real_mobile(QST_MASTER(rnum));
  /* Make sure we assign spec procs to the questmaster */
  if (qmrnum != NOBODY && mob_index[qmrnum].func &&
     mob_index[qmrnum].func != questmaster)
     quest_set_qm_secondary(QST_MASTER(rnum), mob_index[qmrnum].func);
  if(qmrnum != NOBODY) 
    mob_index[qmrnum].func = questmaster;

  /* And make sure we save the updated quest information to disk */
  if (rznum != NOWHERE)
    add_to_save_list(zone_table[rznum].number, SL_QST);
  else
    mudlog(BRF, LVL_BUILDER, TRUE,
           "SYSERR: GenOLC: Cannot determine quest zone.");

  return rnum;
}

/*-------------------------------------------------------------------*/

/* Take player ch off quest vnum qvnum, which delete_quest() just removed,
 * as quest_quit() does for a quest that no longer exists.  A player left
 * on it would keep a vnum that real_quest() no longer finds (do_score read
 * aquest_table[NOTHING] for it) and save it to the player file. */
static void delete_quest_clear_player(struct char_data *ch, qst_vnum qvnum)
{
  if (IS_NPC(ch) || !ch->player_specials || GET_QUEST(ch) != qvnum)
    return;
  TB_WARN_MSG("olc", "quest-delete-player", "quest",
              "quest vnum %d deleted while " DIAG_CHAR_FMT " was on it; quest cleared",
              qvnum, DIAG_CHAR(ch));
  clear_quest(ch);
  send_to_char(ch, "The quest you were on has been removed.\r\n");
}

/* TRUE when dsc holds a loaded player who is out of the game (login MOTD,
 * main menu and the menu's prompts), so not on character_list. */
static bool delete_quest_menu_player(const struct descriptor_data *dsc)
{
  switch (STATE(dsc)) {
  case CON_RMOTD:
  case CON_MENU:
  case CON_PLR_DESC:
  case CON_CHPWD_GETOLD:
  case CON_CHPWD_GETNEW:
  case CON_CHPWD_VRFY:
  case CON_DELCNF1:
  case CON_DELCNF2:
    return dsc->character != NULL;
  default:
    return FALSE;
  }
}

/**
 * Delete quest rnum (qedit delete).
 *
 * Precondition: rnum < total_quests; NOTHING (a quest never saved) and any
 * other out-of-range value are reported and refused.  Frees the quest's
 * strings, shifts later quests down, restores the questmaster's saved
 * spec proc when no remaining quest uses that mob, and takes every loaded
 * player who was on the quest off it.
 *
 * @return TRUE when deleted; FALSE, reported, otherwise.
 */
int delete_quest(qst_rnum rnum)
{
  qst_rnum i;
  zone_rnum rznum;
  mob_vnum qm;
  mob_rnum qmr;
  qst_vnum deleted_vnum;
  SPECIAL (*tempfunc);
  int  quests_remaining = 0;
  struct char_data *ch;
  struct descriptor_data *dsc;

  /* qedit can confirm 'delete' on a quest that was never saved, so the
   * caller's real_quest() may return NOTHING; check before any table read. */
  if (rnum == NOTHING || rnum >= total_quests) {
    TB_ERROR_MSG("olc", "quest-delete-range", "quest",
                 "rnum %d out of range 0..%d", (int)rnum, (int)total_quests - 1);
    return FALSE;
  }
  qm = QST_MASTER(rnum);
  deleted_vnum = QST_NUM(rnum);
  rznum = real_zone_by_thing(QST_NUM(rnum));
  log("GenOLC: delete_quest: Deleting quest #%d (%s).",
       QST_NUM(rnum), QST_NAME(rnum));
  /* make a note of the quest master's secondary spec proc; any of the QM's
   * quests may hold it (see quest_qm_secondary()). */
  tempfunc = quest_qm_secondary(qm);


  free_quest_strings(&aquest_table[rnum]);
  for (i = rnum; i < total_quests - 1; i++) {
    aquest_table[i] = aquest_table[i + 1];
  }
  total_quests--;
  if (total_quests > 0)
    RECREATE(aquest_table, struct aq_data, total_quests);
  else {
    free(aquest_table);
    aquest_table = NULL; 
   }
  if (rznum != NOWHERE)
     add_to_save_list(zone_table[rznum].number, SL_QST);
  else
    mudlog(BRF, LVL_BUILDER, TRUE,
           "SYSERR: GenOLC: Cannot determine quest zone.");
  /* does the questmaster mob have any quests left?  QST_MASTER is a mob
   * VNUM; mob_index[] must be indexed by the prototype's rnum. */
  if (qm != NOBODY) {
    for (i = 0; i < total_quests; i++) {
      if (QST_MASTER(i) == qm)
        quests_remaining++;
    }
    /* QST_MASTER is a mob VNUM: mob_index[] is indexed by rnum. */
    if (quests_remaining == 0) {
      if ((qmr = real_mobile(qm)) == NOBODY)
        TB_ERROR_MSG("genqst", "delete-quest-qm", "mob_index",
                     "quest vnum %d qm mob vnum %d has no rnum; spec proc not restored",
                     (int) deleted_vnum, (int) qm);
      else
        mob_index[qmr].func = tempfunc; // point back to original spec proc
    }
  }
  /* Players in the game (link-dead ones included) are on character_list;
   * players at the menu are not, and are reached through their
   * descriptors. */
  for (ch = character_list; ch; ch = ch->next)
    delete_quest_clear_player(ch, qvnum);
  for (dsc = descriptor_list; dsc; dsc = dsc->next)
    if (delete_quest_menu_player(dsc))
      delete_quest_clear_player(dsc->character, qvnum);
  return TRUE;
}

/*-------------------------------------------------------------------*/

int save_quests(zone_rnum zone_num)
{
  FILE *sf;
  char filename[128], oldname[128], quest_flags[MAX_STRING_LENGTH];
  char quest_desc[MAX_STRING_LENGTH], quest_info[MAX_STRING_LENGTH];
  char quest_done[MAX_STRING_LENGTH], quest_quit[MAX_STRING_LENGTH];
  char buf[MAX_STRING_LENGTH];
  int i, num_quests = 0;

  if (!VALID_ZONE_RNUM(zone_num)) {
    log("SYSERR: GenOLC: save_quests: Invalid zone number %d passed! (0-%d)",
         zone_num, top_of_zone_table);
    return FALSE;
  }

  log("GenOLC: save_quests: Saving quests in zone #%d (%d-%d).",
        zone_table[zone_num].number,
 genolc_zone_bottom(zone_num), zone_table[zone_num].top);

  tb_snprintf(filename, sizeof(filename), "%s/%d.new",
 QST_PREFIX, zone_table[zone_num].number);
  if (!(sf = fopen(filename, "w"))) {
    TB_REPORT_ERRNO("fopen", filename);
    return FALSE;
  }
  for (i = genolc_zone_bottom(zone_num); i <= zone_table[zone_num].top; i++) {
    qst_rnum rnum;
    if ((rnum = real_quest(i)) != NOTHING) {
      /* Copy the text strings and strip off trailing newlines. */
      strlcpy(quest_desc, QST_DESC(rnum) ? QST_DESC(rnum) : "undefined",
              sizeof(quest_desc));
      strlcpy(quest_info, QST_INFO(rnum) ? QST_INFO(rnum) : "undefined",
              sizeof(quest_info));
      strlcpy(quest_done, QST_DONE(rnum) ? QST_DONE(rnum) : "undefined",
              sizeof(quest_done));
      strlcpy(quest_quit, QST_QUIT(rnum) ? QST_QUIT(rnum) : "undefined",
              sizeof(quest_quit));
      strip_cr(quest_desc);
      strip_cr(quest_info);
      strip_cr(quest_done);
      strip_cr(quest_quit);
      /* Save the quest details to the file.  */
      sprintascii(quest_flags, QST_FLAGS(rnum));
      int n = snprintf(buf, MAX_STRING_LENGTH,
        "#%d\n"
        "%s%c\n"
        "%s%c\n"
        "%s%c\n"
        "%s%c\n"
        "%s%c\n"
        "%d %d %s %d %d %d %d\n"
        "%d %d %d %d %d %d %d\n"
        "%d %d %d\n"
        "S\n",
        QST_NUM(rnum),
        QST_NAME(rnum) ? QST_NAME(rnum) : "Untitled", STRING_TERMINATOR,
        quest_desc, STRING_TERMINATOR,
        quest_info, STRING_TERMINATOR,
        quest_done, STRING_TERMINATOR,
        quest_quit, STRING_TERMINATOR,
        QST_TYPE(rnum),
        QST_MASTER(rnum) == NOBODY ? -1 : QST_MASTER(rnum),
        quest_flags,
        QST_TARGET(rnum) == NOTHING ? -1 : QST_TARGET(rnum),
        QST_PREV(rnum)   == NOTHING ? -1 : QST_PREV(rnum),
        QST_NEXT(rnum)   == NOTHING ? -1 : QST_NEXT(rnum),
        QST_PREREQ(rnum) == NOTHING ? -1 : QST_PREREQ(rnum),
        QST_POINTS(rnum), QST_PENALTY(rnum), QST_MINLEVEL(rnum),
        QST_MAXLEVEL(rnum), QST_TIME(rnum),
        QST_RETURNMOB(rnum) == NOBODY ? -1 : QST_RETURNMOB(rnum),
        QST_QUANTITY(rnum), QST_GOLD(rnum), QST_EXP(rnum), QST_OBJ(rnum)
      );
      
      /* A record that does not fit (only reachable from a hand-edited .qst:
       * qedit caps each string far below MSL) must not be skipped while the
       * rest of the zone is committed, or the quest silently disappears
       * from disk.  Abandon the whole zone save, as save_objects() does. */
      if (n < 0 || n >= MAX_STRING_LENGTH) {
        TB_ERROR_MSG("olc", "quest-save-size", filename,
                     "quest vnum %d: record %d bytes >= %d; zone %d not saved",
                     QST_NUM(rnum), n, MAX_STRING_LENGTH,
                     zone_table[zone_num].number);
        mudlog(BRF, LVL_BUILDER, TRUE,
               "SYSERR: Could not save quest #%d due to size (%d >= maximum of %d); "
               "zone %d not saved.", QST_NUM(rnum), n, MAX_STRING_LENGTH,
               zone_table[zone_num].number);
        tb_fclose(sf);
        if (remove(filename) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", filename);
        return FALSE;
      }
      tb_fprintf(sf, "%s", convert_from_tabs(buf));
      num_quests++;
    }
  }
  /* Write the final line and commit only a fully flushed temporary file. */
  tb_fprintf(sf, "$~\n");
  {
    /* ferror() stays set after any failed write.  tb_fprintf() reported
     * each one, but a later flush that succeeds would hide it and commit a
     * file with a hole in it (C2-16). */
    int stream_ok = !ferror(sf);
    int flush_ok = tb_fflush_checked(sf, filename);
    int close_ok = tb_fclose_checked(sf, filename);
    if (!stream_ok)
      TB_ERROR_MSG("olc", "save-stream-error", filename,
                   TB_ERRID_PERSIST_WRITE " zone %d quests: a write failed; previous file kept",
                   zone_table[zone_num].number);
    if (!stream_ok || !flush_ok || !close_ok) {
      if (remove(filename) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", filename);
      return FALSE;
    }
  }

  /* Old file we're replacing. */
  tb_snprintf(oldname, sizeof(oldname), "%s/%d.qst",
           QST_PREFIX, zone_table[zone_num].number);
  if (!tb_replace_file_checked(filename, oldname))
    return FALSE;

  /* Do we need to update the index file? */
  /* The .qst file is committed, but without an index entry it is not
   * loaded at boot: keep the zone in the save list so a later save retries. */
  if (num_quests > 0 && !create_world_index(zone_table[zone_num].number, "qst"))
    return FALSE;

  if (in_save_list(zone_table[zone_num].number, SL_QST))
    remove_from_save_list(zone_table[zone_num].number, SL_QST);
  return TRUE;
}

