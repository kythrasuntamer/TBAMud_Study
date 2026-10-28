/**************************************************************************
 *  File: spell_parser.c                                    Part of tbaMUD *
 *  Usage: Top-level magic routines; outside points of entry to magic sys. *
 *                                                                         *
 *  All rights reserved.  See license for complete information.            *
 *                                                                         *
 *  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
 *  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
 **************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "spells.h"
#include "handler.h"
#include "comm.h"
#include "db.h"
#include "dg_scripts.h"
#include "fight.h"  /* for hit() */

#define SINFO spell_info[spellnum]

/* Global Variables definitions, used elsewhere */
struct spell_info_type spell_info[TOP_SPELL_DEFINE + 1];
/* The target keyword typed to 'cast', for spells that search by name
 * (spell_locate_object). Written only by do_cast and cleared when do_cast
 * returns, so it is non-empty only while a player's cast is in progress;
 * wands, scrolls and dg_cast see "" and the spell falls back to the target
 * object's own keyword. Player text: never log it. */
char cast_arg2[MAX_INPUT_LENGTH];
const char *unused_spellname = "!UNUSED!"; /* So we can get &unused_spellname */

/* Local (File Scope) Function Prototypes */
static void say_spell(struct char_data *ch, int spellnum, struct char_data *tch,
    struct obj_data *tobj);
static void spello(int spl, const char *name, int max_mana, int min_mana,
    int mana_change, int minpos, int targets, int violent, int routines,
    const char *wearoff);
static int mag_manacost(struct char_data *ch, int spellnum);

/* Local (File Scope) Variables */
struct syllable {
  const char *org;
  const char *news;
};
static struct syllable syls[] = { { " ", " " }, { "ar", "abra" },
    { "ate", "i" }, { "cau", "kada" }, { "blind", "nose" }, { "bur", "mosa" }, {
        "cu", "judi" }, { "de", "oculo" }, { "dis", "mar" },
    { "ect", "kamina" }, { "en", "uns" }, { "gro", "cra" }, { "light", "dies" },
    { "lo", "hi" }, { "magi", "kari" }, { "mon", "bar" }, { "mor", "zak" }, {
        "move", "sido" }, { "ness", "lacri" }, { "ning", "illa" }, { "per",
        "duda" }, { "ra", "gru" }, { "re", "candus" }, { "son", "sabru" }, {
        "tect", "infra" }, { "tri", "cula" }, { "ven", "nofo" }, { "word of",
        "inset" }, { "a", "i" }, { "b", "v" }, { "c", "q" }, { "d", "m" }, {
        "e", "o" }, { "f", "y" }, { "g", "t" }, { "h", "p" }, { "i", "u" }, {
        "j", "y" }, { "k", "t" }, { "l", "r" }, { "m", "w" }, { "n", "b" }, {
        "o", "a" }, { "p", "s" }, { "q", "d" }, { "r", "f" }, { "s", "g" }, {
        "t", "h" }, { "u", "e" }, { "v", "z" }, { "w", "x" }, { "x", "n" }, {
        "y", "l" }, { "z", "k" }, { "", "" } };

static int mag_manacost(struct char_data *ch, int spellnum) {
  /* chclass is a signed byte; keep its sign so SPELL_MIN_LEVEL() sees and
   * refuses a negative (corrupt) class instead of a wrapped 0..255 one. */
  int cls = (int) GET_CLASS(ch);

  return MAX(SINFO.mana_max - (SINFO.mana_change *
      (GET_LEVEL(ch) - SPELL_MIN_LEVEL(spellnum, cls))),
  SINFO.mana_min);
}

static char *obfuscate_spell(const char *unobfuscated) {
  static char obfuscated[200];
  size_t maxlen = sizeof(obfuscated) - 1;

  int j, ofs = 0;

  *obfuscated = '\0';

  while (unobfuscated[ofs]) {
    for (j = 0; *(syls[j].org); j++) {
      if (!strncmp(syls[j].org, unobfuscated + ofs, strlen(syls[j].org))) {
        if (strlen(syls[j].news) < maxlen) {
          strncat(obfuscated, syls[j].news, maxlen);
          maxlen -= strlen(syls[j].news);
        } else {
          log("No room in obfuscated version of '%s' (currently obfuscated to '%s') to add syllable '%s'.",
              unobfuscated, obfuscated, syls[j].news);
        }
        ofs += strlen(syls[j].org);
        break;
      }
    }
    /* i.e., we didn't find a match in syls[] */
    if (!*syls[j].org) {
      log("No entry in syllable table for substring of '%s' starting at '%s'.", unobfuscated, unobfuscated + ofs);
      ofs++;
    }
  }
  return obfuscated;
}

static void say_spell(struct char_data *ch, int spellnum, struct char_data *tch,
    struct obj_data *tobj) {
  const char *lead, *spell = skill_name(spellnum);
  char act_buf_original[256], act_buf_obfuscated[256], *obfuscated = obfuscate_spell(spell);


  struct char_data *i;

  /* Pick the act() lead-in; the format itself stays a literal so the
   * compiler can check it (format strings are never chosen at runtime). */
  if (tch != NULL && IN_ROOM(tch) == IN_ROOM(ch)) {
    if (tch == ch)
      lead = "$n closes $s eyes and utters the words";
    else
      lead = "$n stares at $N and utters the words";
  } else if (tobj != NULL
      && ((IN_ROOM(tobj) == IN_ROOM(ch)) || (tobj->carried_by == ch)))
    lead = "$n stares at $p and utters the words";
  else
    lead = "$n utters the words";

  tb_snprintf(act_buf_original, sizeof(act_buf_original), "%s, '%s'.", lead, spell);
  tb_snprintf(act_buf_obfuscated, sizeof(act_buf_obfuscated), "%s, '%s'.", lead, obfuscated);

  for (i = world[IN_ROOM(ch)].people; i; i = i->next_in_room) {
    if (i == ch || i == tch || !i->desc || !AWAKE(i))
      continue;
    if (GET_CLASS(ch) == GET_CLASS(i))
      perform_act(act_buf_original, ch, tobj, tch, i);
    else
      perform_act(act_buf_obfuscated, ch, tobj, tch, i);
  }

  if (tch != NULL && tch != ch && IN_ROOM(tch) == IN_ROOM(ch)) {
    tb_snprintf(act_buf_original, sizeof(act_buf_original), "$n stares at you and utters the words, '%s'.",
    GET_CLASS(ch) == GET_CLASS(tch) ? spell : obfuscated);
    act(act_buf_original, FALSE, ch, NULL, tch, TO_VICT);
  }
}

