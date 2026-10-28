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

#endif /* TBAMUD_DG_CMD_H_INCLUDED */
