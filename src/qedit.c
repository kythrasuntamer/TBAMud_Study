/* ***********************************************************************
*    File:   qedit.c                                  Part of CircleMUD  *
* Version:   2.0 (November 2005) Written for CircleMud CWG / Suntzu      *
* Purpose:   To provide special quest-related code.                      *
* Copyright: Kenneth Ray                                                 *
*                                                                        *
* Made for Oasis OLC                                                     *
* Copyright 1996 Harvey Gilpin.                                          *
*********************************************************************** */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"

#include "comm.h"
#include "db.h"
#include "oasis.h"
#include "improved-edit.h"
#include "screen.h"
#include "genolc.h"
#include "genzon.h"
#include "interpreter.h"
#include "modify.h"
#include "quest.h"

/*-------------------------------------------------------------------*/
/*. Function prototypes . */

static void qedit_setup_new(struct descriptor_data *d);
static void qedit_setup_existing(struct descriptor_data *d, qst_rnum rnum);
static void qedit_disp_menu(struct descriptor_data *d);
static void qedit_save_internally(struct descriptor_data *d);
static int qedit_save_to_disk(struct descriptor_data *d, zone_rnum num);
static int qedit_parse_int_range_at(struct descriptor_data *d, const char *arg,
                                    long lo, long hi, int *out,
                                    const char *operation, const char *field,
                                    const char *file, int line, const char *func);

/** Parse the builder's numeric answer for quest field 'field' into *out.
 * TRUE when arg is a whole number in lo..hi.  Otherwise a WARNING with op
 * 'operation', the quest vnum, the field, the range and the editor is
 * logged at the call site, the builder is re-prompted with the range, and
 * FALSE is returned; the caller returns with OLC_MODE unchanged so the next
 * line answers again.  Junk used to become 0, out-of-range rewards and
 * limits were silently clamped, and vnums wider than IDXTYPE were
 * truncated (68537 -> 3001) before real_mobile()/real_object(). */
#define QEDIT_PARSE_INT(d, arg, lo, hi, out, operation, field) \
  qedit_parse_int_range_at((d), (arg), (lo), (hi), (out), (operation), (field), \
                           __FILE__, __LINE__, __func__)

/** Range for a vnum answer where -1 means "none": the stored types are
 * IDXTYPE, whose all-ones value is NOBODY/NOTHING. */
#define QEDIT_VNUM_MIN  (-1L)
#define QEDIT_VNUM_MAX  ((long)IDXTYPE_MAX - 1)

/*-------------------------------------------------------------------*/

/* Name of a quest type for display.  quest_types[] has NUM_AQ_TYPES
 * entries; a new quest starts as AQ_UNDEFINED (-1), and a loaded quest's
 * type comes from the quest file, so neither may be used as an index
 * unchecked. */
static const char *qedit_type_name(const struct aq_data *quest)
{
  if (quest->type >= 0 && quest->type < NUM_AQ_TYPES)
    return quest_types[quest->type];
  if (quest->type != AQ_UNDEFINED)
    TB_WARN_MSG("quest", "quest-type-range", "aquest_table",
                "quest vnum %d type %d outside 0..%d",
                (int)quest->vnum, quest->type, NUM_AQ_TYPES - 1);
  return "Undefined";
}

static void qedit_save_internally(struct descriptor_data *d)
{
  add_quest(OLC_QUEST(d));
}

/* Writes zone rnum num's quests.  save_quests() reports the I/O cause;
 * this adds the editor and zone, so a failed save is never reported to the
 * builder as "saved".  Returns FALSE when the quest file was not written. */
static int qedit_save_to_disk(struct descriptor_data *d, zone_rnum num)
{
  if (save_quests(num))
    return TRUE;
  TB_ERROR_MSG("olc", "qedit-save", "quest file",
               "zone rnum %d (top %d) quest vnum %d not written; editor " DIAG_CHAR_FMT,
               (int)num, (int)top_of_zone_table, (int)OLC_NUM(d), DIAG_CHAR(d->character));
  return FALSE;
}

