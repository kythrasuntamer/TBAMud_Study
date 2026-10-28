/* ***********************************************************************
*    File:   quest.c                                  Part of CircleMUD  *
* Version:   2.1 (December 2005) Written for CircleMud CWG / Suntzu      *
* Purpose:   To provide special quest-related code.                      *
* Copyright: Kenneth Ray                                                 *
* Original Version Details:                                              *
* Morgaelin - quest.c                                                    *
* Copyright (C) 1997 MS                                                  *
*********************************************************************** */

/**
 * @file quest.c
 * Autoquests: loading (.qst files), the questmaster spec proc, the 'quest'
 * command, and progress/completion checks driven from the game loop.
 *
 * Ownership: aquest_table (db.c) holds total_quests rows in vnum order; it
 * is filled at boot by parse_quest() and edited in place by qedit
 * (genqst.c add_quest/delete_quest), which may reallocate it, so never keep
 * a row pointer or rnum across OLC. Players refer to quests by vnum only
 * (GET_QUEST, completed_quests[]); resolve with real_quest() and check with
 * VALID_QST_RNUM() each time, because quests can be deleted under them.
 *
 * Who controls input: quest records are builder data (world files, qedit),
 * including prev/next links, which are not repaired on delete; quest
 * numbers typed after 'quest list/join/status' come from players.
 */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "comm.h"
#include "screen.h"
#include "quest.h"
#include "act.h" /* for do_tell */


/*--------------------------------------------------------------------------
 * Exported global variables
 *--------------------------------------------------------------------------*/
const char *quest_types[] = {
  "Object",
  "Room",
  "Find mob",
  "Kill mob",
  "Save mob",
  "Return object",
  "Clear room",
  "\n"
};
const char *aq_flags[] = {
  "REPEATABLE",
  "\n"
};


/*--------------------------------------------------------------------------
 * Local (file scope) global variables
 *--------------------------------------------------------------------------*/
static int cmd_tell;

static const char *quest_cmd[] = {
  "list", "history", "join", "leave", "progress", "status", "\n"};

static const char *quest_mort_usage =
  "Usage: quest list | history | progress | join <nn> | leave";

static const char *quest_imm_usage =
  "Usage: quest list | history | progress | join <nn> | leave | status <vnum>";

/*--------------------------------------------------------------------------*/
/* Utility Functions                                                        */
/*--------------------------------------------------------------------------*/

qst_rnum real_quest(qst_vnum vnum)
{
  int rnum;

  for (rnum = 0; rnum < total_quests; rnum++)
    if (QST_NUM(rnum) == vnum)
      return(rnum);
  return(NOTHING);
}

int is_complete(struct char_data *ch, qst_vnum vnum)
{
  int i;

  for (i = 0; i < GET_NUM_QUESTS(ch); i++)
    if (ch->player_specials->saved.completed_quests[i] == vnum)
      return TRUE;
  return FALSE;
}

/* Report a prev/next link of quest `from` that names no existing quest.
 * Links are builder data: qedit stores any number typed and delete_quest()
 * never repairs the links that pointed at the deleted quest. */
static void quest_report_dangling(qst_rnum from, qst_vnum link)
{
  TB_WARN_MSG("quest", "dangling-chain", "aquest_table",
              "quest vnum %d prev %d next %d: linked quest vnum %d does not exist",
              (int) QST_NUM(from), (int) QST_PREV(from), (int) QST_NEXT(from),
              (int) link);
}

/* Display text (name or description) of the quest that link `link` of quest
 * `from` names, or a "(missing quest N)" marker, reported, when the link
 * dangles. The marker lives in a static buffer overwritten by the next call. */
static const char *quest_link_text(qst_rnum from, qst_vnum link, int want_name)
{
  static char missing[48];
  qst_rnum to = real_quest(link);
  const char *text;

  if (VALID_QST_RNUM(to)) {
    text = want_name ? QST_NAME(to) : QST_DESC(to);
    return text ? text : "";
  }
  quest_report_dangling(from, link);
  tb_snprintf(missing, sizeof(missing), "(missing quest %d)", (int) link);
  return missing;
}

qst_vnum find_quest_by_qmnum(struct char_data *ch, mob_vnum qm, int num)
{
  qst_rnum rnum;
  int found=0;
  for (rnum = 0; rnum < total_quests; rnum++) {
    if (qm == QST_MASTER(rnum))
      if (++found == num)
        return (QST_NUM(rnum));
  }
  return NOTHING;
}

/*--------------------------------------------------------------------------*/
/* Quest Loading and Unloading Functions                                    */
/*--------------------------------------------------------------------------*/

void destroy_quests(void)
{
  qst_rnum rnum = 0;

  if (!aquest_table)
    return;

  for (rnum = 0; rnum < total_quests; rnum++){
    free_quest_strings(&aquest_table[rnum]);
  }
  free(aquest_table);
  aquest_table = NULL;
  total_quests = 0;

  return;
}

int count_quests(qst_vnum low, qst_vnum high)
{
  int i, j;

  if (!aquest_table)
    return 0;

  for (i = j = 0; i < total_quests; i++)
    if (QST_NUM(i) >= low && QST_NUM(i) <= high)
      j++;

  return j;
}

/* Quest records contain whitespace-separated signed integers. */
static int quest_parse_int(const char **cursor, int *out)
{
  char *end;
  long value;
  const char *p = *cursor;
  while (isspace((unsigned char)*p))
    p++;
  errno = 0;
  value = strtol(p, &end, 10);
  if (p == end || errno == ERANGE || value < INT_MIN || value > INT_MAX ||
      (*end && !isspace((unsigned char)*end)))
    return FALSE;
  *out = (int)value;
  *cursor = end;
  return TRUE;
}

static int quest_parse_record(const char *line, int *fields, size_t count,
                              char *flags, size_t flag_size)
{
  size_t i;
  const char *p = line;
  for (i = 0; i < count; i++) {
    if (flags && i == 2) {
      size_t n = 0;
      while (isspace((unsigned char)*p))
        p++;
      while (p[n] && !isspace((unsigned char)p[n]))
        n++;
      if (!n || n >= flag_size)
        return FALSE;
      memcpy(flags, p, n);
      flags[n] = '\0';
      p += n;
    }
    if (!quest_parse_int(&p, &fields[i]))
      return FALSE;
  }
  while (isspace((unsigned char)*p))
    p++;
  return *p == '\0';
}

