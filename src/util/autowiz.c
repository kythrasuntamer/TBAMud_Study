/* ************************************************************************
*  file:  autowiz.c                                        Part of tbaMUD *
*  Usage: self-updating wizlists                                          *
*  Written by Jeremy Elson                                                *
*  All Rights Reserved                                                    *
*  Copyright (C) 1993 The Trustees of The Johns Hopkins University        *
************************************************************************* */

/*
 * autowiz <wizlev> <wizlistfile> <immlev> <immlistfile> [pid]
 *
 * Invoked by the server (run_autowiz() in limits.c, via "nice
 * ../bin/autowiz" from lib/) as:
 *   autowiz <CONFIG_MIN_WIZLIST_LEV> WIZLIST_FILE LVL_IMMORT IMMLIST_FILE <server pid>
 * and by hand the same way.  It reads plrfiles/index (relative to the
 * working directory, lib/), the "id name level flags last" records written
 * by save_player_index(), and writes two text lists: players at
 * wizlev..LVL_IMPL to wizlistfile, immlev..wizlev-1 to immlistfile,
 * skipping PINDEX_DELETED and PINDEX_NOWIZLIST.  With a pid it then sends
 * that process SIGUSR1 so the server reloads the lists.
 *
 * Failure: a missing index exits 1 before any list is touched.  A malformed
 * index record is reported (index path, line, field, idnum) and left out;
 * both lists are still written from the records that parsed (they are
 * truncated and rewritten only after the whole index was read), the server
 * is still signalled, and the exit status is 1 ("index-incomplete").  A
 * level argument that is not a whole number in 0..LVL_IMPL, or a pid that
 * is not a whole positive number, is reported ("autowiz-argument") and
 * exits 1 before any list is touched or any process signalled.
 */

#include "conf.h"
#include "sysdep.h"
#include <signal.h>
#include "structs.h"
#include "utils.h"
#include "db.h"

#define IMM_LMARG "   "
#define IMM_NSIZE  16
#define LINE_LEN   64
#define MIN_LEVEL LVL_IMMORT
/* sscanf widths for an index record's fields (buffers hold one more). */
#define FIELD_LEN       63
#define NAME_FIELD_LEN 127

/* max level that should be in columns instead of centered */
#define COL_LEVEL LVL_IMMORT

struct name_rec {
  char name[25];
  struct name_rec *next;
};

struct control_rec {
  int level;
  char *level_name;
};

struct level_rec {
  struct control_rec *params;
  struct level_rec *next;
  struct name_rec *names;
};

struct control_rec level_params[] =
{
  {LVL_IMMORT, "Immortals"},
  {LVL_GOD, "Gods"},
  {LVL_GRGOD, "Greater Gods"},
  {LVL_IMPL, "Implementors"},
  {0, ""}
};

struct level_rec *levels = 0;

void initialize(void)
{
  struct level_rec *tmp;
  int i = 0;

  while (level_params[i].level > 0) {
    tmp = (struct level_rec *) malloc(sizeof(struct level_rec));
    if (tmp == NULL) {
      TB_REPORT_FATAL_ERRNO(TB_ERR_INVARIANT, "malloc", "autowiz level_rec");
      exit(1);
    }
    tmp->names = 0;
    tmp->params = &(level_params[i++]);
    tmp->next = levels;
    levels = tmp;
  }
}

/* Index records rejected while reading.  main() still writes the lists from
 * the records that did parse, then exits 1 so the failure is not silent. */
static int rejected_records = 0;

/* Report one rejected index record (path, line, failing field, idnum text)
 * and count it.  Arguments are evaluated once. */
#define REJECT_RECORD(path, line_no, field, id_text) do { \
  TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "index-record-parse", \
                          "index=%s line=%ld field=%s id=%.20s", (path), \
                          (line_no), (field), (id_text)); \
  rejected_records++; \
} while (0)

/*
 * Read the player index written by save_player_index() (players.c): one
 * "id name level flags last" record per line, ending with a "~" line.  The
 * file is server-written but can be hand-edited or corrupted, so every record
 * that does not parse is reported with its line number and skipped.
 */