/* This function should be used anytime you are not 100% sure that you have
 * a valid spell/skill number.  A typical for() loop would not need to use
 * this because you can guarantee > 0 and <= TOP_SPELL_DEFINE. */
const char *skill_name(int num) {
  if (VALID_SPELLNUM(num))
    return (spell_info[num].name);
  else if (num == -1)
    return ("UNUSED");
  else
    return ("UNDEFINED");
}

int find_skill_num(char *name) {
  int skindex, ok;
  char *temp, *temp2;
  /* any_one_arg() fills MAX_INPUT_LENGTH-byte buffers; name is player input. */
  char first[MAX_INPUT_LENGTH], first2[MAX_INPUT_LENGTH], tempbuf[MAX_INPUT_LENGTH];

  for (skindex = 1; skindex <= TOP_SPELL_DEFINE; skindex++) {
    if (is_abbrev(name, spell_info[skindex].name))
      return (skindex);

    ok = TRUE;
    strlcpy(tempbuf, spell_info[skindex].name, sizeof(tempbuf)); /* strlcpy: OK */
    temp = any_one_arg(tempbuf, first);
    temp2 = any_one_arg(name, first2);
    while (*first && *first2 && ok) {
      if (!is_abbrev(first2, first))
        ok = FALSE;
      temp = any_one_arg(temp, first);
      temp2 = any_one_arg(temp2, first2);
    }

    if (ok && !*first2)
      return (skindex);
  }

  return (-1);
}

/* This function is the very heart of the entire magic system.  All invocations
 * of all types of magic -- objects, spoken and unspoken PC and NPC spells, the
 * works -- all come through this function eventually. This is also the entry
 * point for non-spoken or unrestricted spells. Spellnum 0 is legal but silently
 * ignored here, to make callers simpler. */
/* Re-validate call_magic's participants after a cast trigger ran. DG scripts
 * (builder-controlled) can purge the target object, which extract_obj()
 * frees immediately, so ovict is checked by UID without dereferencing it.
 * Characters are only marked for extraction (freed at end of pulse), so a
 * purged caster or cvict is still allocated but must not be acted on.
 * Returns TRUE when the cast may continue; otherwise reports
 * cast-target-purged and returns FALSE. */
static bool cast_targets_intact(struct char_data *caster,
    struct char_data *cvict, struct obj_data *ovict, long ouid, int spellnum,
    const char *trigger) {
  if (ovict && !obj_uid_matches(ouid, ovict)) {
    TB_WARN_MSG("magic", "cast-target-purged", skill_name(spellnum),
                "spell %d obj uid %ld purged by %s cast trigger; caster "
                DIAG_CHAR_FMT, spellnum, ouid, trigger, DIAG_CHAR(caster));
    return FALSE;
  }
  if (DEAD(caster)) {
    TB_WARN_MSG("magic", "cast-target-purged", skill_name(spellnum),
                "spell %d caster " DIAG_CHAR_FMT " purged by %s cast trigger",
                spellnum, DIAG_CHAR(caster), trigger);
    return FALSE;
  }
  if (cvict && DEAD(cvict)) {
    TB_WARN_MSG("magic", "cast-target-purged", skill_name(spellnum),
                "spell %d victim " DIAG_CHAR_FMT " purged by %s cast trigger; "
                "caster " DIAG_CHAR_FMT, spellnum, DIAG_CHAR(cvict), trigger,
                DIAG_CHAR(caster));
    return FALSE;
  }
  return TRUE;
}

/**
 * Cast spellnum from caster at cvict and/or ovict.
 *
 * The room, object and mob cast triggers run first and can purge or move
 * caster, cvict and ovict. ovict is freed at once by extract_obj(); the
 * function re-validates all three after each trigger and stops (returns 0)
 * when one was purged. Callers that keep using any of these pointers after
 * call_magic() returns must re-validate them the same way (obj by UID with
 * obj_uid_matches(), characters with DEAD()).
 *
 * @return -1 the target died (do not cast again); 0 the cast was blocked,
 *         invalid, or a participant was purged; 1 the spell was cast.
 */
