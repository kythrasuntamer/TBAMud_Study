/**************************************************************************
*  File: players.c                                         Part of tbaMUD *
*  Usage: Player loading/saving and utility routines.                     *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/**
 * @file players.c
 * ASCII player files (lib/plrfiles/<x>/<name>.plr) and the player index
 * (lib/plrfiles/index).
 *
 * Who controls the input: both files are persistent data written by this
 * server but readable as text, so they can be hand-edited, truncated by a
 * full disk, or corrupted. Every value read from them is untrusted: numbers
 * are parsed with overflow checks, values used as table indexes (class,
 * level, sex, skills) are range-checked, and a bad field is reported
 * ("pfile-*" / "index-*" operations with the player idnum) and replaced by
 * its PFDEF_* default rather than aborting the load.
 *
 * Ownership and lifetime:
 * - player_table[] (top_of_p_table + 1 entries) is owned here. Each name is
 *   malloc'd. remove_player_from_index() compacts the table, so any saved
 *   index (GET_PFILEPOS, a local player_i) at or after the removed slot
 *   becomes stale.
 * - load_char() allocates strings into a caller-provided character; the
 *   caller owns them (free_char()).
 * - Writes are staged: save_char() and save_player_index() write a
 *   temporary file and replace the live file only after every write
 *   succeeded, so a failed save keeps the previous file.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "pfdefaults.h"
#include "dg_scripts.h"
#include "comm.h"
#include "interpreter.h"
#include "genolc.h" /* for strip_cr */
#include "config.h" /* for pclean_criteria[] */
#include "dg_scripts.h" /* To enable saving of player variables to disk */
#include "quest.h"
#include "modify.h" /* for parse_at */

/* save_char() takes off every worn item and puts it back, so that base
 * stats are written. With NO_EXTRANEOUS_TRIGGERS undefined it used to fire
 * each item's remove and wear triggers on the way, running DG scripts while
 * the equipment sat in a local array and the affects were stripped. A
 * script that purged an item left a freed pointer there, which was then
 * equipped or given back (use after free, B2-21); one that made the player
 * wear something into an emptied slot could get that item's bonuses
 * written into the saved stats. save_char() therefore runs no scripts, and
 * a build that asks for those triggers stops here instead of compiling the
 * unsafe path. */
#ifndef NO_EXTRANEOUS_TRIGGERS
#error "save_char() does not fire wear/remove triggers (B2-21): keep NO_EXTRANEOUS_TRIGGERS defined in dg_scripts.h"
#endif

/* LOAD_* select which load_HMVS() field a line fills; they are not
 * file-format versions. */
#define LOAD_HIT	0
#define LOAD_MANA	1
#define LOAD_MOVE	2
#define LOAD_STRENGTH	3

/** Replace a heap string field of the character being loaded with a copy
 * of a pfile value (CircleMUD REPLACE_STR style). A tag repeated in the
 * file used to leak the earlier copy; a failed strdup() left the field
 * NULL silently. The old value is freed only once the copy exists.
 * dst is evaluated more than once: pass a plain field lvalue such as
 * GET_TITLE(ch). Only for fields load_char() owns (not shared strings). */
#define PFILE_SET_STRING(dst, src, filename, tag, idnum) do { \
  char *pfile_new_str_ = strdup(src); \
  if (pfile_new_str_ == NULL) \
    TB_ERROR_ERRNO("player", "pfile-string", (filename), \
                   "player idnum %ld tag %.4s not stored", (long) (idnum), (tag)); \
  else { \
    free(dst); \
    (dst) = pfile_new_str_; \
  } \
} while (0)

#define PT_PNAME(i) (player_table[(i)].name)
#define PT_IDNUM(i) (player_table[(i)].id)
#define PT_LEVEL(i) (player_table[(i)].level)
#define PT_FLAGS(i) (player_table[(i)].flags)
#define PT_LLAST(i) (player_table[(i)].last)
/** TRUE when pos names a slot of player_table[] (0..top_of_p_table; an
 * empty table is -1). Evaluates pos twice: pass a plain variable. */
#define VALID_PTABLE_POS(pos) ((pos) >= 0 && (pos) <= top_of_p_table)

/* local functions */
static void load_affects(FILE *fl, struct char_data *ch);
static void load_skills(FILE *fl, struct char_data *ch, const char *filename);
static void load_quests(FILE *fl, struct char_data *ch);
static void load_HMVS(struct char_data *ch, const char *line, int mode,
                      const char *filename, const char *tag);
static void pfile_unknown_tag(const char *filename, const char *tag, long idnum);
static void write_aliases_ascii(FILE *file, struct char_data *ch);
static void read_aliases_ascii(FILE *file, struct char_data *ch, int count,
                               const char *filename, int *alis_seen);

/* Parse one pfile tag's integer value. The pfile is untrusted on read
 * (hand-edited, truncated or corrupted); a malformed or out-of-range value
 * becomes 0 and is reported as "pfile-int" with the player idnum (as known
 * so far: the Id tag is normally first) and the tag. The raw value is not
 * echoed: it is file content and may be arbitrary bytes. */
static int parse_pfile_int_or_zero(const char *value, const char *filename,
                                   const char *tag, long idnum)
{
  char *end = NULL;
  long parsed;

  if (value == NULL) {
    TB_ERROR_MSG("player", "pfile-int", filename ? filename : "(unknown)",
                 "player idnum %ld tag %.4s has no integer value; using 0",
                 idnum, tag ? tag : "????");
    return 0;
  }

  errno = 0;
  parsed = strtol(value, &end, 10);
  while (end && isspace((unsigned char)*end))
    end++;

  if (end == value || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX ||
      (end && *end != '\0')) {
    TB_ERROR_MSG("player", "pfile-int", filename ? filename : "(unknown)",
                 "player idnum %ld tag %.4s malformed or out-of-range integer "
                 "(%zu bytes); using 0", idnum, tag ? tag : "????",
                 strlen(value));
    return 0;
  }

  return (int)parsed;
}

/* As parse_pfile_int_or_zero(), for long-valued tags (Id, Brth, Last). */
static long parse_pfile_long_or_zero(const char *value, const char *filename,
                                     const char *tag, long idnum)
{
  char *end = NULL;
  long parsed;

  if (value == NULL) {
    TB_ERROR_MSG("player", "pfile-int", filename ? filename : "(unknown)",
                 "player idnum %ld tag %.4s has no long value; using 0",
                 idnum, tag ? tag : "????");
    return 0;
  }

  errno = 0;
  parsed = strtol(value, &end, 10);
  while (end && isspace((unsigned char)*end))
    end++;

  if (end == value || errno == ERANGE || (end && *end != '\0')) {
    TB_ERROR_MSG("player", "pfile-int", filename ? filename : "(unknown)",
                 "player idnum %ld tag %.4s malformed or out-of-range long "
                 "(%zu bytes); using 0", idnum, tag ? tag : "????",
                 strlen(value));
    return 0;
  }

  return parsed;
}

/* Parse a pfile integer that is later used as a table index or array
 * bound (class, level, sex). The file is untrusted: an out-of-range value
 * would index class_abbrevs[], title and experience tables out of bounds,
 * so it is replaced by the PFDEF default and reported (pfile-range). */
static int pfile_int_in_range(const char *line, const char *filename,
                              const char *tag, long idnum, int lo, int hi,
                              int dflt)
{
  int value = parse_pfile_int_or_zero(line, filename, tag, idnum);

  if (value < lo || value > hi) {
    TB_ERROR_MSG("player", "pfile-range", filename,
                 "player idnum %ld tag %.4s value %d outside %d..%d; using %d",
                 idnum, tag, value, lo, hi, dflt);
    return dflt;
  }
  return value;
}

/* Parse a pfile integer whose repair is its nearest bound rather than a
 * default: a count that can only have been pushed past a limit (endgame
 * ranks and points). A value outside lo..hi is clamped and reported
 * (pfile-range) with the player idnum, tag and value; it used to be
 * clamped below silently, and not at all above. */
static int pfile_int_clamped(const char *line, const char *filename,
                             const char *tag, long idnum, int lo, int hi)
{
  int value = parse_pfile_int_or_zero(line, filename, tag, idnum);
  int fixed = value < lo ? lo : hi;

  if (value < lo || value > hi) {
    TB_ERROR_MSG("player", "pfile-range", filename,
                 "player idnum %ld tag %.4s value %d outside %d..%d; using %d",
                 idnum, tag, value, lo, hi, fixed);
    return fixed;
  }
  return value;
}

/* Endgame HP/mana upgrade ranks run 0..100: do_endgame() (act.other.c)
 * refuses a purchase once a rank reaches 100. Keep the two in step. */
#define PFILE_ENDGAME_RANK_MAX 100

/* Longest flag word accepted from an "Act :", "Aff :" or "Pref:" line.
 * sprintascii() writes at most 33 characters per 32-bit word, so anything
 * longer is corruption or tampering, not a flag set the server produced. */
#define PFILE_FLAG_WORD 128

/* Load the flag words of an Act/Aff/Pref line into flags[0..3].
 * The line is player-file data (get_line() hands back up to READ_SIZE-1
 * bytes), so each word is length-checked before it is copied; the old
 * unbounded sscanf("%s") overflowed four 128-byte stack buffers.
 * Accepted forms: four words (current), or one word (legacy files, which
 * only set flags[0]; reported as a pfile-flags-legacy warning). Anything
 * else, or a word that is too long, leaves the flags at their defaults and
 * is reported as a pfile-flags error. */
static void load_pfile_flag_words(const char *line, int *flags,
                                  const char *filename, const char *tag,
                                  long idnum)
{
  char words[4][PFILE_FLAG_WORD];
  const char *cursor = line;
  int count = 0, i;

  for (;;) {
    size_t len = 0;

    while (isspace((unsigned char)*cursor))
      cursor++;
    if (*cursor == '\0')
      break;
    while (cursor[len] && !isspace((unsigned char)cursor[len]))
      len++;
    if (count == 4) {
      count++;	/* more than four words */
      break;
    }
    if (len >= PFILE_FLAG_WORD) {
      TB_ERROR_MSG("player", "pfile-flags", filename,
                   "player idnum %ld tag %.4s word %d is %zu bytes (max %d); "
                   "flags left at defaults", idnum, tag, count + 1, len,
                   PFILE_FLAG_WORD - 1);
      return;
    }
    memcpy(words[count], cursor, len);
    words[count][len] = '\0';
    count++;
    cursor += len;
  }
  if (count == 1) {
    TB_WARN_MSG("player", "pfile-flags-legacy", filename,
                "player idnum %ld tag %.4s has the legacy one-word form; "
                "only the first flag word loaded", idnum, tag);
    flags[0] = asciiflag_conv(words[0]);
    return;
  }
  if (count != 4) {
    TB_ERROR_MSG("player", "pfile-flags", filename,
                 "player idnum %ld tag %.4s has %s%d words (expected 4); "
                 "flags left at defaults", idnum, tag, count > 4 ? "over " : "",
                 count > 4 ? 4 : count);
    return;
  }
  for (i = 0; i < 4; i++)
    flags[i] = asciiflag_conv(words[i]);
}

