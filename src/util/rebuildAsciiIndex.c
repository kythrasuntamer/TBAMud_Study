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

#define READ_SIZE 256

#define UTIL_REPORT_ERRNO(operation, context) \
  TB_REPORT_ERRNO((operation), (context))


int walkdir(FILE* index_file, char *dir);
int get_line(FILE *fl, char *buf);

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

char *findLine(FILE *plr_file, char *tag) {
	static char line[5000];
	if (fseek(plr_file, 0L, SEEK_SET) != 0)
		return NULL;

	while (get_line(plr_file, line)) {
		if(!strncmp(tag, line, strlen(tag))) {
			return line+strlen(tag);
		}
	}
	return NULL;
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

  if ((value = findLine(plr_file, "Id  :")) == NULL) {
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
  if ((value = findLine(plr_file, "Levl:")) != NULL &&
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
  if ((value = findLine(plr_file, "Last:")) == NULL) {
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
  if ((value = findLine(plr_file, "Act :")) != NULL &&
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

int get_line(FILE *fl, char *buf)
{
  char temp[READ_SIZE];
  int lines = 0;
  int sl;

  do {
    if (!fgets(temp, READ_SIZE, fl))
      return (0);
    lines++;
  } while (*temp == '*' || *temp == '\n' || *temp == '\r');

  /* Last line of file doesn't always have a \n, but it should. */
  sl = strlen(temp);
  while (sl > 0 && (temp[sl - 1] == '\n' || temp[sl - 1] == '\r'))
    temp[--sl] = '\0';

  memcpy(buf, temp, strlen(temp) + 1);
  return (lines);
}