static int qedit_parse_int_range_at(struct descriptor_data *d, const char *arg,
                                    long lo, long hi, int *out,
                                    const char *operation, const char *field,
                                    const char *file, int line, const char *func)
{
  long value;

  if (tb_parse_long_checked(arg, &value) && value >= lo && value <= hi) {
    *out = (int)value;
    return TRUE;
  }
  /* Only the length of the input is logged: it is free text. */
  tb_error_log_at("WARNING", "olc", operation, field, 0, file, line, func,
                  "quest vnum %d input length %zu not a number in %ld..%ld; editor " DIAG_CHAR_FMT,
                  (int)OLC_NUM(d), strlen(arg), lo, hi, DIAG_CHAR(d->character));
  write_to_output(d, "Enter a number from %ld to %ld : ", lo, hi);
  return FALSE;
}

/*-------------------------------------------------------------------*\
  utility functions
 \*-------------------------------------------------------------------*/

ACMD(do_oasis_qedit)
{
  int number = NOWHERE, save = 0;
  qst_rnum real_num;
  struct descriptor_data *d;
  char buf1[MAX_INPUT_LENGTH];
  char buf2[MAX_INPUT_LENGTH];

  /* A forced immortal, or a mob or switched body, may have no descriptor,
   * and the editor lives on ch->desc. */
  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING) {
    TB_WARN_MSG("olc", "olc-no-descriptor", "qedit",
                DIAG_CHAR_FMT " has_desc %d state %d", DIAG_CHAR(ch),
                ch->desc != NULL, ch->desc ? STATE(ch->desc) : -1);
    return;
  }

  /****************************************************************************/
  /** Parse any arguments.                                                   **/
  /****************************************************************************/
  two_arguments(argument, buf1, buf2);

  if (!*buf1) {
    send_to_char(ch, "Specify a quest VNUM to edit.\r\n");
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

  /****************************************************************************/
  /** If a numeric argument was given, get it.                               **/
  /****************************************************************************/
  if (number == NOWHERE)
    number = tb_parse_int_or_zero(buf1);

  if (number < IDXTYPE_MIN || number > IDXTYPE_MAX) {
    send_to_char(ch, "That quest VNUM can't exist.\r\n");
    return;
  }

  /****************************************************************************/
  /** Check that the quest isn't already being edited.                       **/
  /****************************************************************************/
  for (d = descriptor_list; d; d = d->next) {
    if (STATE(d) == CON_QEDIT) {
      if (d->olc && OLC_NUM(d) == number) {
        send_to_char(ch, "That quest is currently being edited by %s.\r\n",
          PERS(d->character, ch));
        return;
      }
    }
  }

  /****************************************************************************/
  /** Point d to the builder's descriptor.                                   **/
  /****************************************************************************/
  d = ch->desc;

  /****************************************************************************/
  /** Give the descriptor an OLC structure.                                  **/
  /****************************************************************************/
  /* Entry requires CON_PLAYING, where no editor is open, so a leftover
   * d->olc is an invariant break.  free() alone leaked its substructures
   * (help, action, quest, prefs, message list, storage); release them all
   * and report the stale mode. */
  if (d->olc) {
    TB_ERROR_MSG("olc", "olc-reentry", "qedit",
                 DIAG_CHAR_FMT " stale mode %d state %d",
                 DIAG_CHAR(ch), OLC_MODE(d), STATE(d));
    cleanup_olc(d, CLEANUP_ALL);
  }

  CREATE(d->olc, struct oasis_olc_data, 1);

  /****************************************************************************/
  /** Find the zone.                                                         **/
  /****************************************************************************/
  if ((OLC_ZNUM(d) = real_zone_by_thing(number)) == NOWHERE) {
    send_to_char(ch, "Sorry, there is no zone for that number!\r\n");
    free(d->olc);
    d->olc = NULL;
    return;
  }

  /****************************************************************************/
  /** Everyone but IMPLs can only edit zones they have been assigned.        **/
  /****************************************************************************/
  if (!can_edit_zone(ch, OLC_ZNUM(d))) {
    send_to_char(ch, "You do not have permission to edit this zone.\r\n");

    /**************************************************************************/
    /** Free the OLC structure.                                              **/
    /**************************************************************************/
    free(d->olc);
    d->olc = NULL;
    return;
  }

  if (save) {
    send_to_char(ch, "Saving all quests in zone %d.\r\n",
      zone_table[OLC_ZNUM(d)].number);
    mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE,
      "OLC: %s saves quest info for zone %d.",
      GET_NAME(ch), zone_table[OLC_ZNUM(d)].number);

    /**************************************************************************/
    /** Save the quest to the quest file.                                    **/
    /**************************************************************************/
    if (!qedit_save_to_disk(d, OLC_ZNUM(d)))
      send_to_char(ch, "Saving the quest file FAILED; the file on disk is unchanged.\r\n");

    /**************************************************************************/
    /** Free the OLC structure.                                              **/
    /**************************************************************************/
    free(d->olc);
    d->olc = NULL;
    return;
  }

  OLC_NUM(d) = number;

  if ((real_num = real_quest(number)) != NOTHING)
    qedit_setup_existing(d, real_num);
  else
    qedit_setup_new(d);

  STATE(d) = CON_QEDIT;

  act("$n starts using OLC.", TRUE, d->character, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);

  mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,
         "OLC: %s starts editing zone %d allowed zone %d",
         GET_NAME(ch), zone_table[OLC_ZNUM(d)].number, GET_OLC_ZONE(ch));
}