/** Read a '~'-terminated string, like fread_string(), without exit(1).
 * fread_string() terminates the whole server on end of file or on a string
 * of MAX_STRING_LENGTH bytes or more, so a single truncated player
 * description or mail body would take the game down. Player files and the
 * mail file are persistent data that can be truncated on disk.
 * @param fl Stream positioned at the first line of the string.
 * @param resource File name, for diagnostics.
 * @param field Which string is being read (e.g. "Desc"), for diagnostics.
 * @param[out] out On success the string (malloc'd, CR/LF line endings, NULL
 * for an empty string), owned by the caller. NULL on failure.
 * @return TRUE on success. FALSE after reporting an fread-string error: at
 * end of file the stream is at EOF; for an oversize string the rest of it
 * is consumed up to its '~' so the caller can keep parsing. */
int fread_string_checked(FILE *fl, const char *resource, const char *field,
                         char **out)
{
  /* fgets() stores up to 511 characters; a line without its newline then
   * gains "\r\n\0" after them, so tmp needs 514 bytes. */
  char buf[MAX_STRING_LENGTH], tmp[514];
  char *point;
  size_t length = 0, templength;
  int oversize = FALSE;

  *out = NULL;
  *buf = '\0';
  for (;;) {
    if (!fgets(tmp, 512, fl)) {
      TB_ERROR_MSG("db", "fread-string", resource,
                   "%s: end of file before '~' after %zu bytes", field, length);
      return FALSE;
    }
    /* A NUL byte at the start of a chunk (a corrupt or partly written file)
     * makes the chunk look empty: strchr() below returns tmp itself and the
     * scan read tmp[-1]. Drop the chunk, as fread_string() does. */
    if (*tmp == '\0') {
      TB_ERROR_MSG("db", "fread-string", resource,
                   "%s: NUL byte in string data after %zu bytes; rest of that "
                   "line dropped", field, length);
      continue;
    }
    /* Strip the line ending; a trailing '~' ends the string, otherwise the
     * line is stored with a CR/LF ending (same rules as fread_string()).
     * tmp is not empty, so point starts inside it. */
    point = strchr(tmp, '\0');
    for (point--; point > tmp && (*point == '\r' || *point == '\n'); point--)
      ;
    if (*point == '~') {
      *point = '\0';
      templength = (size_t)(point - tmp);
      if (!oversize && length + templength < sizeof(buf)) {
        strlcat(buf, tmp, sizeof(buf));
        length += templength;
      } else
        oversize = TRUE;
      break;
    }
    if (*point == '\n' || *point == '\r')
      *point = '\r';
    else
      *(++point) = '\r';
    *(++point) = '\n';
    *(++point) = '\0';
    templength = (size_t)(point - tmp);
    if (oversize || length + templength >= sizeof(buf)) {
      oversize = TRUE;	/* keep consuming up to the '~' */
      continue;
    }
    strlcat(buf, tmp, sizeof(buf));
    length += templength;
  }

  if (oversize) {
    TB_ERROR_MSG("db", "fread-string", resource,
                 "%s: string exceeds %zu bytes; discarded", field, sizeof(buf) - 1);
    return FALSE;
  }
  parse_at(buf);
  if (*buf && (*out = strdup(buf)) == NULL) {
    TB_ERROR_ERRNO("db", "fread-string", resource, "%s: strdup of %zu bytes",
                   field, length);
    return FALSE;
  }
  return TRUE;
}

/*
 * Consume one whitespace-delimited signed long from *cursor.
 * On success, update *value and advance the cursor, returning TRUE.
 * On failure, return FALSE without changing *value; leading whitespace may
 * already have been consumed from *cursor. Callers requiring rollback must
 * save the original cursor themselves. Valid, non-NULL pointers are required.
 */
static int parse_index_long(const char **cursor, long *value)
{
  char *end;
  long parsed;
  while (isspace((unsigned char)**cursor))
    ++*cursor;
  errno = 0;
  parsed = strtol(*cursor, &end, 10);
  if (end == *cursor || errno == ERANGE || (*end && !isspace((unsigned char)*end)))
    return FALSE;
  *value = parsed;
  *cursor = end;
  return TRUE;
}

/*
 * Copy a single whitespace-delimited index token into the supplied buffer.
 * Reject empty and oversized tokens rather than truncating them.
 * A successful parse NUL-terminates dest and advances *cursor.
 */
static int parse_index_word(const char **cursor, char *dest, size_t capacity)
{
  size_t len = 0;
  while (isspace((unsigned char)**cursor))
    ++*cursor;
  while ((*cursor)[len] && !isspace((unsigned char)(*cursor)[len]))
    ++len;
  if (!len || len >= capacity)
    return FALSE;
  memcpy(dest, *cursor, len);
  dest[len] = '\0';
  *cursor += len;
  return TRUE;
}

/* Index lines that build_player_index() could not parse. They are kept
 * verbatim and written back by save_player_index(), so a damaged record is
 * never erased by the next index rewrite (any level, flag or logon change
 * rewrites the index). An admin repairs them by hand; boot reports them
 * again until then. */
static char **index_kept_lines = NULL;
static int index_kept_count = 0;

static void keep_index_line(const char *line)
{
  RECREATE(index_kept_lines, char *, index_kept_count + 1);
  index_kept_lines[index_kept_count] = strdup(line);
  if (index_kept_lines[index_kept_count] == NULL) {
    TB_ERROR_ERRNO("player", "index-parse", "player-index",
                   "cannot keep malformed record; it will be lost on the next index save");
    return;
  }
  index_kept_count++;
}

/** Build the in-memory player index from LIB_PLRFILES/INDEX_FILE.
 * Each line is "<idnum> <name> <level> <ascii flags> <last logon>", and the
 * file ends with a "~" line. A missing file means a fresh game: the table
 * is empty and the first new character becomes implementor (init_char()).
 * A malformed line is reported (index-parse, by line number; the line can
 * hold a player name and is not echoed), skipped, and kept for
 * save_player_index(); later records still load. If the file holds records
 * but none of them parse, boot stops: an empty table would hand the next
 * new character implementor level. */
void build_player_index(void)
{
  int rec_count = 0, i, line_no = 0;
  FILE *plr_index;
  char index_name[40], line[256], bits[64];
  char arg2[80];

  tb_snprintf(index_name, sizeof(index_name), "%s%s", LIB_PLRFILES, INDEX_FILE);
  if (!(plr_index = fopen(index_name, "r"))) {
    top_of_p_table = -1;
    log("No player index file!  First new char will be IMP!");
    return;
  }

  while (get_line(plr_index, line))
    if (*line != '~')
      rec_count++;
  if (fseek(plr_index, 0L, SEEK_SET) != 0) {
    TB_REPORT_ERRNO("fseek", index_name);
    tb_fclose(plr_index);
    return;
  }

  if (rec_count == 0) {
    player_table = NULL;
    top_of_p_table = -1;
    tb_fclose(plr_index);
    return;
  }

  CREATE(player_table, struct player_index_element, rec_count);
  i = 0;
  while (line_no < rec_count) {
    int fields;

    if (!get_line(plr_index, line)) {
      TB_ERROR_MSG("player", "index-parse", index_name,
                   "index ended early at record %d of %d", line_no + 1, rec_count);
      break;
    }
    if (*line == '~')
      continue;
    line_no++;
    {
      const char *cursor = line;
      long level, last;
      fields = 0;
      if (parse_index_long(&cursor, &player_table[i].id) &&
          parse_index_word(&cursor, arg2, sizeof(arg2)) &&
          parse_index_long(&cursor, &level) &&
          level >= INT_MIN && level <= INT_MAX &&
          parse_index_word(&cursor, bits, sizeof(bits)) &&
          parse_index_long(&cursor, &last)) {
        while (isspace((unsigned char)*cursor))
          cursor++;
        if (*cursor == '\0') {
          player_table[i].level = (int)level;
          player_table[i].last = (time_t)last;
          fields = 5;
        }
      }
    }
    if (fields != 5) {
      TB_ERROR_MSG("player", "index-parse", index_name,
                   "record %d of %d malformed; skipped and kept for the next "
                   "index save", line_no, rec_count);
      keep_index_line(line);
      continue;
    }
    CREATE(player_table[i].name, char, strlen(arg2) + 1);
    memcpy(player_table[i].name, arg2, strlen(arg2) + 1);
    player_table[i].flags = asciiflag_conv(bits);
    top_idnum = MAX(top_idnum, player_table[i].id);
    i++;
  }

  tb_fclose(plr_index);
  top_of_p_file = top_of_p_table = i - 1;

  if (i == 0) {
    TB_FATAL_MSG("player", "index-parse", index_name,
                 "none of %d records parsed; refusing to boot with an empty "
                 "index (the next new character would become implementor)",
                 rec_count);
    abort();
  }
}


/* Create a new entry in the in-memory index table for the player file. If the
 * name already exists, by overwriting a deleted character, then we re-use the
 * old position. */
int create_entry(char *name)
{
  int i, pos;

  if (top_of_p_table == -1) {	/* no table */
    pos = top_of_p_table = 0;
    CREATE(player_table, struct player_index_element, 1);
  } else if ((pos = get_ptable_by_name(name)) == -1) {	/* new name */
    i = ++top_of_p_table + 1;

    RECREATE(player_table, struct player_index_element, i);
    pos = top_of_p_table;
  } else {
    /* Re-using a deleted character's slot: release its old name before the
     * slot is reset below, or it leaks. */
    free(player_table[pos].name);
  }

  /* RECREATE() is realloc(): the new slot is uninitialised. save_char()
   * compares level and last, and save_player_index() writes id, level and
   * last to the index file, before init_char() fills them in, so garbage
   * reached the player index on disk (found by Valgrind; ASan cannot see
   * uninitialised reads). Start every slot, new or re-used, from zero. */
  memset(&player_table[pos], 0, sizeof(player_table[pos]));

  CREATE(player_table[pos].name, char, strlen(name) + 1);

  /* copy lowercase equivalent of name to table field */
  for (i = 0; (player_table[pos].name[i] = LOWER(name[i])); i++)
    /* Nothing */;

  return (pos);
}


