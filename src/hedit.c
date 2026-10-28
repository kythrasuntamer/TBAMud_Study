/**************************************************************************
*  File: hedit.c                                           Part of tbaMUD *
*  Usage: Oasis OLC Help Editor.                                          *
* Author: Steve Wolfe, Scott Meisenholder, Rhade                          *
*                                                                         *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "db.h"
#include "boards.h"
#include "oasis.h"
#include "genolc.h"
#include "genzon.h"
#include "handler.h"
#include "improved-edit.h"
#include "act.h"
#include "hedit.h"
#include "modify.h"

/* local functions */
static void hedit_disp_menu(struct descriptor_data *);
static void hedit_setup_new(struct descriptor_data *);
static void hedit_setup_existing(struct descriptor_data *, int);
static int hedit_save_to_disk(struct descriptor_data *);
static int hedit_save_internally(struct descriptor_data *);


ACMD(do_oasis_hedit)
{
  char arg[MAX_INPUT_LENGTH];
  struct descriptor_data *d;
  int i;

  /* No building as a mob or while being forced. */
  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)
    return;

  if (!can_edit_zone(ch, HEDIT_PERMISSION)) {
    send_to_char(ch, "You don't have access to editing help files.\r\n");
    return;
  }

  for (d = descriptor_list; d; d = d->next) {
    if (STATE(d) == CON_HEDIT) {
      send_to_char(ch, "Sorry, only one can person can edit help files at a time.\r\n");
      return;
    }
  }

  one_argument(argument, arg);

  if (!*arg) {
    send_to_char(ch, "Please specify a help entry to edit.\r\n");
    return;
  }

  d = ch->desc;

  if (!str_cmp("save", arg)) {
    mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE, "OLC: %s saves help files.",
           GET_NAME(ch));
    send_to_char(ch, "Saving help files.\r\n");
    if (!hedit_save_to_disk(d))
      send_to_char(ch, "Saving the help file FAILED; the file on disk is unchanged.\r\n");
    return;
  }

  /* Give descriptor an OLC structure. */
  /* Entry requires CON_PLAYING, where no editor is open, so a leftover
   * d->olc is an invariant break.  free() alone leaked its substructures
   * (help, action, quest, prefs, message list, storage); release them all
   * and report the stale mode. */
  if (d->olc) {
    TB_ERROR_MSG("olc", "olc-reentry", "hedit",
                 DIAG_CHAR_FMT " stale mode %d state %d",
                 DIAG_CHAR(ch), OLC_MODE(d), STATE(d));
    cleanup_olc(d, CLEANUP_ALL);
  }

  CREATE(d->olc, struct oasis_olc_data, 1);
  OLC_NUM(d) = 0;
  OLC_STORAGE(d) = strdup(arg);
  
  /* search_help() returns a row or NOWHERE on a miss.  Test the miss
   * before touching help_table: OLC_ZNUM is unsigned, so NOWHERE indexed
   * row 65535 of a table a few thousand rows long on every "hedit <new
   * topic>". */
  OLC_ZNUM(d) = search_help(OLC_STORAGE(d), LVL_IMPL);

  if (OLC_ZNUM(d) != NOWHERE && !VALID_HELP_RNUM(OLC_ZNUM(d))) {
    TB_ERROR_MSG("olc", "hedit-rnum-range", "help_table",
                 "search_help returned rnum %d top_of_helpt %d editor " DIAG_CHAR_FMT,
                 (int)OLC_ZNUM(d), top_of_helpt, DIAG_CHAR(ch));
    send_to_char(ch, "The help index is inconsistent; edit refused.\r\n");
    cleanup_olc(d, CLEANUP_ALL);
    return;
  }

  /* A duplicate row shares its owner's entry text and is not written by
   * hedit_save_to_disk, so edit the owner instead.  A duplicate with no
   * owner would be edited and then silently dropped on save: refuse it. */
  if (OLC_ZNUM(d) != NOWHERE && help_table[OLC_ZNUM(d)].duplicate) {
    int dup = OLC_ZNUM(d);

    for (i = 0; i < top_of_helpt; i++)
      if (help_table[i].duplicate == 0 && help_table[i].entry == help_table[dup].entry)
        break;
    if (i >= top_of_helpt) {
      TB_WARN_MSG("olc", "hedit-duplicate-owner", "help_table",
                  "rnum %d has no owning entry; editor " DIAG_CHAR_FMT,
                  dup, DIAG_CHAR(ch));
      send_to_char(ch, "That help keyword shares an entry that no longer exists; edit refused.\r\n");
      cleanup_olc(d, CLEANUP_ALL);
      return;
    }
    OLC_ZNUM(d) = i;
  }

  if (OLC_ZNUM(d) == NOWHERE) {
    send_to_char(ch, "Do you wish to add the '%s' help file? ", OLC_STORAGE(d));
    OLC_MODE(d) = HEDIT_CONFIRM_ADD;
  } else {
    send_to_char(ch, "Do you wish to edit the '%s' help file?", help_table[OLC_ZNUM(d)].keywords);
    OLC_MODE(d) = HEDIT_CONFIRM_EDIT;
  }

  STATE(d) = CON_HEDIT;
  act("$n starts using OLC.", TRUE, d->character, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);
  mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), 
    TRUE, "OLC: %s starts editing help files.", GET_NAME(d->character));
}