int call_magic(struct char_data *caster, struct char_data *cvict,
    struct obj_data *ovict, int spellnum, int level, int casttype) {
  int savetype;
  long ouid;

  /* 0 and -1 are the documented "no spell" values of scroll and potion
   * slots, and stay silent. Anything else out of range (object values,
   * osetval, dg_cast) is a data error and is reported. */
  if (!VALID_SPELLNUM(spellnum)) {
    if (spellnum != 0 && spellnum != -1)
      TB_ERROR_MSG("magic", "spellnum-range", "call_magic",
                   "spell %d not in [1,%d] casttype %d by " DIAG_CHAR_FMT
                   " ovict vnum %d", spellnum, TOP_SPELL_DEFINE, casttype,
                   DIAG_CHAR(caster), ovict ? (int) GET_OBJ_VNUM(ovict) : NOTHING);
    return (0);
  }

  /* The cast triggers and the room flag checks below index world[] with
   * the caster's room. A caster stranded in NOWHERE by an earlier, reported
   * failure (see extract_char_final()) can still type 'cast': refuse it
   * rather than read past world[]. */
  if (!VALID_ROOM_RNUM(IN_ROOM(caster))) {
    TB_ERROR_MSG("magic", "caster-nowhere", "call_magic",
                 "spell %d casttype %d by " DIAG_CHAR_FMT " from no valid "
                 "room (rnum %d); refused", spellnum, casttype,
                 DIAG_CHAR(caster), (int) IN_ROOM(caster));
    return (0);
  }

  /* Snapshot the object's identity before any script can free it. */
  ouid = ovict ? obj_script_id(ovict) : 0;

  if (!cast_wtrigger(caster, cvict, ovict, spellnum))
    return 0;
  if (!cast_targets_intact(caster, cvict, ovict, ouid, spellnum, "room"))
    return 0;
  if (!cast_otrigger(caster, ovict, spellnum))
    return 0;
  if (!cast_targets_intact(caster, cvict, ovict, ouid, spellnum, "object"))
    return 0;
  if (!cast_mtrigger(caster, cvict, spellnum))
    return 0;
  if (!cast_targets_intact(caster, cvict, ovict, ouid, spellnum, "mob"))
    return 0;

  if (ROOM_FLAGGED(IN_ROOM(caster), ROOM_NOMAGIC)) {
    send_to_char(caster, "Your magic fizzles out and dies.\r\n");
    act("$n's magic fizzles out and dies.", FALSE, caster, 0, 0, TO_ROOM);
    return (0);
  }
  if (ROOM_FLAGGED(IN_ROOM(caster), ROOM_PEACEFUL) && (SINFO.violent || IS_SET(SINFO.routines, MAG_DAMAGE))) {
    send_to_char(caster, "A flash of white light fills the room, dispelling your violent magic!\r\n");
    act("White light from no particular source suddenly fills the room, then vanishes.", FALSE, caster, 0, 0, TO_ROOM);
    return (0);
  }
  if (cvict && MOB_FLAGGED(cvict, MOB_NOKILL)) {
    send_to_char(caster, "This mob is protected.\r\n");
    return (0);
  }
  /* determine the type of saving throw */
  switch (casttype) {
  case CAST_STAFF:
  case CAST_SCROLL:
  case CAST_POTION:
  case CAST_WAND:
    savetype = SAVING_ROD;
    break;
  case CAST_SPELL:
    savetype = SAVING_SPELL;
    break;
  default:
    savetype = SAVING_BREATH;
    break;
  }

  if (IS_SET(SINFO.routines, MAG_DAMAGE))
    if (mag_damage(level, caster, cvict, spellnum, savetype) == -1)
      return (-1); /* Successful and target died, don't cast again. */

  if (IS_SET(SINFO.routines, MAG_AFFECTS))
    mag_affects(level, caster, cvict, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_UNAFFECTS))
    mag_unaffects(level, caster, cvict, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_POINTS))
    mag_points(level, caster, cvict, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_ALTER_OBJS))
    mag_alter_objs(level, caster, ovict, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_GROUPS))
    mag_groups(level, caster, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_MASSES))
    mag_masses(level, caster, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_AREAS))
    mag_areas(level, caster, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_SUMMONS))
    mag_summons(level, caster, ovict, spellnum, savetype);

  if (IS_SET(SINFO.routines, MAG_CREATIONS))
    mag_creations(level, caster, spellnum);

  if (IS_SET(SINFO.routines, MAG_ROOMS))
    mag_rooms(level, caster, spellnum);

  if (IS_SET(SINFO.routines, MAG_MANUAL))
    switch (spellnum) {
    case SPELL_CHARM:
      MANUAL_SPELL(spell_charm)
      ;
      break;
    case SPELL_CREATE_WATER:
      MANUAL_SPELL(spell_create_water)
      ;
      break;
    case SPELL_DETECT_POISON:
      MANUAL_SPELL(spell_detect_poison)
      ;
      break;
    case SPELL_ENCHANT_WEAPON:
      MANUAL_SPELL(spell_enchant_weapon)
      ;
      break;
    case SPELL_IDENTIFY:
      MANUAL_SPELL(spell_identify)
      ;
      break;
    case SPELL_LOCATE_OBJECT:
      MANUAL_SPELL(spell_locate_object)
      ;
      break;
    case SPELL_SUMMON:
      MANUAL_SPELL(spell_summon)
      ;
      break;
    case SPELL_WORD_OF_RECALL:
      MANUAL_SPELL(spell_recall)
      ;
      break;
    case SPELL_TELEPORT:
      MANUAL_SPELL(spell_teleport)
      ;
      break;
    }

  return (1);
}

/* Check a magic item's spell value before it indexes spell_info[]. Reports
 * item-spellnum (obj vnum/UID, value slot, user) and tells the user it
 * fizzles when the value is not a real spell. */
static bool item_spell_valid(struct char_data *ch, struct obj_data *obj,
    int spl, int slot, const char *kind) {
  if (VALID_SPELLNUM(spl))
    return TRUE;
  TB_ERROR_MSG("magic", "item-spellnum", kind,
               "obj vnum %d uid %ld value%d spell %d out of range [1,%d]; "
               "used by " DIAG_CHAR_FMT, (int) GET_OBJ_VNUM(obj),
               obj_script_id(obj), slot, spl, TOP_SPELL_DEFINE, DIAG_CHAR(ch));
  send_to_char(ch, "It fizzles.\r\n");
  return FALSE;
}

/* Cast a scroll's or potion's spells (values 1-3, at level value 0) until
 * one returns <= 0. Every call_magic() runs DG cast triggers, which can purge
 * the item itself or the target object (both freed at once by extract_obj),
 * so their identities are snapshotted as UIDs and re-checked before the item
 * is read again or the target is passed on. Returns TRUE while the item still
 * exists (the caller then consumes it), FALSE after reporting
 * magic-item-purged when a trigger destroyed it. */
static bool cast_item_spells(struct char_data *ch, struct obj_data *obj,
    struct char_data *tch, struct obj_data *tobj, int casttype,
    const char *kind) {
  long uid = obj_script_id(obj);
  long tuid = tobj ? obj_script_id(tobj) : 0;
  int vnum = GET_OBJ_VNUM(obj), i;

  for (i = 1; i <= 3; i++) {
    if (call_magic(ch, tch, tobj, GET_OBJ_VAL(obj, i), GET_OBJ_VAL(obj, 0),
        casttype) <= 0)
      break;
    if (!obj_uid_matches(uid, obj))
      break;
    if (tobj && !obj_uid_matches(tuid, tobj)) {
      TB_WARN_MSG("magic", "magic-item-purged", kind,
                  "target obj uid %ld purged during %s cast by "
                  DIAG_CHAR_FMT "; remaining spells of obj vnum %d skipped",
                  tuid, kind, DIAG_CHAR(ch), vnum);
      break;
    }
  }
  if (!obj_uid_matches(uid, obj)) {
    TB_WARN_MSG("magic", "magic-item-purged", kind,
                "obj vnum %d uid %ld purged during %s cast by " DIAG_CHAR_FMT,
                vnum, uid, kind, DIAG_CHAR(ch));
    return FALSE;
  }
  return TRUE;
}

/* mag_objectmagic: This is the entry-point for all magic items.  This should
 * only be called by the 'quaff', 'use', 'recite', etc. routines.
 * For reference, object values 0-3:
 * staff  - [0]	level	[1] max charges	[2] num charges	[3] spell num
 * wand   - [0]	level	[1] max charges	[2] num charges	[3] spell num
 * scroll - [0]	level	[1] spell num	[2] spell num	[3] spell num
 * potion - [0] level	[1] spell num	[2] spell num	[3] spell num
 * Staves and wands will default to level 14 if the level is not specified; the
 * DikuMUD format did not specify staff and wand levels in the world files
 *
 * Ownership: a scroll or potion is consumed (extract_obj, freed at once) by
 * this function unless a cast trigger already purged it; a wand or staff
 * may also be purged by a trigger. The caller must not use obj after this
 * returns.
 *
 * Trust: the values are builder/script data (world file, oedit, DG
 * osetval). Boot only logs bad spell numbers, so value 3 of a wand or staff
 * is range-checked here before it indexes spell_info[], and scroll/potion
 * spell values are checked by call_magic(). */