/** Remove an entry from the in-memory player index table.
 * @param pos A table position, as returned by get_ptable_by_name().
 * Frees the entry's name and shifts every later entry down one slot, so
 * any position held for an entry after pos (GET_PFILEPOS of an online
 * character, a caller's player_i) now names a different player; callers
 * must re-look-up by name. Does not write the index file. */
static void remove_player_from_index(int pos)
{
  int i;

  /* Internal invariant (remove_player() checks first): a bad position
   * used to return silently, leaving the caller believing it removed. */
  if (!VALID_PTABLE_POS(pos)) {
    TB_ERROR_MSG("player", "index-pos", "player_table",
                 "position %d outside 0..%d; index unchanged", pos,
                 top_of_p_table);
    return;
  }

  /* We only need to free the name string */
  free(PT_PNAME(pos));

  /* Move every other item in the list down the index */
  for (i = pos+1; i <= top_of_p_table; i++) {
    PT_PNAME(i-1) = PT_PNAME(i);
    PT_IDNUM(i-1) = PT_IDNUM(i);
    PT_LEVEL(i-1) = PT_LEVEL(i);
    PT_FLAGS(i-1) = PT_FLAGS(i);
    PT_LLAST(i-1) = PT_LLAST(i);
  }
  PT_PNAME(top_of_p_table) = NULL;

  /* Reduce the index table counter */
  top_of_p_table--;

  /* And reduce the size of the table */
  if (top_of_p_table >= 0)
    RECREATE(player_table, struct player_index_element, (top_of_p_table+1));
  else {
    free(player_table);
    player_table = NULL;
  }
}

/** Write the in-memory player index to LIB_PLRFILES/INDEX_FILE.
 * The index is the only map from player names to player files, and an
 * empty or truncated index makes the next new character implementor
 * (init_char()). It is therefore staged in a mkstemp() file beside the
 * live one, every write is checked, and the live file is replaced only
 * after a complete, flushed write (tb_replace_file_checked()). On any
 * failure the live index is kept and index-save is reported. */
