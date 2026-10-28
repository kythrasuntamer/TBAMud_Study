/**************************************************************************
*  File: boards.c                                          Part of tbaMUD *
*  Usage: Handling of multiple bulletin boards.                           *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/* FEATURES & INSTALLATION INSTRUCTIONS
 * - Arbitrary number of boards handled by one set of generalized routines.
 *   Adding a new board is as easy as adding another entry to an array.
 * - Safe removal of messages while other messages are being written.
 *
 * TO ADD A NEW BOARD, simply follow our easy 4-step program:
 * 1 - Create a new board object in the object files.
 * 2 - Increase the NUM_OF_BOARDS constant in boards.h.
 * 3 - Add a new line to the board_info array below.  The fields are:
 * 	Board's virtual number.
 * 	Min level one must be to look at this board or read messages on it.
 * 	Min level one must be to post a message to the board.
 * 	Min level one must be to remove other people's messages from this
 * 	  board (but you can always remove your own message).
 * 	Filename of this board, in quotes.
 * 	Last field must always be 0.
 * 4 - In spec_assign.c, find the section which assigns the special procedure
 *     gen_board to the other bulletin boards, and add your new one in a
 *     similar fashion. */

/* LIFETIME AND OWNERSHIP
 * - init_boards() runs once from boot_db(), after objects are indexed and
 *   specials assigned; a board_info vnum with no object is fatal there.
 *   gen_board refuses to run before it.  A board is identified by the vnum
 *   of the object whose procedure fired (find_board), so OLC renumbering of
 *   obj_index cannot point it at another object.
 * - Message text lives in the slot pool msg_storage[INDEX_SIZE], shared by
 *   every board; msg_storage_taken[] marks reserved slots.  Each board's
 *   msg_index[bt][0 .. num_of_msgs[bt]) entries own exactly one heading
 *   (heap) and one reserved slot each.  Every entry at or past
 *   num_of_msgs[bt] owns nothing: slot_num == -1, heading == NULL.  Code
 *   that shrinks the index must reset the vacated entry, and
 *   board_clear_board() reports (never frees) an entry that breaks this.
 * - A post is counted (num_of_msgs) as soon as its writer starts.  While
 *   the editor is open msg_storage[slot] may be NULL, and the writer's
 *   d->str points at &msg_storage[slot]; board_remove_msg() refuses to
 *   remove such a message.  The editor finishes in modify.c
 *   playing_string_cleanup(), which decodes the board as
 *   d->mail_to - BOARD_MAGIC and saves the board.
 * - Who controls the input: headlines and bodies are player text (any
 *   player at a write-level-0 board); board files are written by
 *   board_save_board() but are re-read from disk and treated as untrusted
 *   (see board_load_board). */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "boards.h"
#include "interpreter.h"
#include "handler.h"
#include "improved-edit.h"
#include "modify.h"

/* Board appearance order. */
#define	NEWEST_AT_TOP	FALSE

/* Format: vnum, read lvl, write lvl, remove lvl, filename, 0 at end. Be sure 
 * to also change NUM_OF_BOARDS in board.h*/
struct board_info_type board_info[NUM_OF_BOARDS] = {
  {3099, 0, 0, LVL_GOD, LIB_ETC "board.mortal", 0},
  {3098, LVL_IMMORT, LVL_IMMORT, LVL_GRGOD, LIB_ETC "board.immortal", 0},
  {3097, LVL_IMMORT, LVL_GRGOD, LVL_IMPL, LIB_ETC "board.freeze", 0},
  {3096, 0, 0, LVL_IMMORT, LIB_ETC "board.social", 0},
  {1226, 0, 0, LVL_IMPL, LIB_ETC "board.builder", 0},
  {1227, 0, 0, LVL_IMPL, LIB_ETC "board.staff", 0},
  {1228, 0, 0, LVL_IMPL, LIB_ETC "board.advertising", 0},
};

/* local (file scope) global variables */
static char *msg_storage[INDEX_SIZE];
static int msg_storage_taken[INDEX_SIZE];
static int num_of_msgs[NUM_OF_BOARDS];
static struct board_msginfo msg_index[NUM_OF_BOARDS][MAX_BOARD_MESSAGES];
/* Reset index entry i of board bt to "owns nothing" (slot -1, heading NULL).
 * Entries at or past num_of_msgs[bt] must always be in this state.
 * Evaluates bt and i twice: pass plain variables or simple expressions. */
#define MSG_INDEX_CLEAR(bt, i) do { \
  memset(&msg_index[(bt)][(i)], 0, sizeof(struct board_msginfo)); \
  msg_index[(bt)][(i)].slot_num = -1; \
} while (0)

/* Set once init_boards() has reset the index and loaded the board files. */
static int boards_initialized = FALSE;

/* local static utility functions */
static int find_slot(void);
static int find_board(struct obj_data *board);
static void board_reset_board(int board_type);
static void board_clear_board(int board_type);

