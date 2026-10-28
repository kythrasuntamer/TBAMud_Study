/**
 * @file dg_cmd.h
 * Helpers shared by the DG script command files (dg_mobcmd.c, dg_objcmd.c,
 * dg_wldcmd.c) and the trigger handlers (dg_triggers.c).
 *
 * Callback safety: a command that runs a callback for every character in a
 * room ("wforce all", "mteleport all", ...) must not follow next_in_room
 * saved from before the callback. The callback (a forced command, an enter
 * trigger) can move or extract that next character, and then the loop walks
 * another room's list or a character being extracted. Snapshot the
 * occupants' UIDs first, then re-acquire each one with
 * dg_room_occupant_live() before acting on it. See
 * docs/maintainer-safety-guide.md, section 2.
 */
#ifndef TBAMUD_DG_CMD_H_INCLUDED
#define TBAMUD_DG_CMD_H_INCLUDED

/** One room occupant, by script UID. A list of these is a snapshot of who
 * was in a room; it holds no pointers to characters. */
struct dg_memory_target {
  long uid;
  struct dg_memory_target *next;
};

/** Snapshot the UIDs of everyone in room (a valid rnum), in room order.
 * The caller owns the list and frees it with dg_free_room_occupant_uids(). */
struct dg_memory_target *dg_room_occupant_uids(room_rnum room);

/** Free a list from dg_room_occupant_uids(). NULL is allowed. */
void dg_free_room_occupant_uids(struct dg_memory_target *head);

/** The character with this UID if it still exists, is not being extracted
 * and is still in room; otherwise NULL. Never dereferences a stale
 * pointer: the UID is resolved through the lookup table first. */
char_data *dg_room_occupant_live(long uid, room_rnum room);

/** Report (as an "all-target-moved" warning naming cmd, the room vnum and
 * the character if it still exists) that a snapshotted occupant was skipped
 * because an earlier callback in the same "all" command moved or extracted
 * it. */
void dg_report_occupant_skipped(long uid, room_rnum room, const char *cmd);

/* Numeric script arguments.
 *
 * Script text is written by builders and often carries player input through
 * variables, so a numeric argument can be empty, non-numeric or huge.
 * tb_parse_int_or_zero() turns all of those into 0, which is a valid vnum
 * (room, mob and object 0 exist), and vnum types are unsigned short, so a
 * well-formed 70000 silently became 4464, an unrelated room or prototype.
 * These helpers accept only a whole decimal number in range and narrow it
 * after the check. */

/** Parse a vnum: a whole decimal number in [0, IDXTYPE_MAX - 1] (IDXTYPE_MAX
 * is NOWHERE/NOTHING/NOBODY). Returns FALSE, leaving *out untouched, for
 * NULL, empty, non-numeric or trailing text and out-of-range values.
 * Unlike dg_parse_vnum() (dg_scripts.h), which maps bad text to NOTHING
 * silently, callers use this through the report-on-failure macros below. */
static inline int dg_parse_vnum_strict(const char *text, int *out)
{
  long value;

  if (!tb_parse_long_checked(text, &value) || value < 0 ||
      value >= (long)IDXTYPE_MAX)
    return FALSE;
  *out = (int)value;
  return TRUE;
}

/* Parse-or-report forms. Each evaluates to TRUE on success; on failure it
 * emits a structured error at the *call site* (file, line and function are
 * the caller's) naming the command, the owner (owner_fmt and its arguments,
 * e.g. "room vnum %d", room->number) and the argument length (never the raw
 * text), and evaluates to FALSE. text is evaluated more than once: pass a
 * plain array. */
#define DG_PARSE_VNUM_OR_REPORT(text, outp, cmd, owner_fmt, ...) \
  (dg_parse_vnum_strict((text), (outp)) || \
   (TB_ERROR_MSG("dg", "parse-vnum", (cmd), \
                 owner_fmt ": argument of %zu bytes is not a vnum in [0, %d]", \
                 __VA_ARGS__, strlen(text), (int)IDXTYPE_MAX - 1), FALSE))

#define DG_PARSE_INT_OR_REPORT(text, outp, cmd, owner_fmt, ...) \
  (tb_parse_int_checked((text), (outp)) || \
   (TB_ERROR_MSG("dg", "parse-number", (cmd), \
                 owner_fmt ": argument of %zu bytes is not a whole number " \
                 "in int range", __VA_ARGS__, strlen(text)), FALSE))

#endif /* TBAMUD_DG_CMD_H_INCLUDED */