void save_player_index(void)
{
  int i, failed = FALSE, temp_fd;
  char index_name[50], temp_name[64], bits[64];
  FILE *index_file;

  tb_snprintf(index_name, sizeof(index_name), "%s%s", LIB_PLRFILES, INDEX_FILE);
  tb_snprintf(temp_name, sizeof(temp_name), "%s.tmpXXXXXX", index_name);
  if ((temp_fd = mkstemp(temp_name)) < 0) {
    TB_ERROR_ERRNO("player", "index-save", index_name,
                   "cannot stage %d entries; live index kept", top_of_p_table + 1);
    return;
  }
  if (!(index_file = fdopen(temp_fd, "w"))) {
    TB_ERROR_ERRNO("player", "index-save", temp_name,
                   "cannot open staged index for %d entries; live index kept",
                   top_of_p_table + 1);
    close(temp_fd);
    if (unlink(temp_name) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("unlink", temp_name);
    return;
  }

  for (i = 0; i <= top_of_p_table && !failed; i++)
    if (*player_table[i].name) {
      sprintascii(bits, player_table[i].flags);
      if (fprintf(index_file, "%ld %s %d %s %ld\n", player_table[i].id,
                  player_table[i].name, player_table[i].level, *bits ? bits : "0",
                  (long)player_table[i].last) < 0)
        failed = TRUE;
    }
  /* Records boot could not parse are carried forward, not erased. */
  for (i = 0; i < index_kept_count && !failed; i++)
    if (fprintf(index_file, "%s\n", index_kept_lines[i]) < 0)
      failed = TRUE;
  if (!failed && fprintf(index_file, "~\n") < 0)
    failed = TRUE;
  if (ferror(index_file) || fflush(index_file) != 0)
    failed = TRUE;
  if (fclose(index_file) != 0)
    failed = TRUE;
  if (!failed && !tb_replace_file_checked(temp_name, index_name))
    failed = TRUE;
  if (failed) {
    TB_ERROR_ERRNO("player", "index-save", index_name,
                   "%d entries not written; live index kept", top_of_p_table + 1);
    if (unlink(temp_name) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("unlink", temp_name);
  }
}


void free_player_index(void)
{
  int tp;

  for (tp = 0; tp < index_kept_count; tp++)
    free(index_kept_lines[tp]);
  free(index_kept_lines);
  index_kept_lines = NULL;
  index_kept_count = 0;

  if (player_table) {
    for (tp = 0; tp <= top_of_p_table; tp++)
      if (player_table[tp].name)
        free(player_table[tp].name);
    free(player_table);
    player_table = NULL;
  }
  /* An empty table is -1. This used to be set to 0 with player_table
   * NULL, so any later lookup (get_ptable_by_name(), get_name_by_id(),
   * create_entry()) read player_table[0] through NULL (B2-29). */
  top_of_p_table = top_of_p_file = -1;
}

long get_ptable_by_name(const char *name)
{
  int i;

  for (i = 0; i <= top_of_p_table; i++)
    if (!str_cmp(player_table[i].name, name))
      return (i);

  return (-1);
}

long get_id_by_name(const char *name)
{
  int i;

  for (i = 0; i <= top_of_p_table; i++)
    if (!str_cmp(player_table[i].name, name))
      return (player_table[i].id);

  return (-1);
}

char *get_name_by_id(long id)
{
  int i;

  for (i = 0; i <= top_of_p_table; i++)
    if (player_table[i].id == id)
      return (player_table[i].name);

  return (NULL);
}

/* Stuff related to the save/load player system. */
/** Load an ASCII player file into ch.
 * @param name Player name; must be in the player index.
 * @param ch A freshly allocated, cleared character with player_specials
 * allocated. String fields are assigned, not freed first, so loading into
 * a character that already owns strings leaks them.
 * @return The player's player_table index, or -1 if the name is not in the
 * index or the file cannot be opened (reported).
 * File content is untrusted. A malformed or out-of-range field never fails
 * the load: it is reported (pfile-int, pfile-range, pfile-flags,
 * pfile-desc, pfile-alias, pfile-tag ...) and the field keeps its PFDEF_*
 * default. Unknown tags are reported and skipped. A current quest that is
 * not loaded is reported (pfile-quest) and cleared with clear_quest(). */
int load_char(const char *name, struct char_data *ch)
{
  int id, i;
  FILE *fl;
  char filename[40];
  char line[MAX_INPUT_LENGTH + 1], tag[6];
  trig_data *t = NULL;
  trig_rnum t_rnum = NOTHING;
  int alis_seen = FALSE;	/* an "Alis:" tag was read; see read_aliases_ascii() */

  if ((id = get_ptable_by_name(name)) < 0)
    return (-1);
  else {
    /* get_filename() refuses (and reports) a name that is not a safe path
     * component, such as "../x" from a damaged index; never open a path
     * built from it. The raw name is not logged. */
    if (!get_filename(filename, sizeof(filename), PLR_FILE, player_table[id].name)) {
      TB_ERROR_MSG("player", "pfile-name", "player_table",
                   "player idnum %ld: no safe player file name for its %zu-byte "
                   "index name; not loaded", player_table[id].id,
                   strlen(player_table[id].name));
      return (-1);
    }
    if (!(fl = fopen(filename, "r"))) {
      int saved_errno = errno;
      mudlog(NRM, LVL_GOD, TRUE,
             "SYSERR: Couldn't open player file %s: %s (errno=%d)",
             filename, strerror(saved_errno), saved_errno);
      return (-1);
    }

    /* Character initializations. Necessary to keep some things straight. */
    ch->affected = NULL;
    for (i = 1; i <= MAX_SKILLS; i++)
      GET_SKILL(ch, i) = 0;
    GET_SEX(ch) = PFDEF_SEX;
    GET_CLASS(ch) = PFDEF_CLASS;
    GET_LEVEL(ch) = PFDEF_LEVEL;
    GET_HEIGHT(ch) = PFDEF_HEIGHT;
    GET_WEIGHT(ch) = PFDEF_WEIGHT;
    GET_ALIGNMENT(ch) = PFDEF_ALIGNMENT;
    for (i = 0; i < NUM_OF_SAVING_THROWS; i++)
      GET_SAVE(ch, i) = PFDEF_SAVETHROW;
    GET_LOADROOM(ch) = PFDEF_LOADROOM;
    GET_INVIS_LEV(ch) = PFDEF_INVISLEV;
    GET_FREEZE_LEV(ch) = PFDEF_FREEZELEV;
    GET_WIMP_LEV(ch) = PFDEF_WIMPLEV;
    GET_COND(ch, HUNGER) = PFDEF_HUNGER;
    GET_COND(ch, THIRST) = PFDEF_THIRST;
    GET_COND(ch, DRUNK) = PFDEF_DRUNK;
    GET_BAD_PWS(ch) = PFDEF_BADPWS;
    GET_PRACTICES(ch) = PFDEF_PRACTICES;
    GET_GOLD(ch) = PFDEF_GOLD;
    GET_BANK_GOLD(ch) = PFDEF_BANK;
    GET_EXP(ch) = PFDEF_EXP;
    GET_HITROLL(ch) = PFDEF_HITROLL;
    GET_DAMROLL(ch) = PFDEF_DAMROLL;
    GET_AC(ch) = PFDEF_AC;
    ch->real_abils.str = PFDEF_STR;
    ch->real_abils.str_add = PFDEF_STRADD;
    ch->real_abils.dex = PFDEF_DEX;
    ch->real_abils.intel = PFDEF_INT;
    ch->real_abils.wis = PFDEF_WIS;
    ch->real_abils.con = PFDEF_CON;
    ch->real_abils.cha = PFDEF_CHA;
    GET_HIT(ch) = PFDEF_HIT;
    GET_MAX_HIT(ch) = PFDEF_MAXHIT;
    GET_MANA(ch) = PFDEF_MANA;
    GET_MAX_MANA(ch) = PFDEF_MAXMANA;
    GET_MOVE(ch) = PFDEF_MOVE;
    GET_MAX_MOVE(ch) = PFDEF_MAXMOVE;
    GET_OLC_ZONE(ch) = PFDEF_OLC;
    GET_PAGE_LENGTH(ch) = PFDEF_PAGELENGTH;
    GET_SCREEN_WIDTH(ch) = PFDEF_SCREENWIDTH;
    GET_ALIASES(ch) = NULL;
    SITTING(ch) = NULL;
    NEXT_SITTING(ch) = NULL;
    GET_QUESTPOINTS(ch) = PFDEF_QUESTPOINTS;
    GET_QUEST_COUNTER(ch) = PFDEF_QUESTCOUNT;
    GET_QUEST(ch) = PFDEF_CURRQUEST;
    GET_NUM_QUESTS(ch) = PFDEF_COMPQUESTS;
    GET_LAST_MOTD(ch) = PFDEF_LASTMOTD;
    GET_LAST_NEWS(ch) = PFDEF_LASTNEWS;

    for (i = 0; i < AF_ARRAY_MAX; i++)
      AFF_FLAGS(ch)[i] = PFDEF_AFFFLAGS;
    for (i = 0; i < PM_ARRAY_MAX; i++)
      PLR_FLAGS(ch)[i] = PFDEF_PLRFLAGS;
    for (i = 0; i < PR_ARRAY_MAX; i++)
      PRF_FLAGS(ch)[i] = PFDEF_PREFFLAGS;

    while (get_line(fl, line)) {
      tag_argument(line, tag);
      if (!*tag)
        continue;	/* short line, already reported by tag_argument() */

      switch (*tag) {
      case 'A':
        if (!strcmp(tag, "Ac  "))	GET_AC(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Act ")) {
         load_pfile_flag_words(line, PLR_FLAGS(ch), filename, tag, GET_IDNUM(ch));
      } else if (!strcmp(tag, "Aff ")) {
        load_pfile_flag_words(line, AFF_FLAGS(ch), filename, tag, GET_IDNUM(ch));
	}
	else if (!strcmp(tag, "Affs")) 	load_affects(fl, ch);
        else if (!strcmp(tag, "Alin"))	GET_ALIGNMENT(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Alis"))	read_aliases_ascii(fl, ch, parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch)), filename, &alis_seen);
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'B':
	     if (!strcmp(tag, "Badp"))	GET_BAD_PWS(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Bank"))	GET_BANK_GOLD(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Brth"))	ch->player.time.birth	= parse_pfile_long_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'C':
	     if (!strcmp(tag, "Cha "))	ch->real_abils.cha	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Clas"))	GET_CLASS(ch)		= pfile_int_in_range(line, filename, tag, GET_IDNUM(ch), 0, NUM_CLASSES - 1, PFDEF_CLASS);
	else if (!strcmp(tag, "Con "))	ch->real_abils.con	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'D':
	     if (!strcmp(tag, "Desc")) {
          /* A truncated or oversize description is dropped, not fatal:
           * fread_string() would exit(1) the whole server here. */
          free(ch->player.description);
          if (!fread_string_checked(fl, filename, "Desc", &ch->player.description))
            TB_ERROR_MSG("player", "pfile-desc", filename,
                         "player idnum %ld unterminated or oversize Desc; "
                         "description dropped", GET_IDNUM(ch));
        }
	else if (!strcmp(tag, "Dex "))	ch->real_abils.dex	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Drnk"))	GET_COND(ch, DRUNK)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Drol"))	GET_DAMROLL(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'E':
	     if (!strcmp(tag, "Exp "))	GET_EXP(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
             else if (!strcmp(tag, "EgAP")) GET_ENDGAME_AP(ch) = pfile_int_clamped(line, filename, tag, GET_IDNUM(ch), 0, INT_MAX);
             else if (!strcmp(tag, "EgXP")) GET_ENDGAME_XP(ch) = pfile_int_clamped(line, filename, tag, GET_IDNUM(ch), 0, INT_MAX);
             else if (!strcmp(tag, "EgHP")) GET_ENDGAME_HP_RANK(ch) = pfile_int_clamped(line, filename, tag, GET_IDNUM(ch), 0, PFILE_ENDGAME_RANK_MAX);
             else if (!strcmp(tag, "EgMP")) GET_ENDGAME_MP_RANK(ch) = pfile_int_clamped(line, filename, tag, GET_IDNUM(ch), 0, PFILE_ENDGAME_RANK_MAX);
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'F':
	     if (!strcmp(tag, "Frez"))	GET_FREEZE_LEV(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'G':
	     if (!strcmp(tag, "Gold"))	GET_GOLD(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'H':
	     if (!strcmp(tag, "Hit "))	load_HMVS(ch, line, LOAD_HIT, filename, tag);
	else if (!strcmp(tag, "Hite"))	GET_HEIGHT(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
        else if (!strcmp(tag, "Host"))
          PFILE_SET_STRING(GET_HOST(ch), line, filename, tag, GET_IDNUM(ch));
        else if (!strcmp(tag, "Hrol"))	GET_HITROLL(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Hung"))	GET_COND(ch, HUNGER)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'I':
	     if (!strcmp(tag, "Id  "))	GET_IDNUM(ch)		= parse_pfile_long_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Int "))	ch->real_abils.intel	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Invs"))	GET_INVIS_LEV(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'L':
	     if (!strcmp(tag, "Last"))	ch->player.time.logon	= parse_pfile_long_or_zero(line, filename, tag, GET_IDNUM(ch));
  else if (!strcmp(tag, "Lern"))	GET_PRACTICES(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Levl"))	GET_LEVEL(ch)		= pfile_int_in_range(line, filename, tag, GET_IDNUM(ch), 0, LVL_IMPL, PFDEF_LEVEL);
        else if (!strcmp(tag, "Lmot"))   GET_LAST_MOTD(ch)   = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
        else if (!strcmp(tag, "Lnew"))   GET_LAST_NEWS(ch)   = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'M':
	     if (!strcmp(tag, "Mana"))	load_HMVS(ch, line, LOAD_MANA, filename, tag);
	else if (!strcmp(tag, "Move"))	load_HMVS(ch, line, LOAD_MOVE, filename, tag);
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'N':
	     if (!strcmp(tag, "Name"))	PFILE_SET_STRING(GET_PC_NAME(ch), line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'O':
       if (!strcmp(tag, "Olc "))  GET_OLC_ZONE(ch) = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
  break;

      case 'P':
       if (!strcmp(tag, "Page"))  GET_PAGE_LENGTH(ch) = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Pass"))	strlcpy(GET_PASSWD(ch), line, sizeof(ch->player.passwd));
	else if (!strcmp(tag, "Plyd"))	ch->player.time.played	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "PfIn"))	PFILE_SET_STRING(POOFIN(ch), line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "PfOt"))	PFILE_SET_STRING(POOFOUT(ch), line, filename, tag, GET_IDNUM(ch));
        else if (!strcmp(tag, "Pref")) {
          /* The legacy fallback used to convert f1, which was never
           * written on this path (uninitialised or a stale Act/Aff word). */
          load_pfile_flag_words(line, PRF_FLAGS(ch), filename, tag, GET_IDNUM(ch));
	  }
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
        break;

      case 'Q':
	     if (!strcmp(tag, "Qstp"))  GET_QUESTPOINTS(ch)     = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
       else if (!strcmp(tag, "Qpnt")) GET_QUESTPOINTS(ch) = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch)); /* Backward compatibility */
       else if (!strcmp(tag, "Qcur")) GET_QUEST(ch) = pfile_int_in_range(line, filename, tag, GET_IDNUM(ch), IDXTYPE_MIN, IDXTYPE_MAX, PFDEF_CURRQUEST);
       else if (!strcmp(tag, "Qcnt")) GET_QUEST_COUNTER(ch) = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
       else if (!strcmp(tag, "Qest")) load_quests(fl, ch);
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
        break;

      case 'R':
	     if (!strcmp(tag, "Room"))	GET_LOADROOM(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'S':
	     if (!strcmp(tag, "Sex "))	GET_SEX(ch)		= pfile_int_in_range(line, filename, tag, GET_IDNUM(ch), 0, NUM_GENDERS - 1, PFDEF_SEX);
  else if (!strcmp(tag, "ScrW"))  GET_SCREEN_WIDTH(ch) = parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Skil"))	load_skills(fl, ch, filename);
	else if (!strcmp(tag, "Str "))	load_HMVS(ch, line, LOAD_STRENGTH, filename, tag);
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'T':
	     if (!strcmp(tag, "Thir"))	GET_COND(ch, THIRST)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Thr1"))	GET_SAVE(ch, 0)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Thr2"))	GET_SAVE(ch, 1)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Thr3"))	GET_SAVE(ch, 2)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Thr4"))	GET_SAVE(ch, 3)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Thr5"))	GET_SAVE(ch, 4)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Titl"))	PFILE_SET_STRING(GET_TITLE(ch), line, filename, tag, GET_IDNUM(ch));
        else if (!strcmp(tag, "Trig")) {
          if (CONFIG_SCRIPT_PLAYERS &&
              (t_rnum = real_trigger(parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch)))) != NOTHING) {
            t = read_trigger(t_rnum);
          if (!SCRIPT(ch))
            CREATE(SCRIPT(ch), struct script_data, 1);
          add_trigger(SCRIPT(ch), t, -1);
          }
         }
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      case 'V':
	     if (!strcmp(tag, "Vars"))	read_saved_vars_ascii(fl, ch, parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch)));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
      break;

      case 'W':
	     if (!strcmp(tag, "Wate"))	GET_WEIGHT(ch)		= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Wimp"))	GET_WIMP_LEV(ch)	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else if (!strcmp(tag, "Wis "))	ch->real_abils.wis	= parse_pfile_int_or_zero(line, filename, tag, GET_IDNUM(ch));
	else pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
	break;

      default:
        pfile_unknown_tag(filename, tag, GET_IDNUM(ch));
        break;
      }
    }
  }

  /* Qcur is a quest vnum from the file, range-checked when read to the
   * qst_vnum that real_quest() takes (current_quest is an int). Its quest
   * can be gone by now (deleted in qedit, or its quest file removed), and
   * real_quest() then returns NOTHING, which QST_NAME() and the other
   * aquest_table[] accessors would use as an index (G2-02). Invariant
   * after load: the current quest is NOTHING or a loaded quest. A stale
   * one is left the way quest_progress() leaves a quest that is gone. */
  if (GET_QUEST(ch) != NOTHING && real_quest(GET_QUEST(ch)) == NOTHING) {
    TB_ERROR_MSG("player", "pfile-quest", filename,
                 "player idnum %ld current quest vnum %d is not loaded; "
                 "quest cleared (counter %d dropped)", GET_IDNUM(ch),
                 GET_QUEST(ch), GET_QUEST_COUNTER(ch));
    clear_quest(ch);
  }

  affect_total(ch);

  /* initialization for imms */
  if (GET_LEVEL(ch) >= LVL_IMMORT) {
    for (i = 1; i <= MAX_SKILLS; i++)
      GET_SKILL(ch, i) = 100;
    GET_COND(ch, HUNGER) = -1;
    GET_COND(ch, THIRST) = -1;
    GET_COND(ch, DRUNK) = -1;
  }
  tb_fclose(fl);
  return(id);
}