static void hedit_setup_new(struct descriptor_data *d)
{
  CREATE(OLC_HELP(d), struct help_index_element, 1);

  OLC_HELP(d)->keywords		= strdup(OLC_STORAGE(d));
  OLC_HELP(d)->entry		= strdup("KEYWORDS\r\n\r\nThis help file is unfinished.\r\n");
  OLC_HELP(d)->min_level	= 0;
  OLC_HELP(d)->duplicate	= 0;
  OLC_VAL(d) = 0;

  hedit_disp_menu(d);
}

static void hedit_setup_existing(struct descriptor_data *d, int rnum)
{
  /* rnum comes from OLC_ZNUM, which do_oasis_hedit and the CONFIRM_EDIT walk
   * set from search_help() and a scan of help_table.  Anything outside the
   * table here is an editor bug, and copying from it would read past the
   * allocation, so refuse loudly rather than open a garbage entry. */
  if (!VALID_HELP_RNUM(rnum)) {
    TB_ERROR_MSG("olc", "hedit-rnum-range", "help_table",
                 "setup rnum %d top_of_helpt %d editor " DIAG_CHAR_FMT,
                 rnum, top_of_helpt, DIAG_CHAR(d->character));
    write_to_output(d, "That help entry no longer exists; nothing to edit.\r\n");
    cleanup_olc(d, CLEANUP_ALL);
    return;
  }

  CREATE(OLC_HELP(d), struct help_index_element, 1);

  OLC_HELP(d)->keywords		= str_udup(help_table[rnum].keywords);
  OLC_HELP(d)->entry		= str_udup(help_table[rnum].entry);
  OLC_HELP(d)->duplicate	= help_table[rnum].duplicate;
  OLC_HELP(d)->min_level	= help_table[rnum].min_level;
  OLC_VAL(d) = 0;

  hedit_disp_menu(d);
}

/* Stores the edit in help_table.  Returns FALSE, having reported why, when
 * the edit cannot be stored; the caller still owns OLC_HELP and its strings
 * and must use CLEANUP_ALL.  On TRUE the table owns the strings (use
 * CLEANUP_STRUCTS) and the caller writes the file with hedit_save_to_disk. */
