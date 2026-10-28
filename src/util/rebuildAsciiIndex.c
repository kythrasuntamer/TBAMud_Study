/* ************************************************************************
*  file:  rebuildAsciiIndex.c                              Part of tbaMUD *
*  Copyright (C) 1990, 2010 - see 'license.doc' for complete information. *
*  All Rights Reserved                                                    *
************************************************************************* */

#include "conf.h"
#include "sysdep.h"

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

#include "structs.h"	/* PLR_*, LVL_IMPL */
#include "utils.h"	/* ACMD, needed by db.h */
#include "db.h"		/* PINDEX_* */
#include "pfdefaults.h"	/* PFDEF_LEVEL */


#define UTIL_REPORT_ERRNO(operation, context) \
  TB_REPORT_ERRNO((operation), (context))


int walkdir(FILE* index_file, char *dir);

int main(int argc, char** argv)
{
 	FILE *index_file;
  int skipped;
  if ( argc == 1 )	{
	  printf("Usage: %s indexfile\n",argv[0]);
	  return 0;
 	}
 	if (!(index_file = fopen(argv[1], "w"))) {
    UTIL_REPORT_ERRNO("fopen", argv[1]);
    return 1;
  }
	skipped = walkdir(index_file, ".");

  if (fprintf(index_file, "~\n") < 0) UTIL_REPORT_ERRNO("fprintf", argv[1]);
  if (fclose(index_file) != 0) UTIL_REPORT_ERRNO("fclose", argv[1]);
  if (skipped > 0) {
    /* Each skipped pfile was reported where it was rejected.  A player left
     * out of the index is unknown to the server, and their name and idnum
     * can be taken by a new character, so the operator must look. */
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-index-incomplete",
                            "index=%s skipped=%d", argv[1], skipped);
    return 1;
  }
 	return 0;
}

char *parsename(char *filename) {
	static char copy[1024];
	strlcpy(copy, filename, sizeof(copy));
	char *extension = strchr(copy, '.');
	if (extension == NULL) {
		return NULL;
	}
	if (strcmp(".plr", extension)) {
		return NULL;
	}
	*extension = '\0';
	return copy;
}

/* The record-level tags the index needs, as text after "Tag:" (NULL when the
 * pfile has no such tag).  Each points into its own buffer below. */
struct pfile_tags {
  char *id, *levl, *last, *act;
  char id_buf[64], levl_buf[64], last_buf[64], act_buf[128];
};

/* Store one tag value; a second occurrence or an over-long value is
 * reported and rejects the pfile. */
static int pfile_store_tag(char **slot, char *buf, size_t buf_size,
                           const char *value, const char *path,
                           const char *tag, long line_no)
{
  if (*slot != NULL) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-duplicate-tag",
                            "pfile=%s tag=%s line=%ld: record skipped", path,
                            tag, line_no);
    return 0;
  }
  if (strlcpy(buf, value, buf_size) >= buf_size) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-value",
                            "pfile=%s tag=%s line=%ld: value longer than %zu "
                            "bytes; record skipped", path, tag, line_no,
                            buf_size - 1);
    return 0;
  }
  *slot = buf;
  return 1;
}

/* Number of lines a counted block ("Alis: N", "Vars: N") occupies after its
 * tag line, or -1 (reported) when N is not a sane count. */
static long pfile_block_lines(const char *value, long per_entry,
                              const char *path, const char *tag, long line_no)
{
  long count;

  if (!tb_parse_long_checked(value, &count) || count < 0 || count > 100000) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-value",
                            "pfile=%s tag=%s line=%ld value=%.32s: record "
                            "skipped", path, tag, line_no, value);
    return -1;
  }
  return count * per_entry;
}

/*
 * Read one pfile, collecting the record-level Id/Levl/Last/Act tags.
 *
 * Only tag lines at record level count, as load_char() (players.c) reads
 * them.  The "Desc:" block is player-written free text up to a line ending
 * in '~', and the "Alis:" (3 lines per alias) and "Vars:" (1 line per
 * variable) blocks hold player- or script-chosen text; a line inside them
 * that happens to read "Levl: 60" or "Id  : 1" must not be taken as the
 * player's level or idnum (an older first-match search let a mortal forge
 * both through their description).  A tag seen twice at record level, an
 * unterminated description or an over-long line is reported and rejects
 * the pfile.  Returns 1 on success.
 */
