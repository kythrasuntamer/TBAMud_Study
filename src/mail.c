/**************************************************************************
*  File: mail.c                                            Part of tbaMUD *
*  Usage: Internal funcs and player spec-procs of mudmail system.         *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
*  By Jeremy Elson. Rewritten by Welcor.                                  *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "handler.h"
#include "mail.h"
#include "modify.h"

/* local (file scope) function prototypes */
static void postmaster_send_mail(struct char_data *ch, struct char_data *mailman, int cmd, char *arg);
static void postmaster_check_mail(struct char_data *ch, struct char_data *mailman, int cmd, char *arg);
static void postmaster_receive_mail(struct char_data *ch, struct char_data *mailman, int cmd, char *arg);
static int mail_recip_ok(const char *name);
static int write_mail_record(FILE *mail_file, struct mail_t *record);
static void free_mail_record(struct mail_t *record);
static int read_mail_record(FILE *mail_file, int record_no, struct mail_t **out);

/* read_mail_record() results. A NULL record alone cannot tell a clean end
 * of file from a damaged header, and callers that rewrite the mail file must
 * never treat damage as the end: that silently drops every later message. */
#define MAIL_READ_EOF      0
#define MAIL_READ_OK       1
#define MAIL_READ_CORRUPT (-1)

static int mail_recip_ok(const char *name)
{
  int player_i, ret = FALSE;

  if ((player_i = get_ptable_by_name(name)) >= 0) {
    if (!IS_SET(player_table[player_i].flags, PINDEX_DELETED))
      ret = TRUE;
  }
  return ret;
}

static void free_mail_record(struct mail_t *record)
{
	if (record->body)
		free(record->body);
  free(record);
}

/** Read the next record of MAIL_FILE.
 * The file is a sequence of records: a header line "### <to> <from> <time>"
 * (recipient and sender idnums, send time in seconds) followed by the
 * '~'-terminated message body. The file is server-written, but it can be
 * truncated or damaged on disk.
 * @param record_no 1-based position of this record, for diagnostics.
 * @param[out] out The record (caller frees with free_mail_record()) on
 * MAIL_READ_OK, otherwise NULL.
 * @return MAIL_READ_OK, MAIL_READ_EOF at a clean end of file, or
 * MAIL_READ_CORRUPT after reporting a parse-header error. The header text
 * itself is not logged: only the record number and which field failed. */
static int read_mail_record(FILE *mail_file, int record_no, struct mail_t **out)
{
  char line[READ_SIZE];
  long sender, recipient;
  time_t sent_time;
  struct mail_t *record;

  *out = NULL;
  if (!get_line(mail_file, line))
  	return MAIL_READ_EOF;

  {
    char recipient_text[32], sender_text[32], time_text[32];
    char *end;
    long parsed_time;

    if (sscanf(line, "### %31s %31s %31s", recipient_text, sender_text, time_text) != 3) {
      TB_ERROR_MSG("mail", "parse-header", MAIL_FILE,
                   "record %d header is not '### to from time'", record_no);
      return MAIL_READ_CORRUPT;
    }

    errno = 0;
    recipient = strtol(recipient_text, &end, 10);
    if (errno == ERANGE || end == recipient_text || *end != '\0') {
      TB_ERROR_MSG("mail", "parse-header", MAIL_FILE,
                   "record %d has an invalid recipient idnum", record_no);
      return MAIL_READ_CORRUPT;
    }
    errno = 0;
    sender = strtol(sender_text, &end, 10);
    if (errno == ERANGE || end == sender_text || *end != '\0') {
      TB_ERROR_MSG("mail", "parse-header", MAIL_FILE,
                   "record %d (recipient idnum %ld) has an invalid sender idnum",
                   record_no, recipient);
      return MAIL_READ_CORRUPT;
    }
    errno = 0;
    parsed_time = strtol(time_text, &end, 10);
    if (errno == ERANGE || end == time_text || *end != '\0') {
      TB_ERROR_MSG("mail", "parse-header", MAIL_FILE,
                   "record %d (recipient idnum %ld sender idnum %ld) has an invalid timestamp",
                   record_no, recipient, sender);
      return MAIL_READ_CORRUPT;
    }
    sent_time = (time_t)parsed_time;
  }

  CREATE(record, struct mail_t, 1);

  record->recipient = recipient;
  record->sender = sender;
  record->sent_time = sent_time;
  /* fread_string() would exit(1) on a body cut off by a truncated file;
   * a damaged body is corruption, handled like a damaged header. */
  if (!fread_string_checked(mail_file, MAIL_FILE, "mail body", &record->body)) {
    TB_ERROR_MSG("mail", "parse-body", MAIL_FILE,
                 "record %d (recipient idnum %ld sender idnum %ld) has an "
                 "unterminated or oversize body", record_no, recipient, sender);
    free_mail_record(record);
    return MAIL_READ_CORRUPT;
  }

  *out = record;
  return MAIL_READ_OK;
}