static int hedit_save_internally(struct descriptor_data *d)
{
  struct help_index_element *new_help_table = NULL;

  /* OLC_ZNUM is NOWHERE (append) or a live row (replace); see
   * VALID_HELP_RNUM.  A stale or walked-off index here used to be written
   * through, past the end of help_table. */
  if (OLC_ZNUM(d) != NOWHERE && !VALID_HELP_RNUM(OLC_ZNUM(d))) {
    TB_ERROR_MSG("olc", "hedit-rnum-range", "help_table",
                 "save rnum %d top_of_helpt %d editor " DIAG_CHAR_FMT,
                 (int)OLC_ZNUM(d), top_of_helpt, DIAG_CHAR(d->character));
    return FALSE;
  }

  if (OLC_ZNUM(d) == NOWHERE) {
    int i;
    CREATE(new_help_table, struct help_index_element, top_of_helpt + 2);

    for (i = 0; i < top_of_helpt; i++)
      new_help_table[i] = help_table[i];
      
    new_help_table[top_of_helpt++] = *OLC_HELP(d);
    free(help_table);
    help_table = new_help_table;
  } else {
    struct help_index_element *row = &help_table[OLC_ZNUM(d)];
    char *old_entry = row->entry;
    int i;

    /* do_oasis_hedit redirects a duplicate keyword to its owning row, so
     * the row being replaced owns its entry.  A duplicate row is skipped by
     * hedit_save_to_disk, so storing an edit there would be dropped without
     * a word: refuse it instead. */
    if (row->duplicate || OLC_HELP(d)->duplicate) {
      TB_ERROR_MSG("olc", "hedit-duplicate-save", "help_table",
                   "rnum %d keywords '%s' row duplicate %d edit duplicate %d editor " DIAG_CHAR_FMT,
                   (int)OLC_ZNUM(d), row->keywords ? row->keywords : "(null)",
                   row->duplicate, OLC_HELP(d)->duplicate, DIAG_CHAR(d->character));
      return FALSE;
    }

    /* The struct copy below replaces the row's strings; free the old ones
     * (they used to leak on every edit).  Duplicate rows alias the owner's
     * entry pointer, so move them to the new text first: if the disk write
     * below fails the table is not rebooted and they stay in use. */
    for (i = 0; i < top_of_helpt; i++)
      if (help_table[i].duplicate && help_table[i].entry == old_entry)
        help_table[i].entry = OLC_HELP(d)->entry;
    free(row->keywords);
    free(old_entry);
    *row = *OLC_HELP(d);
  }

  add_to_save_list(HEDIT_PERMISSION, SL_HLP);
  return TRUE;
}

/* Writes help_table to the help index through a verified temporary file,
 * then frees and reboots the table, so no help_table index or pointer may
 * be held across this call.  Returns FALSE, having reported the failure
 * with the editor named, when the file on disk was not replaced; the
 * in-memory table is then unchanged and stays on the save list. */