void mag_objectmagic(struct char_data *ch, struct obj_data *obj, char *argument) {
  char arg[MAX_INPUT_LENGTH];
  int i, k, spl, vnum;
  long uid;
  struct char_data *tch = NULL;
  struct obj_data *tobj = NULL;

  one_argument(argument, arg);

  k = generic_find(arg, FIND_CHAR_ROOM | FIND_OBJ_INV | FIND_OBJ_ROOM |
  FIND_OBJ_EQUIP, ch, &tch, &tobj);

  /* Wand and staff value 3 indexes spell_info[] below. It comes from the
   * world file (boot only logs a bad value), OLC, or DG osetval (any int),
   * so it is untrusted: -1 ("no spell") and out-of-range values fizzle. */
  if ((GET_OBJ_TYPE(obj) == ITEM_WAND || GET_OBJ_TYPE(obj) == ITEM_STAFF) &&
      !item_spell_valid(ch, obj, GET_OBJ_VAL(obj, 3), 3,
                        GET_OBJ_TYPE(obj) == ITEM_WAND ? "wand" : "staff"))
    return;

  switch (GET_OBJ_TYPE(obj)) {
  case ITEM_STAFF:
    act("You tap $p three times on the ground.", FALSE, ch, obj, 0, TO_CHAR);
    if (obj->action_description)
      act(obj->action_description, FALSE, ch, obj, 0, TO_ROOM);
    else
      act("$n taps $p three times on the ground.", FALSE, ch, obj, 0, TO_ROOM);

    if (GET_OBJ_VAL(obj, 2) <= 0) {
      send_to_char(ch, "It seems powerless.\r\n");
      act("Nothing seems to happen.", FALSE, ch, obj, 0, TO_ROOM);
    } else {
      GET_OBJ_VAL(obj, 2)--;
      WAIT_STATE(ch, PULSE_VIOLENCE);
      /* Level to cast spell at. */
      k = GET_OBJ_VAL(obj, 0) ? GET_OBJ_VAL(obj, 0) : DEFAULT_STAFF_LVL;
      /* Each call_magic() runs cast triggers that may purge the staff (a
       * world UID purge frees it at once). Read its values once, and stop
       * as soon as its UID no longer resolves to it. */
      spl = GET_OBJ_VAL(obj, 3);
      uid = obj_script_id(obj);
      vnum = GET_OBJ_VNUM(obj);

      /* Area/mass spells on staves can cause crashes. So we use special cases
       * for those spells spells here. */
      if (HAS_SPELL_ROUTINE(spl, MAG_MASSES | MAG_AREAS)) {
        for (i = 0, tch = world[IN_ROOM(ch)].people; tch;
            tch = tch->next_in_room)
          i++;
        while (i-- > 0 && obj_uid_matches(uid, obj))
          call_magic(ch, NULL, NULL, spl, k, CAST_STAFF);
      } else {
        /* call_magic() runs cast triggers, and the spell itself can kill
         * (death triggers): any of them can teleport or purge the next
         * occupant, so next_in_room is not kept across it. The room is
         * snapshotted as UIDs and each target re-resolved; one that left
         * the caster's room or is pending extraction is skipped and
         * reported, and the staff stops if the caster itself is purged or
         * moved (H2-10). */
        room_rnum origin = IN_ROOM(ch);
        long *targets;
        int count = room_people_uids(origin, &targets);

        for (i = 0; i < count && obj_uid_matches(uid, obj); i++) {
          if (DEAD(ch) || IN_ROOM(ch) != origin) {
            TB_WARN_MSG("magic", "staff-caster-moved", "staff",
                        "obj vnum %d uid %ld: caster " DIAG_CHAR_FMT
                        " purged or moved during the staff cast; %d "
                        "target(s) skipped", vnum, uid, DIAG_CHAR(ch),
                        count - i);
            break;
          }
          if ((tch = room_person_live(targets[i], origin)) == NULL) {
            TB_WARN_MSG("magic", "staff-target-moved", "staff",
                        "target uid %ld moved or purged during the staff "
                        "cast of obj vnum %d by " DIAG_CHAR_FMT "; skipped",
                        targets[i], vnum, DIAG_CHAR(ch));
            continue;
          }
          if (ch != tch)
            call_magic(ch, tch, NULL, spl, k, CAST_STAFF);
        }
        free(targets);
      }
      if (!obj_uid_matches(uid, obj))
        TB_WARN_MSG("magic", "magic-item-purged", "staff",
                    "obj vnum %d uid %ld purged during staff cast by "
                    DIAG_CHAR_FMT "; remaining targets skipped", vnum, uid,
                    DIAG_CHAR(ch));
    }
    break;
  case ITEM_WAND:
    if (k == FIND_CHAR_ROOM) {
      if (tch == ch) {
        act("You point $p at yourself.", FALSE, ch, obj, 0, TO_CHAR);
        act("$n points $p at $mself.", FALSE, ch, obj, 0, TO_ROOM);
      } else {
        act("You point $p at $N.", FALSE, ch, obj, tch, TO_CHAR);
        if (obj->action_description)
          act(obj->action_description, FALSE, ch, obj, tch, TO_ROOM);
        else
          act("$n points $p at $N.", TRUE, ch, obj, tch, TO_ROOM);
      }
    } else if (tobj != NULL) {
      act("You point $p at $P.", FALSE, ch, obj, tobj, TO_CHAR);
      if (obj->action_description)
        act(obj->action_description, FALSE, ch, obj, tobj, TO_ROOM);
      else
        act("$n points $p at $P.", TRUE, ch, obj, tobj, TO_ROOM);
    } else if (IS_SET(spell_info[GET_OBJ_VAL(obj, 3)].routines,
        MAG_AREAS | MAG_MASSES)) {
      /* Wands with area spells don't need to be pointed. */
      act("You point $p outward.", FALSE, ch, obj, NULL, TO_CHAR);
      act("$n points $p outward.", TRUE, ch, obj, NULL, TO_ROOM);
    } else {
      act("At what should $p be pointed?", FALSE, ch, obj, NULL, TO_CHAR);
      return;
    }

    if (GET_OBJ_VAL(obj, 2) <= 0) {
      send_to_char(ch, "It seems powerless.\r\n");
      act("Nothing seems to happen.", FALSE, ch, obj, 0, TO_ROOM);
      return;
    }
    GET_OBJ_VAL(obj, 2)--;
    WAIT_STATE(ch, PULSE_VIOLENCE);
    if (GET_OBJ_VAL(obj, 0))
      call_magic(ch, tch, tobj, GET_OBJ_VAL(obj, 3), GET_OBJ_VAL(obj, 0),
          CAST_WAND);
    else
      call_magic(ch, tch, tobj, GET_OBJ_VAL(obj, 3),
      DEFAULT_WAND_LVL, CAST_WAND);
    break;
  case ITEM_SCROLL:
    if (*arg) {
      if (!k) {
        act("There is nothing to here to affect with $p.", FALSE, ch, obj, NULL,
            TO_CHAR);
        return;
      }
    } else
      tch = ch;

    act("You recite $p which dissolves.", TRUE, ch, obj, 0, TO_CHAR);
    if (obj->action_description)
      act(obj->action_description, FALSE, ch, obj, tch, TO_ROOM);
    else
      act("$n recites $p.", FALSE, ch, obj, NULL, TO_ROOM);

    WAIT_STATE(ch, PULSE_VIOLENCE);
    /* The scroll is consumed here, unless a cast trigger already purged it
     * (extract_obj frees at once, so it must not be extracted twice). */
    if (cast_item_spells(ch, obj, tch, tobj, CAST_SCROLL, "scroll"))
      extract_obj(obj);
    break;
  case ITEM_POTION:
    tch = ch;

    if (!consume_otrigger(obj, ch, OCMD_QUAFF)) /* check trigger */
      return;

    act("You quaff $p.", FALSE, ch, obj, NULL, TO_CHAR);
    if (obj->action_description)
      act(obj->action_description, FALSE, ch, obj, NULL, TO_ROOM);
    else
      act("$n quaffs $p.", TRUE, ch, obj, NULL, TO_ROOM);

    WAIT_STATE(ch, PULSE_VIOLENCE);
    if (cast_item_spells(ch, obj, ch, NULL, CAST_POTION, "potion"))
      extract_obj(obj);
    break;
  default:
    log("SYSERR: Unknown object_type %d in mag_objectmagic.",
        GET_OBJ_TYPE(obj));
    break;
  }
}