static int board_write_exact(const void *ptr, size_t size, size_t count, FILE *fl,
                             int board_type, int message_index, const char *field)
{
  size_t written;

  if (fl == NULL || (ptr == NULL && size != 0 && count != 0)) {
    TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
                 TB_ERRID_INVARIANT " board vnum %d message %d field %s: NULL stream or buffer",
                 BOARD_VNUM(board_type), message_index, field ? field : "(unknown)");
    return FALSE;
  }

  written = fwrite(ptr, size, count, fl);
  if (written != count) {
    if (ferror(fl))
      TB_ERROR_ERRNO("board", "board-save", FILENAME(board_type),
                     TB_ERRID_PERSIST_WRITE " board vnum %d message %d field %s: wrote %lu/%lu elements",
                     BOARD_VNUM(board_type), message_index,
                     field ? field : "(unknown)", (unsigned long)written,
                     (unsigned long)count);
    else
      TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
                   TB_ERRID_PERSIST_WRITE " board vnum %d message %d field %s: short write %lu/%lu elements",
                   BOARD_VNUM(board_type), message_index,
                   field ? field : "(unknown)", (unsigned long)written,
                   (unsigned long)count);
    return FALSE;
  }

  return TRUE;
}

static int find_slot(void)
{
  int i;

  for (i = 0; i < INDEX_SIZE; i++)
    if (!msg_storage_taken[i]) {
      msg_storage_taken[i] = 1;
      return (i);
    }
  return (-1);
}

/* Map the board object whose special procedure fired to its board_info
 * entry.  Matching on the vnum of that very object (not on a cached rnum, and
 * not on "the first board in the room") stays correct when OLC adds or
 * deletes objects and renumbers obj_index, and when two boards share a room.
 * Returns -1 when the object is not one of the configured boards. */
static int find_board(struct obj_data *board)
{
  obj_vnum vnum;
  int i;

  if (!board || (vnum = GET_OBJ_VNUM(board)) == NOTHING)
    return (-1);

  for (i = 0; i < NUM_OF_BOARDS; i++)
    if (BOARD_VNUM(i) == vnum)
      return (i);

  return (-1);
}

/* Reset the message index and slot pool, then load every board file.
 * Called once from boot_db() after the object prototypes are indexed.  Each
 * board_info entry whose object vnum does not exist is reported here; the
 * return value is how many are missing, and boot_db() treats any as fatal,
 * at boot, where exiting loses nothing -- rather than on the first 'look
 * board' hours into a running game, as the lazy initialisation did. */
int init_boards(void)
{
  int i, j, missing = 0;

  for (i = 0; i < INDEX_SIZE; i++) {
    msg_storage[i] = 0;
    msg_storage_taken[i] = 0;
  }

  for (i = 0; i < NUM_OF_BOARDS; i++) {
    if ((BOARD_RNUM(i) = real_object(BOARD_VNUM(i))) == NOTHING) {
      TB_ERROR_MSG("board", "board-init", FILENAME(i),
                   TB_ERRID_BOOT_FATAL " board index %d vnum %d has no object prototype",
                   i, BOARD_VNUM(i));
      missing++;
    }
    num_of_msgs[i] = 0;
    for (j = 0; j < MAX_BOARD_MESSAGES; j++)
      MSG_INDEX_CLEAR(i, j);
    board_load_board(i);
  }

  boards_initialized = TRUE;
  return (missing);
}

SPECIAL(gen_board)
{
  int board_type;
  struct obj_data *board = (struct obj_data *)me;

  /* These were originally globals for some unknown reason. */
  int ACMD_READ, ACMD_LOOK, ACMD_EXAMINE, ACMD_WRITE, ACMD_REMOVE;

  /* boot_db() initialises the boards.  Reaching here without that (specials
   * assigned but boards never booted) means msg_index still holds zeroed
   * entries that all claim slot 0: refuse to touch them. */
  if (!boards_initialized) {
    TB_ERROR_MSG("board", "board-init", "gen_board",
                 TB_ERRID_INVARIANT " board obj vnum %d used before init_boards() " DIAG_CHAR_FMT,
                 GET_OBJ_VNUM(board), DIAG_CHAR(ch));
    return (0);
  }
  if (!ch->desc)
    return (0);

  ACMD_READ = find_command("read");
  ACMD_WRITE = find_command("write");
  ACMD_REMOVE = find_command("remove");
  ACMD_LOOK = find_command("look");
  ACMD_EXAMINE = find_command("examine");

  if (cmd != ACMD_WRITE && cmd != ACMD_LOOK && cmd != ACMD_EXAMINE &&
      cmd != ACMD_READ && cmd != ACMD_REMOVE)
    return (0);

  if ((board_type = find_board(board)) == -1) {
    /* gen_board is attached to an object that board_info does not list. */
    TB_ERROR_MSG("board", "find-board", "gen_board",
                 "obj vnum %d in room %d " DIAG_CHAR_FMT " matched no board_info entry",
                 GET_OBJ_VNUM(board), GET_ROOM_VNUM(IN_ROOM(ch)), DIAG_CHAR(ch));
    return (0);
  }
  if (cmd == ACMD_WRITE)
    return (board_write_message(board_type, ch, argument, board));
  else if (cmd == ACMD_LOOK || cmd == ACMD_EXAMINE)
    return (board_show_board(board_type, ch, argument, board));
  else if (cmd == ACMD_READ)
    return (board_display_msg(board_type, ch, argument, board));
  else if (cmd == ACMD_REMOVE)
    return (board_remove_msg(board_type, ch, argument, board));
  else
    return (0);
}