static void qedit_setup_new(struct descriptor_data *d)
{
  struct aq_data *quest;

  /* Allociate some quest shaped space                          */
  CREATE(quest, struct aq_data, 1);
  /* Set default values                                         */
  quest->vnum       = OLC_NUM(d);     /* Quest vnum             */
  quest->qm         = NOBODY;         /* Questmaster rnum       */
  quest->flags      = 0;              /* Quest bitflags         */
  quest->type       = AQ_UNDEFINED;   /* Quest type             */
  quest->target     = NOTHING;        /* Quest target           */
  quest->prereq     = NOTHING;        /* Prerequisite object    */
  quest->value[0]   = 0;              /* Points for completing  */
  quest->value[1]   = 0;              /* Points for abandoning  */
  quest->value[2]   = 0;              /* Minimum level          */
  quest->value[3]   = LVL_IMPL;       /* Maximim level          */
  quest->value[4]   = -1;             /* Time limit             */
  quest->value[5]   = NOBODY;         /* Mob to return object   */
  quest->value[6]   = 1;              /* Quantity of targets    */
  quest->prev_quest = NOTHING;        /* Previous quest         */
  quest->next_quest = NOTHING;        /* Next quest             */
  quest->gold_reward= 0;              /* Prize in gold coins    */
  quest->exp_reward = 0;              /* Prize in exp points    */
  quest->obj_reward = NOTHING;        /* vnum of reward object  */
  quest->name       = strdup("Undefined Quest");
  quest->desc       = strdup("Quest definition is incomplete.");
  quest->info       = strdup("There is no information on this quest.\r\n");
  quest->done       = strdup("You have completed the quest.\r\n");
  quest->quit       = strdup("You have abandoned the quest.\r\n");
  quest->func       = NULL;           /* Secondary qm spec proc */
  /* Set the descriptor OLC structure to point to this quest    */
  OLC_QUEST(d) = quest;
  /* Show the main quest edit menu                              */
  qedit_disp_menu(d);
}

/*-------------------------------------------------------------------*/

static void qedit_setup_existing(struct descriptor_data *d, qst_rnum r_num)
{
  /*. Alloc some quest shaped space . */
  CREATE(OLC_QUEST(d), struct aq_data, 1);
  copy_quest(OLC_QUEST(d), aquest_table + r_num, FALSE);
  qedit_disp_menu(d);
}

/*-------------------------------------------------------------------*/

/**************************************************************************
 Menu functions
**************************************************************************/

/*-------------------------------------------------------------------*/
/*. Display main menu . */