/* cast_spell is used generically to cast any spoken spell, assuming we already
 * have the target char/obj and spell number.  It checks all restrictions,
 * prints the words, etc. Entry point for NPC casts.  Recommended entry point
 * for spells cast by NPCs via specprocs. */
int cast_spell(struct char_data *ch, struct char_data *tch,
    struct obj_data *tobj, int spellnum) {
  if (spellnum < 0 || spellnum > TOP_SPELL_DEFINE) {
    log("SYSERR: cast_spell trying to call spellnum %d/%d.", spellnum,
    TOP_SPELL_DEFINE);
    return (0);
  }

  if (GET_POS(ch) < SINFO.min_position) {
    switch (GET_POS(ch)) {
      case POS_SLEEPING:
        send_to_char(ch, "You dream about great magical powers.\r\n");
        break;
      case POS_RESTING:
        send_to_char(ch, "You cannot concentrate while resting.\r\n");
        break;
      case POS_SITTING:
        send_to_char(ch, "You can't do this sitting!\r\n");
        break;
      case POS_FIGHTING:
        send_to_char(ch, "Impossible!  You can't concentrate enough!\r\n");
        break;
      default:
        send_to_char(ch, "You can't do much of anything like this!\r\n");
        break;
    }
    return (0);
  }
  if (AFF_FLAGGED(ch, AFF_CHARM) && (ch->master == tch)) {
    send_to_char(ch, "You are afraid you might hurt your master!\r\n");
    return (0);
  }
  if ((tch != ch) && IS_SET(SINFO.targets, TAR_SELF_ONLY)) {
    send_to_char(ch, "You can only cast this spell upon yourself!\r\n");
    return (0);
  }
  if ((tch == ch) && IS_SET(SINFO.targets, TAR_NOT_SELF)) {
    send_to_char(ch, "You cannot cast this spell upon yourself!\r\n");
    return (0);
  }
  if (IS_SET(SINFO.routines, MAG_GROUPS) && !GROUP(ch)) {
    send_to_char(ch, "You can't cast this spell if you're not in a group!\r\n");
    return (0);
  }
  send_to_char(ch, "%s", CONFIG_OK);
  say_spell(ch, spellnum, tch, tobj);

  return (call_magic(ch, tch, tobj, spellnum, GET_LEVEL(ch), CAST_SPELL));
}

/* do_cast is the entry point for PC-casted spells.  It parses the arguments,
 * determines the spell number and finds a target, throws the die to see if
 * the spell can be cast, checks for sufficient mana and subtracts it, and
 * passes control to cast_spell(). */
static void do_cast_body(struct char_data *ch, char *argument);

ACMD(do_cast) {
  do_cast_body(ch, argument);
  /* Never leave this cast's target keyword for a later wand, scroll or
   * script cast to pick up. */
  *cast_arg2 = '\0';
}