/** Start a new post on board_type from ch's 'write <headline>' command.
 *
 * @param board_type Index into board_info; the caller (gen_board) found it.
 * @param ch         The writer; must have a descriptor (gen_board checks).
 * @param arg        The command tail.  It may point anywhere inside the
 *                   caller's command buffer and is only read: the headline
 *                   is copied, $$-collapsed and cut to BOARD_HEADLINE_MAX
 *                   in a local buffer.
 * @param board      The board object (unused).
 * @return Always 1 (the command was handled), including on refusal.
 *
 * On success the post is counted and ch's editor writes into
 * msg_storage[slot] with mail_to = board_type + BOARD_MAGIC; see the
 * ownership notes at the top of this file.  The slot is reserved only
 * after every check passed, so no early return leaks one. */
int board_write_message(int board_type, struct char_data *ch, char *arg, struct obj_data *board)
{
  time_t ct;
  char buf[MAX_INPUT_LENGTH], buf2[MAX_NAME_LENGTH + 3], tmstr[100];
  char headline[MAX_INPUT_LENGTH], *heading;
  size_t arg_len;
  int slot, used;

  if (GET_LEVEL(ch) < WRITE_LVL(board_type)) {
    send_to_char(ch, "You are not holy enough to write on this board.\r\n");
    return (1);
  }
  if (num_of_msgs[board_type] >= MAX_BOARD_MESSAGES) {
    send_to_char(ch, "The board is full.\r\n");
    return (1);
  }
  /* The headline is player text, and arg points into the caller's command
   * line (game_loop's comm[MAX_INPUT_LENGTH], or a force/script buffer) at
   * whatever offset the leading spaces leave it.  Work on a bounded local
   * copy: storing the 80-column cut into arg itself wrote a NUL up to 80
   * bytes past the end of the caller's buffer when the headline started
   * late in the line. */
  skip_spaces(&arg);
  arg_len = strlcpy(headline, arg, sizeof(headline));
  delete_doubledollar(headline);

  /* JE Truncate headline at 80 chars if it's longer than that. */
  if (arg_len >= sizeof(headline) || strlen(headline) > BOARD_HEADLINE_MAX) {
    TB_WARN_MSG("board", "headline-truncate", FILENAME(board_type),
                "board vnum %d " DIAG_CHAR_FMT " headline %lu bytes truncated to %d",
                BOARD_VNUM(board_type), DIAG_CHAR(ch), (unsigned long)arg_len,
                BOARD_HEADLINE_MAX);
    headline[BOARD_HEADLINE_MAX] = '\0';
  }
  arg = headline;

  /* Validate everything before reserving a slot from the pool shared by
   * every board: a slot taken and then abandoned on an early return is
   * never released, and enough of them disable all boards. */
  if (!*arg) {
    send_to_char(ch, "We must have a headline!\r\n");
    return (1);
  }
  ct = time(0);
  tb_strftime(tmstr, sizeof(tmstr), "%a %b %d %Y", localtime(&ct));

  tb_snprintf(buf2, sizeof(buf2), "(%s)", GET_NAME(ch));
  tb_snprintf(buf, sizeof(buf), "%s %-12s :: %s", tmstr, buf2, arg);
  if (!(heading = strdup(buf))) {
    TB_ERROR_ERRNO("board", "board-write", FILENAME(board_type),
                   "board vnum %d " DIAG_CHAR_FMT " could not allocate a %lu-byte heading",
                   BOARD_VNUM(board_type), DIAG_CHAR(ch), (unsigned long)strlen(buf) + 1);
    send_to_char(ch, "The board is malfunctioning - sorry.\r\n");
    return (1);
  }

  if ((slot = find_slot()) == -1) {
    for (used = 0, slot = 0; slot < INDEX_SIZE; slot++)
      used += msg_storage_taken[slot] ? 1 : 0;
    TB_ERROR_MSG("board", "slot-exhausted", FILENAME(board_type),
                 "board vnum %d " DIAG_CHAR_FMT " message slot pool exhausted (%d/%d in use)",
                 BOARD_VNUM(board_type), DIAG_CHAR(ch), used, INDEX_SIZE);
    send_to_char(ch, "The board is malfunctioning - sorry.\r\n");
    free(heading);
    return (1);
  }
  NEW_MSG_INDEX(board_type).slot_num = slot;
  NEW_MSG_INDEX(board_type).heading = heading;
  NEW_MSG_INDEX(board_type).level = GET_LEVEL(ch);

  send_to_char(ch, "Write your message.\r\n");
  send_editor_help(ch->desc);
  act("$n starts to write a message.", TRUE, ch, 0, 0, TO_ROOM);

  string_write(ch->desc, &(msg_storage[slot]),
		MAX_MESSAGE_LENGTH, board_type + BOARD_MAGIC, NULL);

  num_of_msgs[board_type]++;
  return (1);
}