static int hedit_save_to_disk(struct descriptor_data *d)
{
  FILE *fp;
  char buf1[MAX_STRING_LENGTH], index_name[READ_SIZE], temp_name[READ_SIZE];
  int i, name_result;

  tb_snprintf(index_name, sizeof(index_name), "%s%s", HLP_PREFIX, HELP_FILE);
  name_result = snprintf(temp_name, sizeof(temp_name), "%s.tmp", index_name);
  if (name_result < 0 || (size_t)name_result >= sizeof(temp_name)) {
    log("SYSERR: help temporary filename is too long for %s", index_name);
    goto failed;
  }

  if (!(fp = fopen(temp_name, "w"))) {
    TB_REPORT_ERRNO("fopen", temp_name);
    goto failed;
  }

  for (i = 0; i < top_of_helpt; i++) {
    if (help_table[i].duplicate)
      continue;
    strncpy(buf1, help_table[i].entry ? help_table[i].entry : "Empty\r\n", sizeof(buf1) - 1);
    strip_cr(buf1);

    /* Forget making a buffer, lets just write the thing now. */
    tb_fprintf(fp, "%s#%d\n", convert_from_tabs(buf1), help_table[i].min_level);
  }
  /* Write final line and commit only a verified temporary file. */
  tb_fprintf(fp, "$~\n");
  {
    int flush_ok = tb_fflush_checked(fp, temp_name);
    int close_ok = tb_fclose_checked(fp, temp_name);
    if (!flush_ok || !close_ok) {
      if (remove(temp_name) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", temp_name);
      goto failed;
    }
  }

  if (!tb_replace_file_checked(temp_name, index_name))
    goto failed;

  remove_from_save_list(HEDIT_PERMISSION, SL_HLP);

  /* Reboot the help files only after the replacement committed. */
  free_help_table();
  index_boot(DB_BOOT_HLP);
  return TRUE;

failed:
  /* The low-level cause (errno, path) was reported above; this names the
   * editor and the table size so the failed save can be traced. */
  TB_ERROR_MSG("olc", "hedit-save", index_name,
               "help index not written; %d rows stay in memory only; editor " DIAG_CHAR_FMT,
               top_of_helpt, DIAG_CHAR(d->character));
  return FALSE;
}

/* The main menu. */
static void hedit_disp_menu(struct descriptor_data *d)
{
  get_char_colors(d->character);

  write_to_output(d,
      "%s-- Help file editor\r\n"
      "%s1%s) Entry       :\r\n%s%s"
      "%s2%s) Min Level   : %s%d\r\n"
      "%sQ%s) Quit\r\n"
      "Enter choice : ",
       nrm,
       grn, nrm, yel, OLC_HELP(d)->entry,
       grn, nrm, yel, OLC_HELP(d)->min_level,
       grn, nrm
  );
  OLC_MODE(d) = HEDIT_MAIN_MENU;
}

void hedit_parse(struct descriptor_data *d, char *arg)
{
  char buf[MAX_STRING_LENGTH];
  /* The improved editor keeps this as d->backstr and free()s it on save or
   * restores it as the entry on abort, so it must be heap or NULL.  It was
   * the literal "", which '/c' then '/s' handed to free(). */
  char *oldtext = NULL;
  int number;

  switch (OLC_MODE(d)) {
  case HEDIT_CONFIRM_SAVESTRING:
    switch (*arg) {
    case 'y':
    case 'Y':
      tb_snprintf(buf, sizeof(buf), "OLC: %s edits help for %s.", GET_NAME(d->character),
               OLC_HELP(d)->keywords);
      mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), TRUE, "%s", buf);
      if (!hedit_save_internally(d)) {
        write_to_output(d, "Save refused: the help entry being edited is no longer valid.\r\n");
        cleanup_olc(d, CLEANUP_ALL);
        break;
      }
      if (hedit_save_to_disk(d))
        write_to_output(d, "Help saved to disk.\r\n");
      else
        write_to_output(d, "Help saved to memory, but writing the help file FAILED; "
                           "the file on disk is unchanged.\r\n");

      /* Do not free strings, just the help structure. */
      cleanup_olc(d, CLEANUP_STRUCTS);
      break;
    case 'n':
    case 'N':
      /* Free everything up, including strings, etc. */
      cleanup_olc(d, CLEANUP_ALL);
      break;
    default:
      write_to_output(d, "Invalid choice!\r\nDo you wish to save your changes? : \r\n");
      break;
    }
    return;

  case HEDIT_CONFIRM_EDIT:
    switch (*arg)  {
    case 'y': case 'Y':
      hedit_setup_existing(d, OLC_ZNUM(d));
      break;
    case 'q': case 'Q': 
      cleanup_olc(d, CLEANUP_ALL);
      break;       
    case 'n': case 'N': {
      /* Offer the next row that matches, or fall through to "add".  The old
       * loop set OLC_ZNUM to top_of_helpt + 1 on the first non-matching row
       * and then incremented it, leaving top_of_helpt + 2: past the table but
       * not NOWHERE, so "add" then overwrote help_table[top + 2].  Search
       * with a local and store only a live row or NOWHERE. */
      int next;

      /* Duplicate rows are other keywords of an entry owned by another row;
       * hedit_save_internally refuses them because hedit_save_to_disk skips
       * them, so only offer owning rows here. */
      for (next = (int)OLC_ZNUM(d) + 1; next < top_of_helpt; next++)
        if (!help_table[next].duplicate &&
            is_abbrev(OLC_STORAGE(d), help_table[next].keywords))
          break;

      if (next >= top_of_helpt) {
        OLC_ZNUM(d) = NOWHERE;
        write_to_output(d, "Do you wish to add the '%s' help file? ",
            OLC_STORAGE(d));
        OLC_MODE(d) = HEDIT_CONFIRM_ADD;
      } else {
        OLC_ZNUM(d) = next;
        write_to_output(d, "Do you wish to edit the '%s' help file? ",
            help_table[OLC_ZNUM(d)].keywords);
        OLC_MODE(d) = HEDIT_CONFIRM_EDIT;
      }
      break;
    }
    default:
      write_to_output(d, "Invalid choice!\r\n"
                         "Do you wish to edit the '%s' help file? ",
                         help_table[OLC_ZNUM(d)].keywords);
      break;
    }
    return;

  case HEDIT_CONFIRM_ADD:
    switch (*arg)  {
      case 'y': case 'Y':
      hedit_setup_new(d);
      break;
    case 'n': case 'N': case 'q': case 'Q':
      cleanup_olc(d, CLEANUP_ALL);
      break;
    default:
      write_to_output(d, "Invalid choice!\r\n"
                         "Do you wish to add the '%s' help file? ",
                         OLC_STORAGE(d));
      break;
    }
    return;

  case HEDIT_MAIN_MENU:
    switch (*arg) {
    case 'q':
    case 'Q':
      if (OLC_VAL(d)) {
        /* Something has been modified. */
        write_to_output(d, "Do you wish to save your changes? : ");
        OLC_MODE(d) = HEDIT_CONFIRM_SAVESTRING;
      } else {
        write_to_output(d, "No changes made.\r\n");
        cleanup_olc(d, CLEANUP_ALL);
      }
      break;
    case '1':
      OLC_MODE(d) = HEDIT_ENTRY;
      clear_screen(d);
      send_editor_help(d);
      write_to_output(d, "Enter help entry: (/s saves /h for help)\r\n");
      if (OLC_HELP(d)->entry) {
        write_to_output(d, "%s", OLC_HELP(d)->entry);
        oldtext = strdup(OLC_HELP(d)->entry);
      }
      string_write(d, &OLC_HELP(d)->entry, MAX_MESSAGE_LENGTH, 0, oldtext);
      OLC_VAL(d) = 1;
      break;
    case '2':
      write_to_output(d, "Enter min level : ");
      OLC_MODE(d) = HEDIT_MIN_LEVEL;
      break;
    default:
      write_to_output(d, "Invalid choice!\r\n");
      hedit_disp_menu(d);
      break;
    }
    return;

  case HEDIT_KEYWORDS:
    if (OLC_HELP(d)->keywords)
      free(OLC_HELP(d)->keywords);
    if (strlen(arg) > MAX_HELP_KEYWORDS)
      arg[MAX_HELP_KEYWORDS - 1] = '\0';
    strip_cr(arg);
    OLC_HELP(d)->keywords = str_udup(arg);
    break;

  case HEDIT_ENTRY:
    /* Input in HEDIT_ENTRY goes to string_add, not here; reaching this means
     * the descriptor's mode and its string editor disagree. */
    TB_ERROR_MSG("olc", "bad-mode", "hedit",
                 "mode %d (HEDIT_ENTRY) reached hedit_parse state %d " DIAG_CHAR_FMT,
                 OLC_MODE(d), STATE(d), DIAG_CHAR(d->character));
    break;

  case HEDIT_MIN_LEVEL:
    number = tb_parse_int_or_zero(arg);
    if ((number < 0) || (number > LVL_IMPL))
      write_to_output(d, "That is not a valid choice!\r\nEnter min level:-\r\n] ");
    else {
      OLC_HELP(d)->min_level = number;
      break;
    }
    return;

  default:
    /* OLC_MODE is set only by this file; an unknown value is an editor bug. */
    TB_ERROR_MSG("olc", "bad-mode", "hedit",
                 "unknown mode %d state %d " DIAG_CHAR_FMT,
                 OLC_MODE(d), STATE(d), DIAG_CHAR(d->character));
    break;
  }

  /* If we get this far, something has been changed. */
  OLC_VAL(d) = 1;
  hedit_disp_menu(d);
}