static void do_cast_body(struct char_data *ch, char *argument) {
  struct char_data *tch = NULL;
  struct obj_data *tobj = NULL;
  char *s, *t;
  int number, mana, spellnum, i, target = 0;

  if (IS_NPC(ch))
    return;

  /* GET_CLASS indexes spell_info[].min_level[]; it comes from the pfile,
   * which does not range-check it. A bad class can never cast. */
  if (!VALID_CLASS_NUM(GET_CLASS(ch))) {
    TB_ERROR_MSG("magic", "player-class", "min_level",
                 DIAG_CHAR_FMT " class %d not in [0,%d); cast refused",
                 DIAG_CHAR(ch), (int) GET_CLASS(ch), NUM_CLASSES);
    send_to_char(ch, "You do not know that spell!\r\n");
    return;
  }

  /* get: blank, spell name, target name */
  s = strtok(argument, "'");

  if (s == NULL) {
    send_to_char(ch, "Cast what where?\r\n");
    return;
  }
  s = strtok(NULL, "'");
  if (s == NULL) {
    send_to_char(ch,
        "Spell names must be enclosed in the Holy Magic Symbols: '\r\n");
    return;
  }
  t = strtok(NULL, "\0");

  skip_spaces(&s);

  /* spellnum = search_block(s, spells, 0); */
  spellnum = find_skill_num(s);

  if ((spellnum < 1) || (spellnum > MAX_SPELLS) || !*s) {
    send_to_char(ch, "Cast what?!?\r\n");
    return;
  }
  if (GET_LEVEL(ch) < SPELL_MIN_LEVEL(spellnum, (int) GET_CLASS(ch))) {
    send_to_char(ch, "You do not know that spell!\r\n");
    return;
  }
  if (GET_SKILL(ch, spellnum) == 0) {
    send_to_char(ch, "You are unfamiliar with that spell.\r\n");
    return;
  }
  /* Find the target */
  if (t != NULL) {
    char arg[MAX_INPUT_LENGTH];

    strlcpy(arg, t, sizeof(arg));
    one_argument(arg, t);
    skip_spaces(&t);

    /* Copy target to global cast_arg2, for use in spells like locate object */
    strlcpy(cast_arg2, t, sizeof(cast_arg2));
  }
  if (IS_SET(SINFO.targets, TAR_IGNORE)) {
    target = TRUE;
  } else if (t != NULL && *t) {
    number = get_number(&t);
    if (!target && (IS_SET(SINFO.targets, TAR_CHAR_ROOM))) {
      if ((tch = get_char_vis(ch, t, &number, FIND_CHAR_ROOM)) != NULL)
        target = TRUE;
    }
    if (!target && IS_SET(SINFO.targets, TAR_CHAR_WORLD))
      if ((tch = get_char_vis(ch, t, &number, FIND_CHAR_WORLD)) != NULL)
        target = TRUE;

    if (!target && IS_SET(SINFO.targets, TAR_OBJ_INV))
      if ((tobj = get_obj_in_list_vis(ch, t, &number, ch->carrying)) != NULL)
        target = TRUE;

    if (!target && IS_SET(SINFO.targets, TAR_OBJ_EQUIP)) {
      for (i = 0; !target && i < NUM_WEARS; i++)
        if (GET_EQ(ch, i) && isname(t, GET_EQ(ch, i)->name)) {
          tobj = GET_EQ(ch, i);
          target = TRUE;
        }
    }
    if (!target && IS_SET(SINFO.targets, TAR_OBJ_ROOM))
      if ((tobj = get_obj_in_list_vis(ch, t, &number,
          world[IN_ROOM(ch)].contents)) != NULL)
        target = TRUE;

    if (!target && IS_SET(SINFO.targets, TAR_OBJ_WORLD))
      if ((tobj = get_obj_vis(ch, t, &number)) != NULL)
        target = TRUE;

  } else { /* if target string is empty */
    if (!target && IS_SET(SINFO.targets, TAR_FIGHT_SELF))
      if (FIGHTING(ch) != NULL) {
        tch = ch;
        target = TRUE;
      }
    if (!target && IS_SET(SINFO.targets, TAR_FIGHT_VICT))
      if (FIGHTING(ch) != NULL) {
        tch = FIGHTING(ch);
        target = TRUE;
      }
    /* if no target specified, and the spell isn't violent, default to self */
    if (!target && IS_SET(SINFO.targets, TAR_CHAR_ROOM) && !SINFO.violent) {
      tch = ch;
      target = TRUE;
    }
    if (!target) {
      send_to_char(ch, "Upon %s should the spell be cast?\r\n",
          IS_SET(SINFO.targets, TAR_OBJ_ROOM | TAR_OBJ_INV | TAR_OBJ_WORLD | TAR_OBJ_EQUIP) ?
              "what" : "who");
      return;
    }
  }

  if (target && (tch == ch) && SINFO.violent) {
    send_to_char(ch, "You shouldn't cast that on yourself -- could be bad for your health!\r\n");
    return;
  }
  if (!target) {
    send_to_char(ch, "Cannot find the target of your spell!\r\n");
    return;
  }
  mana = mag_manacost(ch, spellnum);
  if ((mana > 0) && (GET_MANA(ch) < mana) && (GET_LEVEL(ch) < LVL_IMMORT)) {
    send_to_char(ch, "You haven't the energy to cast that spell!\r\n");
    return;
  }

  /* You throws the dice and you takes your chances.. 101% is total failure */
  if (rand_number(0, 101) > GET_SKILL(ch, spellnum)) {
    WAIT_STATE(ch, PULSE_VIOLENCE);
    if (!tch || !skill_message(0, ch, tch, spellnum))
      send_to_char(ch, "You lost your concentration!\r\n");
    if (mana > 0)
      GET_MANA(ch) = MAX(0, MIN(GET_MAX_MANA(ch), GET_MANA(ch) - (mana / 2)));
    if (SINFO.violent && tch && IS_NPC(tch))
    hit(tch, ch, TYPE_UNDEFINED);
  } else { /* cast spell returns 1 on success; subtract mana & set waitstate */
    if (cast_spell(ch, tch, tobj, spellnum)) {
      WAIT_STATE(ch, PULSE_VIOLENCE);
      if (mana > 0)
        GET_MANA(ch) = MAX(0, MIN(GET_MAX_MANA(ch), GET_MANA(ch) - mana));
    }
  }
}

void spell_level(int spell, int chclass, int level) {
  int bad = 0;

  if (spell < 0 || spell > TOP_SPELL_DEFINE) {
    log("SYSERR: attempting assign to illegal spellnum %d/%d", spell,
        TOP_SPELL_DEFINE);
    return;
  }

  if (chclass < 0 || chclass >= NUM_CLASSES) {
    log("SYSERR: assigning '%s' to illegal class %d/%d.", skill_name(spell),
        chclass, NUM_CLASSES - 1);
    bad = 1;
  }

  if (level < 1 || level > LVL_IMPL) {
    log("SYSERR: assigning '%s' to illegal level %d/%d.", skill_name(spell),
        level, LVL_IMPL);
    bad = 1;
  }

  if (!bad)
    spell_info[spell].min_level[chclass] = level;
}