static void qedit_disp_menu(struct descriptor_data *d)
{
  struct aq_data *quest;
  char quest_flags[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];
  char targetname[MAX_STRING_LENGTH];
  mob_vnum return_mob;

  quest = OLC_QUEST(d);

  clear_screen(d);
  sprintbit(quest->flags, aq_flags, quest_flags, sizeof(quest_flags));
  if (quest->type == AQ_OBJ_RETURN) {
    if ((return_mob = real_mobile(quest->value[5])) != NOBODY)
      tb_snprintf(buf2, sizeof(buf2), "to %s [%d]",
        mob_proto[return_mob].player.short_descr,
        quest->value[5]);
    else
      tb_snprintf(buf2, sizeof(buf2), "to an unknown mob [%d].",
               quest->value[5]);
  }
  switch (quest->type) {
    case AQ_OBJ_FIND:
    case AQ_OBJ_RETURN:
      tb_snprintf(targetname, sizeof(targetname), "%s",
               real_object(quest->target) == NOTHING ?
               "An unknown object" :
               obj_proto[real_object(quest->target)].short_description);
      break;
    case AQ_ROOM_FIND:
    case AQ_ROOM_CLEAR:
      tb_snprintf(targetname, sizeof(targetname), "%s",
               real_room(quest->target) == NOWHERE ?
               "An unknown room" :
               world[real_room(quest->target)].name);
      break;
    case AQ_MOB_FIND:
    case AQ_MOB_KILL:
    case AQ_MOB_SAVE:
      tb_snprintf(targetname, sizeof(targetname), "%s",
               real_mobile(quest->target) == NOBODY ?
               "An unknown mobile" :
               GET_NAME(&mob_proto[real_mobile(quest->target)]));
      break;
    default:
      tb_snprintf(targetname, sizeof(targetname), "Unknown");
      break;
  }
  write_to_output(d,
    "-- Quest Number    : \tn[\tc%6d\tn]\r\n"
    "\tg 1\tn) Quest Name     : \ty%s\r\n"
    "\tg 2\tn) Description    : \ty%s\r\n"
    "\tg 3\tn) Accept Message\r\n\ty%s"
    "\tg 4\tn) Completion Message\r\n\ty%s"
    "\tg 5\tn) Quit Message\r\n\ty%s"
    "\tg 6\tn) Quest Flags    : \tc%s\r\n"
    "\tg 7\tn) Quest Type     : \tc%s %s\r\n"
    "\tg 8\tn) Quest Master   : [\tc%6d\tn] \ty%s\r\n"
    "\tg 9\tn) Quest Target   : [\tc%6d\tn] \ty%s\r\n"
    "\tg A\tn) Quantity       : [\tc%6d\tn]\r\n"
    "\tn    Quest Point Rewards\r\n"
    "\tg B\tn) Completed      : [\tc%6d\tn] \tg C\tn) Abandoned   : [\tc%6d\tn]\r\n"
    "\tn    Other Rewards Rewards\r\n"
    "\tg G\tn) Gold Coins     : [\tc%6d\tn] \tg T\tn) Exp Points  : [\tc%6d\tn] \tg O\tn) Object : [\tc%6d\tn]\r\n"
    "\tn    Level Limits to Accept Quest\r\n"
    "\tg D\tn) Lower Level    : [\tc%6d\tn] \tg E\tn) Upper Level : [\tc%6d\tn]\r\n"
    "\tg F\tn) Prerequisite   : [\tc%6d\tn] \ty%s\r\n"
    "\tg L\tn) Time Limit     : [\tc%6d\tn]\r\n"
    "\tg N\tn) Next Quest     : [\tc%6d\tn] \ty%s\r\n"
    "\tg P\tn) Previous Quest : [\tc%6d\tn] \ty%s\r\n"
    "\tg X\tn) Delete Quest\r\n"
    "\tg Q\tn) Quit\r\n"
    "Enter Choice : ",
    quest->vnum,
    quest->name,
    quest->desc,
    quest->info && (str_cmp(quest->info, "undefined"))
     ? quest->info : "Nothing\r\n",
    quest->done && (str_cmp(quest->done, "undefined"))
     ? quest->done : "Nothing\r\n",
    quest->quit && (str_cmp(quest->quit, "undefined"))
     ? quest->quit : "Nothing\r\n",
    quest_flags,
    qedit_type_name(quest),
    quest->type == AQ_OBJ_RETURN ? buf2 : "",
    quest->qm == NOBODY ? -1 :     quest->qm,
    real_mobile(quest->qm) == NOBODY ? "Invalid Mob" : mob_proto[(real_mobile(quest->qm))].player.short_descr,
    quest->target == NOBODY ? -1 : quest->target, targetname,
    quest->value[6],
    quest->value[0], quest->value[1],
    quest->gold_reward, quest->exp_reward, quest->obj_reward == NOTHING ? -1 : quest->obj_reward,
    quest->value[2], quest->value[3],
    quest->prereq     == NOTHING ? -1 : quest->prereq,
    quest->prereq     == NOTHING ? "" :
      real_object(quest->prereq) == NOTHING ? "an unknown object" :
                  obj_proto[real_object(quest->prereq)].short_description,
    quest->value[4],
    quest->next_quest == NOTHING ? -1 : quest->next_quest,
    real_quest(quest->next_quest) == NOTHING ? "" : QST_DESC(real_quest(quest->next_quest)),
    quest->prev_quest == NOTHING ? -1 : quest->prev_quest,
    real_quest(quest->prev_quest) == NOTHING ? "" : QST_DESC(real_quest(quest->prev_quest)));
  OLC_MODE(d) = QEDIT_MAIN_MENU;
}
/* For quest type.  */
static void qedit_disp_type_menu(struct descriptor_data *d)
{
  clear_screen(d);
  column_list(d->character, 0, quest_types, NUM_AQ_TYPES, TRUE);
  write_to_output(d, "\r\nEnter Quest type : ");
  OLC_MODE(d) = QEDIT_TYPES;
}
/* For quest flags.  */
static void qedit_disp_flag_menu(struct descriptor_data *d)
{
  char bits[MAX_STRING_LENGTH];

  get_char_colors(d->character);
  clear_screen(d);
  column_list(d->character, 0, aq_flags, NUM_AQ_FLAGS, TRUE);
  sprintbit(OLC_QUEST(d)->flags, aq_flags, bits, sizeof(bits));
  write_to_output(d, "\r\nQuest flags: \tc%s\tn\r\n"
          "Enter quest flags, 0 to quit : ", bits);
  OLC_MODE(d) = QEDIT_FLAGS;
}
/**************************************************************************
  The GARGANTUAN event handler
**************************************************************************/