/* A malformed .qst record stops the boot (as every world-file loader does),
 * naming the quest vnum, the line that failed and the file offset. The raw
 * line is not logged. Expands at the call site so the record names the
 * failing check. */
#define QUEST_PARSE_FATAL(fl, vnum, what) do { \
    TB_FATAL_MSG("quest", "parse", "qst-file", \
                 "quest vnum %d: missing or malformed %s line near byte %ld", \
                 (vnum), (what), ftell(fl)); \
    exit(1); \
  } while (0)

/**
 * Read one quest record (the part after "#<nr>") from a .qst file into the
 * next free row of aquest_table.
 *
 * Record grammar:
 *   name~ / desc~ / info~ / done~ / quit~     (fread_string, '~'-terminated)
 *   <type> <qm vnum> <flags> <target> <prev> <next> <prereq>
 *   <points> <penalty> <minlvl> <maxlvl> <time> <return mob> <quantity>
 *   <gold> <exp> <obj vnum>
 *   S                                        (end of record)
 * -1 in target/prev/next/prereq/obj means "none" (stored as NOTHING).
 *
 * Validation: every number must parse as an int; type must be in
 * [0, NUM_AQ_TYPES) (boot stops otherwise). The QM vnum is checked against
 * the mob prototypes, so mobs must be loaded first (boot_world order); an
 * unknown QM is reported and stored as NOBODY. Chain links (prev/next) are
 * cross-checked later, in assign_the_quests(), once every quest is loaded.
 * Levels, quantity, time and rewards are taken as written.
 *
 * The row index is a function-static counter shared by every call:
 * index_boot() sizes aquest_table for the whole set of files and calls this
 * once per record, and total_quests follows the counter. Input is world-file
 * (builder) data.
 */
void parse_quest(FILE *quest_f, int nr)
{
  static char line[256];
  static int i = 0, j;
  int t[7];
  char f1[128], buf2[MAX_STRING_LENGTH];
  aquest_table[i].vnum = nr;
  aquest_table[i].qm = NOBODY;
  aquest_table[i].name = NULL;
  aquest_table[i].desc = NULL;
  aquest_table[i].info = NULL;
  aquest_table[i].done = NULL;
  aquest_table[i].quit = NULL;
  aquest_table[i].flags = 0;
  aquest_table[i].type = -1;
  aquest_table[i].target = -1;
  aquest_table[i].prereq = NOTHING;
  for (j = 0; j < 7; j++)
    aquest_table[i].value[j] = 0;
  aquest_table[i].prev_quest = NOTHING;
  aquest_table[i].next_quest = NOTHING;
  aquest_table[i].func = NULL;

  aquest_table[i].gold_reward = 0;
  aquest_table[i].exp_reward  = 0;
  aquest_table[i].obj_reward  = NOTHING;

  /* begin to parse the data */
  aquest_table[i].name = fread_string(quest_f, buf2);
  aquest_table[i].desc = fread_string(quest_f, buf2);
  aquest_table[i].info = fread_string(quest_f, buf2);
  aquest_table[i].done = fread_string(quest_f, buf2);
  aquest_table[i].quit = fread_string(quest_f, buf2);
  if (!get_line(quest_f, line) ||
      !quest_parse_record(line, t, 6, f1, sizeof(f1)))
    QUEST_PARSE_FATAL(quest_f, nr, "header (type qm flags target prev next prereq)");
  /* The type indexes quest_types[] and selects the trigger check; only qedit
   * limits it, so a hand-edited .qst file can hold anything. */
  if (t[0] < 0 || t[0] >= NUM_AQ_TYPES) {
    TB_FATAL_MSG("quest", "parse-type-range", "qst-file",
                 "quest vnum %d type %d out of range [0,%d) near byte %ld",
                 nr, t[0], NUM_AQ_TYPES, ftell(quest_f));
    exit(1);
  }
  aquest_table[i].type = t[0];
  /* Mobiles are loaded before quests (boot_world), so an unknown QM vnum here
   * is a real world-file error; the quest still loads but has no QM. */
  if (real_mobile(t[1]) == NOBODY) {
    TB_WARN_MSG("quest", "unknown-questmaster", "aquest_table",
                "quest vnum %d qm mob vnum %d does not exist; quest has no questmaster",
                nr, t[1]);
    aquest_table[i].qm = NOBODY;
  } else
    aquest_table[i].qm = t[1];
  aquest_table[i].flags = asciiflag_conv(f1);
  aquest_table[i].target = (t[2] == -1) ? NOTHING : t[2];
  aquest_table[i].prev_quest = (t[3] == -1) ? NOTHING : t[3];
  aquest_table[i].next_quest = (t[4] == -1) ? NOTHING : t[4];
  aquest_table[i].prereq = (t[5] == -1) ? NOTHING : t[5];
  if (!get_line(quest_f, line) ||
      !quest_parse_record(line, t, 7, NULL, 0))
    QUEST_PARSE_FATAL(quest_f, nr, "values (7 integers)");
  for (j = 0; j < 7; j++)
    aquest_table[i].value[j] = t[j];
  if (!get_line(quest_f, line) ||
      !quest_parse_record(line, t, 3, NULL, 0))
    QUEST_PARSE_FATAL(quest_f, nr, "rewards (gold exp obj)");

  aquest_table[i].gold_reward = t[0];
  aquest_table[i].exp_reward  = t[1];
  aquest_table[i].obj_reward  = (t[2] == -1) ? NOTHING : t[2];
  /* Legal, but almost certainly a builder mistake: picking up the target
   * completes the quest and hands out another copy of the target. */
  if (aquest_table[i].type == AQ_OBJ_FIND &&
      aquest_table[i].obj_reward != NOTHING &&
      aquest_table[i].obj_reward == aquest_table[i].target)
    TB_WARN_MSG("quest", "reward-equals-target", "aquest_table",
                "quest vnum %d type %d target obj vnum %d is also the reward",
                nr, aquest_table[i].type, (int) aquest_table[i].target);

  for (;;) {
    if (!get_line(quest_f, line))
      QUEST_PARSE_FATAL(quest_f, nr, "terminator ('S' before end of file)");
    switch(*line) {
    case 'S':
      total_quests = ++i;
      return;
    default:
      /* No other record lines are defined; say what is being ignored. */
      TB_WARN_MSG("quest", "parse-skip-line", "qst-file",
                  "quest vnum %d: ignoring unknown line starting 0x%02x (%zu bytes) near byte %ld",
                  nr, (unsigned char) *line, strlen(line), ftell(quest_f));
      break;
    }
  }
} /* parse_quest */