void read_file(void)
{
  void add_name(int level, char *name);
  char *CAP(char *txt);
  int read_index_line(FILE *fl, char *buf, size_t buf_size, const char *path,
               long *line_no);
  bitvector_t asciiflag_conv(char *flag);

  FILE *fl;
  int level = 0, got;
  bitvector_t flags = 0;
  long line_no = 0;
  char index_name[40], line[256], bits[FIELD_LEN + 1];
  char name[MAX_NAME_LENGTH + 1];

  tb_snprintf(index_name, sizeof(index_name), "%s%s", LIB_PLRFILES, INDEX_FILE);
  if (!(fl = fopen(index_name, "r"))) {
    TB_REPORT_ERRNO("fopen", index_name);
    exit(1);
  }

  while ((got = read_index_line(fl, line, sizeof(line), index_name, &line_no)) != 0) {
    if (got < 0) {		/* already reported by read_index_line() */
      rejected_records++;
      continue;
    }
    if (*line == '~')		/* end-of-index marker */
      continue;
    {
      char id_text[FIELD_LEN + 1] = "", level_text[FIELD_LEN + 1];
      char last_text[FIELD_LEN + 1], name_text[NAME_FIELD_LEN + 1];
      char extra[2], *end;
      long parsed;
      int fields = sscanf(line, TB_SCAN_STR(FIELD_LEN) " "
                          TB_SCAN_STR(NAME_FIELD_LEN) " "
                          TB_SCAN_STR(FIELD_LEN) " " TB_SCAN_STR(FIELD_LEN) " "
                          TB_SCAN_STR(FIELD_LEN) " %1s",
                          id_text, name_text, level_text, bits, last_text, extra);
      if (fields != 5) {
        REJECT_RECORD(index_name, line_no,
                      fields > 5 ? "trailing-text" : "field-count", id_text);
        continue;
      }
      if (strlen(name_text) >= sizeof(name)) {
        REJECT_RECORD(index_name, line_no, "name-length", id_text);
        continue;
      }
      tb_snprintf(name, sizeof(name), "%s", name_text);
      errno = 0;
      parsed = strtol(id_text, &end, 10);
      if (end == id_text || *end || errno == ERANGE) {
        REJECT_RECORD(index_name, line_no, "idnum", id_text);
        continue;
      }
      errno = 0;
      parsed = strtol(level_text, &end, 10);
      if (end == level_text || *end || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
        REJECT_RECORD(index_name, line_no, "level", id_text);
        continue;
      }
      level = (int)parsed;
      /* add_name() files a name under the highest bucket whose level does
       * not exceed it; a level above LVL_IMPL (or a negative one) cannot
       * come from the server and has no correct bucket. */
      if (level < 0 || level > LVL_IMPL) {
        TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "index-level-range",
                                "index=%s line=%ld id=%.20s level=%d "
                                "range=0..%d", index_name, line_no, id_text,
                                level, LVL_IMPL);
        rejected_records++;
        continue;
      }
      /* The name becomes wizlist text; the server only creates letter
       * names, so anything else means a corrupt or edited index. */
      {
        const char *ptr;

        for (ptr = name; *ptr && isalpha((unsigned char)*ptr); ptr++)
          ;
        if (*name == '\0' || *ptr != '\0') {
          REJECT_RECORD(index_name, line_no, "name-chars", id_text);
          continue;
        }
      }
      errno = 0;
      parsed = strtol(last_text, &end, 10);
      if (end == last_text || *end || errno == ERANGE) {
        REJECT_RECORD(index_name, line_no, "last", id_text);
        continue;
      }
      /* Timestamp validated; wizard listing does not use it. */
    }
    CAP(name);
    flags = asciiflag_conv(bits);
    if (level >= MIN_LEVEL &&
	!(IS_SET(flags, PINDEX_NOWIZLIST)) &&
	!(IS_SET(flags, PINDEX_DELETED)))
	add_name(level, name);
  }
  tb_fclose(fl);
}

/* File name under the level bucket it belongs to.  level is an int (it was a
 * signed `byte`, so index levels 128-255 wrapped negative and the bucket walk
 * below ran off the list).  read_file() has already checked
 * MIN_LEVEL <= level <= LVL_IMPL and that name is letters only. */
void add_name(int level, char *name)
{
  struct name_rec *tmp;
  struct level_rec *curr_level;

  curr_level = levels;
  while (curr_level && curr_level->params->level > level)
    curr_level = curr_level->next;
  if (curr_level == NULL) {
    /* Only reachable if read_file()'s range check and level_params[] drift
     * apart (a level below the lowest bucket). */
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_INVARIANT, "wizlist-bucket",
                            "name=%s level=%d: no wizlist bucket at or below "
                            "this level", name, level);
    rejected_records++;
    return;
  }

  tmp = (struct name_rec *) malloc(sizeof(struct name_rec));
  if (tmp == NULL) {
    TB_REPORT_FATAL_ERRNO(TB_ERR_INVARIANT, "malloc", "autowiz name_rec");
    exit(1);
  }
  strlcpy(tmp->name, name, sizeof(tmp->name));
  tmp->next = 0;

  tmp->next = curr_level->names;
  curr_level->names = tmp;
}