int board_show_board(int board_type, struct char_data *ch, char *arg, struct obj_data *board)
{
  int i;
  char tmp[MAX_STRING_LENGTH], buf[MAX_STRING_LENGTH];

  if (!ch->desc)
    return (0);

  one_argument(arg, tmp);

  if (!*tmp || !isname(tmp, board->name))
    return (0);

  if (GET_LEVEL(ch) < READ_LVL(board_type)) {
    send_to_char(ch, "You try but fail to understand the holy words.\r\n");
    return (1);
  }
  act("$n studies the board.", TRUE, ch, 0, 0, TO_ROOM);

  if (!num_of_msgs[board_type])
    send_to_char(ch, "This is a bulletin board.  Usage: READ/REMOVE <messg #>, WRITE <header>.\r\nThe board is empty.\r\n");
  else {
    size_t len = 0;
    int nlen;

    len = snprintf(buf, sizeof(buf),
		"This is a bulletin board.  Usage: READ/REMOVE <messg #>, WRITE <header>.\r\n"
		"You will need to look at the board to save your message.\r\n"
		"There are %d messages on the board.\r\n",
		num_of_msgs[board_type]);
#if NEWEST_AT_TOP
    for (i = num_of_msgs[board_type] - 1; i >= 0; i--) {
      if (!MSG_HEADING(board_type, i))
        goto fubar;

      nlen = snprintf(buf + len, sizeof(buf) - len, "%-2d : %s\r\n", num_of_msgs[board_type] - i, MSG_HEADING(board_type, i));
      if (len + nlen >= sizeof(buf) || nlen < 0)
        break;
      len += nlen;
    }
#else
    for (i = 0; i < num_of_msgs[board_type]; i++) {
      if (!MSG_HEADING(board_type, i))
        goto fubar;

      nlen = snprintf(buf + len, sizeof(buf) - len, "%-2d : %s\r\n", i + 1, MSG_HEADING(board_type, i));
      if (len + nlen >= sizeof(buf) || nlen < 0)
        break;
      len += nlen;
    }
#endif
    page_string(ch->desc, buf, TRUE);
  }
  return (1);

fubar:
  TB_ERROR_MSG("board", "msg-index-invariant", FILENAME(board_type),
               TB_ERRID_INVARIANT " board vnum %d message %d of %d has no heading " DIAG_CHAR_FMT,
               BOARD_VNUM(board_type), i, num_of_msgs[board_type], DIAG_CHAR(ch));
  send_to_char(ch, "Sorry, the board isn't working.\r\n");
  return (1);
}

int board_display_msg(int board_type, struct char_data *ch, char *arg, struct obj_data *board)
{
  char number[MAX_INPUT_LENGTH], buffer[MAX_STRING_LENGTH];
  int msg, ind;

  one_argument(arg, number);
  if (!*number)
    return (0);
  if (isname(number, board->name))	/* so "read board" works */
    return (board_show_board(board_type, ch, arg, board));
  if (!is_number(number))	/* read 2.mail, look 2.sword */
    return (0);
  if (!(msg = tb_parse_int_or_zero(number)))
    return (0);

  if (GET_LEVEL(ch) < READ_LVL(board_type)) {
    send_to_char(ch, "You try but fail to understand the holy words.\r\n");
    return (1);
  }
  if (!num_of_msgs[board_type]) {
    send_to_char(ch, "The board is empty!\r\n");
    return (1);
  }
  if (msg < 1 || msg > num_of_msgs[board_type]) {
    send_to_char(ch, "That message exists only in your imagination.\r\n");
    return (1);
  }
#if NEWEST_AT_TOP
  ind = num_of_msgs[board_type] - msg;
#else
  ind = msg - 1;
#endif
  if (!MSG_SLOT_OK(MSG_SLOTNUM(board_type, ind))) {
    send_to_char(ch, "Sorry, the board is not working.\r\n");
    TB_ERROR_MSG("board", "msg-index-invariant", FILENAME(board_type),
                 TB_ERRID_INVARIANT " board vnum %d message %d slot %d outside 0..%d (read by " DIAG_CHAR_FMT " in room %d)",
                 BOARD_VNUM(board_type), ind, MSG_SLOTNUM(board_type, ind),
                 INDEX_SIZE - 1, DIAG_CHAR(ch), GET_ROOM_VNUM(IN_ROOM(ch)));
    return (1);
  }
  if (!(MSG_HEADING(board_type, ind))) {
    send_to_char(ch, "That message appears to be screwed up.\r\n");
    return (1);
  }
  if (!(msg_storage[MSG_SLOTNUM(board_type, ind)])) {
    send_to_char(ch, "That message seems to be empty.\r\n");
    return (1);
  }
  tb_snprintf(buffer, sizeof(buffer), "Message %d : %s\r\n\r\n%s\r\n", msg,
	  MSG_HEADING(board_type, ind),
	  msg_storage[MSG_SLOTNUM(board_type, ind)]);

  page_string(ch->desc, buffer, TRUE);

  return (1);
}