/* Questmaster secondary spec procs.
 *
 * A questmaster mob's mob_index[].func is replaced by questmaster(); the
 * mob's original proc (shop_keeper, a guild, ...) is parked in QST_FUNC of
 * that mob's quests, and questmaster() calls it first. Invariant: any quest
 * of the QM may hold it. qedit-created quests start with QST_FUNC == NULL
 * and add_quest() inserts them in vnum order, so the proc must be looked up
 * across all of the QM's quests, never only in the first one. */
quest_spec_proc *quest_qm_secondary(mob_vnum qm)
{
  qst_rnum rnum;

  if (qm == NOBODY)
    return NULL;
  for (rnum = 0; rnum < total_quests; rnum++)
    if (QST_MASTER(rnum) == qm && QST_FUNC(rnum) &&
        QST_FUNC(rnum) != questmaster)
      return QST_FUNC(rnum);
  return NULL;
}

/* Record func as the secondary proc on every quest of questmaster qm. */
void quest_set_qm_secondary(mob_vnum qm, quest_spec_proc *func)
{
  qst_rnum rnum;

  if (qm == NOBODY || func == questmaster)
    return;
  for (rnum = 0; rnum < total_quests; rnum++)
    if (QST_MASTER(rnum) == qm)
      QST_FUNC(rnum) = func;
}

void assign_the_quests(void)
{
  qst_rnum rnum;
  mob_rnum mrnum;

  cmd_tell = find_command("tell");

  for (rnum = 0; rnum < total_quests; rnum ++) {
    /* All quests are loaded now: report chain links naming no quest. */
    if (QST_PREV(rnum) != NOTHING && real_quest(QST_PREV(rnum)) == NOTHING)
      quest_report_dangling(rnum, QST_PREV(rnum));
    if (QST_NEXT(rnum) != NOTHING && real_quest(QST_NEXT(rnum)) == NOTHING)
      quest_report_dangling(rnum, QST_NEXT(rnum));
    if (QST_MASTER(rnum) == NOBODY) {
      TB_WARN_MSG("quest", "no-questmaster", "aquest_table",
                  "quest vnum %d has no questmaster; nobody can offer it",
                  (int) QST_NUM(rnum));
      continue;
    }
    if ((mrnum = real_mobile(QST_MASTER(rnum))) == NOBODY) {
      TB_WARN_MSG("quest", "unknown-questmaster", "aquest_table",
                  "quest vnum %d qm mob vnum %d does not exist",
                  (int) QST_NUM(rnum), (int) QST_MASTER(rnum));
      continue;
    }
    if (mob_index[(mrnum)].func &&
 mob_index[(mrnum)].func != questmaster)
      quest_set_qm_secondary(QST_MASTER(rnum), mob_index[(mrnum)].func);
    mob_index[(mrnum)].func = questmaster;
  }
}

/*--------------------------------------------------------------------------*/
/* Quest Completion Functions                                               */
/*--------------------------------------------------------------------------*/
void set_quest(struct char_data *ch, qst_rnum rnum)
{
  GET_QUEST(ch) = QST_NUM(rnum);
  GET_QUEST_TIME(ch) = QST_TIME(rnum);
  GET_QUEST_COUNTER(ch) = QST_QUANTITY(rnum);
  SET_BIT_AR(PRF_FLAGS(ch), PRF_QUEST);
  return;
}

void clear_quest(struct char_data *ch)
{
  GET_QUEST(ch) = NOTHING;
  GET_QUEST_TIME(ch) = -1;
  GET_QUEST_COUNTER(ch) = 0;
  REMOVE_BIT_AR(PRF_FLAGS(ch), PRF_QUEST);
  return;
}

/* NOLINTBEGIN(clang-analyzer-security.ArrayBound): completed_quests length is tracked by GET_NUM_QUESTS. */
void add_completed_quest(struct char_data *ch, qst_vnum vnum)
{
  qst_vnum *temp;
  int count, i;

  if (!ch || !ch->player_specials) {
    TB_ERROR_MSG("quest", "completed-list-owner", "player_specials",
                 "quest vnum %d completed by %s", (int) vnum,
                 ch ? "a character with no player_specials" : "NULL character");
    return;
  }

  /* Invariant: GET_NUM_QUESTS(ch) is the length of completed_quests (both
   * come from the pfile). If they disagree the list is rebuilt from what is
   * usable, and the lost history is reported. */
  count = GET_NUM_QUESTS(ch);
  if (count < 0) {
    TB_ERROR_MSG("quest", "completed-list-corrupt", "player_specials",
                 DIAG_CHAR_FMT " completed quest count %d is negative; reset to 0",
                 DIAG_CHAR(ch), count);
    count = 0;
  }
  if (count > 0 && !ch->player_specials->saved.completed_quests) {
    TB_ERROR_MSG("quest", "completed-list-corrupt", "player_specials",
                 DIAG_CHAR_FMT " completed quest count %d but no list; history dropped",
                 DIAG_CHAR(ch), count);
    count = 0;
  }

  CREATE(temp, qst_vnum, count + 1);
  for (i = 0; i < count; i++)
    temp[i] = ch->player_specials->saved.completed_quests[i]; /* NOLINT(clang-analyzer-security.ArrayBound): source count is tracked with the list. */

  temp[count] = vnum;

  free(ch->player_specials->saved.completed_quests);
  ch->player_specials->saved.completed_quests = temp;
  GET_NUM_QUESTS(ch) = count + 1;
}