void hedit_string_cleanup(struct descriptor_data *d, int terminator)
{
  switch (OLC_MODE(d)) {
  case HEDIT_ENTRY:
    /* '/c' leaves the entry NULL, string_add's save keeps it NULL, and an
     * abort restores a NULL backstr.  The menu prints it with %s and
     * hedit_save_to_disk writes it as the entry, whose first line must be
     * the keywords, so put back a parseable placeholder and say so. */
    if (OLC_HELP(d)->entry == NULL) {
      char placeholder[MAX_STRING_LENGTH];

      TB_WARN_MSG("olc", "hedit-empty-entry", "help_table",
                  "terminator %d keywords length %zu editor " DIAG_CHAR_FMT,
                  terminator, strlen(OLC_HELP(d)->keywords ? OLC_HELP(d)->keywords : ""),
                  DIAG_CHAR(d->character));
      tb_snprintf(placeholder, sizeof(placeholder),
                  "%s\r\n\r\nThis help file is unfinished.\r\n",
                  OLC_HELP(d)->keywords ? OLC_HELP(d)->keywords : "KEYWORDS");
      OLC_HELP(d)->entry = strdup(placeholder);
      write_to_output(d, "The entry was empty; a placeholder was put back.\r\n");
    }
    hedit_disp_menu(d);
    break;
  }
}