static int pfile_scan(FILE *plr_file, const char *path, struct pfile_tags *tags)
{
  char line[8192];
  long line_no = 0, skip_lines = 0;
  int in_desc = 0;

  memset(tags, 0, sizeof(*tags));
  while (fgets(line, sizeof(line), plr_file) != NULL) {
    size_t len = strlen(line);

    line_no++;
    if (len > 0 && line[len - 1] != '\n' && !feof(plr_file)) {
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-line-too-long",
                              "pfile=%s line=%ld limit=%zu: record skipped",
                              path, line_no, sizeof(line) - 1);
      return 0;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if (in_desc) {		/* player text: ends at a line ending in '~' */
      if (len > 0 && line[len - 1] == '~')
        in_desc = 0;
      continue;
    }
    if (skip_lines > 0) {	/* inside an Alis/Vars block */
      skip_lines--;
      continue;
    }
    if (len < 5 || line[4] != ':')	/* not a tag line (block data, blank) */
      continue;

    if (!strncmp(line, "Desc:", 5))
      in_desc = 1;
    else if (!strncmp(line, "Alis:", 5)) {
      if ((skip_lines = pfile_block_lines(line + 5, 3, path, "Alis", line_no)) < 0)
        return 0;
    } else if (!strncmp(line, "Vars:", 5)) {
      if ((skip_lines = pfile_block_lines(line + 5, 1, path, "Vars", line_no)) < 0)
        return 0;
    } else if (!strncmp(line, "Id  :", 5)) {
      if (!pfile_store_tag(&tags->id, tags->id_buf, sizeof(tags->id_buf),
                           line + 5, path, "Id", line_no))
        return 0;
    } else if (!strncmp(line, "Levl:", 5)) {
      if (!pfile_store_tag(&tags->levl, tags->levl_buf, sizeof(tags->levl_buf),
                           line + 5, path, "Levl", line_no))
        return 0;
    } else if (!strncmp(line, "Last:", 5)) {
      if (!pfile_store_tag(&tags->last, tags->last_buf, sizeof(tags->last_buf),
                           line + 5, path, "Last", line_no))
        return 0;
    } else if (!strncmp(line, "Act :", 5)) {
      if (!pfile_store_tag(&tags->act, tags->act_buf, sizeof(tags->act_buf),
                           line + 5, path, "Act", line_no))
        return 0;
    }
  }
  if (ferror(plr_file)) {
    UTIL_REPORT_ERRNO("fgets", path);
    return 0;
  }
  if (in_desc) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-desc-unterminated",
                            "pfile=%s: Desc block has no '~' line; record "
                            "skipped", path);
    return 0;
  }
  if (skip_lines > 0) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-block-truncated",
                            "pfile=%s missing=%ld: Alis/Vars block ends early; "
                            "record skipped", path, skip_lines);
    return 0;
  }
  return 1;
}

/* Convert a pfile "Act :" word (letters a-z = bits 0-25, A-Z = 26-51, or a
 * decimal number) to a bitvector, as db.c's asciiflag_conv() does; a letter
 * past the width of bitvector_t is ignored. */
static bitvector_t pfile_flag_conv(const char *flag)
{
  bitvector_t flags = 0;
  const char *p;
  long number;

  if (tb_parse_long_checked(flag, &number))
    return (bitvector_t)number;
  for (p = flag; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    int bit = -1;

    if (islower(ch))
      bit = ch - 'a';
    else if (isupper(ch))
      bit = 26 + (ch - 'A');
    if (bit >= 0 && bit < (int)(sizeof(bitvector_t) * CHAR_BIT))
      flags |= (bitvector_t)1 << bit;
  }
  return flags;
}

/* Index flag letters (PINDEX_*, see players.c build_player_index) for the
 * first 32-bit word of the pfile's player flags. */
static void pfile_index_flags(bitvector_t plr0, char *out, size_t out_size)
{
  static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEF";
  bitvector_t index_flags = 0;
  size_t i, j = 0;

  if (plr0 & ((bitvector_t)1 << PLR_DELETED))
    index_flags |= PINDEX_DELETED;
  if (plr0 & ((bitvector_t)1 << PLR_NODELETE))
    index_flags |= PINDEX_NODELETE;
  if (plr0 & ((bitvector_t)1 << PLR_NOWIZLIST))
    index_flags |= PINDEX_NOWIZLIST;

  for (i = 0; letters[i] != '\0' && j + 1 < out_size; i++)
    if (index_flags & ((bitvector_t)1 << i))
      out[j++] = letters[i];
  if (j == 0 && out_size > 1)
    out[j++] = '0';
  out[j] = '\0';
}

/*
 * Read the fields of one pfile that the player index needs and write the
 * index record.  Returns 1 when a record was written, 0 when the pfile was
 * reported and skipped.
 *
 * Output format is exactly what build_player_index() (players.c) parses:
 * "id name level flags last" -- five fields, nothing after "last".  The
 * level is written as stored: there is no admin level in this codebase and
 * immortal levels run to LVL_IMPL, so an older version's clamp to 30 and its
 * extra admin-level column demoted every immortal and made the server reject
 * the whole index.
 *
 * Who controls the input: pfiles are written by the server, but they are
 * text and may be hand-edited or corrupted; Id and Last are always written
 * by save_char(), Levl only when it differs from PFDEF_LEVEL.
 */