void remove_completed_quest(struct char_data *ch, qst_vnum vnum)
{
  qst_vnum *old, *temp;
  int count, found = -1, i, j;

  if (!ch || !ch->player_specials)
    return;

  count = GET_NUM_QUESTS(ch);
  old = ch->player_specials->saved.completed_quests;
  if (count <= 0 || !old)
    return;

  for (i = 0; i < count; i++) {
    if (old[i] == vnum) {
      found = i;
      break;
    }
  }

  if (found < 0)
    return;

  if (count == 1) {
    free(old);
    ch->player_specials->saved.completed_quests = NULL;
    GET_NUM_QUESTS(ch) = 0;
    return;
  }

  CREATE(temp, qst_vnum, count - 1);
  for (i = 0, j = 0; i < count; i++)
    if (i != found)
      temp[j++] = old[i];

  free(old);
  ch->player_specials->saved.completed_quests = temp;
  GET_NUM_QUESTS(ch) = j;
}

/* NOLINTEND(clang-analyzer-security.ArrayBound) */

/* TRUE while generic_complete_quest() is paying out rewards. */
static int completing_quest = FALSE;

/**
 * Count one goal of ch's active quest and, when the counter reaches zero,
 * complete it: record it (unless repeatable), clear it, pay points, gold,
 * exp and the reward object, then start the chained next quest if any.
 *
 * Re-entrancy: rewards call out of this file. gain_exp() can level the
 * player and obj_to_char() runs autoquest_trigger_check(AQ_OBJ_FIND) again.
 * The quest is cleared before any reward is paid, so a nested check finds no
 * active quest; a static guard reports any other re-entry. When this returns,
 * GET_QUEST(ch) may have changed (cleared, or the next quest in the chain),
 * so callers that loop must re-check it.
 *
 * @pre ch is a player whose GET_QUEST() is the quest being counted; a quest
 *      that no longer resolves is reported and cleared.
 */
void generic_complete_quest(struct char_data *ch)
{
  qst_rnum rnum;
  qst_vnum vnum = GET_QUEST(ch), next;
  struct obj_data *new_obj;
  int happy_qp, happy_gold, happy_exp;

  /* Invariant: the caller's active quest still exists in aquest_table.
   * Completion clears GET_QUEST(ch) and builders can delete quests with
   * qedit, so a stale or NOTHING vnum here would make every QST_*() below
   * read aquest_table[NOTHING]. Resolve before touching the counter. */
  if ((rnum = real_quest(vnum)) == NOTHING) {
    TB_ERROR_MSG("quest", "complete-missing-quest", "aquest_table",
                 DIAG_CHAR_FMT " quest vnum %d has no rnum (counter %d)",
                 DIAG_CHAR(ch), (int) vnum, GET_QUEST_COUNTER(ch));
    clear_quest(ch);
    return;
  }

  /* Invariant: completion is not re-entered. Rewards call out of this file
   * (obj_to_char() runs autoquest_trigger_check() for AQ_OBJ_FIND), and a
   * quest whose reward object is also its target would otherwise pay out
   * again on every nested level until the stack overflows. The quest is
   * cleared before any reward is handed out, so this only fires if a new
   * path re-enters; report it rather than paying twice. */
  if (completing_quest) {
    TB_ERROR_MSG("quest", "complete-reentry", "aquest_table",
                 DIAG_CHAR_FMT " quest vnum %d completion re-entered",
                 DIAG_CHAR(ch), (int) vnum);
    return;
  }

  if (--GET_QUEST_COUNTER(ch) <= 0) {
    next = QST_NEXT(rnum);
    /* The quest is finished before rewards are paid, so a reward that
     * triggers another quest check (see above) sees no active quest. */
    if (!IS_SET(QST_FLAGS(rnum), AQ_REPEATABLE))
      add_completed_quest(ch, vnum);
    clear_quest(ch);
    completing_quest = TRUE;
    if (IS_HAPPYHOUR && IS_HAPPYQP) {
      happy_qp = (int)(QST_POINTS(rnum) * (((float)(100+HAPPY_QP))/(float)100));
      happy_qp = MAX(happy_qp, 0);
      GET_QUESTPOINTS(ch) += happy_qp;
      send_to_char(ch,
          "%s\r\nYou have been awarded %d quest points for your service.\r\n",
          QST_DONE(rnum), happy_qp);
	} else {
      GET_QUESTPOINTS(ch) += QST_POINTS(rnum);
      send_to_char(ch,
          "%s\r\nYou have been awarded %d quest points for your service.\r\n",
          QST_DONE(rnum), QST_POINTS(rnum));
    }
    if (QST_GOLD(rnum)) {
      if ((IS_HAPPYHOUR) && (IS_HAPPYGOLD)) {
        happy_gold = (int)(QST_GOLD(rnum) * (((float)(100+HAPPY_GOLD))/(float)100));
        happy_gold = MAX(happy_gold, 0);
        increase_gold(ch, happy_gold);
        send_to_char(ch,
              "You have been awarded %d gold coins for your service.\r\n",
              happy_gold);
	  } else {
        increase_gold(ch, QST_GOLD(rnum));
        send_to_char(ch,
              "You have been awarded %d gold coins for your service.\r\n",
              QST_GOLD(rnum));
      }
    }
    if (QST_EXP(rnum)) {
      gain_exp(ch, QST_EXP(rnum));
      if ((IS_HAPPYHOUR) && (IS_HAPPYEXP)) {
        happy_exp = (int)(QST_EXP(rnum) * (((float)(100+HAPPY_EXP))/(float)100));
        happy_exp = MAX(happy_exp, 0);
        send_to_char(ch,
              "You have been awarded %d experience for your service.\r\n",
              happy_exp);
      } else {
        send_to_char(ch,
              "You have been awarded %d experience points for your service.\r\n",
              QST_EXP(rnum));
      }
    }
    if (QST_OBJ(rnum) && QST_OBJ(rnum) != NOTHING) {
      /* The reward vnum is builder data; the object may have been deleted
       * since the quest was written. Tell the player and the log. */
      if (real_object(QST_OBJ(rnum)) == NOTHING ||
          (new_obj = read_object((QST_OBJ(rnum)),VIRTUAL)) == NULL) {
        TB_ERROR_MSG("quest", "reward-missing", "obj_proto",
                     DIAG_CHAR_FMT " quest vnum %d reward obj vnum %d could not be loaded",
                     DIAG_CHAR(ch), (int) vnum, (int) QST_OBJ(rnum));
        send_to_char(ch, "The reward object for this quest could not be found; please tell an immortal.\r\n");
      } else {
        obj_to_char(new_obj, ch);
        send_to_char(ch, "You have been presented with %s%s for your service.\r\n",
            GET_OBJ_SHORT(new_obj), CCNRM(ch, C_NRM));
      }
    }
    completing_quest = FALSE;
    if (next != NOTHING && real_quest(next) == NOTHING)
      quest_report_dangling(rnum, next);
    else if ((real_quest(next) != NOTHING) &&
        (next != vnum) &&
        !is_complete(ch, next)) {
      rnum = real_quest(next);
      set_quest(ch, rnum);
      send_to_char(ch,
          "The next stage of your quest awaits:\r\n%s",
          QST_INFO(rnum));
    }
  }
  save_char(ch);
}

