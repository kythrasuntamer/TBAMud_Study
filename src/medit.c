/**
* @file medit.c
* Oasis OLC - Mobiles: the mobile prototype editor.
*
* medit edits a scratch copy of mob_proto[real_mobile(OLC_NUM)] and saves
* it with add_mobile(). The scratch copy's nr must name the prototype being
* edited (or NOBODY): free_mobile(), delete and save rely on it.
*
* Shared contract of the OasisOLC prototype editors (oedit, medit, redit,
* zedit, sedit; see olc_checks.h):
* - Each session edits a private scratch copy in d->olc; nothing reaches the
*   live tables until the builder answers 'y' at the save prompt, and the
*   copy is released by cleanup_olc() on every exit path.
* - Scratch copies refer to other prototypes by rnum. Those positions move
*   when any builder inserts or deletes a prototype; olc_renumber_sessions()
*   moves every open copy, and OLC_CHECK_RNUM() guards every table index.
* - *_parse(d, arg): arg is the builder's line, a mutable pointer into the
*   descriptor's input buffer (at least MAX_INPUT_LENGTH bytes, ASCII after
*   process_input). It is builder-controlled: validate before storing, and
*   report a refused or clamped value (OLC_SET_CLAMPED, TB_WARN_MSG).
* - Diagnostics use op names in kebab-case with OLC_DIAG / DIAG_CHAR
*   identity; "should never get here" paths use OLC_ABORT.
*
* Copyright 1996 Harvey Gilpin. 1997-2001 George Greer.
*/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "comm.h"
#include "spells.h"
#include "db.h"
#include "shop.h"
#include "genolc.h"
#include "genmob.h"
#include "genzon.h"
#include "genshp.h"
#include "oasis.h"
#include "handler.h"
#include "constants.h"
#include "improved-edit.h"
#include "dg_olc.h"
#include "screen.h"
#include "fight.h"
#include "modify.h"      /* for smash_tilde */
#include "olc_checks.h"

/* local functions */
static void medit_setup_new(struct descriptor_data *d);
static void init_mobile(struct char_data *mob);
static void medit_save_to_disk(zone_vnum zone_num);
static void medit_disp_positions(struct descriptor_data *d);
static void medit_disp_sex(struct descriptor_data *d);
static void medit_disp_attack_types(struct descriptor_data *d);
static bool medit_illegal_mob_flag(int fl);
static int  medit_get_mob_flag_by_number(int num);
static void medit_disp_mob_flags(struct descriptor_data *d);
static void medit_disp_aff_flags(struct descriptor_data *d);
static void medit_disp_menu(struct descriptor_data *d);

/*  utility functions */
ACMD(do_oasis_medit)
{
  int number = NOBODY, save = 0, real_num;
  struct descriptor_data *d;
  char buf1[MAX_STRING_LENGTH];
  char buf2[MAX_STRING_LENGTH];

  /* No building as a mob or while being forced. */
  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)
    return;

  /* Parse any arguments */
  two_arguments(argument, buf1, buf2);

  if (!*buf1) {
    send_to_char(ch, "Specify a mobile VNUM to edit.\r\n");
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

  /* If a numeric argument was given (like a room number), get it. */
  if (number == NOBODY)
    number = tb_parse_int_or_zero(buf1);

  if (number < IDXTYPE_MIN || number > IDXTYPE_MAX) {
    send_to_char(ch, "That mobile VNUM can't exist.\r\n");
    return;
  }

  /* Check that whatever it is isn't already being edited. */
  for (d = descriptor_list; d; d = d->next) {
    if (STATE(d) == CON_MEDIT) {
      if (d->olc && OLC_NUM(d) == number) {
        send_to_char(ch, "That mobile is currently being edited by %s.\r\n",
          GET_NAME(d->character));
        return;
      }
    }
  }

  d = ch->desc;

  /* Give descriptor an OLC structure. */
  if (d->olc)
    OLC_DISCARD_STALE_SESSION(d, ch);

  CREATE(d->olc, struct oasis_olc_data, 1);

  /* Find the zone. */
  OLC_ZNUM(d) = save ? real_zone(number) : real_zone_by_thing(number);
  if (OLC_ZNUM(d) == NOWHERE) {
    send_to_char(ch, "Sorry, there is no zone for that number!\r\n");
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

  /* If save is TRUE, save the mobiles. */
  if (save) {
    send_to_char(ch, "Saving all mobiles in zone %d.\r\n",
      zone_table[OLC_ZNUM(d)].number);
    mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE,
      "OLC: %s saves mobile info for zone %d.",
      GET_NAME(ch), zone_table[OLC_ZNUM(d)].number);

    /* Save the mobiles. */
    save_mobiles(OLC_ZNUM(d));

    /* Free the olc structure stored in the descriptor. */
    free(d->olc);
    d->olc = NULL;
    return;
  }

  OLC_NUM(d) = number;

  /* If this is a new mobile, setup a new one, otherwise, setup the
     existing mobile. */
  if ((real_num = real_mobile(number)) == NOBODY)
    medit_setup_new(d);
  else
    medit_setup_existing(d, real_num);

  medit_disp_menu(d);
  STATE(d) = CON_MEDIT;

  /* Display the OLC messages to the players in the same room as the
     builder and also log it. */
  act("$n starts using OLC.", TRUE, d->character, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);

  mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(ch)), TRUE,"OLC: %s starts editing zone %d allowed zone %d",
    GET_NAME(ch), zone_table[OLC_ZNUM(d)].number, GET_OLC_ZONE(ch));
}

static void medit_save_to_disk(zone_vnum foo)
{
  save_mobiles(real_zone(foo));
}