/** Write a player's file (the ASCII player file save routine).
 * Side effects, all undone before return: every worn item is unequipped
 * and every affect is removed so the base stats can be written, then both
 * are put back. No wear/remove trigger fires and no script runs in between
 * (see the B2-21 note at the top of this file), so nothing can purge an
 * item or re-enter save_char() mid-save. At most MAX_AFFECT affects are
 * written; extra ones stay on the character and are reported
 * (save-affects).
 * The file is staged and replaced only after every write succeeded; any
 * failure is reported and leaves the previous file in place. Afterwards the
 * player index entry is refreshed (and the index rewritten if it changed).
 * A character missing from the index is reported (save-index-missing) and
 * the index is left as it was. NPCs and characters with no PFILEPOS are not
 * saved. */
void save_char(struct char_data * ch)
{
  FILE *fl;
  char filename[40], buf[MAX_STRING_LENGTH], bits[127], bits2[127], bits3[127], bits4[127];
  char temporary_filename[sizeof(filename) + 16];
  int temp_fd;
  int save_failed = FALSE;
  int i, j, id, save_index = FALSE;
  struct affected_type *aff, tmp_aff[MAX_AFFECT];
  struct affected_type *extra_aff = NULL;	/* affects past MAX_AFFECT */
  int num_extra = 0;
  struct obj_data *char_eq[NUM_WEARS];
  trig_data *t;

  if (IS_NPC(ch) || GET_PFILEPOS(ch) < 0)
    return;

  /* If ch->desc is not null, then update session data before saving. */
  if (ch->desc) {
    if (*ch->desc->host) {
      if (!GET_HOST(ch))
        GET_HOST(ch) = strdup(ch->desc->host);
      else if (GET_HOST(ch) && strcmp(GET_HOST(ch), ch->desc->host)) {
        free(GET_HOST(ch));
        GET_HOST(ch) = strdup(ch->desc->host);
      }
    }

    /* Only update the time.played and time.logon if the character is playing. */
    if (STATE(ch->desc) == CON_PLAYING) {
      ch->player.time.played += time(0) - ch->player.time.logon;
      ch->player.time.logon = time(0);
    }
  }

  if (!get_filename(filename, sizeof(filename), PLR_FILE, GET_NAME(ch))) {
    TB_ERROR_MSG("player", "get-filename", "(player save path)",
                 "player idnum %ld has no player file name; not saved",
                 GET_IDNUM(ch));
    return;
  }
  /* Stage saves beside the original; never truncate a valid player file.
   * A truncated template could address an unintended file: reject it. */
  {
    int path_len = snprintf(temporary_filename, sizeof(temporary_filename),
                            "%s.tmpXXXXXX", filename);
    if (path_len < 0 || (size_t)path_len >= sizeof(temporary_filename)) {
      TB_ERROR_MSG("player", "snprintf", filename,
                   "player idnum %ld: staging path cannot be formatted or is too long; not saved",
                   GET_IDNUM(ch));
      return;
    }
  }
  temp_fd = mkstemp(temporary_filename);
  if (temp_fd < 0) {
    TB_ERROR_ERRNO("player", "mkstemp", filename, "Unable to stage player save");
    return;
  }
  fl = fdopen(temp_fd, "w");
  if (fl == NULL) {
    TB_ERROR_ERRNO("player", "fdopen", temporary_filename, "Unable to open staged save");
    close(temp_fd);
    if (unlink(temporary_filename) != 0)
      TB_ERROR_ERRNO("player", "unlink", temporary_filename,
                     "player idnum %ld: could not remove staged save after fdopen failure",
                     GET_IDNUM(ch));
    return;
  }

  /* Unaffect everything a character can be affected by. No remove
   * trigger: char_eq[] must not be left holding an item a script purged. */
  for (i = 0; i < NUM_WEARS; i++) {
    if (GET_EQ(ch, i))
      char_eq[i] = unequip_char(ch, i);
    else
      char_eq[i] = NULL;
  }

  for (aff = ch->affected, i = 0; i < MAX_AFFECT; i++) {
    if (aff) {
      tmp_aff[i] = *aff;
      for (j=0; j<AF_ARRAY_MAX; j++)
        tmp_aff[i].bitvector[j] = aff->bitvector[j];
      tmp_aff[i].next = 0;
      aff = aff->next;
    } else {
      new_affect(&(tmp_aff[i]));
      tmp_aff[i].next = 0;
    }
  }

  /* The file holds at most MAX_AFFECT affects. The rest cannot be saved,
   * but they must not be lost from the live character either: keep copies
   * and re-apply them with the saved ones below. (They used to be removed
   * and dropped, silently when there was exactly one extra.) */
  if (aff) {
    struct affected_type *a;

    for (a = aff; a; a = a->next)
      num_extra++;
    CREATE(extra_aff, struct affected_type, num_extra);
    for (j = 0, a = aff; a; a = a->next, j++) {
      extra_aff[j] = *a;
      extra_aff[j].next = NULL;
    }
    TB_ERROR_MSG("player", "save-affects", filename,
                 "player idnum %ld has %d affects; only %d saved, %d kept in "
                 "memory only", GET_IDNUM(ch), MAX_AFFECT + num_extra,
                 MAX_AFFECT, num_extra);
  }

  /* Remove the affections so that the raw values are stored; otherwise the
   * effects are doubled when the char logs back in. */

  while (ch->affected)
    affect_remove(ch, ch->affected);

  ch->aff_abils = ch->real_abils;
  /* end char_to_store code */

  if (GET_NAME(ch))				tb_fprintf(fl, "Name: %s\n", GET_NAME(ch));
  if (GET_PASSWD(ch))				tb_fprintf(fl, "Pass: %s\n", GET_PASSWD(ch));
  if (GET_TITLE(ch))				tb_fprintf(fl, "Titl: %s\n", GET_TITLE(ch));
  if (ch->player.description && *ch->player.description) {
    strlcpy(buf, ch->player.description, sizeof(buf));
    strip_cr(buf);
    tb_fprintf(fl, "Desc:\n%s~\n", buf);
  }
  if (POOFIN(ch))				tb_fprintf(fl, "PfIn: %s\n", POOFIN(ch));
  if (POOFOUT(ch))				tb_fprintf(fl, "PfOt: %s\n", POOFOUT(ch));
  if (GET_SEX(ch)	     != PFDEF_SEX)	tb_fprintf(fl, "Sex : %d\n", GET_SEX(ch));
  if (GET_CLASS(ch)	   != PFDEF_CLASS)	tb_fprintf(fl, "Clas: %d\n", GET_CLASS(ch));
  if (GET_LEVEL(ch)	   != PFDEF_LEVEL)	tb_fprintf(fl, "Levl: %d\n", GET_LEVEL(ch));

  tb_fprintf(fl, "Id  : %ld\n", GET_IDNUM(ch));
  tb_fprintf(fl, "Brth: %ld\n", (long)ch->player.time.birth);
  tb_fprintf(fl, "Plyd: %d\n",  ch->player.time.played);
  tb_fprintf(fl, "Last: %ld\n", (long)ch->player.time.logon);

  if (GET_LAST_MOTD(ch) != PFDEF_LASTMOTD)
    tb_fprintf(fl, "Lmot: %d\n", (int)GET_LAST_MOTD(ch));
  if (GET_LAST_NEWS(ch) != PFDEF_LASTNEWS)
    tb_fprintf(fl, "Lnew: %d\n", (int)GET_LAST_NEWS(ch));

  if (GET_HOST(ch))				tb_fprintf(fl, "Host: %s\n", GET_HOST(ch));
  if (GET_HEIGHT(ch)	   != PFDEF_HEIGHT)	tb_fprintf(fl, "Hite: %d\n", GET_HEIGHT(ch));
  if (GET_WEIGHT(ch)	   != PFDEF_WEIGHT)	tb_fprintf(fl, "Wate: %d\n", GET_WEIGHT(ch));
  if (GET_ALIGNMENT(ch)  != PFDEF_ALIGNMENT)	tb_fprintf(fl, "Alin: %d\n", GET_ALIGNMENT(ch));


  sprintascii(bits,  PLR_FLAGS(ch)[0]);
  sprintascii(bits2, PLR_FLAGS(ch)[1]);
  sprintascii(bits3, PLR_FLAGS(ch)[2]);
  sprintascii(bits4, PLR_FLAGS(ch)[3]);
  tb_fprintf(fl, "Act : %s %s %s %s\n", bits, bits2, bits3, bits4);

  sprintascii(bits,  AFF_FLAGS(ch)[0]);
  sprintascii(bits2, AFF_FLAGS(ch)[1]);
  sprintascii(bits3, AFF_FLAGS(ch)[2]);
  sprintascii(bits4, AFF_FLAGS(ch)[3]);
  tb_fprintf(fl, "Aff : %s %s %s %s\n", bits, bits2, bits3, bits4);

  sprintascii(bits,  PRF_FLAGS(ch)[0]);
  sprintascii(bits2, PRF_FLAGS(ch)[1]);
  sprintascii(bits3, PRF_FLAGS(ch)[2]);
  sprintascii(bits4, PRF_FLAGS(ch)[3]);
  tb_fprintf(fl, "Pref: %s %s %s %s\n", bits, bits2, bits3, bits4);

 if (GET_SAVE(ch, 0)	   != PFDEF_SAVETHROW)	tb_fprintf(fl, "Thr1: %d\n", GET_SAVE(ch, 0));
  if (GET_SAVE(ch, 1)	   != PFDEF_SAVETHROW)	tb_fprintf(fl, "Thr2: %d\n", GET_SAVE(ch, 1));
  if (GET_SAVE(ch, 2)	   != PFDEF_SAVETHROW)	tb_fprintf(fl, "Thr3: %d\n", GET_SAVE(ch, 2));
  if (GET_SAVE(ch, 3)	   != PFDEF_SAVETHROW)	tb_fprintf(fl, "Thr4: %d\n", GET_SAVE(ch, 3));
  if (GET_SAVE(ch, 4)	   != PFDEF_SAVETHROW)	tb_fprintf(fl, "Thr5: %d\n", GET_SAVE(ch, 4));

  if (GET_WIMP_LEV(ch)	   != PFDEF_WIMPLEV)	tb_fprintf(fl, "Wimp: %d\n", GET_WIMP_LEV(ch));
  if (GET_FREEZE_LEV(ch)   != PFDEF_FREEZELEV)	tb_fprintf(fl, "Frez: %d\n", GET_FREEZE_LEV(ch));
  if (GET_INVIS_LEV(ch)	   != PFDEF_INVISLEV)	tb_fprintf(fl, "Invs: %d\n", GET_INVIS_LEV(ch));
  if (GET_LOADROOM(ch)	   != PFDEF_LOADROOM)	tb_fprintf(fl, "Room: %d\n", GET_LOADROOM(ch));

  if (GET_BAD_PWS(ch)	   != PFDEF_BADPWS)	tb_fprintf(fl, "Badp: %d\n", GET_BAD_PWS(ch));
  if (GET_PRACTICES(ch)	   != PFDEF_PRACTICES)	tb_fprintf(fl, "Lern: %d\n", GET_PRACTICES(ch));

  if (GET_COND(ch, HUNGER)   != PFDEF_HUNGER && GET_LEVEL(ch) < LVL_IMMORT) tb_fprintf(fl, "Hung: %d\n", GET_COND(ch, HUNGER));
  if (GET_COND(ch, THIRST) != PFDEF_THIRST && GET_LEVEL(ch) < LVL_IMMORT) tb_fprintf(fl, "Thir: %d\n", GET_COND(ch, THIRST));
  if (GET_COND(ch, DRUNK)  != PFDEF_DRUNK  && GET_LEVEL(ch) < LVL_IMMORT) tb_fprintf(fl, "Drnk: %d\n", GET_COND(ch, DRUNK));

  if (GET_HIT(ch)	   != PFDEF_HIT  || GET_MAX_HIT(ch)  != PFDEF_MAXHIT)  tb_fprintf(fl, "Hit : %d/%d\n", GET_HIT(ch),  GET_MAX_HIT(ch));
  if (GET_MANA(ch)	   != PFDEF_MANA || GET_MAX_MANA(ch) != PFDEF_MAXMANA) tb_fprintf(fl, "Mana: %d/%d\n", GET_MANA(ch), GET_MAX_MANA(ch));
  if (GET_MOVE(ch)	   != PFDEF_MOVE || GET_MAX_MOVE(ch) != PFDEF_MAXMOVE) tb_fprintf(fl, "Move: %d/%d\n", GET_MOVE(ch), GET_MAX_MOVE(ch));

  if (GET_STR(ch)	   != PFDEF_STR  || GET_ADD(ch)      != PFDEF_STRADD)  tb_fprintf(fl, "Str : %d/%d\n", GET_STR(ch),  GET_ADD(ch));


  if (GET_INT(ch)	   != PFDEF_INT)	tb_fprintf(fl, "Int : %d\n", GET_INT(ch));
  if (GET_WIS(ch)	   != PFDEF_WIS)	tb_fprintf(fl, "Wis : %d\n", GET_WIS(ch));
  if (GET_DEX(ch)	   != PFDEF_DEX)	tb_fprintf(fl, "Dex : %d\n", GET_DEX(ch));
  if (GET_CON(ch)	   != PFDEF_CON)	tb_fprintf(fl, "Con : %d\n", GET_CON(ch));
  if (GET_CHA(ch)	   != PFDEF_CHA)	tb_fprintf(fl, "Cha : %d\n", GET_CHA(ch));

  if (GET_AC(ch)	   != PFDEF_AC)		tb_fprintf(fl, "Ac  : %d\n", GET_AC(ch));
  if (GET_GOLD(ch)	   != PFDEF_GOLD)	tb_fprintf(fl, "Gold: %d\n", GET_GOLD(ch));
  if (GET_BANK_GOLD(ch)	   != PFDEF_BANK)	tb_fprintf(fl, "Bank: %d\n", GET_BANK_GOLD(ch));
  if (GET_EXP(ch)	   != PFDEF_EXP)	tb_fprintf(fl, "Exp : %d\n", GET_EXP(ch));
  if (GET_ENDGAME_AP(ch)) tb_fprintf(fl, "EgAP: %d\n", GET_ENDGAME_AP(ch));
  if (GET_ENDGAME_XP(ch)) tb_fprintf(fl, "EgXP: %d\n", GET_ENDGAME_XP(ch));
  if (GET_ENDGAME_HP_RANK(ch)) tb_fprintf(fl, "EgHP: %d\n", GET_ENDGAME_HP_RANK(ch));
  if (GET_ENDGAME_MP_RANK(ch)) tb_fprintf(fl, "EgMP: %d\n", GET_ENDGAME_MP_RANK(ch));
  if (GET_HITROLL(ch)	   != PFDEF_HITROLL)	tb_fprintf(fl, "Hrol: %d\n", GET_HITROLL(ch));
  if (GET_DAMROLL(ch)	   != PFDEF_DAMROLL)	tb_fprintf(fl, "Drol: %d\n", GET_DAMROLL(ch));
  if (GET_OLC_ZONE(ch)     != PFDEF_OLC)        tb_fprintf(fl, "Olc : %d\n", GET_OLC_ZONE(ch));
  if (GET_PAGE_LENGTH(ch)  != PFDEF_PAGELENGTH) tb_fprintf(fl, "Page: %d\n", GET_PAGE_LENGTH(ch));
  if (GET_SCREEN_WIDTH(ch) != PFDEF_SCREENWIDTH) tb_fprintf(fl, "ScrW: %d\n", GET_SCREEN_WIDTH(ch));
  if (GET_QUESTPOINTS(ch)  != PFDEF_QUESTPOINTS) tb_fprintf(fl, "Qstp: %d\n", GET_QUESTPOINTS(ch));
  if (GET_QUEST_COUNTER(ch)!= PFDEF_QUESTCOUNT)  tb_fprintf(fl, "Qcnt: %d\n", GET_QUEST_COUNTER(ch));
  if (GET_NUM_QUESTS(ch)   != PFDEF_COMPQUESTS) {
    tb_fprintf(fl, "Qest:\n");
    for (i = 0; i < GET_NUM_QUESTS(ch); i++)
      tb_fprintf(fl, "%d\n", ch->player_specials->saved.completed_quests[i]);
    tb_fprintf(fl, "%d\n", NOTHING);
  }
  if (GET_QUEST(ch)        != PFDEF_CURRQUEST)  tb_fprintf(fl, "Qcur: %d\n", GET_QUEST(ch));

 if (SCRIPT(ch)) {
   for (t = TRIGGERS(SCRIPT(ch)); t; t = t->next)
   tb_fprintf(fl, "Trig: %d\n",GET_TRIG_VNUM(t));
}

  /* Save skills */
  if (GET_LEVEL(ch) < LVL_IMMORT) {
    tb_fprintf(fl, "Skil:\n");
    for (i = 1; i <= MAX_SKILLS; i++) {
     if (GET_SKILL(ch, i))
	tb_fprintf(fl, "%d %d\n", i, GET_SKILL(ch, i));
    }
    tb_fprintf(fl, "0 0\n");
  }

  /* Save affects */
  if (tmp_aff[0].spell > 0) {
    tb_fprintf(fl, "Affs:\n");
    for (i = 0; i < MAX_AFFECT; i++) {
      aff = &tmp_aff[i];
      if (aff->spell)
		tb_fprintf(fl, "%d %d %d %d %d %d %d %d\n", aff->spell, aff->duration,
          aff->modifier, aff->location, aff->bitvector[0], aff->bitvector[1], aff->bitvector[2], aff->bitvector[3]);
    }
    tb_fprintf(fl, "0 0 0 0 0 0 0 0\n");
  }

  write_aliases_ascii(fl, ch);
  save_char_vars_ascii(fl, ch);

  /* Report failures at their origin. Cleanup must not masquerade as the
   * original error or replace an existing valid player file. */
  if (ferror(fl)) {
    TB_ERROR_MSG("player", "write", temporary_filename,
                 "player idnum %ld: staged save stream reports a write error",
                 GET_IDNUM(ch));
    save_failed = TRUE;
  }
  if (fflush(fl) != 0) {
    TB_ERROR_ERRNO("player", "fflush", temporary_filename,
                   "player idnum %ld: could not flush staged player save",
                   GET_IDNUM(ch));
    save_failed = TRUE;
  }
  if (fclose(fl) != 0) {
    TB_ERROR_ERRNO("player", "fclose", temporary_filename,
                   "player idnum %ld: could not close staged player save",
                   GET_IDNUM(ch));
    save_failed = TRUE;
  }
  if (!save_failed && rename(temporary_filename, filename) != 0) {
    TB_ERROR_ERRNO("player", "rename", filename,
                   "player idnum %ld: could not commit staged save %s; previous file kept",
                   GET_IDNUM(ch), temporary_filename);
    save_failed = TRUE;
  }
  if (save_failed && unlink(temporary_filename) != 0)
    TB_ERROR_ERRNO("player", "unlink", temporary_filename,
                   "player idnum %ld: could not clean up failed staged player save",
                   GET_IDNUM(ch));

  /* More char_to_store code to add spell and eq affections back in. */
  for (i = 0; i < MAX_AFFECT; i++) {
    if (tmp_aff[i].spell)
      affect_to_char(ch, &tmp_aff[i]);
  }
  for (i = 0; i < num_extra; i++)
    affect_to_char(ch, &extra_aff[i]);
  free(extra_aff);

  /* Every item goes back to the slot it came from; no wear trigger (B2-21). */
  for (i = 0; i < NUM_WEARS; i++)
    if (char_eq[i])
      equip_char(ch, char_eq[i], i);
  /* end char_to_store code */

  if (save_failed)
    return;

  /* The pfile is saved, but a character missing from the index cannot be
   * found by name at the next login (and its name could be re-created over
   * it). This used to return silently; nanny's deleted-character path once
   * kept a stale GET_PFILEPOS and so never indexed the new character. */
  if ((id = get_ptable_by_name(GET_NAME(ch))) < 0) {
    TB_ERROR_MSG("player", "save-index-missing", filename,
                 "player idnum %ld saved but not in the player index (pfilepos %ld)",
                 GET_IDNUM(ch), (long)GET_PFILEPOS(ch));
    return;
  }

  /* update the player in the player index */
  if (player_table[id].level != GET_LEVEL(ch)) {
    save_index = TRUE;
    player_table[id].level = GET_LEVEL(ch);
  }
  if (player_table[id].last != ch->player.time.logon) {
    save_index = TRUE;
    player_table[id].last = ch->player.time.logon;
  }
  i = player_table[id].flags;
  if (PLR_FLAGGED(ch, PLR_DELETED))
    SET_BIT(player_table[id].flags, PINDEX_DELETED);
  else
    REMOVE_BIT(player_table[id].flags, PINDEX_DELETED);
  if (PLR_FLAGGED(ch, PLR_NODELETE) || PLR_FLAGGED(ch, PLR_CRYO))
    SET_BIT(player_table[id].flags, PINDEX_NODELETE);
  else
    REMOVE_BIT(player_table[id].flags, PINDEX_NODELETE);

  if (PLR_FLAGGED(ch, PLR_FROZEN) || PLR_FLAGGED(ch, PLR_NOWIZLIST))
    SET_BIT(player_table[id].flags, PINDEX_NOWIZLIST);
  else
    REMOVE_BIT(player_table[id].flags, PINDEX_NOWIZLIST);

  if (player_table[id].flags != i || save_index)
    save_player_index();
}