/**
 * Check whether an event advances ch's active quest of the given type.
 *
 * Callers and what each type needs:
 *   AQ_OBJ_FIND   obj_to_char()           object non-NULL
 *   AQ_ROOM_FIND  char_to_room()          IN_ROOM(ch) valid
 *   AQ_MOB_FIND   char_to_room()          IN_ROOM(ch) valid
 *   AQ_MOB_KILL   raw_kill()              vict non-NULL
 *   AQ_MOB_SAVE   raw_kill()              IN_ROOM(ch) valid; vict may be NULL
 *   AQ_OBJ_RETURN perform_give()          vict and object non-NULL
 *   AQ_ROOM_CLEAR raw_kill()              IN_ROOM(ch) valid
 * Every caller has just placed ch in a room or acted on vict/object, which
 * is what makes those guarantees hold.
 *
 * AQ_OBJ_RETURN extracts the returned object on completion: the caller must
 * not use object afterwards. Completion can change GET_QUEST(ch) (see
 * generic_complete_quest()).
 */
void autoquest_trigger_check(struct char_data *ch, struct char_data *vict,
                struct obj_data *object, int type)
{
  struct char_data *i;
  qst_rnum rnum;
  qst_vnum active;
  int found = TRUE;

  if (IS_NPC(ch))
    return;
  if ((active = GET_QUEST(ch)) == NOTHING)  /* No current quest, skip this */
    return;
  if (GET_QUEST_TYPE(ch) != type)
    return;
  if ((rnum = real_quest(GET_QUEST(ch))) == NOTHING)
    return;
  switch (type) {
    case AQ_OBJ_FIND:
      if (QST_TARGET(rnum) == GET_OBJ_VNUM(object))
        generic_complete_quest(ch);
      break;
    case AQ_ROOM_FIND:
      if (QST_TARGET(rnum) == world[IN_ROOM(ch)].number)
        generic_complete_quest(ch);
      break;
    case AQ_MOB_FIND:
      /* Each matching NPC counts once towards the quantity, but only while
       * this quest is still the active one: generic_complete_quest() clears
       * the quest (or chains the next one) when the counter reaches zero,
       * and later matches must not count against whatever follows. */
      for (i=world[IN_ROOM(ch)].people; i && GET_QUEST(ch) == active;
           i = i->next_in_room)
        if (IS_NPC(i))
          if (QST_TARGET(rnum) == GET_MOB_VNUM(i))
            generic_complete_quest(ch);
      break;
    case AQ_MOB_KILL:
      if (!IS_NPC(ch) && IS_NPC(vict) && (ch != vict))
          if (QST_TARGET(rnum) == GET_MOB_VNUM(vict))
            generic_complete_quest(ch);
      break;
    case AQ_MOB_SAVE:
       if (ch == vict)
        found = FALSE;
      for (i = world[IN_ROOM(ch)].people; i && found; i = i->next_in_room)
          if (i && IS_NPC(i) && !MOB_FLAGGED(i, MOB_NOTDEADYET))
            if ((GET_MOB_VNUM(i) != QST_TARGET(rnum)) &&
                !AFF_FLAGGED(i, AFF_CHARM))
              found = FALSE;
      if (found)
        generic_complete_quest(ch);
      break;
    case AQ_OBJ_RETURN:
      if (IS_NPC(vict) && (GET_MOB_VNUM(vict) == QST_RETURNMOB(rnum)))
        if (object && (GET_OBJ_VNUM(object) == QST_TARGET(rnum))) {
          generic_complete_quest(ch);
          extract_obj(object);
        }
      break;
    case AQ_ROOM_CLEAR:
      if (QST_TARGET(rnum) == world[IN_ROOM(ch)].number) {
        for (i = world[IN_ROOM(ch)].people; i && found; i = i->next_in_room)
          if (i && IS_NPC(i) && !MOB_FLAGGED(i, MOB_NOTDEADYET))
            found = FALSE;
        if (found)
   generic_complete_quest(ch);
      }
      break;
    default:
      TB_ERROR_MSG("quest", "trigger-type", "aquest_table",
                   DIAG_CHAR_FMT " quest vnum %d has invalid type %d",
                   DIAG_CHAR(ch), (int) active, type);
      break;
  }
}

void quest_timeout(struct char_data *ch)
{
  if ((GET_QUEST(ch) != NOTHING) && (GET_QUEST_TIME(ch) != -1)) {
    clear_quest(ch);
    send_to_char(ch, "You have run out of time to complete the quest.\r\n");
  }
}

void check_timed_quests(void)
{
  struct char_data *ch;

  for (ch = character_list; ch; ch = ch->next)
    if (!IS_NPC(ch) && (GET_QUEST(ch) != NOTHING) && (GET_QUEST_TIME(ch) != -1))
      if (--GET_QUEST_TIME(ch) == 0)
        quest_timeout(ch);
}

/*--------------------------------------------------------------------------*/
/* Quest Command Helper Functions                                           */
/*--------------------------------------------------------------------------*/