int board_remove_msg(int board_type, struct char_data *ch, char *arg, struct obj_data *board)
{
  int ind, msg, slot_num;
  char number[MAX_INPUT_LENGTH], buf[MAX_INPUT_LENGTH];
  struct descriptor_data *d;

  one_argument(arg, number);

  if (!*number || !is_number(number))
    return (0);
  if (!(msg = tb_parse_int_or_zero(number)))
    return (0);

  if (!num_of_msgs[board_type]) {
    send_to_char(ch, "The board is empty!\r\n");
    return (1);
  }
  if (msg < 1 || msg > num_of_msgs[board_type]) {
    send_to_char(ch, "That message exists only in your imagination.\r\n");
    return (1);
  }
#if NEWEST_AT_TOP
  ind = num_of_msgs[board_type] - msg;
#else
  ind = msg - 1;
#endif
  if (!MSG_HEADING(board_type, ind)) {
    send_to_char(ch, "That message appears to be screwed up.\r\n");
    return (1);
  }
  tb_snprintf(buf, sizeof(buf), "(%s)", GET_NAME(ch));
  if (GET_LEVEL(ch) < REMOVE_LVL(board_type) &&
      !(strstr(MSG_HEADING(board_type, ind), buf))) {
    send_to_char(ch, "You are not holy enough to remove other people's messages.\r\n");
    return (1);
  }
  if (GET_LEVEL(ch) < MSG_LEVEL(board_type, ind)) {
    send_to_char(ch, "You can't remove a message holier than yourself.\r\n");
    return (1);
  }
  slot_num = MSG_SLOTNUM(board_type, ind);
  if (!MSG_SLOT_OK(slot_num)) {
    send_to_char(ch, "That message is majorly screwed up.\r\n");
    TB_ERROR_MSG("board", "msg-index-invariant", FILENAME(board_type),
                 TB_ERRID_INVARIANT " board vnum %d message %d slot %d outside 0..%d (remove by " DIAG_CHAR_FMT " in room %d)",
                 BOARD_VNUM(board_type), ind, slot_num, INDEX_SIZE - 1,
                 DIAG_CHAR(ch), GET_ROOM_VNUM(IN_ROOM(ch)));
    return (1);
  }
  for (d = descriptor_list; d; d = d->next)
    if (STATE(d) == CON_PLAYING && d->str == &(msg_storage[slot_num])) {
      send_to_char(ch, "At least wait until the author is finished before removing it!\r\n");
      return (1);
    }
  if (msg_storage[slot_num])
    free(msg_storage[slot_num]);
  msg_storage[slot_num] = 0;
  msg_storage_taken[slot_num] = 0;
  if (MSG_HEADING(board_type, ind))
    free(MSG_HEADING(board_type, ind));

  for (; ind < num_of_msgs[board_type] - 1; ind++) {
    MSG_HEADING(board_type, ind) = MSG_HEADING(board_type, ind + 1);
    MSG_SLOTNUM(board_type, ind) = MSG_SLOTNUM(board_type, ind + 1);
    MSG_LEVEL(board_type, ind) = MSG_LEVEL(board_type, ind + 1);
  }
  num_of_msgs[board_type]--;
  /* The shift leaves the old last entry duplicated in the vacated position
   * (or, when the last message was removed, still holding the heading just
   * freed).  Entries at or past num_of_msgs must own nothing: reset it, or
   * board_clear_board() frees that heading and slot a second time. */
  MSG_INDEX_CLEAR(board_type, num_of_msgs[board_type]);

  send_to_char(ch, "Message removed.\r\n");
  tb_snprintf(buf, sizeof(buf), "$n just removed message %d.", msg);
  act(buf, FALSE, ch, 0, 0, TO_ROOM);
  board_save_board(board_type);

  return (1);
}