static int index_pfile(FILE *index_file, FILE *plr_file, const char *path,
                       const char *name)
{
  char *value, flag_text[64] = "0", index_flags[40];
  struct pfile_tags tags;
  long id, last, level = PFDEF_LEVEL;
  const char *p;

  /* The name comes from the file name and becomes a space-separated index
   * field, so it must be a single word of letters (as the server names
   * pfiles). */
  for (p = name; *p; p++)
    if (!isalpha((unsigned char)*p))
      break;
  if (*name == '\0' || *p != '\0' || p - name > MAX_NAME_LENGTH) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-name",
                            "pfile=%s: file name is not a player name; "
                            "record skipped", path);
    return 0;
  }

  if (!pfile_scan(plr_file, path, &tags))	/* reported there */
    return 0;

  if ((value = tags.id) == NULL) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-missing",
                            "pfile=%s tag=Id: record skipped", path);
    return 0;
  }
  if (!tb_parse_long_checked(value, &id) || id <= 0) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-value",
                            "pfile=%s tag=Id value=%.32s: record skipped",
                            path, value);
    return 0;
  }
  if ((value = tags.levl) != NULL &&
      !tb_parse_long_checked(value, &level)) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-value",
                            "pfile=%s idnum=%ld tag=Levl value=%.32s: record "
                            "skipped", path, id, value);
    return 0;
  }
  if (level < 0 || level > LVL_IMPL) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-level-range",
                            "pfile=%s idnum=%ld level=%ld range=0..%d: record "
                            "skipped", path, id, level, LVL_IMPL);
    return 0;
  }
  if ((value = tags.last) == NULL) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-missing",
                            "pfile=%s idnum=%ld tag=Last: record skipped",
                            path, id);
    return 0;
  }
  if (!tb_parse_long_checked(value, &last)) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-value",
                            "pfile=%s idnum=%ld tag=Last value=%.32s: record "
                            "skipped", path, id, value);
    return 0;
  }
  /* "Act : w0 w1 w2 w3": only word 0 holds the flags the index carries. */
  if ((value = tags.act) != NULL &&
      sscanf(value, "%63s", flag_text) != 1) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "pfile-tag-value",
                            "pfile=%s idnum=%ld tag=Act: record skipped",
                            path, id);
    return 0;
  }
  pfile_index_flags(pfile_flag_conv(flag_text), index_flags, sizeof(index_flags));

  if (fprintf(index_file, "%ld %s %ld %s %ld\n", id, name, level, index_flags,
              last) < 0)
    UTIL_REPORT_ERRNO("fprintf", "player index");
  return 1;
}


/* Index every *.plr file under dir.  Returns the number of pfiles or
 * directory entries that were reported and left out of the index. */
int walkdir(FILE *index_file, char *dir) {
 	char filename_qfd[1000] ;
	struct dirent *dp;
 	DIR *dfd;
  int skipped = 0;

 	if ((dfd = opendir(dir)) == NULL)
 	{
	  TB_REPORT_DOMAIN_ERRNO(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "opendir", dir);
	  return 1;
 	}
 	while ((dp = readdir(dfd)) != NULL)
 	{
	  struct stat stbuf ;
	  if (snprintf(filename_qfd, sizeof(filename_qfd), "%s/%s",dir,dp->d_name) >= (int)sizeof(filename_qfd)) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "path-build",
                            "dir=%.200s entry=%.200s: path exceeds %zu bytes",
                            dir, dp->d_name, sizeof(filename_qfd));
    skipped++;
    continue;
  }
	  if (lstat(filename_qfd, &stbuf) == -1) {
      UTIL_REPORT_ERRNO("lstat", filename_qfd);
      skipped++;
   		continue ;
  	}

    if (S_ISLNK(stbuf.st_mode)) {
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_WARN, TB_ERR_PERSISTENCE,
                              "pfile-symlink-skipped",
                              "path=%s: symbolic links are not followed",
                              filename_qfd);
      continue;
    }

  	if ( ( stbuf.st_mode & S_IFMT ) == S_IFDIR ) {
			if (!strcmp(".", dp->d_name) || !strcmp("..", dp->d_name))
   			continue;

   		skipped += walkdir(index_file, filename_qfd);
  	} else {
			char *name = parsename(dp->d_name);

			if (name != NULL) {
  			FILE *plr_file = fopen(filename_qfd, "r");
        if (!plr_file) {
          UTIL_REPORT_ERRNO("fopen", filename_qfd);
          skipped++;
          continue;
        }
        if (!index_pfile(index_file, plr_file, filename_qfd, name))
          skipped++;

        if (fclose(plr_file) != 0) UTIL_REPORT_ERRNO("fclose", filename_qfd);
  		}
  	}
 	}
  if (closedir(dfd) != 0) {
    UTIL_REPORT_ERRNO("closedir", dir);
    skipped++;
  }
  return skipped;
}