/** Separate a 4-character id tag from the data it precedes.
 * Used by the player-file and rent-file parsers on lines read from disk.
 * @param[in,out] argument A line such as "Name: Joe". On return it holds only
 * the value ("Joe"), shifted down in place.
 * @param[out] tag At least 5 bytes; receives the 4-character tag.
 * A line shorter than 4 characters cannot hold a tag: the old code copied
 * 4 bytes and stepped past the terminator, reading (and then copying)
 * whatever followed the string. Such a line now yields an empty tag and an
 * empty value, which callers skip, and is reported as a pfile-tag warning. */
void tag_argument(char *argument, char *tag)
{
  char *tmp = argument, *ttag = tag, *wrt = argument;
  size_t len = 0;
  int i;

  while (len < 4 && argument[len] != '\0')
    len++;
  if (len < 4) {
    TB_WARN_MSG("player", "pfile-tag", "tag-line",
                "short tag line (%zu bytes); line skipped", len);
    tag[0] = '\0';
    argument[0] = '\0';
    return;
  }

  for (i = 0; i < 4; i++)
    *(ttag++) = *(tmp++);
  *ttag = '\0';

  while (*tmp == ':' || *tmp == ' ')
    tmp++;

  while (*tmp)
    *(wrt++) = *(tmp++);
  *wrt = '\0';
}