void board_save_board(int board_type)
{
  FILE *fl;
  int i;
  char *tmp1, *tmp2 = NULL;
  char tmpname[MAX_STRING_LENGTH], bakname[MAX_STRING_LENGTH];
  int name_result;

  /* modify.c decodes board_type from d->mail_to - BOARD_MAGIC. */
  if (!BOARD_TYPE_OK(board_type)) {
    TB_ERROR_MSG("board", "board-save", "board_save_board",
                 TB_ERRID_INVARIANT " board type %d outside 0..%d; nothing saved",
                 board_type, NUM_OF_BOARDS - 1);
    return;
  }
  if (!num_of_msgs[board_type]) {
    if (remove(FILENAME(board_type)) != 0 && errno != ENOENT)
      TB_ERROR_ERRNO("board", "board-save", FILENAME(board_type),
                     TB_ERRID_PERSIST_WRITE " board vnum %d is empty but its file could not be removed",
                     BOARD_VNUM(board_type));
    return;
  }

  name_result = snprintf(tmpname, sizeof(tmpname), "%s.tmp", FILENAME(board_type));
  if (name_result < 0 || (size_t)name_result >= sizeof(tmpname)) {
    TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
                 TB_ERRID_PERSIST_WRITE " board vnum %d temporary filename too long; not saved",
                 BOARD_VNUM(board_type));
    return;
  }
  name_result = snprintf(bakname, sizeof(bakname), "%s.bak", FILENAME(board_type));
  if (name_result < 0 || (size_t)name_result >= sizeof(bakname)) {
    TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
                 TB_ERRID_PERSIST_WRITE " board vnum %d backup filename too long; not saved",
                 BOARD_VNUM(board_type));
    return;
  }

  if (!(fl = fopen(tmpname, "wb"))) {
    TB_ERROR_ERRNO("board", "board-save", tmpname,
                   TB_ERRID_PERSIST_WRITE " board vnum %d could not open its temporary file; not saved",
                   BOARD_VNUM(board_type));
    return;
  }
  if (!board_write_exact(&(num_of_msgs[board_type]), sizeof(int), 1, fl,
                         board_type, -1, "message-count header"))
    goto write_fail;

  for (i = 0; i < num_of_msgs[board_type]; i++) {
    if ((tmp1 = MSG_HEADING(board_type, i)) != NULL)
      msg_index[board_type][i].heading_len = strlen(tmp1) + 1;
    else
      msg_index[board_type][i].heading_len = 0;

    if (!MSG_SLOT_OK(MSG_SLOTNUM(board_type, i)) ||
	(!(tmp2 = msg_storage[MSG_SLOTNUM(board_type, i)])))
      msg_index[board_type][i].message_len = 0;
    else
      msg_index[board_type][i].message_len = strlen(tmp2) + 1;

    /* The record keeps its historical layout (struct board_msginfo), but
     * the in-memory heading pointer and slot are meaningless on disk: write
     * them as NULL / -1 rather than leaking a heap address into the file. */
    {
      struct board_msginfo rec;

      memset(&rec, 0, sizeof(rec));
      rec.slot_num = -1;
      rec.heading = NULL;
      rec.level = MSG_LEVEL(board_type, i);
      rec.heading_len = msg_index[board_type][i].heading_len;
      rec.message_len = msg_index[board_type][i].message_len;
      if (!board_write_exact(&rec, sizeof(rec), 1, fl, board_type, i, "message metadata"))
        goto write_fail;
    }
    if (tmp1 && !board_write_exact(tmp1, sizeof(char), msg_index[board_type][i].heading_len,
                                   fl, board_type, i, "heading"))
      goto write_fail;
    if (tmp2 && !board_write_exact(tmp2, sizeof(char), msg_index[board_type][i].message_len,
                                   fl, board_type, i, "body"))
      goto write_fail;
  }

  {
    int flush_ok = tb_fflush_checked(fl, tmpname);
    int close_ok = tb_fclose_checked(fl, tmpname);
    if (!flush_ok || !close_ok) {
      TB_ERROR_MSG("board", "board-save", tmpname,
                   TB_ERRID_PERSIST_COMMIT " board vnum %d temporary file did not flush/close cleanly; not committed",
                   BOARD_VNUM(board_type));
      if (remove(tmpname) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", tmpname);
      return;
    }
  }

  /* POSIX rename replaces atomically; some platforms require a backup dance. */
  if (rename(tmpname, FILENAME(board_type)) != 0) {
    int replace_errno = errno;

    if (remove(bakname) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("remove", bakname);

    if (rename(FILENAME(board_type), bakname) != 0) {
      errno = replace_errno;
      TB_REPORT_ERRNO("rename", FILENAME(board_type));
      TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
                   TB_ERRID_PERSIST_COMMIT " board vnum %d not committed; original file intact, new data left in %s",
                   BOARD_VNUM(board_type), tmpname);
      return;
    }

    if (rename(tmpname, FILENAME(board_type)) != 0) {
      int commit_errno = errno;
      TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
                   TB_ERRID_PERSIST_ROLLBACK " board vnum %d replacement failed; rolling back from %s",
                   BOARD_VNUM(board_type), bakname);
      if (rename(bakname, FILENAME(board_type)) != 0)
        TB_REPORT_ERRNO("rename", FILENAME(board_type));
      errno = commit_errno;
      TB_REPORT_ERRNO("rename", tmpname);
      return;
    }

    if (remove(bakname) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("remove", bakname);
  }
  return;

write_fail:
  TB_ERROR_MSG("board", "board-save", FILENAME(board_type),
               TB_ERRID_PERSIST_WRITE " board vnum %d save aborted at the first failed write; previous file kept",
               BOARD_VNUM(board_type));
  (void)tb_fclose_checked(fl, tmpname);
  if (remove(tmpname) != 0 && errno != ENOENT)
    TB_REPORT_ERRNO("remove", tmpname);
}

