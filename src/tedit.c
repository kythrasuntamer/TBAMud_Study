/**************************************************************************
*  File: tedit.c                                           Part of tbaMUD *
*  Usage: Oasis OLC - Text files.                                         *
*                                                                         *
* By Michael Scott [Manx].                                                *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "comm.h"
#include "db.h"
#include "genolc.h"
#include "oasis.h"
#include "improved-edit.h"
#include "modify.h"

/** @file tedit.c
 * Editor for the whole-file texts (news, motd, greetings, help pages...).
 *
 * The live globals in tedit_files[] are read by the rest of the server
 * while an edit is open (GREETINGS on every new connection, motd at
 * login), so the editor never writes them in place: do_tedit gives the
 * string editor a private copy in OLC_STORAGE, and tedit_string_cleanup
 * writes the file through a verified temporary file and only then swaps
 * the copy into the global.  Post-condition: the global is never NULL
 * (an emptied text is published as "").  The text is builder (GRGOD/IMPL)
 * free text, so it must only ever be printed with "%s".
 *
 * Concurrency.  One editor per file: do_tedit refuses a file another
 * descriptor has open in tedit (each editor edits its own copy and a save
 * replaces the whole file and global, so the last save would silently
 * discard the other's work). */

extern time_t motdmod;
extern time_t newsmod;

/* The text files tedit can edit.  buffer is the live global that the rest
 * of the server reads (GREETINGS on every new connection, motd at login,
 * and so on); it must never be NULL and is only replaced, whole, on save. */
static const struct tedit_file {
  const char *cmd;
  char level;
  char **buffer;
  int  size;
  const char *filename;
} tedit_files[] = {
	/* edit the lvls to your own needs */
	{ "credits",	LVL_IMPL,	&credits,	2400,	CREDITS_FILE},
	{ "news",	LVL_GRGOD,	&news,		8192,	NEWS_FILE},
	{ "motd",	LVL_GRGOD,	&motd,		2400,	MOTD_FILE},
	{ "imotd",	LVL_IMPL,	&imotd,		2400,	IMOTD_FILE},
        { "greetings",  LVL_IMPL,       &GREETINGS,     2400,   GREETINGS_FILE},
        { "help",       LVL_GRGOD,      &help,          2400,   HELP_PAGE_FILE},
	{ "ihelp",      LVL_GRGOD,	&ihelp, 	2400,	IHELP_PAGE_FILE},
	{ "info",	LVL_GRGOD,	&info,		8192,	INFO_FILE},
	{ "background",	LVL_IMPL,	&background,	8192,	BACKGROUND_FILE},
	{ "handbook",   LVL_IMPL,	&handbook,	8192,   HANDBOOK_FILE},
	{ "policies",	LVL_IMPL,	&policies,	8192,	POLICIES_FILE},
        { "wizlist",    LVL_IMPL,       &wizlist,       2400,   WIZLIST_FILE},
        { "immlist",    LVL_GRGOD,      &immlist,       2400,   IMMLIST_FILE},
	{ "\n",		0,		NULL,		0,	NULL }
};

#define NUM_TEDIT_FILES ((int)(sizeof(tedit_files) / sizeof(tedit_files[0])) - 1)

/* While editing, OLC_NUM is the tedit_files[] row and OLC_STORAGE is the
 * builder's private copy of the text (d->str points at it).  The live
 * global is left alone until a save publishes the copy, so '/c' or a
 * disconnect can no longer leave GREETINGS NULL or freed under every other
 * connection, and two editors of one file no longer share one buffer. */
/* Replace filename with text through filename.tmp.  Returns FALSE, after the
 * sysdep helpers have reported the failing call, if anything failed; the
 * original file is then untouched. */
static int tedit_write_file(const char *filename, const char *text)
{
  FILE *fl;
  char temp_name[MAX_STRING_LENGTH];
  int name_result, flush_ok, close_ok;

  name_result = snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename);
  if (name_result < 0 || (size_t)name_result >= sizeof(temp_name)) {
    errno = ENAMETOOLONG;
    return FALSE;
  }
  if (!(fl = fopen(temp_name, "w")))
    return FALSE;
  tb_fputs(text, fl);
  flush_ok = tb_fflush_checked(fl, temp_name);
  close_ok = tb_fclose_checked(fl, temp_name);
  if (!flush_ok || !close_ok) {
    int saved_errno = errno;
    if (remove(temp_name) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("remove", temp_name);
    errno = saved_errno;
    return FALSE;
  }
  return tb_replace_file_checked(temp_name, filename);
}