ACMD(do_helpcheck)
{

  char buf[MAX_STRING_LENGTH];
  int i, count = 0;
  size_t len = 0, nlen;

  for (i = 1; *(complete_cmd_info[i].command) != '\n'; i++) {
    if (complete_cmd_info[i].command_pointer != do_action && complete_cmd_info[i].minimum_level >= 0) {
      if (search_help(complete_cmd_info[i].command, LVL_IMPL) == NOWHERE) {
        nlen = snprintf(buf + len, sizeof(buf) - len, "%-20.20s%s", complete_cmd_info[i].command,
                        (++count % 3 ? "" : "\r\n"));
        if (len + nlen >= sizeof(buf))
          break;
        len += nlen;
      }
    }
  }
  if (count % 3 && len < sizeof(buf))
    tb_snprintf(buf + len, sizeof(buf) - len, "\r\n");

  if (ch->desc) {
	if (len == 0)
	 send_to_char(ch, "All commands have help entries.\r\n");
	else {
	 send_to_char(ch, "Commands without help entries:\r\n");
	 page_string(ch->desc, buf, TRUE);
	}
  }
}

/* Room held back at the end of buf so that the footer, and the line saying
 * the listing was cut, can always be written however full the listings are.
 * The footer is two lines of about forty bytes; the rest is slack. */
#define HINDEX_TAIL 256