void list_quests(struct char_data *ch, zone_rnum zone, qst_vnum vmin, qst_vnum vmax)
{
  qst_rnum rnum;
  qst_vnum bottom, top;
  int counter = 0;

  if (zone != NOWHERE) {
    bottom = zone_table[zone].bot;
    top    = zone_table[zone].top;
  } else {
    bottom = vmin;
    top    = vmax;
  }
  /* Print the header for the quest listing. */
  send_to_char (ch,
  "Index VNum    Description                                  Questmaster\r\n"
  "----- ------- -------------------------------------------- -----------\r\n");
  for (rnum = 0; rnum < total_quests ; rnum++)
    if (QST_NUM(rnum) >= bottom && QST_NUM(rnum) <= top)
      send_to_char(ch, "\tg%4d\tn) [\tg%-5d\tn] \tc%-44.44s\tn \ty[%5d]\tn\r\n",
          ++counter, QST_NUM(rnum), QST_NAME(rnum),
          QST_MASTER(rnum) == NOBODY ? 0 : QST_MASTER(rnum));
  if (!counter)
    send_to_char(ch, "None found.\r\n");
}

static void quest_hist(struct char_data *ch)
{
  int i = 0, counter = 0;
  qst_rnum rnum = NOTHING;

  send_to_char(ch, "Quests that you have completed:\r\n"
    "Index Description                                          Questmaster\r\n"
    "----- ---------------------------------------------------- -----------\r\n");
  for (i = 0; i < GET_NUM_QUESTS(ch); i++) {
    if ((rnum = real_quest(ch->player_specials->saved.completed_quests[i])) != NOTHING)
      send_to_char(ch, "\tg%4d\tn) \tc%-52.52s\tn \ty%s\tn\r\n",
 ++counter, QST_DESC(rnum), (real_mobile(QST_MASTER(rnum)) == NOBODY) ? "Unknown" : GET_NAME(&mob_proto[(real_mobile(QST_MASTER(rnum)))]));
    else
      send_to_char(ch,
        "\tg%4d\tn) \tcUnknown Quest (it no longer exists)\tn\r\n", ++counter);
  }
  if (!counter)
    send_to_char(ch, "You haven't completed any quests yet.\r\n");
}

static void quest_join(struct char_data *ch, struct char_data *qm, char argument[MAX_INPUT_LENGTH])
{
  qst_vnum vnum;
  qst_rnum rnum;
  char buf[MAX_INPUT_LENGTH];

  if (!*argument)
    tb_snprintf(buf, sizeof(buf),
             "%s What quest did you wish to join?", GET_NAME(ch));
  else if (GET_QUEST(ch) != NOTHING)
    tb_snprintf(buf, sizeof(buf),
             "%s But you are already part of a quest!", GET_NAME(ch));
  else if((vnum = find_quest_by_qmnum(ch, GET_MOB_VNUM(qm), tb_parse_int_or_zero(argument))) == NOTHING)
    tb_snprintf(buf, sizeof(buf),
             "%s I don't know of such a quest!", GET_NAME(ch));
  else if ((rnum = real_quest(vnum)) == NOTHING)
    tb_snprintf(buf, sizeof(buf),
             "%s I don't know of such a quest!", GET_NAME(ch));
  else if (GET_LEVEL(ch) < QST_MINLEVEL(rnum))
    tb_snprintf(buf, sizeof(buf),
             "%s You are not experienced enough for that quest!", GET_NAME(ch));
  else if (GET_LEVEL(ch) > QST_MAXLEVEL(rnum))
    tb_snprintf(buf, sizeof(buf),
             "%s You are too experienced for that quest!", GET_NAME(ch));
  else if (is_complete(ch, vnum))
    tb_snprintf(buf, sizeof(buf),
             "%s You have already completed that quest!", GET_NAME(ch));
  else if ((QST_PREV(rnum) != NOTHING) && !is_complete(ch, QST_PREV(rnum)))
    tb_snprintf(buf, sizeof(buf),
             "%s That quest is not available to you yet!", GET_NAME(ch));
  else if ((QST_PREREQ(rnum) != NOTHING) &&
           (real_object(QST_PREREQ(rnum)) != NOTHING) &&
           (get_obj_in_list_num(real_object(QST_PREREQ(rnum)),
       ch->carrying) == NULL))
    tb_snprintf(buf, sizeof(buf),
             "%s You need to have %s first!", GET_NAME(ch),
      obj_proto[real_object(QST_PREREQ(rnum))].short_description);
  else {
    act("You join the quest.",    TRUE, ch, NULL, NULL, TO_CHAR);
    act("$n has joined a quest.", TRUE, ch, NULL, NULL, TO_ROOM);
    tb_snprintf(buf, sizeof(buf),
             "%s Listen carefully to the instructions.", GET_NAME(ch));
    do_tell(qm, buf, cmd_tell, 0);
    set_quest(ch, rnum);
    send_to_char(ch, "%s", QST_INFO(rnum));
    if (QST_TIME(rnum) != -1)
      tb_snprintf(buf, sizeof(buf),
        "%s You have a time limit of %d turn%s to complete the quest.",
        GET_NAME(ch), QST_TIME(rnum), QST_TIME(rnum) == 1 ? "" : "s");
    else
      tb_snprintf(buf, sizeof(buf),
        "%s You can take however long you want to complete the quest.",
 GET_NAME(ch));
  }
  do_tell(qm, buf, cmd_tell, 0);
  save_char(ch);
}

void quest_list(struct char_data *ch, struct char_data *qm, char argument[MAX_INPUT_LENGTH])
{
  qst_vnum vnum;
  qst_rnum rnum;

  if ((vnum = find_quest_by_qmnum(ch, GET_MOB_VNUM(qm), tb_parse_int_or_zero(argument))) == NOTHING)
    send_to_char(ch, "That is not a valid quest!\r\n");
  else if ((rnum = real_quest(vnum)) == NOTHING)
    send_to_char(ch, "That is not a valid quest!\r\n");
  else if (QST_INFO(rnum)) {
    send_to_char(ch,"Complete Details on Quest %d \tc%s\tn:\r\n%s",
                      vnum,
         QST_DESC(rnum),
         QST_INFO(rnum));
    if (QST_PREV(rnum) != NOTHING)
      send_to_char(ch, "You have to have completed quest %s first.\r\n",
          quest_link_text(rnum, QST_PREV(rnum), TRUE));
    if (QST_TIME(rnum) != -1)
      send_to_char(ch,
         "There is a time limit of %d turn%s to complete the quest.\r\n",
          QST_TIME(rnum),
          QST_TIME(rnum) == 1 ? "" : "s");
  } else
    send_to_char(ch, "There is no further information on that quest.\r\n");
}