static int write_mail_record(FILE *mail_file, struct mail_t *record)
{
  if (mail_file == NULL || record == NULL || record->body == NULL) {
    log("SYSERR: write_mail_record called with invalid argument (stream=%p record=%p body=%p)",
        (void *)mail_file, (void *)record, record ? (void *)record->body : NULL);
    return FALSE;
  }

  if (fprintf(mail_file, "### %ld %ld %ld\n%s~\n",
              record->recipient, record->sender, (long)record->sent_time,
              record->body) < 0) {
    int saved_errno = errno;
    log("SYSERR: mail write failed for sender %ld recipient %ld: %s (errno=%d)",
        record->sender, record->recipient, strerror(saved_errno), saved_errno);
    return FALSE;
  }
  return TRUE;
}

/* int scan_file(none)
 * Returns false if mail file is corrupted or true if everything correct.
 *
 * This is called once during boot-up.  It scans through the mail file
 * and indexes all entries currently in the mail file. */
int scan_file(void)
{
  FILE *mail_file;
  int count = 0, result;
  struct mail_t *record;

  if (!(mail_file = fopen(MAIL_FILE, "r"))) {
    int saved_errno = errno;
    if (saved_errno == ENOENT) {
      log("   Mail file does not exist; creating %s.", MAIL_FILE);
      if (touch(MAIL_FILE) != 0)
        return FALSE;
      return TRUE;
    }
    log("SYSERR: Cannot open mail file %s for scan: %s (errno=%d)",
        MAIL_FILE, strerror(saved_errno), saved_errno);
    return FALSE;
  }

  while ((result = read_mail_record(mail_file, count + 1, &record)) == MAIL_READ_OK) {
    free_mail_record(record);
    count++;
  }

  tb_fclose(mail_file);
  if (result == MAIL_READ_CORRUPT) {
    /* The caller disables mail: rewriting a damaged file would drop every
     * message after the bad record. */
    TB_ERROR_MSG("mail", "scan", MAIL_FILE,
                 "mail file damaged after %d good messages; mail disabled", count);
    return FALSE;
  }
 	log("   Mail file read -- %d messages.", count);
 	return TRUE;
}

/* int has_mail(long #1)
 * #1 - id number of the person to check for mail.
 * Returns true or false.
 *
 * A simple little function which tells you if the player has mail or not. */
int has_mail(long recipient)
{
  FILE *mail_file;
  struct mail_t *record;
  int record_no = 0, result;

  if (!(mail_file = fopen(MAIL_FILE, "r"))) {
    TB_REPORT_ERRNO("fopen", MAIL_FILE);
    return FALSE;
  }

  while ((result = read_mail_record(mail_file, ++record_no, &record)) == MAIL_READ_OK) {
  	if (record->recipient == recipient) {
  		free_mail_record(record);
  		tb_fclose(mail_file);
  		return TRUE;
    }
    free_mail_record(record);
  }
  tb_fclose(mail_file);
  if (result == MAIL_READ_CORRUPT) {
    no_mail = TRUE;
    TB_ERROR_MSG("mail", "has-mail", MAIL_FILE,
                 "mail file damaged at record %d while checking recipient idnum %ld; "
                 "mail disabled", record_no, recipient);
  }
  return FALSE;
}

/* void store_mail(long #1, long #2, char * #3)
 * #1 - id number of the person to mail to.
 * #2 - id number of the person the mail is from.
 * #3 - The actual message to send.
 *
 * call store_mail to store mail.  (hard, huh? :-) )  Pass 3 arguments:
 * who the mail is to (long), who it's from (long), and a pointer to the
 * actual message text (char *). */