/* NOLINTBEGIN(clang-analyzer-security.ArrayBound): len/len2 are clamped below the fixed buffer limit before every indexed write. */
ACMD(do_hindex)
{
  int len, len2, nlen, count = 0, count2 = 0, i;
  int limit = (int)MAX_STRING_LENGTH - HINDEX_TAIL;
  int cut = FALSE, cut2 = FALSE;
  int matched = 0, matched2 = 0;
  char buf[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];

  skip_spaces(&argument);

  if (!*argument) {
    send_to_char(ch, "Usage: hindex <string>\r\n");
    return;
  }

  len = snprintf(buf, sizeof(buf), "\t1Help index entries beginning with '%s':\t2\r\n", argument);
  len2 = snprintf(buf2, sizeof(buf2), "\t1Help index entries containing '%s':\t2\r\n", argument);

  /* Not reachable: argument is one command line, so at most
   * MAX_INPUT_LENGTH against a MAX_STRING_LENGTH buffer.  Bounded anyway,
   * so that everything below is safe on its own terms rather than on that
   * arithmetic staying true. */
  if (len < 0) {
    /* An output error, not a truncation: nothing was written, so start
     * from empty.  Taking the whole array as a string would hand
     * page_string() 48K of uninitialised stack. */
    len = 0;
    buf[0] = '\0';
  } else if (len >= limit) {
    len = limit - 1;
    buf[len] = '\0';
  }
  if (len2 < 0) {
    len2 = 0;
    buf2[0] = '\0';
  } else if (len2 >= limit) {
    len2 = limit - 1;
    buf2[len2] = '\0';
  }
  /* snprintf() returns the length it wanted to write, so adding it blind
   * carries len past sizeof(buf): buf + len is then off the end of the
   * array and sizeof(buf) - len underflows into a huge size_t, making the
   * next call an unbounded write.  Nothing here tested it at all, and
   * there are six more writes after the loop.  Take the length
   * separately and add it only when it fits, the way boards.c:256-259
   * does.
   *
   * A stock help table is about 2600 rows at roughly twenty bytes a row,
   * so it takes an argument matching most of them to fill 48K -- but the
   * table is what hedit exists to grow, and the two buffers are joined
   * into one at the end, which halves the room. */
  for (i = 0; i < top_of_helpt; i++) {
    if (is_abbrev(argument, help_table[i].keywords)
        && (GET_LEVEL(ch) >= help_table[i].min_level)) {
      /* count is what is on the page and drives the layout; matched is
       * what the argument found.  They were one variable, incremented
       * inside the argument list of the write that might be rejected, so
       * the layout counted rows it had not written. */
      matched++;
      if (!cut) {
        nlen = snprintf(buf + len, (size_t)(limit - len), "%-20.20s%s", help_table[i].keywords,
                     ((count + 1) % 3 ? "" : "\r\n"));
        /* Two bytes under the limit, so the row terminator written after
         * the loop always fits.  Cutting on the last cell that fits
         * exactly can leave one or two bytes, which is not enough for
         * it, and then the join fails, strrchr() finds nothing to cut
         * back to, and the footer runs on to the end of the last
         * keyword. */
        if (nlen < 0 || len + nlen >= limit - 2) {
          /* snprintf() has already written what fits and terminated it. */
          buf[len] = '\0';
          cut = TRUE;
        } else {
          len += nlen;
          count++;
        }
      }
    } else if (strstr(help_table[i].keywords, argument)
        && (GET_LEVEL(ch) >= help_table[i].min_level)) {
      matched2++;
      if (!cut2) {
        /* Same bound and margin as buf.  Nothing past limit can reach the
         * page, since the join below copies buf2 into what is left of
         * buf; and the margin keeps the row terminator affordable here
         * too, rather than leaving a partial row for the join to cut
         * back. */
        nlen = snprintf(buf2 + len2, (size_t)(limit - len2), "%-20.20s%s", help_table[i].keywords,
                     ((count2 + 1) % 3 ? "" : "\r\n"));
        if (nlen < 0 || len2 + nlen >= limit - 2) {
          buf2[len2] = '\0';
          cut2 = TRUE;
        } else {
          len2 += nlen;
          count2++;
        }
      }
    }
    /* No break.  One listing filling up used to end the shared loop and
     * stop the other collecting: they are separate buffers with separate
     * lengths, and help_table is sorted by keyword, so the abbreviation
     * matches are one contiguous block while the substring matches are
     * spread across the alphabet -- whichever came later was never
     * reached.  Ending it once both are full would stop the counting too,
     * and the footer reports what the argument matched.  The two writes
     * above are already skipped once their buffer is cut, so what is left
     * is the walk master made anyway. */
  }
  if (count % 3) {
    nlen = snprintf(buf + len, (size_t)(limit - len), "\r\n");
    if (nlen >= 0 && len + nlen < limit)
      len += nlen;
    else
      buf[len] = '\0';
  }
  if (count2 % 3) {
    nlen = snprintf(buf2 + len2, (size_t)(limit - len2), "\r\n");
    if (nlen >= 0 && len2 + nlen < limit)
      len2 += nlen;
    else
      buf2[len2] = '\0';
  }

  if (!count) {
    nlen = snprintf(buf + len, (size_t)(limit - len), "  None.\r\n");
    if (nlen >= 0 && len + nlen < limit)
      len += nlen;
    else
      buf[len] = '\0';
  }
  if (!count2)
    tb_snprintf(buf2 + len2, (size_t)(limit - len2), "  None.\r\n");

  /* Join the two listings.  Copy what fits and cut back to the last
   * complete line rather than dropping the second listing whole: it is
   * only ever too big when the table has grown, and that is exactly when
   * throwing away thousands of entries and their heading is worst.
   *
   * Terminate before searching.  Under CIRCLE_WINDOWS snprintf is
   * _snprintf (sysdep.h), which on truncation fills every byte, writes
   * no terminator and returns -1, so strrchr() would run off the end. */
  if (len >= limit)
    cut2 = TRUE;
  else {
    int room = limit - len;

    nlen = snprintf(buf + len, (size_t)room, "%s", buf2);
    if (nlen >= 0 && nlen < room)
      len += nlen;
    else {
      char *last;

      buf[len + room - 1] = '\0';
      last = strrchr(buf + len, '\n');
      len = last ? (int)(last - buf) + 1 : len;
      buf[len] = '\0';
      cut2 = TRUE;
    }
  }

  /* Written into the room held back above, so it always fits.
   *
   * The counts are of what the argument matched, which is what this
   * footer has always reported and is not always what is on the page: an
   * entry can be left out because its buffer filled, or because the join
   * above took only part of the second listing.  Without the line saying
   * so, a short listing under a large total reads as a display fault
   * rather than as a listing that would not fit. */
  tb_snprintf(buf + len, sizeof(buf) - len,
           "%s\t1Applicable Index Entries: \t3%d\r\n"
           "\t1Total Index Entries: \t3%d\tn\r\n",
           (cut || cut2) ? "\t1The listing was too long to show in full.\tn\r\n" : "",
           matched + matched2, top_of_helpt);

  page_string(ch->desc, buf, TRUE);
}
/* NOLINTEND(clang-analyzer-security.ArrayBound) */