static void medit_setup_new(struct descriptor_data *d)
{
  struct char_data *mob;

  /* Allocate a scratch mobile structure. */
  CREATE(mob, struct char_data, 1);

  init_mobile(mob);

  GET_MOB_RNUM(mob) = NOBODY;
  /* Set up some default strings. */
  GET_ALIAS(mob) = strdup("mob unfinished");
  GET_SDESC(mob) = strdup("the unfinished mob");
  GET_LDESC(mob) = strdup("An unfinished mob stands here.\r\n");
  GET_DDESC(mob) = strdup("It looks unfinished.\r\n");
  SCRIPT(mob) = NULL;
  mob->proto_script = OLC_SCRIPT(d) = NULL;

  OLC_MOB(d) = mob;
  /* Has changed flag. (It hasn't so far, we just made it.) */
  OLC_VAL(d) = FALSE;
  OLC_ITEM_TYPE(d) = MOB_TRIGGER;
}

/* Start editing a copy of mob_proto[rmob_num]. The copy keeps the source's
 * nr (copy_mobile is a struct copy); callers that copy a different
 * prototype into this session ('W') must reset nr to
 * real_mobile(OLC_NUM(d)): free_mobile(), delete and save treat nr as the
 * identity of the prototype being edited. */
void medit_setup_existing(struct descriptor_data *d, int rmob_num)
{
  struct char_data *mob;

  /* Allocate a scratch mobile structure. */
  CREATE(mob, struct char_data, 1);

  copy_mobile(mob, mob_proto + rmob_num);

  OLC_MOB(d) = mob;
  OLC_ITEM_TYPE(d) = MOB_TRIGGER;
  dg_olc_script_copy(d);
  /*
   * The edited mob must not have a script.
   * It will be assigned to the updated mob later, after editing.
   */
  SCRIPT(mob) = NULL;
  OLC_MOB(d)->proto_script = NULL;
}

/* Ideally, this function should be in db.c, but I'll put it here for portability. */
static void init_mobile(struct char_data *mob)
{
  clear_char(mob);

  GET_HIT(mob) = GET_MANA(mob) = 1;
  GET_MAX_MANA(mob) = GET_MAX_MOVE(mob) = 100;
  GET_NDD(mob) = GET_SDD(mob) = 1;
  GET_WEIGHT(mob) = 200;
  GET_HEIGHT(mob) = 198;

  mob->real_abils.str = mob->real_abils.intel = mob->real_abils.wis = 11;
  mob->real_abils.dex = mob->real_abils.con = mob->real_abils.cha = 11;
  mob->aff_abils = mob->real_abils;

  GET_SAVE(mob, SAVING_PARA)   = 0;
  GET_SAVE(mob, SAVING_ROD)    = 0;
  GET_SAVE(mob, SAVING_PETRI)  = 0;
  GET_SAVE(mob, SAVING_BREATH) = 0;
  GET_SAVE(mob, SAVING_SPELL)  = 0;

  SET_BIT_AR(MOB_FLAGS(mob), MOB_ISNPC);
  mob->player_specials = &dummy_mob;
}

/* Save new/edited mob to memory. */
/* Save the scratch mobile as mob_proto[real_mobile(OLC_NUM)].
 *
 * Precondition: OLC_MOB(d)->nr is real_mobile(OLC_NUM) or NOBODY (repaired
 * and reported as mob-proto-nr otherwise); its proto_script is NULL and
 * OLC_SCRIPT(d) holds the edited trigger list. add_mobile() overwrites the
 * prototype by struct copy and does not take ownership of proto_script:
 * this function frees the previous list, gives the prototype OLC_SCRIPT
 * (clearing the session's handle) and rebuilds live mobs' triggers.
 * Postcondition on insert: other sessions' mobile rnums were moved.
 * Returns FALSE, after reporting medit-save, when nothing was saved. */
static int medit_save_checked(struct descriptor_data *d)
{
  int i;
  mob_rnum new_rnum, existing;
  struct char_data *mob;
  struct trig_proto_list *old_proto;

  existing = real_mobile(OLC_NUM(d));
  i = (existing == NOBODY);
  /* add_mobile() struct-copies the scratch mobile (proto_script NULL, see
   * medit_setup_existing) over the prototype, so after it returns the old
   * trigger list is reachable only through this pointer. */
  old_proto = i ? NULL : mob_proto[existing].proto_script;

  /* Invariant: the scratch copy's nr is the rnum of the prototype this
   * session edits (OLC_NUM), or NOBODY for a new one. add_mobile()'s update
   * branch struct-copies it into mob_proto[], and read_mobile() copies it
   * into every new instance, so a wrong nr (left by 'W' or a missed
   * renumber) would make that prototype's mobs report another vnum and
   * drift both mob_index counters. OLC_NUM is the authority: repair it. */
  if (GET_MOB_RNUM(OLC_MOB(d)) != existing) {
    struct char_data *builder = d->character;
    TB_ERROR_MSG("olc", "mob-proto-nr", "mobile",
                 "builder " DIAG_CHAR_FMT " mob vnum %d scratch nr %d != rnum %d; repaired before save",
                 DIAG_CHAR(builder), OLC_NUM(d),
                 (int)GET_MOB_RNUM(OLC_MOB(d)), (int)existing);
    GET_MOB_RNUM(OLC_MOB(d)) = existing;
  }

  if ((new_rnum = add_mobile(OLC_MOB(d), OLC_NUM(d))) == NOBODY) {
    struct char_data *builder = d->character;
    TB_ERROR_MSG("olc", "medit-save", "mobile",
                 "builder " DIAG_CHAR_FMT " mob vnum %d: add_mobile returned NOBODY; edits not saved",
                 DIAG_CHAR(builder), OLC_NUM(d));
    return FALSE;
  }

  /* Update triggers and free the old proto list. */
  if (old_proto && old_proto != OLC_SCRIPT(d)) {
    mob_proto[new_rnum].proto_script = old_proto;
    free_proto_script(&mob_proto[new_rnum], MOB_TRIGGER);
  }

  /* The prototype now owns the list; clear the session's handle so
   * cleanup_olc() does not free it. */
  mob_proto[new_rnum].proto_script = OLC_SCRIPT(d);
  OLC_SCRIPT(d) = NULL;

  /* this takes care of the mobs currently in-game */
  for (mob = character_list; mob; mob = mob->next) {
    if (GET_MOB_RNUM(mob) != new_rnum)
      continue;

    /* remove any old scripts */
    if (SCRIPT(mob))
      extract_script(mob, MOB_TRIGGER);

    free_proto_script(mob, MOB_TRIGGER);
    copy_proto_script(&mob_proto[new_rnum], mob, MOB_TRIGGER);
    assign_triggers(mob, MOB_TRIGGER);
  }
  /* end trigger update */

  if (!i)	/* Only renumber on new mobiles. */
    return TRUE;

  /* add_mobile() shifted mob_proto up from new_rnum. Move every other open
   * session's mobile rnums with it: zedit 'M' commands, sedit keepers and
   * other medit scratch identities. */
  olc_renumber_sessions(OLC_RNUM_MOB, new_rnum, +1, d);
  return TRUE;
}