/* Stuff related to the player file cleanup system. */

/** Delete a player: unlink every file the player owns, drop the index entry
 * and rewrite the index. Used for self-deletion, immortal deletion and the
 * auto-wipe (clean_pfiles()).
 * @param pfilepos The player's player_table position.
 * The entry's name is freed and later entries shift down one slot (see
 * remove_player_from_index()); every held position >= pfilepos is stale
 * afterwards. Unlink failures other than "no such file" are logged.
 * A position outside the table, or an entry with no name, is reported
 * (index-pos) and nothing is deleted; so is a name get_filename() refuses
 * (pclean-name), and that entry stays in the index for an admin. */
void remove_player(int pfilepos)
{
  char filename[MAX_STRING_LENGTH], timestr[25];
  int i;

  /* pfilepos comes from the caller's earlier lookup, and every removal
   * compacts the table; a stale or bad position indexed past it (B2-29). */
  if (!VALID_PTABLE_POS(pfilepos)) {
    TB_ERROR_MSG("player", "index-pos", "player_table",
                 "position %d outside 0..%d; nothing deleted", pfilepos,
                 top_of_p_table);
    return;
  }
  /* Names are blanked only just before removal, so an unnamed entry is a
   * broken index; its files cannot be found by name. */
  if (!PT_PNAME(pfilepos) || !*PT_PNAME(pfilepos)) {
    TB_ERROR_MSG("player", "index-pos", "player_table",
                 "position %d (idnum %ld) has no name; nothing deleted",
                 pfilepos, PT_IDNUM(pfilepos));
    return;
  }
  /* Name every file before deleting any. get_filename() refuses (and
   * reports) a name that is not a safe path component, such as "../x"
   * from a damaged index: then nothing is unlinked, and the entry is kept
   * rather than half-deleted. The raw name is not logged. */
  for (i = 0; i < MAX_FILES; i++)
    if (!get_filename(filename, sizeof(filename), i, PT_PNAME(pfilepos))) {
      TB_ERROR_MSG("player", "pclean-name", "player_table",
                   "player idnum %ld: no safe name for file type %d from its "
                   "%zu-byte index name; nothing deleted", PT_IDNUM(pfilepos),
                   i, strlen(PT_PNAME(pfilepos)));
      return;
    }

  /* Unlink all player-owned files */
  for (i = 0; i < MAX_FILES; i++) {
    if (get_filename(filename, sizeof(filename), i, player_table[pfilepos].name)) {
      if (unlink(filename) != 0 && errno != ENOENT) {
        int saved_errno = errno;
        log("SYSERR: player cleanup unlink failed for %s file %s: %s (errno=%d)",
            player_table[pfilepos].name, filename, strerror(saved_errno), saved_errno);
      }
    }
  }

  /* 'last' comes from the player index file. localtime() returns NULL for a
   * value it cannot represent (around 1e17). tb_strftime() used to leave
   * timestr unwritten then, so the log line below printed stack garbage; it
   * now reports and leaves "", and this check names the player instead. */
  {
    const struct tm *last_tm = localtime(&(player_table[pfilepos].last));

    if (last_tm == NULL) {
      TB_WARN_MSG("player", "pclean-time", player_table[pfilepos].name,
                  "player idnum %ld index last-login %lld is not a valid time",
                  player_table[pfilepos].id,
                  (long long) player_table[pfilepos].last);
      strlcpy(timestr, "unknown", sizeof(timestr));
    } else
      tb_strftime(timestr, sizeof(timestr), "%c", last_tm);
  }
  log("PCLEAN: %s Lev: %d Last: %s",
	player_table[pfilepos].name, player_table[pfilepos].level,
	timestr);
  player_table[pfilepos].name[0] = '\0';

  /* Update index table. */
  remove_player_from_index(pfilepos);

  save_player_index();
}

/* remove_player() for the sweep: TRUE only if the entry really left the
 * table. remove_player() refuses (and reports) a player it cannot delete
 * safely, e.g. one whose name get_filename() rejects; the sweep must then
 * move on, not re-examine that slot forever. */
static int pclean_remove(int pos)
{
  int before = top_of_p_table;

  remove_player(pos);
  return top_of_p_table < before;
}

/* Walk the player index and remove deleted or expired players.
 *
 * remove_player() compacts player_table: the entry after i moves into slot
 * i and top_of_p_table drops by one. The loop must therefore re-examine
 * slot i after a removal instead of advancing past it, or every player that
 * directly follows a removed one escapes the sweep. A refused removal
 * leaves the entry in place, so the loop moves on from it. */
void clean_pfiles(void)
{
  int i, ci, removed;

  for (i = 0; i <= top_of_p_table; i++) {
    removed = FALSE;
    /* We only want to go further if the player isn't protected from deletion
     * and hasn't already been deleted. */
    if (!IS_SET(player_table[i].flags, PINDEX_NODELETE) &&
        *player_table[i].name) {
      /* If the player is already flagged for deletion, then go ahead and get
       * rid of him. */
      if (IS_SET(player_table[i].flags, PINDEX_DELETED)) {
	removed = pclean_remove(i);
      } else {
        /* Check to see if the player has overstayed his welcome based on level. */
	for (ci = 0; pclean_criteria[ci].level > -1; ci++) {
	  if (player_table[i].level <= pclean_criteria[ci].level &&
	      ((time(0) - player_table[i].last) >
	       (pclean_criteria[ci].days * SECS_PER_REAL_DAY))) {
	    removed = pclean_remove(i);
	    break;
	  }
	}
        /* If we got this far and the players hasn't been kicked out, then he
	 * can stay a little while longer. */
      }
    }
    if (removed)
      i--;	/* slot i now holds the next player; examine it */
  }
  /* After everything is done, we should rebuild player_index and remove the
   * entries of the players that were just deleted. */
}

/* Both historical five-column and current eight-column affects remain supported. */
static void load_affects(FILE *fl, struct char_data *ch)
{
  char line[MAX_INPUT_LENGTH + 1];
  int values[8];
  int count, index;
  const char *cursor;
  char *end;
  long parsed;
  struct affected_type af;

  while (get_line(fl, line)) {
    cursor = line;
    count = 0;
    while (count < 8) {
      while (isspace((unsigned char)*cursor))
        cursor++;
      if (*cursor == '\0')
        break;
      errno = 0;
      parsed = strtol(cursor, &end, 10);
      if (end == cursor || errno == ERANGE || parsed < INT_MIN ||
          parsed > INT_MAX || (*end && !isspace((unsigned char)*end)))
        break;
      values[count++] = (int)parsed;
      cursor = end;
    }
    while (isspace((unsigned char)*cursor))
      cursor++;
    if (*cursor != '\0' || count == 0) {
      log("SYSERR: Malformed affects in pfile (%s)", GET_NAME(ch));
      break;
    }
    if (values[0] == 0)
      break;
    if (count != 5 && count != 8) {
      log("SYSERR: Invalid affects in pfile (%s), expecting 5 or 8 values", GET_NAME(ch));
      break;
    }
    new_affect(&af);
    af.spell = values[0];
    af.duration = values[1];
    af.modifier = values[2];
    af.location = values[3];
    if (count == 8) {
      for (index = 0; index < 4; index++)
        af.bitvector[index] = values[index + 4];
    } else if (values[4] > 0 && values[4] < NUM_AFF_FLAGS) {
      SET_BIT_AR(af.bitvector, values[4]);
    }
    if (af.spell > 0)
      affect_to_char(ch, &af);
  }
}