void sort_names(void)
{
  struct level_rec *curr_level;
  struct name_rec *a, *b;
  char temp[100];

  for (curr_level = levels; curr_level; curr_level = curr_level->next) {
    for (a = curr_level->names; a && a->next; a = a->next) {
      for (b = a->next; b; b = b->next) {
	if (strcmp(a->name, b->name) > 0) {
	  strlcpy(temp, a->name, sizeof(temp));
	  strlcpy(a->name, b->name, sizeof(a->name));
	  strlcpy(b->name, temp, sizeof(b->name));
	}
      }
    }
  }
}

void write_wizlist(FILE * out, int minlev, int maxlev)
{
  char buf[100];

  if (!out)
    return;
  struct level_rec *curr_level;
  struct name_rec *curr_name;
  int i, j;

  tb_fprintf(out,
"*******************************************************************************\n"
"*          The following people have reached immortality on tbaMUD.           *\n"
"*******************************************************************************\n\n");

  for (curr_level = levels; curr_level; curr_level = curr_level->next) {
    if (curr_level->params->level < minlev ||
	curr_level->params->level > maxlev)
      continue;
    i = 39 - ((int)strlen(curr_level->params->level_name) >> 1);
    for (j = 1; j <= i; j++)
      tb_fputc(' ', out);
    tb_fprintf(out, "%s\n", curr_level->params->level_name);
    for (j = 1; j <= i; j++)
      tb_fputc(' ', out);
    for (j = 1; j <= (int)strlen(curr_level->params->level_name); j++)
      tb_fputc('~', out);
    tb_fprintf(out, "\n");

    strlcpy(buf, "", sizeof(buf));
    curr_name = curr_level->names;
    while (curr_name) {
      strlcat(buf, curr_name->name, sizeof(buf));
      if (strlen(buf) > LINE_LEN) {
	if (curr_level->params->level <= COL_LEVEL)
	  tb_fprintf(out, IMM_LMARG);
	else {
	  i = 40 - (strlen(buf) >> 1);
	  for (j = 1; j <= i; j++)
	    tb_fputc(' ', out);
	}
	tb_fprintf(out, "%s\n", buf);
	strlcpy(buf, "", sizeof(buf));
      } else {
	if (curr_level->params->level <= COL_LEVEL) {
	  for (j = 1; j <= (IMM_NSIZE - (int)strlen(curr_name->name)); j++)
	    strlcat(buf, " ", sizeof(buf));
	}
	if (curr_level->params->level > COL_LEVEL)
	  strlcat(buf, "   ", sizeof(buf));
      }
      curr_name = curr_name->next;
    }

    if (*buf) {
      if (curr_level->params->level <= COL_LEVEL)
	tb_fprintf(out, "%s%s\n", IMM_LMARG, buf);
      else {
	i = 40 - (strlen(buf) >> 1);
	for (j = 1; j <= i; j++)
	  tb_fputc(' ', out);
	tb_fprintf(out, "%s\n", buf);
      }
    }
    tb_fprintf(out, "\n");
  }
}

int main(int argc, char **argv)
{
  int wizlevel, immlevel, pid = 0;
  FILE *fl;

  if (argc != 5 && argc != 6) {
    printf("Format: %s wizlev wizlistfile immlev immlistfile [pid to signal]\n",
	   argv[0]);
    exit(0);
  }
  /* The arguments come from the server (run_autowiz() in limits.c) or an
   * operator.  atoi-style parsing turned "abc" or an overflowing level into
   * 0 and quietly produced wrong lists (I2-26), so a level must be a whole
   * number in 0..LVL_IMPL and a pid a whole positive number; anything else
   * is reported and nothing is written or signalled. */
  if (!tb_parse_int_checked(argv[1], &wizlevel) || wizlevel < 0 ||
      wizlevel > LVL_IMPL) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "autowiz-argument",
                            "arg=wizlev text=%.40s range=0..%d",
                            argv[1], LVL_IMPL);
    return (1);
  }
  if (!tb_parse_int_checked(argv[3], &immlevel) || immlevel < 0 ||
      immlevel > LVL_IMPL) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "autowiz-argument",
                            "arg=immlev text=%.40s range=0..%d",
                            argv[3], LVL_IMPL);
    return (1);
  }