/* Read exactly count elements for one field of a board file, reporting a
 * short read, EOF or I/O error against the caller's location with the board
 * vnum, message index and field.  Returns TRUE when the field was read. */
static int board_read_exact_at(void *ptr, size_t size, size_t count, FILE *fl,
                               int board_type, int message_index,
                               const char *field, const char *src_file,
                               int src_line, const char *src_func)
{
  size_t got = fread(ptr, size, count, fl);

  if (got == count)
    return TRUE;

  if (ferror(fl))
    tb_error_log_at("ERROR", "board", "board-load", FILENAME(board_type), errno,
                    src_file, src_line, src_func,
                    TB_ERRID_PARSER_EOF " board vnum %d message %d field %s: I/O error after %lu/%lu elements",
                    BOARD_VNUM(board_type), message_index, field,
                    (unsigned long)got, (unsigned long)count);
  else
    tb_error_log_at("ERROR", "board", "board-load", FILENAME(board_type), 0,
                    src_file, src_line, src_func,
                    TB_ERRID_PARSER_EOF " board vnum %d message %d field %s: file ends after %lu/%lu elements",
                    BOARD_VNUM(board_type), message_index, field,
                    (unsigned long)got, (unsigned long)count);
  return FALSE;
}

/** Call-site-capturing form of board_read_exact_at(). */
#define BOARD_READ_EXACT(ptr, size, count, fl, bt, i, field) \
  board_read_exact_at((ptr), (size), (count), (fl), (bt), (i), (field), \
                      __FILE__, __LINE__, __func__)

/* Load one board file into the (already reset) index for board_type.
 *
 * The file is a message count followed by, per message, a raw
 * struct board_msginfo, heading_len bytes of heading and message_len bytes
 * of body.  The record's heading pointer and slot_num were written by some
 * earlier process (and possibly another architecture): they are never used.
 * Only the lengths and level are taken from disk, and the lengths are
 * bounds-checked before anything is allocated.  num_of_msgs grows only as
 * each message is fully loaded, so a failure part-way through leaves just
 * complete, owned entries for board_reset_board() to free. */