void store_mail(long to, long from, char *message_pointer)
{
  FILE *mail_file;
  struct mail_t *record;

  if (!(mail_file = fopen(MAIL_FILE, "a"))) {
    TB_REPORT_ERRNO("fopen", MAIL_FILE);
    log("SYSERR: store_mail failed before writing message from %ld to %ld", from, to);
    return;
  }
  CREATE(record, struct mail_t, 1);

  record->recipient = to;
  record->sender = from;
  record->sent_time = time(0);
  record->body = message_pointer;

  if (!write_mail_record(mail_file, record)) {
    log("SYSERR: store_mail failed while persisting message from %ld to %ld", from, to);
  } else if (!tb_fflush_checked(mail_file, MAIL_FILE)) {
    log("SYSERR: store_mail write for message from %ld to %ld could not be flushed", from, to);
  }
  free(record); /* don't free the body */
  if (!tb_fclose_checked(mail_file, MAIL_FILE))
    log("SYSERR: store_mail close failed after message from %ld to %ld", from, to);
}

/* Abandon a read_delete() rewrite: close both streams and remove the
 * temporary file, leaving MAIL_FILE exactly as it was. */
static void read_delete_abort(FILE *mail_file, FILE *new_file)
{
  if (mail_file)
    tb_fclose(mail_file);
  if (new_file)
    tb_fclose(new_file);
  if (remove(MAIL_FILE_TMP) != 0 && errno != ENOENT)
    TB_REPORT_ERRNO("remove", MAIL_FILE_TMP);
}

/** Remove the first message for a recipient from MAIL_FILE.
 * The file is rewritten without that message into MAIL_FILE_TMP, which then
 * replaces MAIL_FILE (tb_replace_file_checked() keeps the old file on a
 * failed commit).
 * @param recipient Player idnum whose oldest message is taken.
 * @param[out] out On success, the formatted message (malloc'd; the caller
 * owns it, normally as a note's action_description). NULL on failure.
 * @return TRUE only when the message was removed from MAIL_FILE. FALSE means
 * the message is still on disk (open, parse, write or rename failure, or no
 * message for this recipient); the cause has been reported. Callers that
 * loop on has_mail() must stop on FALSE, or they spin forever. */