/* Save the scratch mobile into mob_proto[]; FALSE (reported) when nothing was saved.
 * Entry point for oasis_copy.c's copy table, which has no failure path;
 * the failure was already reported by medit_save_checked(). */
void medit_save_internally(struct descriptor_data *d)
{
  (void)medit_save_checked(d);
}

/* Menu functions
   Display positions. (sitting, standing, etc) */
static void medit_disp_positions(struct descriptor_data *d)
{
  get_char_colors(d->character);
  clear_screen(d);
  column_list(d->character, 0, position_types, NUM_POSITIONS, TRUE);
  write_to_output(d, "Enter position number : ");
}

/* Display the gender of the mobile. */
static void medit_disp_sex(struct descriptor_data *d)
{
  get_char_colors(d->character);
  clear_screen(d);
  column_list(d->character, 0, genders, NUM_GENDERS, TRUE);
  write_to_output(d, "Enter gender number : ");
}

/* Display attack types menu. */
static void medit_disp_attack_types(struct descriptor_data *d)
{
  int i;

  get_char_colors(d->character);
  clear_screen(d);

  for (i = 0; i < NUM_ATTACK_TYPES; i++) {
    write_to_output(d, "%s%2d%s) %s\r\n", grn, i, nrm, attack_hit_text[i].singular);
  }
  write_to_output(d, "Enter attack type : ");
}

/* Find mob flags that shouldn't be set by builders */
static bool medit_illegal_mob_flag(int fl)
{
  int i;

  /* add any other flags you dont want them setting */
  const int illegal_flags[] = {
    MOB_ISNPC,
    MOB_NOTDEADYET,
  };

  const int num_illegal_flags = sizeof(illegal_flags)/sizeof(int);


  for (i=0; i < num_illegal_flags;i++)
    if (fl == illegal_flags[i])
      return (TRUE);

  return (FALSE);

}

/* Due to illegal mob flags not showing in the mob flags list,
   we need this to convert the list number back to flag value */
static int medit_get_mob_flag_by_number(int num)
{
  int i, count = 0;
  for (i = 0; i < NUM_MOB_FLAGS; i++) {
    if (medit_illegal_mob_flag(i)) continue;
    if ((++count) == num) return i;
  }
  /* Return 'illegal flag' value */
  return -1;
}

/* Display mob-flags menu. */
static void medit_disp_mob_flags(struct descriptor_data *d)
{
  int i, count = 0, columns = 0;
  char flags[MAX_STRING_LENGTH];

  get_char_colors(d->character);
  clear_screen(d);

  /* Mob flags has special handling to remove illegal flags from the list */
  for (i = 0; i < NUM_MOB_FLAGS; i++) {
    if (medit_illegal_mob_flag(i)) continue;
    write_to_output(d, "%s%2d%s) %-20.20s  %s", grn, ++count, nrm, action_bits[i],
                !(++columns % 2) ? "\r\n" : "");
  }

  SPRINTBITARRAY_FIELD(MOB_FLAGS(OLC_MOB(d)), action_bits, flags);
  write_to_output(d, "\r\nCurrent flags : %s%s%s\r\nEnter mob flags (0 to quit) : ", cyn, flags, nrm);
}

/* Display affection flags menu. */
static void medit_disp_aff_flags(struct descriptor_data *d)
{
  char flags[MAX_STRING_LENGTH];

  get_char_colors(d->character);
  clear_screen(d);
  /* +1/-1 antics needed because AFF_FLAGS doesn't start at 0. */
  column_list(d->character, 0, affected_bits + 1, NUM_AFF_FLAGS - 1, TRUE);
  SPRINTBITARRAY_FIELD(AFF_FLAGS(OLC_MOB(d)), affected_bits, flags);
  write_to_output(d, "\r\nCurrent flags   : %s%s%s\r\nEnter aff flags (0 to quit) : ",
                          cyn, flags, nrm);
}