void tedit_string_cleanup(struct descriptor_data *d, int terminator)
{
  int row = (int)OLC_NUM(d);
  const struct tedit_file *file;
  char *text;

  if (row < 0 || row >= NUM_TEDIT_FILES) {
    TB_ERROR_MSG("olc", "tedit-row-range", "tedit_files",
                 "row %d outside 0..%d editor " DIAG_CHAR_FMT,
                 row, NUM_TEDIT_FILES - 1, DIAG_CHAR(d->character));
    write_to_output(d, "Edit aborted: the file being edited is unknown.\r\n");
    cleanup_olc(d, CLEANUP_ALL);
    STATE(d) = CON_PLAYING;
    return;
  }
  file = &tedit_files[row];

  switch (terminator) {
  case STRINGADD_SAVE:
    /* '/c' then '/s' leaves the copy NULL.  Publish "" rather than NULL:
     * comm.c runs strlen(GREETINGS) on each new connection. */
    text = OLC_STORAGE(d);
    OLC_STORAGE(d) = NULL;
    if (!text || !*text) {
      TB_WARN_MSG("olc", "tedit-empty", file->filename,
                  "saved empty text editor " DIAG_CHAR_FMT, DIAG_CHAR(d->character));
      free(text);
      text = strdup("");
    }
    strip_cr(text);

    /* Write a temporary file, verify flush and close, then rename it over
     * the original, so a full disk or I/O error leaves the old file intact
     * instead of truncated.  Publish to the live global only after the
     * file committed, so memory and disk agree. */
    if (!tedit_write_file(file->filename, text)) {
      TB_ERROR_ERRNO("olc", "tedit-save", file->filename,
                     "text length %zu editor " DIAG_CHAR_FMT,
                     strlen(text), DIAG_CHAR(d->character));
      mudlog(CMP, LVL_IMPL, TRUE, "SYSERR: Can't write file '%s'.", file->filename);
      write_to_output(d, "Save FAILED; the file and the in-game text are unchanged.\r\n");
      free(text);
      break;
    }
    free(*file->buffer);
    *file->buffer = text;
    mudlog(CMP, MAX(LVL_GOD, GET_INVIS_LEV(d->character)), TRUE, "OLC: %s saves '%s'.", GET_NAME(d->character), file->filename);
    write_to_output(d, "Saved.\r\n");
    if (!strcmp(file->filename, NEWS_FILE))
      newsmod = time(0);
    if (!strcmp(file->filename, MOTD_FILE))
      motdmod = time(0);
    break;
  case STRINGADD_ABORT:
    write_to_output(d, "Edit aborted.\r\n");
    act("$n stops editing some scrolls.", TRUE, d->character, 0, 0, TO_ROOM);
    break;
  default:
    log("SYSERR: tedit_string_cleanup: Unknown terminator status.");
    break;
  }

  /* Common cleanup code. */
  cleanup_olc(d, CLEANUP_ALL);
  STATE(d) = CON_PLAYING;
}

ACMD(do_tedit)
{
  int l, i = 0;
  char field[MAX_INPUT_LENGTH];
  char *backstr = NULL;
  const struct tedit_file *fields = tedit_files;
  struct descriptor_data *d;

  /* No building as a mob or while being forced. */
  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)
    return;

  one_argument(argument, field);

  if (!*field) {
    send_to_char(ch, "Files available to be edited:\r\n");
    for (l = 0; *fields[l].cmd != '\n'; l++) {
      if (GET_LEVEL(ch) >= fields[l].level) {
	send_to_char(ch, "%-11.11s ", fields[l].cmd);
	if (!(++i % 7))
	  send_to_char(ch, "\r\n");
      }
    }
    if (i % 7)
      send_to_char(ch, "\r\n");
    if (i == 0)
      send_to_char(ch, "None.\r\n");
    return;
  }
  for (l = 0; *(fields[l].cmd) != '\n'; l++)
    if (!strncmp(field, fields[l].cmd, strlen(field)))
      break;

  if (*fields[l].cmd == '\n') {
    send_to_char(ch, "Invalid text editor option.\r\n");
    return;
  }

  if (GET_LEVEL(ch) < fields[l].level) {
    send_to_char(ch, "You are not godly enough for that!\r\n");
    return;
  }

  /* One editor per file. Each edits a private copy and a save replaces the
   * whole file and global, so a second editor's save would silently discard
   * the first one's work. An open tedit session's OLC_NUM is its
   * tedit_files[] row. */
  for (d = descriptor_list; d; d = d->next)
    if (STATE(d) == CON_TEDIT && d->olc && OLC_NUM(d) == l) {
      send_to_char(ch, "Someone is already editing that file.\r\n");
      return;
    }

  /* set up editor stats */
  clear_screen(ch->desc);
  send_editor_help(ch->desc);
  send_to_char(ch, "Edit file below:\r\n\r\n");

  /* Entry requires CON_PLAYING, where no editor is open, so a leftover
   * d->olc is an invariant break.  free() alone leaked its substructures
   * (help, action, quest, prefs, message list, storage); release them all
   * and report the stale mode. */
  if (ch->desc->olc) {
    TB_ERROR_MSG("olc", "olc-reentry", "tedit",
                 DIAG_CHAR_FMT " stale mode %d state %d",
                 DIAG_CHAR(ch), OLC_MODE(ch->desc), STATE(ch->desc));
    cleanup_olc(ch->desc, CLEANUP_ALL);
  }
  CREATE(ch->desc->olc, struct oasis_olc_data, 1);

  if (*fields[l].buffer) {
    send_to_char(ch, "%s", *fields[l].buffer);
    backstr = strdup(*fields[l].buffer);
    OLC_STORAGE(ch->desc) = strdup(*fields[l].buffer);
  }

  /* Edit a private copy; tedit_string_cleanup publishes it on save. */
  OLC_NUM(ch->desc) = l;
  string_write(ch->desc, &OLC_STORAGE(ch->desc), fields[l].size, 0, backstr);

  act("$n begins editing a scroll.", TRUE, ch, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);
  STATE(ch->desc) = CON_TEDIT;
}