int read_delete(long recipient, char **out)
{
  FILE *mail_file, *new_file;
  struct mail_t *record, *record_to_keep = NULL;
  char buf[MAX_STRING_LENGTH];
  int record_no = 0, result;

  *out = NULL;
  if (!(mail_file = fopen(MAIL_FILE, "r"))) {
    TB_ERROR_ERRNO("mail", "read-delete", MAIL_FILE,
                   "cannot open mail store for recipient idnum %ld", recipient);
    return FALSE;
  }

  if (!(new_file = fopen(MAIL_FILE_TMP, "w"))) {
    TB_ERROR_ERRNO("mail", "read-delete", MAIL_FILE_TMP,
                   "cannot create rewrite file for recipient idnum %ld", recipient);
    tb_fclose(mail_file);
    return FALSE;
  }

  while ((result = read_mail_record(mail_file, ++record_no, &record)) == MAIL_READ_OK) {
  	if (!record_to_keep && record->recipient == recipient) {
  		record_to_keep = record;
  		continue; /* don't write and free this one just yet */
    }
    if (!write_mail_record(new_file, record)) {
      TB_ERROR_MSG("mail", "read-delete", MAIL_FILE_TMP,
                   "rewrite for recipient idnum %ld failed copying record %d "
                   "(sender idnum %ld recipient idnum %ld)",
                   recipient, record_no, record->sender, record->recipient);
      free_mail_record(record);
      if (record_to_keep)
        free_mail_record(record_to_keep);
      read_delete_abort(mail_file, new_file);
      return FALSE;
    }
    free_mail_record(record);
  }

  if (result == MAIL_READ_CORRUPT) {
    /* Committing the partial rewrite would delete every message after the
     * damaged record. Keep the live file and stop the mail system. */
    no_mail = TRUE;
    TB_ERROR_MSG("mail", "parse-header", MAIL_FILE,
                 "record %d malformed; rewrite aborted for recipient idnum %ld; "
                 "mail disabled", record_no, recipient);
    if (record_to_keep)
      free_mail_record(record_to_keep);
    read_delete_abort(mail_file, new_file);
    return FALSE;
  }

  if (!record_to_keep) {
    /* has_mail() said yes but the message is gone: another rewrite raced
     * us, or the file changed underneath. Nothing to deliver. */
    TB_ERROR_MSG("mail", "read-delete", MAIL_FILE,
                 "no message for recipient idnum %ld in %d records", recipient,
                 record_no - 1);
    read_delete_abort(mail_file, new_file);
    return FALSE;
  } else {
    char timestr[25], *from, *to;

    tb_strftime(timestr, sizeof(timestr), "%c", localtime(&(record_to_keep->sent_time)));

    from = get_name_by_id(record_to_keep->sender);
    to = get_name_by_id(record_to_keep->recipient);

 		tb_snprintf(buf, sizeof(buf),
             " * * * * tbaMUD Mail System * * * *\r\n"
             "Date: %s\r\n"
             "To  : %s\r\n"
             "From: %s\r\n"
             "\r\n"
             "%s",

             timestr,
             to ? to : "Unknown",
             from ? from : "Unknown",
             record_to_keep->body ? record_to_keep->body : "No message" );

    free_mail_record(record_to_keep);
  }
  if (!tb_fclose_checked(mail_file, MAIL_FILE)) {
    TB_ERROR_ERRNO("mail", "read-delete", MAIL_FILE,
                   "close failed on source store for recipient idnum %ld", recipient);
    read_delete_abort(NULL, new_file);
    return FALSE;
  }

  {
    int flush_ok = tb_fflush_checked(new_file, MAIL_FILE_TMP);
    int close_ok = tb_fclose_checked(new_file, MAIL_FILE_TMP);
    if (!flush_ok || !close_ok) {
      TB_ERROR_ERRNO("mail", "read-delete", MAIL_FILE_TMP,
                     "rewrite for recipient idnum %ld not durably written", recipient);
      read_delete_abort(NULL, NULL);
      return FALSE;
    }
  }

  if (!tb_replace_file_checked(MAIL_FILE_TMP, MAIL_FILE)) {
    TB_ERROR_ERRNO("mail", "read-delete", MAIL_FILE,
                   "replacing the store failed for recipient idnum %ld; "
                   "live store kept", recipient);
    read_delete_abort(NULL, NULL);
    return FALSE;
  }

  *out = strdup(buf);
  return TRUE;
}

/* spec_proc for a postmaster using the above routines.  By Jeremy Elson */
SPECIAL(postmaster)
{
  if (!ch->desc || IS_NPC(ch))
    return (0);			/* so mobs don't get caught here */

  if (!(CMD_IS("mail") || CMD_IS("check") || CMD_IS("receive")))
    return (0);

  if (no_mail) {
    send_to_char(ch, "Sorry, the mail system is having technical difficulties.\r\n");
    return (0);
  }

  if (CMD_IS("mail")) {
    postmaster_send_mail(ch, (struct char_data *)me, cmd, argument);
    return (1);
  } else if (CMD_IS("check")) {
    postmaster_check_mail(ch, (struct char_data *)me, cmd, argument);
    return (1);
  } else if (CMD_IS("receive")) {
    postmaster_receive_mail(ch, (struct char_data *)me, cmd, argument);
    return (1);
  } else
    return (0);
}

static void postmaster_send_mail(struct char_data *ch, struct char_data *mailman,
			  int cmd, char *arg)
{
  long recipient;
  char buf[MAX_INPUT_LENGTH], **mailwrite;

  if (GET_LEVEL(ch) < MIN_MAIL_LEVEL) {
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'Sorry, you have to be level %d to send mail!'", MIN_MAIL_LEVEL);
    act(buf, FALSE, mailman, 0, ch, TO_VICT);
    return;
  }
  one_argument(arg, buf);

  if (!*buf) {			/* you'll get no argument from me! */
    act("$n tells you, 'You need to specify an addressee!'",
	FALSE, mailman, 0, ch, TO_VICT);
    return;
  }
  if (GET_GOLD(ch) < STAMP_PRICE && GET_LEVEL(ch) < LVL_IMMORT) {
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'A stamp costs %d coin%s.'\r\n"
	    "$n tells you, '...which I see you can't afford.'", STAMP_PRICE,
            STAMP_PRICE == 1 ? "" : "s");
    act(buf, FALSE, mailman, 0, ch, TO_VICT);
    return;
  }
  if ((recipient = get_id_by_name(buf)) < 0 || !mail_recip_ok(buf)) {
    act("$n tells you, 'No one by that name is registered here!'",
	FALSE, mailman, 0, ch, TO_VICT);
    return;
  }
  act("$n starts to write some mail.", TRUE, ch, 0, 0, TO_ROOM);

  if (GET_LEVEL(ch) < LVL_IMMORT) {
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'I'll take %d coins for the stamp.'", STAMP_PRICE);
    act(buf, FALSE, mailman, 0, ch, TO_VICT);
    decrease_gold(ch, STAMP_PRICE);
  }

  act("$n tells you, 'Write your message. (/s saves /h for help).'", FALSE, mailman, 0, ch, TO_VICT);

  SET_BIT_AR(PLR_FLAGS(ch), PLR_MAILING);	/* string_write() sets writing. */

  /* Start writing! */
  CREATE(mailwrite, char *, 1);
  string_write(ch->desc, mailwrite, MAX_MAIL_SIZE, recipient, NULL);
}