/* Assign the spells on boot up */
static void spello(int spl, const char *name, int max_mana, int min_mana,
    int mana_change, int minpos, int targets, int violent, int routines,
    const char *wearoff) {
  int i;

  /* Each spell is registered once; a second spello() silently overwrote
   * the first (SPELL_IDENTIFY once became free to cast from any position). */
  if (spell_info[spl].name && spell_info[spl].name != unused_spellname)
    TB_WARN_MSG("magic", "spello-duplicate", name,
                "spell %d registered twice; the later entry wins", spl);

  for (i = 0; i < NUM_CLASSES; i++)
    spell_info[spl].min_level[i] = LVL_IMMORT;
  spell_info[spl].mana_max = max_mana;
  spell_info[spl].mana_min = min_mana;
  spell_info[spl].mana_change = mana_change;
  spell_info[spl].min_position = minpos;
  spell_info[spl].targets = targets;
  spell_info[spl].violent = violent;
  spell_info[spl].routines = routines;
  spell_info[spl].name = name;
  spell_info[spl].wear_off_msg = wearoff;
}

void unused_spell(int spl) {
  int i;

  for (i = 0; i < NUM_CLASSES; i++)
    spell_info[spl].min_level[i] = LVL_IMPL + 1;
  spell_info[spl].mana_max = 0;
  spell_info[spl].mana_min = 0;
  spell_info[spl].mana_change = 0;
  spell_info[spl].min_position = 0;
  spell_info[spl].targets = 0;
  spell_info[spl].violent = 0;
  spell_info[spl].routines = 0;
  spell_info[spl].name = unused_spellname;
}

#define skillo(skill, name) spello(skill, name, 0, 0, 0, 0, 0, 0, 0, NULL);
/* Arguments for spello calls:
 * spellnum, maxmana, minmana, manachng, minpos, targets, violent?, routines.
 * spellnum:  Number of the spell.  Usually the symbolic name as defined in
 *  spells.h (such as SPELL_HEAL).
 * spellname: The name of the spell.
 * maxmana :  The maximum mana this spell will take (i.e., the mana it
 *  will take when the player first gets the spell).
 * minmana :  The minimum mana this spell will take, no matter how high
 *  level the caster is.
 * manachng:  The change in mana for the spell from level to level.  This
 *  number should be positive, but represents the reduction in mana cost as
 *  the caster's level increases.
 * minpos  :  Minimum position the caster must be in for the spell to work
 *  (usually fighting or standing). targets :  A "list" of the valid targets
 *  for the spell, joined with bitwise OR ('|').
 * violent :  TRUE or FALSE, depending on if this is considered a violent
 *  spell and should not be cast in PEACEFUL rooms or on yourself.  Should be
 *  set on any spell that inflicts damage, is considered aggressive (i.e.
 *  charm, curse), or is otherwise nasty.
 * routines:  A list of magic routines which are associated with this spell
 *  if the spell uses spell templates.  Also joined with bitwise OR ('|').
 * See the documentation for a more detailed description of these fields. You
 * only need a spello() call to define a new spell; to decide who gets to use
 * a spell or skill, look in class.c.  -JE */