static void quest_quit(struct char_data *ch)
{
  qst_rnum rnum;

  if (GET_QUEST(ch) == NOTHING)
    send_to_char(ch, "But you currently aren't on a quest!\r\n");
  else if ((rnum = real_quest(GET_QUEST(ch))) == NOTHING) {
    clear_quest(ch);
    send_to_char(ch, "You are now no longer part of the quest.\r\n");
    save_char(ch);
  } else {
    clear_quest(ch);
    if (QST_QUIT(rnum) && (str_cmp(QST_QUIT(rnum), "undefined") != 0))
      send_to_char(ch, "%s", QST_QUIT(rnum));
    else
      send_to_char(ch, "You are now no longer part of the quest.\r\n");
    if (QST_PENALTY(rnum)) {
      GET_QUESTPOINTS(ch) -= QST_PENALTY(rnum);
      send_to_char(ch,
        "You have lost %d quest points for your cowardice.\r\n",
        QST_PENALTY(rnum));
    }
    save_char(ch);
  }
}

static void quest_progress(struct char_data *ch)
{
  qst_rnum rnum;

  if (GET_QUEST(ch) == NOTHING)
    send_to_char(ch, "But you currently aren't on a quest!\r\n");
  else if ((rnum = real_quest(GET_QUEST(ch))) == NOTHING) {
    clear_quest(ch);
    send_to_char(ch, "Your quest seems to no longer exist.\r\n");
  } else {
    send_to_char(ch, "You are on the following quest:\r\n%s\r\n%s",
        QST_DESC(rnum), QST_INFO(rnum));
    if (QST_QUANTITY(rnum) > 1)
      send_to_char(ch,
          "You still have to achieve %d out of %d goals for the quest.\r\n",
   GET_QUEST_COUNTER(ch), QST_QUANTITY(rnum));
    if (GET_QUEST_TIME(ch) > 0)
      send_to_char(ch,
          "You have %d turn%s remaining to complete the quest.\r\n",
   GET_QUEST_TIME(ch),
   GET_QUEST_TIME(ch) == 1 ? "" : "s");
  }
}

static void quest_show(struct char_data *ch, mob_vnum qm)
{
  qst_rnum rnum;
  int counter = 0;

  send_to_char(ch,
  "The following quests are available:\r\n"
  "Index Description                                          ( Vnum) Done?\r\n"
  "----- ---------------------------------------------------- ------- -----\r\n");
  for (rnum = 0; rnum < total_quests; rnum++)
    if (qm == QST_MASTER(rnum))
      send_to_char(ch, "\tg%4d\tn) \tc%-52.52s\tn \ty(%5d)\tn \ty(%s)\tn\r\n",
        ++counter, QST_DESC(rnum), QST_NUM(rnum),
        (is_complete(ch, QST_NUM(rnum)) ? "Yes" : "No "));
  if (!counter)
    send_to_char(ch, "There are no quests available here at the moment.\r\n");
}

static void quest_stat(struct char_data *ch, char *argument)
{
  qst_rnum rnum;
  mob_rnum qmrnum;
  char buf[MAX_STRING_LENGTH];
  char targetname[MAX_STRING_LENGTH];

  if (!*argument)
    send_to_char(ch, "%s\r\n", quest_imm_usage);
  else if ((rnum = real_quest(tb_parse_int_or_zero(argument))) == NOTHING )
    send_to_char(ch, "That quest does not exist.\r\n");
  else {
    sprintbit(QST_FLAGS(rnum), aq_flags, buf, sizeof(buf));
    switch (QST_TYPE(rnum)) {
      case AQ_OBJ_FIND:
      case AQ_OBJ_RETURN:
        tb_snprintf(targetname, sizeof(targetname), "%s",
                 real_object(QST_TARGET(rnum)) == NOTHING ?
                 "An unknown object" :
    obj_proto[real_object(QST_TARGET(rnum))].short_description);
 break;
      case AQ_ROOM_FIND:
      case AQ_ROOM_CLEAR:
        tb_snprintf(targetname, sizeof(targetname), "%s",
          real_room(QST_TARGET(rnum)) == NOWHERE ?
                 "An unknown room" :
    world[real_room(QST_TARGET(rnum))].name);
        break;
      case AQ_MOB_FIND:
      case AQ_MOB_KILL:
      case AQ_MOB_SAVE:
 tb_snprintf(targetname, sizeof(targetname), "%s",
                 real_mobile(QST_TARGET(rnum)) == NOBODY ?
    "An unknown mobile" :
    GET_NAME(&mob_proto[real_mobile(QST_TARGET(rnum))]));
 break;
      default:
 tb_snprintf(targetname, sizeof(targetname), "Unknown");
 break;
    }
    qmrnum = real_mobile(QST_MASTER(rnum));
    send_to_char(ch,
        "VNum  : [\ty%5d\tn], RNum: [\ty%5d\tn] -- Questmaster: [\ty%5d\tn] \ty%s\tn\r\n"
        "Name  : \ty%s\tn\r\n"
 "Desc  : \ty%s\tn\r\n"
 "Accept Message:\r\n\tc%s\tn"
 "Completion Message:\r\n\tc%s\tn"
 "Quit Message:\r\n\tc%s\tn"
 "Type  : \ty%s\tn\r\n"
        "Target: \ty%d\tn \ty%s\tn, Quantity: \ty%d\tn\r\n"
 "Value : \ty%d\tn, Penalty: \ty%d\tn, Min Level: \ty%2d\tn, Max Level: \ty%2d\tn\r\n"
 "Flags : \tc%s\tn\r\n",
     QST_NUM(rnum), rnum,
 QST_MASTER(rnum) == NOBODY ? -1 : QST_MASTER(rnum),
 (qmrnum == NOBODY) ? "(Invalid vnum)" : GET_NAME(&mob_proto[(qmrnum)]),
        QST_NAME(rnum), QST_DESC(rnum),
        QST_INFO(rnum), QST_DONE(rnum),
 (QST_QUIT(rnum) &&
  (str_cmp(QST_QUIT(rnum), "undefined") != 0)
          ? QST_QUIT(rnum) : "Nothing\r\n"),
     QST_TYPE_NAME(rnum),
 QST_TARGET(rnum) == NOBODY ? -1 : QST_TARGET(rnum),
 targetname,
 QST_QUANTITY(rnum),
     QST_POINTS(rnum), QST_PENALTY(rnum), QST_MINLEVEL(rnum),
 QST_MAXLEVEL(rnum), buf);
    if (QST_PREREQ(rnum) != NOTHING)
      send_to_char(ch, "Preq  : [\ty%5d\tn] \ty%s\tn\r\n",
        QST_PREREQ(rnum) == NOTHING ? -1 : QST_PREREQ(rnum),
        QST_PREREQ(rnum) == NOTHING ? "" :
   real_object(QST_PREREQ(rnum)) == NOTHING ? "an unknown object" :
       obj_proto[real_object(QST_PREREQ(rnum))].short_description);
    if (QST_TYPE(rnum) == AQ_OBJ_RETURN)
      send_to_char(ch, "Mob   : [\ty%5d\tn] \ty%s\tn\r\n",
        QST_RETURNMOB(rnum),
 real_mobile(QST_RETURNMOB(rnum)) == NOBODY ? "an unknown mob" :
           mob_proto[real_mobile(QST_RETURNMOB(rnum))].player.short_descr);
    if (QST_TIME(rnum) != -1)
      send_to_char(ch, "Limit : There is a time limit of %d turn%s to complete.\r\n",
   QST_TIME(rnum),
   QST_TIME(rnum) == 1 ? "" : "s");
    else
      send_to_char(ch, "Limit : There is no time limit on this quest.\r\n");
    send_to_char(ch, "Prior :");
    if (QST_PREV(rnum) == NOTHING)
      send_to_char(ch, " \tyNone.\tn\r\n");
    else
      send_to_char(ch, " [\ty%5d\tn] \tc%s\tn\r\n",
        QST_PREV(rnum), quest_link_text(rnum, QST_PREV(rnum), FALSE));
    send_to_char(ch, "Next  :");
    if (QST_NEXT(rnum) == NOTHING)
      send_to_char(ch, " \tyNone.\tn\r\n");
    else
      send_to_char(ch, " [\ty%5d\tn] \tc%s\tn\r\n",
        QST_NEXT(rnum), quest_link_text(rnum, QST_NEXT(rnum), FALSE));
  }
}