/* Display main menu. */
static void medit_disp_menu(struct descriptor_data *d)
{
  struct char_data *mob;
  char flags[MAX_STRING_LENGTH], flag2[MAX_STRING_LENGTH];

  mob = OLC_MOB(d);
  get_char_colors(d->character);
  clear_screen(d);

  write_to_output(d,
  "-- Mob Number:  [%s%d%s]\r\n"
  "%s1%s) Sex: %s%-7.7s%s	         %s2%s) Keywords: %s%s\r\n"
  "%s3%s) S-Desc: %s%s\r\n"
  "%s4%s) L-Desc:-\r\n%s%s\r\n"
  "%s5%s) D-Desc:-\r\n%s%s\r\n",

	  cyn, OLC_NUM(d), nrm,
	  grn, nrm, yel, OLC_TABLE_NAME(d, genders, NUM_GENDERS, (int)GET_SEX(mob)), nrm,
	  grn, nrm, yel, GET_ALIAS(mob),
	  grn, nrm, yel, GET_SDESC(mob),
	  grn, nrm, yel, GET_LDESC(mob),
	  grn, nrm, yel, GET_DDESC(mob)
	  );

  SPRINTBITARRAY_FIELD(MOB_FLAGS(mob), action_bits, flags);
  SPRINTBITARRAY_FIELD(AFF_FLAGS(mob), affected_bits, flag2);
  write_to_output(d,
	  "%s6%s) Position  : %s%s\r\n"
	  "%s7%s) Default   : %s%s\r\n"
	  "%s8%s) Attack    : %s%s\r\n"
      "%s9%s) Stats Menu...\r\n"
	  "%sA%s) NPC Flags : %s%s\r\n"
	  "%sB%s) AFF Flags : %s%s\r\n"
          "%sS%s) Script    : %s%s\r\n"
          "%sW%s) Copy mob\r\n"
	  "%sX%s) Delete mob\r\n"
	  "%sQ%s) Quit\r\n"
	  "Enter choice : ",

	  grn, nrm, yel, OLC_TABLE_NAME(d, position_types, NUM_POSITIONS, (int)GET_POS(mob)),
	  grn, nrm, yel, OLC_TABLE_NAME(d, position_types, NUM_POSITIONS, (int)GET_DEFAULT_POS(mob)),
	  grn, nrm, yel, OLC_CHECK_INDEX(d, "attack_hit_text", (int)GET_ATTACK(mob), NUM_ATTACK_TYPES) ?
	                 attack_hit_text[(int)GET_ATTACK(mob)].singular : "<invalid>",
	  grn, nrm,
	  grn, nrm, cyn, flags,
	  grn, nrm, cyn, flag2,
          grn, nrm, cyn, OLC_SCRIPT(d) ?"Set.":"Not Set.",
          grn, nrm,
	  grn, nrm,
	  grn, nrm
	  );

  OLC_MODE(d) = MEDIT_MAIN_MENU;
}