#ifdef CIRCLE_UNIX	/* Perhaps #ifndef CIRCLE_WINDOWS but ... */
  if (argc == 6 && (!tb_parse_int_checked(argv[5], &pid) || pid <= 0)) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "autowiz-argument",
                            "arg=pid text=%.40s range=1..%d", argv[5],
                            INT_MAX);
    return (1);
  }
#endif

  initialize();
  read_file();
  sort_names();

  fl = fopen(argv[2], "w");
  if (!fl) {
    TB_REPORT_ERRNO("fopen", argv[2]);
    return 1;
  }
  write_wizlist(fl, wizlevel, LVL_IMPL);
  tb_fclose(fl);

  fl = fopen(argv[4], "w");
  if (!fl) {
    TB_REPORT_ERRNO("fopen", argv[4]);
    return 1;
  }
  write_wizlist(fl, immlevel, wizlevel - 1);
  tb_fclose(fl);

#ifdef CIRCLE_UNIX	/* ... I don't have the platforms to test. */
  if (pid)
    kill(pid, SIGUSR1);
#endif

  /* The lists above hold every record that parsed; a non-zero exit tells the
   * operator that at least one index record was reported and left out. */
  if (rejected_records > 0) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "index-incomplete",
                            "rejected=%d wizlist=%s immlist=%s",
                            rejected_records, argv[2], argv[4]);
    return (1);
  }
  return (0);
}

char *CAP(char *txt)
{
  *txt = UPPER(*txt);
  return (txt);
}

/* read_index_line reads the next non-blank line off of the input stream into buf.
 * buf_size must be the caller's real array size: sizeof() of a pointer
 * parameter here would be 8 and cut every record (the old bug).  The newline
 * character is removed.  Lines which begin with '*' are considered to be
 * comments.  *line_no is advanced by every physical line consumed so callers
 * can name the line in diagnostics.  Returns 1 for a line, 0 at end of file,
 * and -1 for a line that was reported here and must be skipped. */
int read_index_line(FILE * fl, char *buf, size_t buf_size, const char *path,
             long *line_no)
{
  char temp[256];
  size_t len;

  do {
    if (fgets(temp, sizeof(temp), fl) == NULL)
      return (0);
    (*line_no)++;
  } while (*temp == '*' || *temp == '\n');

  len = strlen(temp);
  /* fgets() filled temp without reaching a newline: the record is longer
   * than any the server writes.  Drain the rest of the physical line so the
   * next call starts on a record boundary, and reject this one. */
  if (len > 0 && temp[len - 1] != '\n' && !feof(fl)) {
    int ch;

    while ((ch = fgetc(fl)) != EOF && ch != '\n')
      ;
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "index-line-too-long",
                            "index=%s line=%ld limit=%zu", path, *line_no,
                            sizeof(temp) - 1);
    return (-1);
  }
  /* Strip only real line endings.  A line that begins with a NUL byte has
   * length 0 (the old `temp[strlen(temp) - 1]` wrote temp[-1]), and a last
   * line without '\n' must keep its final character. */
  while (len > 0 && (temp[len - 1] == '\n' || temp[len - 1] == '\r'))
    temp[--len] = '\0';
  if (strlcpy(buf, temp, buf_size) >= buf_size) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "index-line-truncated",
                            "index=%s line=%ld length=%zu buffer=%zu", path,
                            *line_no, strlen(temp), buf_size);
    return (-1);
  }
  return (1);
}

/* Convert the index "flags" field (letters a-z = bits 0-25, A-Z = 26-51, or
 * a decimal number) to a bitvector, as db.c's asciiflag_conv() does.  The
 * text comes from the index file, so a letter whose bit does not fit
 * bitvector_t is reported ("ascii-flag-range") and ignored instead of being
 * an undefined shift. */
bitvector_t asciiflag_conv(char *flag)
{
  bitvector_t flags = 0;
  int is_number = 1;
  register char *p;

  for (p = flag; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    int bit = -1;

    if (islower(ch))
      bit = ch - 'a';
    else if (isupper(ch))
      bit = 26 + (ch - 'A');
    if (bit >= (int)(sizeof(bitvector_t) * CHAR_BIT))
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "ascii-flag-range",
                              "flags=%.64s letter=%c bit=%d width=%zu", flag,
                              (char)ch, bit, sizeof(bitvector_t) * CHAR_BIT);
    else if (bit >= 0)
      flags |= (bitvector_t)1 << bit;

    if (!isdigit(ch))
      is_number = 0;
  }

  if (is_number)
    flags = tb_parse_long_or_zero(flag);

  return (flags);
}