void mag_assign_spells(void) {
  int i;

  /* Do not change the loop below. */
  for (i = 0; i <= TOP_SPELL_DEFINE; i++)
    unused_spell(i);
  /* Do not change the loop above. */

  spello(SPELL_ANIMATE_DEAD, "animate dead", 35, 10, 3, POS_STANDING,
  TAR_OBJ_ROOM, FALSE, MAG_SUMMONS, NULL);

  spello(SPELL_ARMOR, "armor", 30, 15, 3, POS_FIGHTING,
  TAR_CHAR_ROOM, FALSE, MAG_AFFECTS, "You feel less protected.");

  spello(SPELL_BLESS, "bless", 35, 5, 3, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV, FALSE, MAG_AFFECTS | MAG_ALTER_OBJS,
      "You feel less righteous.");

  spello(SPELL_BLINDNESS, "blindness", 35, 25, 1, POS_STANDING,
  TAR_CHAR_ROOM | TAR_NOT_SELF, FALSE, MAG_AFFECTS,
      "You feel a cloak of blindness dissolve.");

  spello(SPELL_BURNING_HANDS, "burning hands", 30, 10, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_CALL_LIGHTNING, "call lightning", 40, 25, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_CHARM, "charm person", 75, 50, 2, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_NOT_SELF, TRUE, MAG_MANUAL,
      "You feel more self-confident.");

  spello(SPELL_CHILL_TOUCH, "chill touch", 30, 10, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE | MAG_AFFECTS,
      "You feel your strength return.");

  spello(SPELL_CLONE, "clone", 80, 65, 5, POS_STANDING,
  TAR_IGNORE, FALSE, MAG_SUMMONS, NULL);

  spello(SPELL_COLOR_SPRAY, "color spray", 30, 15, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_CONTROL_WEATHER, "control weather", 75, 25, 5, POS_STANDING,
  TAR_IGNORE, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_CREATE_FOOD, "create food", 30, 5, 4, POS_STANDING,
  TAR_IGNORE, FALSE, MAG_CREATIONS, NULL);

  spello(SPELL_CREATE_WATER, "create water", 30, 5, 4, POS_STANDING,
  TAR_OBJ_INV | TAR_OBJ_EQUIP, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_CURE_BLIND, "cure blind", 30, 5, 2, POS_STANDING,
  TAR_CHAR_ROOM, FALSE, MAG_UNAFFECTS, NULL);

  spello(SPELL_CURE_CRITIC, "cure critic", 30, 10, 2, POS_FIGHTING,
  TAR_CHAR_ROOM, FALSE, MAG_POINTS, NULL);

  spello(SPELL_CURE_LIGHT, "cure light", 30, 10, 2, POS_FIGHTING,
  TAR_CHAR_ROOM, FALSE, MAG_POINTS, NULL);

  spello(SPELL_CURSE, "curse", 80, 50, 2, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV, TRUE, MAG_AFFECTS | MAG_ALTER_OBJS,
      "You feel more optimistic.");

  spello(SPELL_DARKNESS, "darkness", 30, 5, 4, POS_STANDING,
  TAR_IGNORE, FALSE, MAG_ROOMS, NULL);

  spello(SPELL_DETECT_ALIGN, "detect alignment", 20, 10, 2, POS_STANDING,
  TAR_CHAR_ROOM | TAR_SELF_ONLY, FALSE, MAG_AFFECTS, "You feel less aware.");

  spello(SPELL_DETECT_INVIS, "detect invisibility", 20, 10, 2, POS_STANDING,
  TAR_CHAR_ROOM | TAR_SELF_ONLY, FALSE, MAG_AFFECTS,
      "Your eyes stop tingling.");

  spello(SPELL_DETECT_MAGIC, "detect magic", 20, 10, 2, POS_STANDING,
  TAR_CHAR_ROOM | TAR_SELF_ONLY, FALSE, MAG_AFFECTS,
      "The detect magic wears off.");

  spello(SPELL_DETECT_POISON, "detect poison", 15, 5, 1, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV | TAR_OBJ_ROOM, FALSE, MAG_MANUAL,
      "The detect poison wears off.");

  spello(SPELL_DISPEL_EVIL, "dispel evil", 40, 25, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_DISPEL_GOOD, "dispel good", 40, 25, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_EARTHQUAKE, "earthquake", 40, 25, 3, POS_FIGHTING,
  TAR_IGNORE, TRUE, MAG_AREAS, NULL);

  spello(SPELL_ENCHANT_WEAPON, "enchant weapon", 150, 100, 10, POS_STANDING,
  TAR_OBJ_INV, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_ENERGY_DRAIN, "energy drain", 40, 25, 1, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE | MAG_MANUAL, NULL);

  spello(SPELL_GROUP_ARMOR, "group armor", 50, 30, 2, POS_STANDING,
  TAR_IGNORE, FALSE, MAG_GROUPS, NULL);

  spello(SPELL_FIREBALL, "fireball", 40, 30, 2, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_FLY, "fly", 40, 20, 2, POS_FIGHTING,
  TAR_CHAR_ROOM, FALSE, MAG_AFFECTS, "You drift slowly to the ground.");

  spello(SPELL_GROUP_HEAL, "group heal", 80, 60, 5, POS_STANDING,
  TAR_IGNORE, FALSE, MAG_GROUPS, NULL);

  spello(SPELL_HARM, "harm", 75, 45, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_HEAL, "heal", 60, 40, 3, POS_FIGHTING,
  TAR_CHAR_ROOM, FALSE, MAG_POINTS | MAG_UNAFFECTS, NULL);

  spello(SPELL_INFRAVISION, "infravision", 25, 10, 1, POS_STANDING,
  TAR_CHAR_ROOM | TAR_SELF_ONLY, FALSE, MAG_AFFECTS,
      "Your night vision seems to fade.");

  spello(SPELL_INVISIBLE, "invisibility", 35, 25, 1, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV | TAR_OBJ_ROOM, FALSE,
      MAG_AFFECTS | MAG_ALTER_OBJS, "You feel yourself exposed.");

  spello(SPELL_LIGHTNING_BOLT, "lightning bolt", 30, 15, 1, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_LOCATE_OBJECT, "locate object", 25, 20, 1, POS_STANDING,
  TAR_OBJ_WORLD, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_MAGIC_MISSILE, "magic missile", 25, 10, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_POISON, "poison", 50, 20, 3, POS_STANDING,
  TAR_CHAR_ROOM | TAR_NOT_SELF | TAR_OBJ_INV, TRUE,
  MAG_AFFECTS | MAG_ALTER_OBJS, "You feel less sick.");

  spello(SPELL_PROT_FROM_EVIL, "protection from evil", 40, 10, 3, POS_STANDING,
  TAR_CHAR_ROOM | TAR_SELF_ONLY, FALSE, MAG_AFFECTS,
      "You feel less protected.");

  spello(SPELL_REMOVE_CURSE, "remove curse", 45, 25, 5, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV | TAR_OBJ_EQUIP, FALSE,
  MAG_UNAFFECTS | MAG_ALTER_OBJS, NULL);

  spello(SPELL_REMOVE_POISON, "remove poison", 40, 8, 4, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV | TAR_OBJ_ROOM, FALSE,
      MAG_UNAFFECTS | MAG_ALTER_OBJS, NULL);

  spello(SPELL_SANCTUARY, "sanctuary", 110, 85, 5, POS_STANDING,
  TAR_CHAR_ROOM, FALSE, MAG_AFFECTS, "The white aura around your body fades.");

  spello(SPELL_SENSE_LIFE, "sense life", 20, 10, 2, POS_STANDING,
  TAR_CHAR_ROOM | TAR_SELF_ONLY, FALSE, MAG_AFFECTS,
      "You feel less aware of your surroundings.");

  spello(SPELL_SHOCKING_GRASP, "shocking grasp", 30, 15, 3, POS_FIGHTING,
  TAR_CHAR_ROOM | TAR_FIGHT_VICT, TRUE, MAG_DAMAGE, NULL);

  spello(SPELL_SLEEP, "sleep", 40, 25, 5, POS_STANDING,
  TAR_CHAR_ROOM, TRUE, MAG_AFFECTS, "You feel less tired.");

  spello(SPELL_STRENGTH, "strength", 35, 30, 1, POS_STANDING,
  TAR_CHAR_ROOM, FALSE, MAG_AFFECTS, "You feel weaker.");

  spello(SPELL_SUMMON, "summon", 75, 50, 3, POS_STANDING,
  TAR_CHAR_WORLD | TAR_NOT_SELF, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_TELEPORT, "teleport", 75, 50, 3, POS_STANDING,
  TAR_CHAR_ROOM, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_WATERWALK, "waterwalk", 40, 20, 2, POS_STANDING,
  TAR_CHAR_ROOM, FALSE, MAG_AFFECTS, "Your feet seem less buoyant.");

  spello(SPELL_WORD_OF_RECALL, "word of recall", 20, 10, 2, POS_FIGHTING,
  TAR_CHAR_ROOM, FALSE, MAG_MANUAL, NULL);

  spello(SPELL_IDENTIFY, "identify", 50, 25, 5, POS_STANDING,
  TAR_CHAR_ROOM | TAR_OBJ_INV | TAR_OBJ_ROOM, FALSE, MAG_MANUAL, NULL);

  /* NON-castable spells should appear below here. */

  /* you might want to name this one something more fitting to your theme -Welcor*/
  spello(SPELL_DG_AFFECT, "Script-inflicted", 0, 0, 0, POS_SITTING,
  TAR_IGNORE, TRUE, 0, NULL);

  /* Declaration of skills - this actually doesn't do anything except set it up
   * so that immortals can use these skills by default.  The min level to use
   * the skill for other classes is set up in class.c. */
  skillo(SKILL_BACKSTAB, "backstab");
  skillo(SKILL_BASH, "bash");
  skillo(SKILL_HIDE, "hide");
  skillo(SKILL_KICK, "kick");
  skillo(SKILL_PICK_LOCK, "pick lock");
  skillo(SKILL_RESCUE, "rescue");
  skillo(SKILL_SNEAK, "sneak");
  skillo(SKILL_STEAL, "steal");
  skillo(SKILL_TRACK, "track");
  skillo(SKILL_WHIRLWIND, "whirlwind");
  skillo(SKILL_BANDAGE, "bandage");
}