static void postmaster_check_mail(struct char_data *ch, struct char_data *mailman,
			  int cmd, char *arg)
{
  if (has_mail(GET_IDNUM(ch)))
    act("$n tells you, 'You have mail waiting.'", FALSE, mailman, 0, ch, TO_VICT);
  else
    act("$n tells you, 'Sorry, you don't have any mail waiting.'", FALSE, mailman, 0, ch, TO_VICT);
}

static void postmaster_receive_mail(struct char_data *ch, struct char_data *mailman,
			  int cmd, char *arg)
{
  char buf[256];
  struct obj_data *obj;
  int y;

  if (!has_mail(GET_IDNUM(ch))) {
    tb_snprintf(buf, sizeof(buf), "$n tells you, 'Sorry, you don't have any mail waiting.'");
    act(buf, FALSE, mailman, 0, ch, TO_VICT);
    return;
  }
  while (has_mail(GET_IDNUM(ch))) {
    char *text = NULL;

    /* Each pass must remove one message from MAIL_FILE, or has_mail()
     * stays true and this loop never ends (one new note per pass). */
    if (!read_delete(GET_IDNUM(ch), &text)) {
      no_mail = TRUE;
      TB_ERROR_MSG("mail", "read-delete", MAIL_FILE,
                   "player idnum %ld message not removed; receive aborted; "
                   "mail disabled", GET_IDNUM(ch));
      act("$n tells you, 'Sorry, the mail system is having technical difficulties.'",
          FALSE, mailman, 0, ch, TO_VICT);
      break;
    }
    obj = create_obj();
    /* A mail note has no prototype. item_number is an rnum: the old
     * hard-coded 1 made extract_obj() decrement obj_index[1].number and
     * GET_OBJ_VNUM() report (and the rent file save) some unrelated
     * object's vnum. NOTHING also makes the note unrentable, as in
     * CircleMUD. */
    obj->item_number = NOTHING;
    obj->name = strdup("mail paper letter");
    obj->short_description = strdup("a piece of mail");
    obj->description = strdup("Someone has left a piece of mail here.");

    GET_OBJ_TYPE(obj) = ITEM_NOTE;
    for(y = 0; y < TW_ARRAY_MAX; y++)
      obj->obj_flags.wear_flags[y] = 0;
    SET_BIT_AR(GET_OBJ_WEAR(obj), ITEM_WEAR_TAKE);
    GET_OBJ_WEIGHT(obj) = 1;
    GET_OBJ_COST(obj) = 30;
    GET_OBJ_RENT(obj) = 10;
    obj->action_description = text;

    if (obj->action_description == NULL)
      obj->action_description =
	strdup("Mail system error - please report.  Error #11.\r\n");

    obj_to_char(obj, ch);

    act("$n gives you a piece of mail.", FALSE, mailman, 0, ch, TO_VICT);
    act("$N gives $n a piece of mail.", FALSE, ch, 0, mailman, TO_ROOM);
  }
}

void notify_if_playing(struct char_data *from, int recipient_id) 
{ 
  struct descriptor_data *d; 

  for (d = descriptor_list; d; d = d->next) 
    if ((IS_PLAYING(d)) && (GET_IDNUM(d->character) == recipient_id) && (has_mail(GET_IDNUM(d->character)))) 
      send_to_char(d->character, "You have new mudmail from %s.\r\n", GET_NAME(from)); 
} 