/* Parse one or two bounded integers without scanf's overflow ambiguity. */
static int parse_saved_ints(const char *line, int *first, int *second)
{
  char *end;
  long value;
  int count = 0;
  int *dest[2] = { first, second };
  const char *cursor = line;

  if (line == NULL || first == NULL)
    return 0;
  while (count < (second != NULL ? 2 : 1)) {
    while (isspace((unsigned char)*cursor))
      cursor++;
    if (*cursor == '\0')
      break;
    errno = 0;
    value = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || value < INT_MIN || value > INT_MAX)
      return 0;
    *dest[count++] = (int)value;
    cursor = end;
  }
  while (isspace((unsigned char)*cursor))
    cursor++;
  return *cursor == '\0' ? count : 0;
}

static void load_skills(FILE *fl, struct char_data *ch, const char *filename)
{
  int num, num2;
  char line[MAX_INPUT_LENGTH + 1];

  while (get_line(fl, line)) {
    if (parse_saved_ints(line, &num, &num2) != 2) {
      log("SYSERR: Invalid skill record in pfile (%s)", GET_NAME(ch));
      break;
    }
    if (num == 0)
      break;
    if (num < 1 || num > MAX_SKILLS) {
      log("SYSERR: Out-of-range skill in pfile (%s): %d", GET_NAME(ch), num);
      break;
    }
    /* skills[] is unsigned char: a larger value would be silently cut. */
    if (num2 < 0 || num2 > UCHAR_MAX) {
      TB_ERROR_MSG("player", "pfile-range", filename,
                   "player idnum %ld skill %d value %d outside 0..%d; skill not loaded",
                   GET_IDNUM(ch), num, num2, UCHAR_MAX);
      continue;
    }
    GET_SKILL(ch, num) = num2;
  }
}

void load_quests(FILE *fl, struct char_data *ch)
{
  int num;
  char line[MAX_INPUT_LENGTH + 1];

  while (get_line(fl, line)) {
    if (parse_saved_ints(line, &num, NULL) != 1) {
      log("SYSERR: Invalid quest record in pfile (%s)", GET_NAME(ch));
      break;
    }
    if (num == NOTHING)
      break;
    add_completed_quest(ch, num);
  }
}

/* Report a pfile tag that load_char() does not recognise. Its line is
 * skipped; the value is not echoed (file content, possibly arbitrary). A
 * tag the code writes but this switch forgets would otherwise be dropped
 * silently on every load. */
static void pfile_unknown_tag(const char *filename, const char *tag, long idnum)
{
  TB_WARN_MSG("player", "pfile-tag", filename,
              "player idnum %ld unknown tag %.4s ignored", idnum, tag);
}

static void load_HMVS(struct char_data *ch, const char *line, int mode,
                      const char *filename, const char *tag)
{
  char *end = NULL;
  long first, second;
  int num, num2;

  /* Reject malformed or overflowing saved values instead of silently
   * replacing existing statistics with zero. Legacy 'number/number'
   * fields, including signed strength modifiers, remain supported. */
  if (line == NULL) {
    TB_ERROR_MSG("player", "pfile-int", filename,
                 "player idnum %ld tag %.4s has no value; unchanged",
                 GET_IDNUM(ch), tag);
    return;
  }
  errno = 0;
  first = strtol(line, &end, 10);
  if (end == line || errno == ERANGE || first < INT_MIN || first > INT_MAX ||
      *end != '/') {
    TB_ERROR_MSG("player", "pfile-int", filename,
                 "player idnum %ld tag %.4s invalid first value; unchanged",
                 GET_IDNUM(ch), tag);
    return;
  }
  line = end + 1;
  errno = 0;
  second = strtol(line, &end, 10);
  if (end == line || errno == ERANGE || second < INT_MIN || second > INT_MAX) {
    TB_ERROR_MSG("player", "pfile-int", filename,
                 "player idnum %ld tag %.4s invalid second value; unchanged",
                 GET_IDNUM(ch), tag);
    return;
  }
  while (isspace((unsigned char)*end))
    end++;
  if (*end != '\0') {
    TB_ERROR_MSG("player", "pfile-int", filename,
                 "player idnum %ld tag %.4s trailing junk; unchanged",
                 GET_IDNUM(ch), tag);
    return;
  }
  num = (int)first;
  num2 = (int)second;

  switch (mode) {
  case LOAD_HIT:
    GET_HIT(ch) = num;
    GET_MAX_HIT(ch) = num2;
    break;
  case LOAD_MANA:
    GET_MANA(ch) = num;
    GET_MAX_MANA(ch) = num2;
    break;
  case LOAD_MOVE:
    GET_MOVE(ch) = num;
    GET_MAX_MOVE(ch) = num2;
    break;
  case LOAD_STRENGTH:
    ch->real_abils.str = num;
    ch->real_abils.str_add = num2;
    break;
  }
}

static void write_aliases_ascii(FILE *file, struct char_data *ch)
{
  struct alias_data *temp;
  int count = 0;

  if (GET_ALIASES(ch) == NULL)
    return;

  for (temp = GET_ALIASES(ch); temp; temp = temp->next)
    count++;

  tb_fprintf(file, "Alis: %d\n", count);

  for (temp = GET_ALIASES(ch); temp; temp = temp->next)
    tb_fprintf(file, " %s\n"   /* Alias: prepend a space in order to avoid issues with aliases beginning
                             * with * (get_line treats lines beginning with * as comments and ignores them */
                  "%s\n"    /* Replacement: always prepended with a space in memory anyway */
                  "%d\n",   /* Type */
                  temp->alias,
                  temp->replacement,
                  temp->type);
}

/* Upper bound on the "Alis: <count>" value. The count comes from the file,
 * and every alias costs three get_line() calls at login. */
#define PFILE_MAX_ALIASES 1000

/* Name for pfile diagnostics: "Alis:" may come before "Name:" in a
 * damaged or hand-edited file, so the name can still be unset. */
#define PFILE_DIAG_NAME(ch) (GET_PC_NAME(ch) ? GET_PC_NAME(ch) : "(no name yet)")

/* Free every alias in ch's list and leave the list empty. */
static void free_alias_list(struct char_data *ch)
{
  struct alias_data *a, *next_a;

  for (a = GET_ALIASES(ch); a; a = next_a) {
    next_a = a->next;
    free_alias(a);
  }
  GET_ALIASES(ch) = NULL;
}

/* Read count alias records (alias line, replacement line, type line) that
 * follow an "Alis: <count>" tag. A record cut off by end of file used to
 * leave the line buffers uninitialised or holding the previous record, so
 * an alias was built from garbage or duplicated; it now ends the list with
 * a pfile-alias error.
 *
 * Duplicate "Alis:" tags. save_char() writes exactly one, so a second one
 * means a damaged or hand-edited file. Policy: the LAST block wins, as for
 * every other pfile tag read twice. The duplicate is reported as
 * pfile-alias-dup (naming the aliases dropped), the earlier list is freed,
 * and the later block is read in its place -- including a later
 * "Alis: 0", which leaves no aliases. Previously "Alis: 0" just set the
 * list to NULL, leaking the aliases already loaded, and a later non-zero
 * block silently merged into the earlier list. *alis_seen is load_char()'s
 * per-file flag. */
static void read_aliases_ascii(FILE *file, struct char_data *ch, int count,
                               const char *filename, int *alis_seen)
{
  int i;

  if (*alis_seen) {
    int dropped = 0;
    struct alias_data *a;

    for (a = GET_ALIASES(ch); a; a = a->next)
      dropped++;
    TB_ERROR_MSG("player", "pfile-alias-dup", filename,
                 "player idnum %ld name %s: duplicate Alis: tag (count %d); "
                 "the later block replaces the earlier one, %d alias(es) dropped",
                 GET_IDNUM(ch), PFILE_DIAG_NAME(ch), count, dropped);
    free_alias_list(ch);
  }
  *alis_seen = TRUE;

  if (count == 0)
    return; /* No aliases in the list. */
  if (count < 0 || count > PFILE_MAX_ALIASES) {
    TB_ERROR_MSG("player", "pfile-alias", filename,
                 "player idnum %ld alias count %d outside 1..%d; aliases not loaded",
                 GET_IDNUM(ch), count, PFILE_MAX_ALIASES);
    return;
  }

  /* This code goes both ways for the old format (where alias and replacement start at the
   * first character on the line) and the new (where they are prepended by a space in order
   * to avoid the possibility of a * at the start of the line */
  for (i = 0; i < count; i++) {
    char abuf[MAX_INPUT_LENGTH+1], rbuf[MAX_INPUT_LENGTH+1], tbuf[MAX_INPUT_LENGTH];

    /* Read the aliased command, then the replacement (which needs a space
     * prepended in memory; it may be there already), then the type. */
    rbuf[0] = ' ';
    if (!get_line(file, abuf) || !get_line(file, rbuf+1) || !get_line(file, tbuf)) {
      TB_ERROR_MSG("player", "pfile-alias", filename,
                   "player idnum %ld alias %d of %d truncated by end of file",
                   GET_IDNUM(ch), i + 1, count);
      return;
    }

    if (abuf[0] && rbuf[1] && *tbuf) {
      struct alias_data *temp;
      CREATE(temp, struct alias_data, 1);
      temp->alias       = strdup(abuf[0] == ' ' ? abuf+1 : abuf);
      temp->replacement = strdup(rbuf[1] == ' ' ? rbuf+1 : rbuf);
      temp->type        = tb_parse_int_or_zero(tbuf);
      temp->next        = GET_ALIASES(ch);
      GET_ALIASES(ch)   = temp;
    } else
      TB_WARN_MSG("player", "pfile-alias", filename,
                  "player idnum %ld alias %d of %d has an empty part; skipped",
                  GET_IDNUM(ch), i + 1, count);
  }
}