/*--------------------------------------------------------------------------*/
/* Quest Command Processing Function and Questmaster Special                */
/*--------------------------------------------------------------------------*/

ACMD(do_quest)
{
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
  int  tp;

  two_arguments(argument, arg1, arg2);
  if (!*arg1)
    send_to_char(ch, "%s\r\n", GET_LEVEL(ch) < LVL_IMMORT ?
                     quest_mort_usage : quest_imm_usage);
  else if (((tp = search_block(arg1, quest_cmd, FALSE)) == -1))
    send_to_char(ch, "%s\r\n", GET_LEVEL(ch) < LVL_IMMORT ?
                     quest_mort_usage : quest_imm_usage);
  else {
    switch (tp) {
      case SCMD_QUEST_LIST:
      case SCMD_QUEST_JOIN:
        /* list, join should hve been handled by questmaster spec proc */
        send_to_char(ch, "Sorry, but you cannot do that here!\r\n");
        break;
      case SCMD_QUEST_HISTORY:
        quest_hist(ch);
        break;
      case SCMD_QUEST_LEAVE:
        quest_quit(ch);
        break;
      case SCMD_QUEST_PROGRESS:
 quest_progress(ch);
 break;
      case SCMD_QUEST_STATUS:
        if (GET_LEVEL(ch) < LVL_IMMORT)
          send_to_char(ch, "%s\r\n", quest_mort_usage);
        else
          quest_stat(ch, arg2);
        break;
      default: /* Whe should never get here, but... */
        send_to_char(ch, "%s\r\n", GET_LEVEL(ch) < LVL_IMMORT ?
                     quest_mort_usage : quest_imm_usage);
 break;
    } /* switch on subcmd number */
  }
}

/**
 * Spec proc of every mob that offers quests (installed by
 * assign_the_quests() and add_quest()). The mob's original spec proc, if
 * any, is kept on its quests and found with quest_qm_secondary(); it gets
 * the first chance at every command. Handles 'quest list' and 'quest join';
 * other quest subcommands fall through to do_quest().
 */
SPECIAL(questmaster)
{
  qst_rnum rnum;
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH];
  int  tp;
  struct char_data *qm = (struct char_data *)me;
  quest_spec_proc *secondary;

  /* check that qm mob has quests assigned */
  for (rnum = 0; (rnum < total_quests &&
      QST_MASTER(rnum) != GET_MOB_VNUM(qm)) ; rnum ++);
  if (rnum >= total_quests)
    return FALSE; /* No quests for this mob */
  else if ((secondary = quest_qm_secondary(GET_MOB_VNUM(qm))) != NULL &&
           secondary(ch, me, cmd, argument))
    return TRUE;  /* The secondary spec proc handled this command */
  else if (CMD_IS("quest")) {
    two_arguments(argument, arg1, arg2);
    if (!*arg1)
      return FALSE;
    else if (((tp = search_block(arg1, quest_cmd, FALSE)) == -1))
      return FALSE;
    else {
      switch (tp) {
      case SCMD_QUEST_LIST:
        if (!*arg2)
          quest_show(ch, GET_MOB_VNUM(qm));
        else
   quest_list(ch, qm, arg2);
        break;
      case SCMD_QUEST_JOIN:
        quest_join(ch, qm, arg2);
        break;
      default:
 return FALSE; /* fall through to the do_quest command processor */
      } /* switch on subcmd number */
      return TRUE;
    }
  } else {
    return FALSE; /* not a questmaster command */
  }
}