void qedit_parse(struct descriptor_data *d, char *arg)
{
  int number = tb_parse_int_or_zero(arg);
  char *oldtext = NULL;

  switch (OLC_MODE(d)) {
    /*-------------------------------------------------------------------*/
    case QEDIT_CONFIRM_SAVESTRING:
      switch (*arg) {
        case 'y':
        case 'Y':
          send_to_char(d->character, "Saving Quest to memory.\r\n");
          qedit_save_internally(d);
          mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), TRUE,
     "OLC: %s edits quest %d", GET_NAME(d->character), OLC_NUM(d));
          if (CONFIG_OLC_SAVE) {
            if (qedit_save_to_disk(d, real_zone_by_thing(OLC_NUM(d))))
              write_to_output(d, "Quest %d saved to disk.\r\n", OLC_NUM(d));
            else
              write_to_output(d, "Quest %d saved to memory, but writing the quest file "
                                 "FAILED; the file on disk is unchanged.\r\n", OLC_NUM(d));
          } else
            write_to_output(d, "Quest %d saved to memory.\r\n", OLC_NUM(d));
          /* add_quest copies every string into aquest_table (copy_quest ->
           * str_udup), so the editor still owns OLC_QUEST's strings: free
           * them too.  CLEANUP_STRUCTS leaked all five on every save. */
          cleanup_olc(d, CLEANUP_ALL);
   return;
        case 'n':
        case 'N':
          cleanup_olc(d, CLEANUP_ALL);
          return;
        default:
          write_to_output(d,
            "Invalid choice!\r\nDo you wish to save the quest? : ");
          return;
      }
    /*-------------------------------------------------------------------*/
    case QEDIT_CONFIRM_DELETE:
      switch (*arg) {
        case 'y':
        case 'Y': {
          /* A quest opened with "qedit <new vnum>" is not in aquest_table
           * until it is saved, so real_quest() is NOTHING; delete_quest()
           * read aquest_table[NOTHING] before its own range test (and the
           * table is NULL when no quests exist).  Check here, where the
           * vnum and the builder are known. */
          qst_rnum del_rnum = real_quest(OLC_NUM(d));

          if (del_rnum == NOTHING) {
            TB_WARN_MSG("olc", "qedit-delete-unsaved", "aquest_table",
                        "quest vnum %d has no table row; editor " DIAG_CHAR_FMT,
                        (int)OLC_NUM(d), DIAG_CHAR(d->character));
            write_to_output(d, "Quest %d was never saved; there is nothing to delete.\r\n",
                            (int)OLC_NUM(d));
            cleanup_olc(d, CLEANUP_ALL);
            return;
          }
          if (delete_quest(del_rnum))
            write_to_output(d, "Quest deleted.\r\n");
          else {
            TB_ERROR_MSG("olc", "qedit-delete", "aquest_table",
                         "delete_quest failed for vnum %d rnum %d editor " DIAG_CHAR_FMT,
                         (int)OLC_NUM(d), (int)del_rnum, DIAG_CHAR(d->character));
            write_to_output(d, "Couldn't delete the quest!\r\n");
            cleanup_olc(d, CLEANUP_ALL);
            return;
          }
          }
          if (CONFIG_OLC_SAVE) {
            if (qedit_save_to_disk(d, real_zone_by_thing(OLC_NUM(d))))
              write_to_output(d, "Quest file saved to disk.\r\n");
            else
              write_to_output(d, "Quest deleted in memory, but writing the quest file "
                                 "FAILED; the file on disk is unchanged.\r\n");
          } else
            write_to_output(d, "Quest file saved to memory.\r\n");
          cleanup_olc(d, CLEANUP_ALL);
   return;
        case 'n':
        case 'N':
          qedit_disp_menu(d);
          return;
        default:
          write_to_output(d,
            "Invalid choice!\r\nDo you wish to delete the quest? : ");
          return;
      }

    /*-------------------------------------------------------------------*/
    case QEDIT_MAIN_MENU:
      switch (*arg) {
        case 'q':
 case 'Q':
   if (OLC_VAL(d)) {   /*. Anything been changed? . */
            write_to_output(d,
              "Do you wish to save the changes to the Quest? (y/n) : ");
            OLC_MODE(d) = QEDIT_CONFIRM_SAVESTRING;
          } else
            cleanup_olc(d, CLEANUP_ALL);
          return;
 case 'x':
 case 'X':
          OLC_MODE(d) = QEDIT_CONFIRM_DELETE;
   write_to_output(d, "Do you wish to delete the Quest? (y/n) : ");
   break;
 case '1':
   OLC_MODE(d) = QEDIT_NAME;
   write_to_output(d, "Enter the quest name : ");
   break;
        case '2':
   OLC_MODE(d) = QEDIT_DESC;
   write_to_output(d, "Enter the quest description :-\r\n] ");
   break;
        case '3':
          OLC_MODE(d) = QEDIT_INFO;
          clear_screen(d);
          send_editor_help(d);
          write_to_output(d, "Enter quest acceptance message:\r\n\r\n");

          if (OLC_QUEST(d)->info) {
            write_to_output(d, "%s", OLC_QUEST(d)->info);
            oldtext = strdup(OLC_QUEST(d)->info);
          }
          string_write(d, &OLC_QUEST(d)->info, MAX_QUEST_MSG, 0, oldtext);
          OLC_VAL(d) = 1;
          break;
        case '4':
          OLC_MODE(d) = QEDIT_COMPLETE;
          clear_screen(d);
          send_editor_help(d);
          write_to_output(d, "Enter quest completion message:\r\n\r\n");

          if (OLC_QUEST(d)->done) {
            write_to_output(d, "%s", OLC_QUEST(d)->done);
            oldtext = strdup(OLC_QUEST(d)->done);
          }
          string_write(d, &OLC_QUEST(d)->done, MAX_QUEST_MSG, 0, oldtext);
          OLC_VAL(d) = 1;
          break;
        case '5':
          OLC_MODE(d) = QEDIT_ABANDON;
          clear_screen(d);
          send_editor_help(d);
          write_to_output(d, "Enter quest quit message:\r\n\r\n");

          if (OLC_QUEST(d)->quit) {
            write_to_output(d, "%s", OLC_QUEST(d)->quit);
            oldtext = strdup(OLC_QUEST(d)->quit);
          }
          string_write(d, &OLC_QUEST(d)->quit, MAX_QUEST_MSG, 0, oldtext);
          OLC_VAL(d) = 1;
          break;
        case '6':
          OLC_MODE(d) = QEDIT_FLAGS;
          qedit_disp_flag_menu(d);
          break;
        case '7':
          OLC_MODE(d) = QEDIT_TYPES;
          qedit_disp_type_menu(d);
          break;
        case '8':
          OLC_MODE(d) = QEDIT_QUESTMASTER;
          write_to_output(d, "Enter vnum of quest master : ");
          break;
        case '9':
          OLC_MODE(d) = QEDIT_TARGET;
          write_to_output(d, "Enter target vnum : ");
          break;
 case 'a':
 case 'A':
   OLC_MODE(d) = QEDIT_QUANTITY;
   write_to_output(d, "Enter quantity of target : ");
   break;
        case 'b':
        case 'B':
   OLC_MODE(d) = QEDIT_POINTSCOMP;
          write_to_output(d, "Enter points for completing the quest : " );
          break;
 case 'c':
        case 'C':
   OLC_MODE(d) = QEDIT_POINTSQUIT;
          write_to_output(d, "Enter points for quitting the quest : " );
          break;
 case 'd':
        case 'D':
   OLC_MODE(d) = QEDIT_LEVELMIN;
          write_to_output(d, "Enter minimum level to accept the quest : " );
          break;
 case 'e':
        case 'E':
   OLC_MODE(d) = QEDIT_LEVELMAX;
          write_to_output(d, "Enter maximum level to accept the quest : " );
          break;
 case 'f':
 case 'F':
   OLC_MODE(d) = QEDIT_PREREQ;
   write_to_output(d, "Enter a prerequisite object vnum (-1 for none) : ");
   break;
 case 'g':
 case 'G':
   OLC_MODE(d) = QEDIT_GOLD;
   write_to_output(d, "Enter the number of gold coins (0 for none) : ");
   break;
 case 't':
 case 'T':
   OLC_MODE(d) = QEDIT_EXP;
   write_to_output(d, "Enter a number of experience points (0 for none) : ");
   break;
 case 'o':
 case 'O':
   OLC_MODE(d) = QEDIT_OBJ;
   write_to_output(d, "Enter the prize object vnum (-1 for none) : ");
   break;
 case 'l':
        case 'L':
   OLC_MODE(d) = QEDIT_TIMELIMIT;
          write_to_output(d, "Enter time limit to complete (-1 for none) : " );
          break;
 case 'n':
 case 'N':
          OLC_MODE(d) = QEDIT_NEXTQUEST;
          write_to_output(d, "Enter vnum of next quest (-1 for none) : ");
          break;
   case 'p':
 case 'P':
          OLC_MODE(d) = QEDIT_PREVQUEST;
          write_to_output(d, "Enter vnum of previous quest (-1 for none) : ");
          break;
        default:
   write_to_output(d, "Invalid choice!\r\n");
   qedit_disp_menu(d);
   break;
      }
      return;
    /*-------------------------------------------------------------------*/
    case QEDIT_NAME:
      if (!genolc_checkstring(d, arg))
        break;
      if (OLC_QUEST(d)->name)
        free(OLC_QUEST(d)->name);
      arg[MAX_QUEST_NAME - 1] = '\0';
      OLC_QUEST(d)->name = str_udup(arg);
      break;
    case QEDIT_DESC:
      if (!genolc_checkstring(d, arg))
        break;
      if (OLC_QUEST(d)->desc)
        free(OLC_QUEST(d)->desc);
      arg[MAX_QUEST_DESC - 1] = '\0';
      OLC_QUEST(d)->desc = str_udup(arg);
      break;
    case QEDIT_QUESTMASTER:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "questmaster"))
        return;
      if (number != -1)
        if (real_mobile(number) == NOBODY) {
          write_to_output(d, "That mobile does not exist, try again : ");
          return;
        }
      OLC_QUEST(d)->qm = number;
      break;
    case QEDIT_TYPES:
      number--;
      if (number < 0 || number >= NUM_AQ_TYPES) {
        write_to_output(d, "Invalid choice!\r\n");
        qedit_disp_type_menu(d);
        return;
      }
      OLC_QUEST(d)->type = number;
      if (number == AQ_OBJ_RETURN) {
        OLC_MODE(d) = QEDIT_RETURNMOB;
        write_to_output(d, "Enter mob vnum to return object to : ");
        return;
      }
      break;
    case QEDIT_FLAGS:
      if (number < 0 || number > NUM_AQ_FLAGS) {
        write_to_output(d, "That is not a valid choice!\r\n");
        qedit_disp_flag_menu(d);
      } else if (number == 0)
        break;
      else {
        TOGGLE_BIT(OLC_QUEST(d)->flags, number );
        qedit_disp_flag_menu(d);
      }
      return;
    case QEDIT_QUANTITY:
      if (!QEDIT_PARSE_INT(d, arg, 1, 50, &number, "input-range", "quantity"))
        return;
      OLC_QUEST(d)->value[6] = number;
      break;
    case QEDIT_POINTSCOMP:
      if (!QEDIT_PARSE_INT(d, arg, 0, 999999, &number, "input-range", "points_complete"))
        return;
      OLC_QUEST(d)->value[0] = number;
      break;
    case QEDIT_POINTSQUIT:
      if (!QEDIT_PARSE_INT(d, arg, 0, 999999, &number, "input-range", "points_quit"))
        return;
      OLC_QUEST(d)->value[1] = number;
      break;
    case QEDIT_PREREQ:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "prereq"))
        return;
      if (number != -1)
        if (real_object(number) == NOTHING) {
          write_to_output(d, "That object does not exist, try again : ");
          return;
        }
      OLC_QUEST(d)->prereq = number;
      break;
    case QEDIT_LEVELMIN:
      if (number < 0 || number > LVL_IMPL) {
        write_to_output(d, "Level must be between 0 and %d!\r\n", LVL_IMPL);
 write_to_output(d, "Enter minimum level to accept the quest : " );
        return;
      }  else if (number > OLC_QUEST(d)->value[3]) {
 write_to_output(d, "Minimum level can't be above maximum level!\r\n");
 write_to_output(d, "Enter minimum level to accept the quest : " );
        return;
      } else {
        OLC_QUEST(d)->value[2] = number;
        break;
      }
    case QEDIT_LEVELMAX:
      if (number < 0 || number > LVL_IMPL) {
        write_to_output(d, "Level must be between 0 and %d!\r\n", LVL_IMPL);
 write_to_output(d, "Enter maximum level to accept the quest : " );
        return;
      } else if (number < OLC_QUEST(d)->value[2]) {
 write_to_output(d, "Maximum level can't be below minimum level!\r\n");
 write_to_output(d, "Enter maximum level to accept the quest : " );
        return;
      } else {
        OLC_QUEST(d)->value[3] = number;
        break;
      }
    case QEDIT_TIMELIMIT:
      if (!QEDIT_PARSE_INT(d, arg, -1, 100, &number, "input-range", "time_limit"))
        return;
      OLC_QUEST(d)->value[4] = number;
      break;
    case QEDIT_RETURNMOB:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "return_mob"))
        return;
      if (number != -1)
        if (real_mobile(number) == NOBODY) {
          write_to_output(d, "That mobile does not exist, try again : ");
          return;
        }
      OLC_QUEST(d)->value[5] = number;
      break;
    case QEDIT_TARGET:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "target"))
        return;
      OLC_QUEST(d)->target = number;
      break;
    case QEDIT_NEXTQUEST:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "next_quest"))
        return;
      if (number != -1) {
        if (real_quest(number) == NOTHING) {
          write_to_output(d, "That is not a valid quest, try again (-1 for none) : ");
          return;
        }
      }
      OLC_QUEST(d)->next_quest = (number == -1 ? NOTHING : (qst_vnum)number);
      break;
    case QEDIT_PREVQUEST:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "prev_quest"))
        return;
      if (number != -1) {
        if (real_quest(number) == NOTHING) {
          write_to_output(d, "That is not a valid quest, try again (-1 for none) : ");
          return;
        }
      }
      OLC_QUEST(d)->prev_quest = (number == -1 ? NOTHING : (qst_vnum)number);
      break;
    case QEDIT_GOLD:
      if (!QEDIT_PARSE_INT(d, arg, 0, 99999, &number, "input-range", "gold_reward"))
        return;
      OLC_QUEST(d)->gold_reward = number;
      break;
    case QEDIT_EXP:
      if (!QEDIT_PARSE_INT(d, arg, 0, 99999, &number, "input-range", "exp_reward"))
        return;
      OLC_QUEST(d)->exp_reward = number;
      break;
    case QEDIT_OBJ:
      if (!QEDIT_PARSE_INT(d, arg, QEDIT_VNUM_MIN, QEDIT_VNUM_MAX, &number,
                           "vnum-range", "obj_reward"))
        return;
      if (number != -1)
        if (real_object(number) == NOTHING) {
          write_to_output(d, "That object does not exist, try again : ");
          return;
        }
      OLC_QUEST(d)->obj_reward = number;
      break;
    default:
      /*. We should never get here . */
      cleanup_olc(d, CLEANUP_ALL);
      mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: OLC: qedit_parse(): "
        "Reached default case!");
      write_to_output(d, "Oops...\r\n");
      break;
  }
  /*-------------------------------------------------------------------*/
  /*. END OF CASE
  If we get here, we have probably changed something, and now want to
  return to main menu.  Use OLC_VAL as a 'has changed' flag . */

  OLC_VAL(d) = 1;
  qedit_disp_menu(d);
}

void qedit_string_cleanup(struct descriptor_data *d, int terminator)
{
  switch (OLC_MODE(d)) {
  case QEDIT_INFO:
  case QEDIT_COMPLETE:
  case QEDIT_ABANDON:
    qedit_disp_menu(d);
    break;
  }
}