void board_load_board(int board_type)
{
  FILE *fl;
  int i, count, len1, len2, slot;
  char *tmp1, *tmp2;
  struct board_msginfo rec;

  if (!(fl = fopen(FILENAME(board_type), "rb"))) {
    if (errno != ENOENT)
      TB_ERROR_ERRNO("board", "board-load", FILENAME(board_type),
                     "board vnum %d could not open board file for reading",
                     BOARD_VNUM(board_type));
    return;
  }
  num_of_msgs[board_type] = 0;
  if (!BOARD_READ_EXACT(&count, sizeof(int), 1, fl, board_type, -1, "message-count header")) {
    tb_fclose(fl);
    return;
  }
  if (count < 1 || count > MAX_BOARD_MESSAGES) {
    TB_ERROR_MSG("board", "board-load", FILENAME(board_type),
                 TB_ERRID_PARSER_RANGE " board vnum %d message count %d outside 1..%d; resetting",
                 BOARD_VNUM(board_type), count, MAX_BOARD_MESSAGES);
    tb_fclose(fl);
    board_reset_board(board_type);
    return;
  }
  for (i = 0; i < count; i++) {
    if (!BOARD_READ_EXACT(&rec, sizeof(rec), 1, fl, board_type, i, "message metadata"))
      goto load_fail;

    /* A heading is built in a MAX_INPUT_LENGTH buffer (board_write_message)
     * and a body is capped at MAX_MESSAGE_LENGTH by the editor, both counted
     * with their terminator.  Anything else is a corrupt or foreign file. */
    len1 = rec.heading_len;
    len2 = rec.message_len;
    if (len1 <= 0 || len1 > MAX_INPUT_LENGTH || len2 < 0 || len2 > MAX_MESSAGE_LENGTH) {
      TB_ERROR_MSG("board", "board-load", FILENAME(board_type),
                   TB_ERRID_PARSER_RANGE " board vnum %d message %d heading_len %d (1..%d) message_len %d (0..%d); resetting",
                   BOARD_VNUM(board_type), i, len1, MAX_INPUT_LENGTH,
                   len2, MAX_MESSAGE_LENGTH);
      goto load_fail;
    }

    CREATE(tmp1, char, len1);
    if (!BOARD_READ_EXACT(tmp1, sizeof(char), (size_t)len1, fl, board_type, i, "heading")) {
      free(tmp1);
      goto load_fail;
    }
    /* The writer stores strlen + 1 bytes.  Without the terminator, every
     * later %s and strstr() on the heading reads past the allocation. */
    if (tmp1[len1 - 1] != '\0') {
      TB_ERROR_MSG("board", "board-load", FILENAME(board_type),
                   TB_ERRID_PARSER_MALFORMED " board vnum %d message %d field heading not NUL-terminated (%d bytes); resetting",
                   BOARD_VNUM(board_type), i, len1);
      free(tmp1);
      goto load_fail;
    }

    if ((slot = find_slot()) == -1) {
      TB_ERROR_MSG("board", "slot-exhausted", FILENAME(board_type),
                   "board vnum %d message %d: all %d message slots in use while loading; resetting",
                   BOARD_VNUM(board_type), i, INDEX_SIZE);
      free(tmp1);
      goto load_fail;
    }

    tmp2 = NULL;
    if (len2 > 0) {
      CREATE(tmp2, char, len2);
      if (!BOARD_READ_EXACT(tmp2, sizeof(char), (size_t)len2, fl, board_type, i, "body")) {
        free(tmp2);
        free(tmp1);
        msg_storage_taken[slot] = 0;
        goto load_fail;
      }
      if (tmp2[len2 - 1] != '\0') {
        TB_ERROR_MSG("board", "board-load", FILENAME(board_type),
                     TB_ERRID_PARSER_MALFORMED " board vnum %d message %d field body not NUL-terminated (%d bytes); resetting",
                     BOARD_VNUM(board_type), i, len2);
        free(tmp2);
        free(tmp1);
        msg_storage_taken[slot] = 0;
        goto load_fail;
      }
    }

    /* Commit the fully read message. */
    MSG_INDEX_CLEAR(board_type, i);
    MSG_HEADING(board_type, i) = tmp1;
    MSG_SLOTNUM(board_type, i) = slot;
    MSG_LEVEL(board_type, i) = rec.level;
    msg_index[board_type][i].heading_len = len1;
    msg_index[board_type][i].message_len = len2;
    msg_storage[slot] = tmp2;
    num_of_msgs[board_type] = i + 1;
  }

  tb_fclose(fl);
  return;

load_fail:
  tb_fclose(fl);
  board_reset_board(board_type);
}

/* When shutting down, clear all boards. */
void board_clear_all(void)
{
  int i;

  for (i = 0; i < NUM_OF_BOARDS; i++)
    board_clear_board(i);
}

/* Clear the in-memory structures.  Only entries [0, num_of_msgs) own a
 * heading and a storage slot.  An entry past that which still names a slot
 * breaks the index invariant: it is a stale copy of an owning entry, so it
 * is reported and reset without freeing anything (freeing it would free the
 * owner's heading and body a second time). */
void board_clear_board(int board_type)
{
  int i, slot, num = num_of_msgs[board_type];

  if (num < 0 || num > MAX_BOARD_MESSAGES) {
    TB_ERROR_MSG("board", "msg-index-invariant", FILENAME(board_type),
                 TB_ERRID_INVARIANT " board vnum %d message count %d outside 0..%d; nothing freed",
                 BOARD_VNUM(board_type), num, MAX_BOARD_MESSAGES);
    num = 0;
  }

  for (i = 0; i < MAX_BOARD_MESSAGES; i++) {
    slot = MSG_SLOTNUM(board_type, i);
    if (i >= num) {
      if (slot != -1 || MSG_HEADING(board_type, i))
        TB_ERROR_MSG("board", "msg-index-invariant", FILENAME(board_type),
                     TB_ERRID_INVARIANT " board vnum %d stale entry %d (count %d) slot %d",
                     BOARD_VNUM(board_type), i, num, slot);
    } else {
      if (MSG_HEADING(board_type, i))
        free(MSG_HEADING(board_type, i));
      if (MSG_SLOT_OK(slot)) {
        if (msg_storage[slot])
          free(msg_storage[slot]);
        msg_storage[slot] = NULL;
        msg_storage_taken[slot] = 0;
      } else if (slot != -1)
        TB_ERROR_MSG("board", "msg-index-invariant", FILENAME(board_type),
                     TB_ERRID_INVARIANT " board vnum %d message %d slot %d outside 0..%d",
                     BOARD_VNUM(board_type), i, slot, INDEX_SIZE - 1);
    }
    MSG_INDEX_CLEAR(board_type, i);
  }
  num_of_msgs[board_type] = 0;
}

/* Destroy the on-disk and in-memory board. */
static void board_reset_board(int board_type)
{
  board_clear_board(board_type);
  if (remove(FILENAME(board_type)) != 0 && errno != ENOENT)
    TB_ERROR_ERRNO("board", "board-reset", FILENAME(board_type),
                   "board vnum %d was reset in memory but its file could not be removed",
                   BOARD_VNUM(board_type));
}