/* Display main menu. */
static void medit_disp_stats_menu(struct descriptor_data *d)
{
  struct char_data *mob;
  char buf[MAX_STRING_LENGTH];

  mob = OLC_MOB(d);
  get_char_colors(d->character);
  clear_screen(d);

  /* Color codes have to be used here, for count_color_codes to work */
  tb_snprintf(buf, sizeof(buf), "(range \ty%d\tn to \ty%d\tn)", GET_HIT(mob) + GET_MOVE(mob), (GET_HIT(mob) * GET_MANA(mob)) + GET_MOVE(mob));

  /* Top section - standard stats */
  write_to_output(d,
  "-- Mob Number:  %s[%s%d%s]%s\r\n"
  "(%s1%s) Level:       %s[%s%4d%s]%s\r\n"
  "(%s2%s) %sAuto Set Stats (based on level)%s\r\n\r\n"
  "Hit Points  (xdy+z):        Bare Hand Damage (xdy+z): \r\n"
  "(%s3%s) HP NumDice:  %s[%s%5d%s]%s    (%s6%s) BHD NumDice:  %s[%s%5d%s]%s\r\n"
  "(%s4%s) HP SizeDice: %s[%s%5d%s]%s    (%s7%s) BHD SizeDice: %s[%s%5d%s]%s\r\n"
  "(%s5%s) HP Addition: %s[%s%5d%s]%s    (%s8%s) DamRoll:      %s[%s%5d%s]%s\r\n"
  "%-*s(range %s%d%s to %s%d%s)\r\n\r\n"

  "(%sA%s) Armor Class: %s[%s%4d%s]%s        (%sD%s) Hitroll:   %s[%s%5d%s]%s\r\n"
  "(%sB%s) Exp Points:  %s[%s%10d%s]%s  (%sE%s) Alignment: %s[%s%5d%s]%s\r\n"
  "(%sC%s) Gold:        %s[%s%10d%s]%s\r\n\r\n",
      cyn, yel, OLC_NUM(d), cyn, nrm,
      cyn, nrm, cyn, yel, GET_LEVEL(mob), cyn, nrm,
      cyn, nrm, cyn, nrm,
      cyn, nrm, cyn, yel, GET_HIT(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_NDD(mob), cyn, nrm,
      cyn, nrm, cyn, yel, GET_MANA(mob), cyn, nrm,  cyn, nrm, cyn, yel, GET_SDD(mob), cyn, nrm,
      cyn, nrm, cyn, yel, GET_MOVE(mob), cyn, nrm,  cyn, nrm, cyn, yel, GET_DAMROLL(mob), cyn, nrm,

      count_color_chars(buf)+28, buf,
      yel, GET_NDD(mob) + GET_DAMROLL(mob), nrm,
      yel, (GET_NDD(mob) * GET_SDD(mob)) + GET_DAMROLL(mob), nrm,

      cyn, nrm, cyn, yel, GET_AC(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_HITROLL(mob), cyn, nrm,
      cyn, nrm, cyn, yel, GET_EXP(mob), cyn, nrm,  cyn, nrm, cyn, yel, GET_ALIGNMENT(mob), cyn, nrm,
      cyn, nrm, cyn, yel, GET_GOLD(mob), cyn, nrm
      );

  if (CONFIG_MEDIT_ADVANCED) {
    /* Bottom section - non-standard stats, togglable in cedit */
    write_to_output(d,
    "(%sF%s) Str: %s[%s%2d/%3d%s]%s   Saving Throws\r\n"
    "(%sG%s) Int: %s[%s%3d%s]%s      (%sL%s) Paralysis     %s[%s%3d%s]%s\r\n"
    "(%sH%s) Wis: %s[%s%3d%s]%s      (%sM%s) Rods/Staves   %s[%s%3d%s]%s\r\n"
    "(%sI%s) Dex: %s[%s%3d%s]%s      (%sN%s) Petrification %s[%s%3d%s]%s\r\n"
    "(%sJ%s) Con: %s[%s%3d%s]%s      (%sO%s) Breath        %s[%s%3d%s]%s\r\n"
    "(%sK%s) Cha: %s[%s%3d%s]%s      (%sP%s) Spells        %s[%s%3d%s]%s\r\n\r\n",
        cyn, nrm, cyn, yel, GET_STR(mob), GET_ADD(mob), cyn, nrm,
        cyn, nrm, cyn, yel, GET_INT(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_SAVE(mob, SAVING_PARA), cyn, nrm,
        cyn, nrm, cyn, yel, GET_WIS(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_SAVE(mob, SAVING_ROD), cyn, nrm,
        cyn, nrm, cyn, yel, GET_DEX(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_SAVE(mob, SAVING_PETRI), cyn, nrm,
        cyn, nrm, cyn, yel, GET_CON(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_SAVE(mob, SAVING_BREATH), cyn, nrm,
        cyn, nrm, cyn, yel, GET_CHA(mob), cyn, nrm,   cyn, nrm, cyn, yel, GET_SAVE(mob, SAVING_SPELL), cyn, nrm
        );
  }

  /* Quit to previous menu option */
  write_to_output(d, "(%sQ%s) Quit to main menu\r\nEnter choice : ", cyn, nrm);

  OLC_MODE(d) = MEDIT_STATS_MENU;
}

void medit_parse(struct descriptor_data *d, char *arg)
{
  int i = -1, j;
  char *oldtext = NULL;

  if (OLC_MODE(d) > MEDIT_NUMERICAL_RESPONSE) {
    i = tb_parse_int_or_zero(arg);
    /* Builder input: every numeric mode but the y/n delete prompt takes a
     * whole number. tb_parse_int_or_zero() reads "abc" as 0, so refuse
     * anything else here rather than store a 0 the builder never typed. */
    if (!*arg || (OLC_MODE(d) != MEDIT_DELETE && !is_number(arg))) {
      write_to_output(d, "Please enter a whole number, try again : ");
      return;
    }
  } else {	/* String response. */
    if (!genolc_checkstring(d, arg))
      return;
  }
  switch (OLC_MODE(d)) {
  case MEDIT_CONFIRM_SAVESTRING:
    /* Ensure mob has MOB_ISNPC set. */
    SET_BIT_AR(MOB_FLAGS(OLC_MOB(d)), MOB_ISNPC);
    switch (*arg) {
    case 'y':
    case 'Y':
      /* Save the mob in memory and to disk. */
      if (!medit_save_checked(d)) {
        write_to_output(d, "Internal error: mobile %d could not be saved; nothing was changed.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), TRUE, "OLC: %s edits mob %d", GET_NAME(d->character), OLC_NUM(d));
      if (CONFIG_OLC_SAVE) {
        medit_save_to_disk(zone_table[real_zone_by_thing(OLC_NUM(d))].number);
        write_to_output(d, "Mobile saved to disk.\r\n");
      } else
        write_to_output(d, "Mobile saved to memory.\r\n");
      cleanup_olc(d, CLEANUP_ALL);
      return;
    case 'n':
    case 'N':
      /* Not saving: cleanup_olc() frees the unsaved OLC_SCRIPT copy. */
      cleanup_olc(d, CLEANUP_ALL);
      return;
    default:
      write_to_output(d, "Invalid choice!\r\n");
      write_to_output(d, "Do you wish to save your changes? : ");
      return;
    }

  case MEDIT_MAIN_MENU:
    i = 0;
    switch (*arg) {
    case 'q':
    case 'Q':
      if (OLC_VAL(d)) {	/* Anything been changed? */
	      write_to_output(d, "Do you wish to save your changes? : ");
	      OLC_MODE(d) = MEDIT_CONFIRM_SAVESTRING;
      } else
	cleanup_olc(d, CLEANUP_ALL);
      return;
    case '1':
      OLC_MODE(d) = MEDIT_SEX;
      medit_disp_sex(d);
      return;
    case '2':
      OLC_MODE(d) = MEDIT_KEYWORD;
      i--;
      break;
    case '3':
      OLC_MODE(d) = MEDIT_S_DESC;
      i--;
      break;
    case '4':
      OLC_MODE(d) = MEDIT_L_DESC;
      i--;
      break;
    case '5':
      OLC_MODE(d) = MEDIT_D_DESC;
      send_editor_help(d);
      write_to_output(d, "Enter mob description:\r\n\r\n");
      if (OLC_MOB(d)->player.description) {
	      write_to_output(d, "%s", OLC_MOB(d)->player.description);
	      oldtext = strdup(OLC_MOB(d)->player.description);
      }
      string_write(d, &OLC_MOB(d)->player.description, MAX_MOB_DESC, 0, oldtext);
      OLC_VAL(d) = 1;
      return;
    case '6':
      OLC_MODE(d) = MEDIT_POS;
      medit_disp_positions(d);
      return;
    case '7':
      OLC_MODE(d) = MEDIT_DEFAULT_POS;
      medit_disp_positions(d);
      return;
    case '8':
      OLC_MODE(d) = MEDIT_ATTACK;
      medit_disp_attack_types(d);
      return;
    case '9':
      OLC_MODE(d) = MEDIT_STATS_MENU;
      medit_disp_stats_menu(d);
      return;
    case 'a':
    case 'A':
      OLC_MODE(d) = MEDIT_NPC_FLAGS;
      medit_disp_mob_flags(d);
      return;
    case 'b':
    case 'B':
      OLC_MODE(d) = MEDIT_AFF_FLAGS;
      medit_disp_aff_flags(d);
      return;
    case 'w':
    case 'W':
      write_to_output(d, "Copy what mob? ");
      OLC_MODE(d) = MEDIT_COPY;
      return;
    case 'x':
    case 'X':
      write_to_output(d, "Are you sure you want to delete this mobile? ");
      OLC_MODE(d) = MEDIT_DELETE;
      return;
    case 's':
    case 'S':
      OLC_SCRIPT_EDIT_MODE(d) = SCRIPT_MAIN_MENU;
      dg_script_menu(d);
      return;
    default:
      medit_disp_menu(d);
      return;
    }
    if (i == 0)
      break;
    else if (i == 1)
      write_to_output(d, "\r\nEnter new value : ");
    else if (i == -1)
      write_to_output(d, "\r\nEnter new text :\r\n] ");
    else
      write_to_output(d, "Oops...\r\n");
    return;

  case MEDIT_STATS_MENU:
    i=0;
    switch(*arg) {
    case 'q':
    case 'Q':
      medit_disp_menu(d);
      return;
    case '1':  /* Edit level */
      OLC_MODE(d) = MEDIT_LEVEL;
      i++;
      break;
    case '2':  /* Autoroll stats */
      medit_autoroll_stats(d);
      medit_disp_stats_menu(d);
      OLC_VAL(d) = TRUE;
      return;
    case '3':
      OLC_MODE(d) = MEDIT_NUM_HP_DICE;
      i++;
      break;
    case '4':
      OLC_MODE(d) = MEDIT_SIZE_HP_DICE;
      i++;
      break;
    case '5':
      OLC_MODE(d) = MEDIT_ADD_HP;
      i++;
      break;
    case '6':
      OLC_MODE(d) = MEDIT_NDD;
      i++;
      break;
    case '7':
      OLC_MODE(d) = MEDIT_SDD;
      i++;
      break;
    case '8':
      OLC_MODE(d) = MEDIT_DAMROLL;
      i++;
      break;
    case 'a':
    case 'A':
      OLC_MODE(d) = MEDIT_AC;
      i++;
      break;
    case 'b':
    case 'B':
      OLC_MODE(d) = MEDIT_EXP;
      i++;
      break;
    case 'c':
    case 'C':
      OLC_MODE(d) = MEDIT_GOLD;
      i++;
      break;
    case 'd':
    case 'D':
      OLC_MODE(d) = MEDIT_HITROLL;
      i++;
      break;
    case 'e':
    case 'E':
      OLC_MODE(d) = MEDIT_ALIGNMENT;
      i++;
      break;
    case 'f':
    case 'F':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_STR;
      i++;
      break;
    case 'g':
    case 'G':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_INT;
      i++;
      break;
    case 'h':
    case 'H':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_WIS;
      i++;
      break;
    case 'i':
    case 'I':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_DEX;
      i++;
      break;
    case 'j':
    case 'J':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_CON;
      i++;
      break;
    case 'k':
    case 'K':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_CHA;
      i++;
      break;
    case 'l':
    case 'L':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_PARA;
      i++;
      break;
    case 'm':
    case 'M':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_ROD;
      i++;
      break;
    case 'n':
    case 'N':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_PETRI;
      i++;
      break;
    case 'o':
    case 'O':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_BREATH;
      i++;
      break;
    case 'p':
    case 'P':
      if (!CONFIG_MEDIT_ADVANCED) {
        write_to_output(d, "Invalid Choice!\r\nEnter Choice : ");
        return;
	  }
      OLC_MODE(d) = MEDIT_SPELL;
      i++;
      break;
    default:
      medit_disp_stats_menu(d);
      return;
    }
    if (i == 0)
      break;
    else if (i == 1)
      write_to_output(d, "\r\nEnter new value : ");
    else if (i == -1)
      write_to_output(d, "\r\nEnter new text :\r\n] ");
    else
      write_to_output(d, "Oops...\r\n");
    return;

  case OLC_SCRIPT_EDIT:
    if (dg_script_edit_parse(d, arg)) return;
    break;

  case MEDIT_KEYWORD:
    smash_tilde(arg);
    OLC_REPLACE_STRING(GET_ALIAS(OLC_MOB(d)), arg);
    break;

  case MEDIT_S_DESC:
    smash_tilde(arg);
    OLC_REPLACE_STRING(GET_SDESC(OLC_MOB(d)), arg);
    break;

  case MEDIT_L_DESC:
    smash_tilde(arg);
    if (GET_LDESC(OLC_MOB(d)))
      free(GET_LDESC(OLC_MOB(d)));
    if (arg && *arg) {
      /* Builder input of up to MAX_INPUT_LENGTH - 1 bytes plus "\r\n":
       * size the copy from the input so the line ending is never cut off
       * (a fixed MAX_INPUT_LENGTH buffer silently dropped it, and the
       * room listing ran this mob into the next line). */
      size_t len = strlen(arg);
      char *ldesc;

      CREATE(ldesc, char, len + 3);
      tb_snprintf(ldesc, len + 3, "%s\r\n", arg);   /* fits exactly */
      GET_LDESC(OLC_MOB(d)) = ldesc;
    } else
      GET_LDESC(OLC_MOB(d)) = strdup("undefined");

    break;

  case MEDIT_D_DESC:
    /*
     * We should never get here.
     */
    OLC_ABORT(d, "medit-bad-mode");

  case MEDIT_NPC_FLAGS:
    if ((i = tb_parse_int_or_zero(arg)) <= 0)
      break;
    else if ( (j = medit_get_mob_flag_by_number(i)) == -1) {
       write_to_output(d, "Invalid choice!\r\n");
       write_to_output(d, "Enter mob flags (0 to quit) :");
       return;
    } else if (j <= NUM_MOB_FLAGS) {
      TOGGLE_BIT_AR(MOB_FLAGS(OLC_MOB(d)), (j));
    }
    medit_disp_mob_flags(d);
    return;

  case MEDIT_AFF_FLAGS:
    if ((i = tb_parse_int_or_zero(arg)) <= 0)
      break;
    else if (i < NUM_AFF_FLAGS)
      TOGGLE_BIT_AR(AFF_FLAGS(OLC_MOB(d)), i);

    /* Remove unwanted bits right away. */
    REMOVE_BIT_AR(AFF_FLAGS(OLC_MOB(d)), AFF_CHARM);
    REMOVE_BIT_AR(AFF_FLAGS(OLC_MOB(d)), AFF_POISON);
    REMOVE_BIT_AR(AFF_FLAGS(OLC_MOB(d)), AFF_SLEEP);
    medit_disp_aff_flags(d);
    return;

/* Numerical responses. */

  case MEDIT_SEX:
    OLC_SET_CLAMPED(d, i, i, 1, NUM_GENDERS, "Gender choice");
    GET_SEX(OLC_MOB(d)) = i - 1;
    break;

  case MEDIT_HITROLL:
    OLC_SET_CLAMPED(d, GET_HITROLL(OLC_MOB(d)), i, 0, 50, "Hitroll");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_DAMROLL:
    OLC_SET_CLAMPED(d, GET_DAMROLL(OLC_MOB(d)), i, 0, 50, "Damroll");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_NDD:
    OLC_SET_CLAMPED(d, GET_NDD(OLC_MOB(d)), i, 0, 30, "Damage dice count");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_SDD:
    OLC_SET_CLAMPED(d, GET_SDD(OLC_MOB(d)), i, 0, 127, "Damage dice size");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_NUM_HP_DICE:
    OLC_SET_CLAMPED(d, GET_HIT(OLC_MOB(d)), i, 0, 30, "HP dice count");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_SIZE_HP_DICE:
    OLC_SET_CLAMPED(d, GET_MANA(OLC_MOB(d)), i, 0, 1000, "HP dice size");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_ADD_HP:
    OLC_SET_CLAMPED(d, GET_MOVE(OLC_MOB(d)), i, 0, 30000, "HP bonus");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_AC:
    OLC_SET_CLAMPED(d, GET_AC(OLC_MOB(d)), i, -200, 200, "Armor class");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_EXP:
    OLC_SET_CLAMPED(d, GET_EXP(OLC_MOB(d)), i, 0, MAX_MOB_EXP, "Experience");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_GOLD:
    OLC_SET_CLAMPED(d, GET_GOLD(OLC_MOB(d)), i, 0, MAX_MOB_GOLD, "Gold");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_STR:
    OLC_SET_CLAMPED(d, GET_STR(OLC_MOB(d)), i, 11, 25, "Strength");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_INT:
    OLC_SET_CLAMPED(d, GET_INT(OLC_MOB(d)), i, 11, 25, "Intelligence");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_WIS:
    OLC_SET_CLAMPED(d, GET_WIS(OLC_MOB(d)), i, 11, 25, "Wisdom");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_DEX:
    OLC_SET_CLAMPED(d, GET_DEX(OLC_MOB(d)), i, 11, 25, "Dexterity");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_CON:
    OLC_SET_CLAMPED(d, GET_CON(OLC_MOB(d)), i, 11, 25, "Constitution");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_CHA:
    OLC_SET_CLAMPED(d, GET_CHA(OLC_MOB(d)), i, 11, 25, "Charisma");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_PARA:
    OLC_SET_CLAMPED(d, GET_SAVE(OLC_MOB(d), SAVING_PARA), i, 0, 100, "Save vs paralysis");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_ROD:
    OLC_SET_CLAMPED(d, GET_SAVE(OLC_MOB(d), SAVING_ROD), i, 0, 100, "Save vs rods");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_PETRI:
    OLC_SET_CLAMPED(d, GET_SAVE(OLC_MOB(d), SAVING_PETRI), i, 0, 100, "Save vs petrification");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_BREATH:
    OLC_SET_CLAMPED(d, GET_SAVE(OLC_MOB(d), SAVING_BREATH), i, 0, 100, "Save vs breath");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_SPELL:
    OLC_SET_CLAMPED(d, GET_SAVE(OLC_MOB(d), SAVING_SPELL), i, 0, 100, "Save vs spells");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_POS:
    OLC_SET_CLAMPED(d, i, i, 1, NUM_POSITIONS, "Position choice");
    GET_POS(OLC_MOB(d)) = i - 1;
    break;

  case MEDIT_DEFAULT_POS:
    OLC_SET_CLAMPED(d, i, i, 1, NUM_POSITIONS, "Default position choice");
    GET_DEFAULT_POS(OLC_MOB(d)) = i - 1;
    break;

  case MEDIT_ATTACK:
    OLC_SET_CLAMPED(d, GET_ATTACK(OLC_MOB(d)), i, 0, NUM_ATTACK_TYPES - 1, "Attack type");
    break;

  case MEDIT_LEVEL:
    OLC_SET_CLAMPED(d, GET_LEVEL(OLC_MOB(d)), i, 1, LVL_IMPL, "Level");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_ALIGNMENT:
    OLC_SET_CLAMPED(d, GET_ALIGNMENT(OLC_MOB(d)), i, -1000, 1000, "Alignment");
    OLC_VAL(d) = TRUE;
    medit_disp_stats_menu(d);
    return;

  case MEDIT_COPY:
    if ((i = real_mobile(tb_parse_int_or_zero(arg))) != NOWHERE) {
      /* setup allocates a fresh scratch mobile and trigger list: release
       * the ones this session holds first, as cleanup_olc() would. */
      olc_free_script_copy(d);
      free_mobile(OLC_MOB(d));
      OLC_MOB(d) = NULL;
      medit_setup_existing(d, i);
      /* copy_mobile() kept the source's rnum (nr), but this session still
       * edits OLC_NUM(d): delete, save and free_mobile() rely on nr naming
       * that prototype or NOBODY. */
      GET_MOB_RNUM(OLC_MOB(d)) = real_mobile(OLC_NUM(d));
    } else
      write_to_output(d, "That mob does not exist.\r\n");
    break;

  case MEDIT_DELETE:
    if (*arg == 'y' || *arg == 'Y') {
      /* Delete the prototype this session edits (OLC_NUM), never whatever
       * rnum the scratch copy holds: after 'W' or a missed renumber that
       * names another mobile, possibly in a zone the builder cannot edit. */
      mob_rnum doomed = real_mobile(OLC_NUM(d));
      struct char_data *builder = d->character;

      if (doomed == NOBODY) {
        TB_WARN_MSG("olc", "delete-target-missing", "mobile",
                    "builder " DIAG_CHAR_FMT " mob vnum %d has no prototype to delete",
                    DIAG_CHAR(builder), OLC_NUM(d));
        write_to_output(d, "Mobile %d has never been saved; there is nothing to delete.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      if (!can_edit_zone(builder, real_zone_by_thing(OLC_NUM(d)))) {
        TB_WARN_MSG("olc", "delete-permission", "mobile",
                    "builder " DIAG_CHAR_FMT " mob vnum %d delete refused: zone not editable",
                    DIAG_CHAR(builder), OLC_NUM(d));
        write_to_output(d, "You do not have permission to delete mobile %d.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      if (GET_MOB_RNUM(OLC_MOB(d)) != doomed) {
        TB_ERROR_MSG("olc", "delete-target-mismatch", "mobile",
                     "builder " DIAG_CHAR_FMT " editing mob vnum %d (rnum %d) but scratch rnum is %d; delete refused",
                     DIAG_CHAR(builder), OLC_NUM(d), doomed, GET_MOB_RNUM(OLC_MOB(d)));
        write_to_output(d, "Internal error: this editor lost track of mobile %d; nothing was deleted.\r\n", OLC_NUM(d));
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }

      if (delete_mobile(doomed) != NOBODY) {
        /* Other builders' scratch copies hold mobile rnums that just moved. */
        olc_renumber_sessions(OLC_RNUM_MOB, doomed, -1, d);
        write_to_output(d, "Mobile deleted.\r\n");
      } else
        write_to_output(d, "Couldn't delete the mobile!\r\n");

      cleanup_olc(d, CLEANUP_ALL);
      return;
    } else if (*arg == 'n' || *arg == 'N') {
      medit_disp_menu(d);
      OLC_MODE(d) = MEDIT_MAIN_MENU;
      return;
    } else
      write_to_output(d, "Please answer 'Y' or 'N': ");
    break;

  default:
    /* We should never get here. */
    OLC_ABORT(d, "medit-bad-mode");
  }

/* END OF CASE If we get here, we have probably changed something, and now want
   to return to main menu.  Use OLC_VAL as a 'has changed' flag */

  OLC_VAL(d) = TRUE;
  medit_disp_menu(d);
}

void medit_string_cleanup(struct descriptor_data *d, int terminator)
{
  switch (OLC_MODE(d)) {

  case MEDIT_D_DESC:
  default:
     medit_disp_menu(d);
     break;
  }
}

void medit_autoroll_stats(struct descriptor_data *d)
{
  int mob_lev;

  mob_lev = GET_LEVEL(OLC_MOB(d));
  mob_lev = GET_LEVEL(OLC_MOB(d)) = LIMIT(mob_lev, 1, LVL_IMPL);

  GET_MOVE(OLC_MOB(d))    = mob_lev*10;          /* hit point bonus (mobs don't use movement points */
  GET_HIT(OLC_MOB(d))     = mob_lev/5;           /* number of hitpoint dice */
  GET_MANA(OLC_MOB(d))    = mob_lev/5;           /* size of hitpoint dice   */

  GET_NDD(OLC_MOB(d))     = MAX(1, mob_lev/6);   /* number damage dice 1-5  */
  GET_SDD(OLC_MOB(d))     = MAX(2, mob_lev/6);   /* size of damage dice 2-5 */
  GET_DAMROLL(OLC_MOB(d)) = mob_lev/6;           /* damroll (dam bonus) 0-5 */

  GET_HITROLL(OLC_MOB(d)) = mob_lev/3;           /* hitroll 0-10            */
  GET_EXP(OLC_MOB(d))     = (mob_lev*mob_lev*100);
  GET_GOLD(OLC_MOB(d))    = (mob_lev*10);
  GET_AC(OLC_MOB(d))      = (100-(mob_lev*6));   /* AC 94 to -80            */

  /* 'Advanced' stats are only rolled if advanced options are enabled */
  if (CONFIG_MEDIT_ADVANCED) {
    GET_STR(OLC_MOB(d))     = LIMIT((mob_lev*2)/3, 11, 18); /* 2/3 level in range 11 to 18 */
    GET_INT(OLC_MOB(d))     = LIMIT((mob_lev*2)/3, 11, 18);
    GET_WIS(OLC_MOB(d))     = LIMIT((mob_lev*2)/3, 11, 18);
    GET_DEX(OLC_MOB(d))     = LIMIT((mob_lev*2)/3, 11, 18);
    GET_CON(OLC_MOB(d))     = LIMIT((mob_lev*2)/3, 11, 18);
    GET_CHA(OLC_MOB(d))     = LIMIT((mob_lev*2)/3, 11, 18);

    GET_SAVE(OLC_MOB(d), SAVING_PARA)   = mob_lev / 4;  /* All Saving throws */
    GET_SAVE(OLC_MOB(d), SAVING_ROD)    = mob_lev / 4;  /* set to a quarter  */
    GET_SAVE(OLC_MOB(d), SAVING_PETRI)  = mob_lev / 4;  /* of the mobs level */
    GET_SAVE(OLC_MOB(d), SAVING_BREATH) = mob_lev / 4;
    GET_SAVE(OLC_MOB(d), SAVING_SPELL)  = mob_lev / 4;
  }

}
