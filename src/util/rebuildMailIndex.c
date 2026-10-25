/* ************************************************************************
*  file:  rebuildMailIndex.c                               Part of tbaMUD *
*  Copyright (C) 1990, 2010 - see 'license.doc' for complete information. *
*  All Rights Reserved                                                    *
************************************************************************* */

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

#define READ_SIZE 256

static long parse_long_or_zero(const char *text)
{
  char *end = NULL;
  long value;

  if (text == NULL)
    return 0;

  errno = 0;
  value = strtol(text, &end, 10);
  if (end == text || errno == ERANGE)
    return 0;

  return value;
}

static int parse_int_or_zero(const char *text)
{
  long value = parse_long_or_zero(text);

  if (value < INT_MIN || value > INT_MAX)
    return 0;

  return (int)value;
}

#define FLAG(n) (1 << (n))

#ifndef FALSE
typedef enum boolean_type {
  FALSE=0, TRUE
} bool;
#endif

/* 128-bit flag defines (from utils.h) */
#define Q_FIELD(x)  ((int) (x) / 32)
#define Q_BIT(x)    (1 << ((x) % 32))
#define IS_SET_AR(var, bit)       ((var)[Q_FIELD(bit)] & Q_BIT(bit))
#define SET_BIT_AR(var, bit)      ((var)[Q_FIELD(bit)] |= Q_BIT(bit))

/* 32-bit flag defines (from utils.h) */
#define IS_SET(flag,bit)  ((flag) & (bit))
#define SET_BIT(var,bit)  ((var) |= (bit))

/* Mail index flags - taken from mail.h */
#define MINDEX_DELETED   FLAG(0)  /* Mail has been marked for deletion     */
#define MINDEX_URGENT    FLAG(1)  /* Mail is flagged as urgent by sender   */
#define MINDEX_HAS_OBJ   FLAG(2)  /* Mail has an attached object           */
#define MINDEX_HAS_GOLD  FLAG(3)  /* Mail contains some gold coins         */
#define MINDEX_IS_COD    FLAG(4)  /* Mail requires some gold coins         */
#define MINDEX_FROM_MOB  FLAG(5)  /* Mail has been sent by using scripts   */
#define MINDEX_READ      FLAG(6)  /* Mail has been viewed but not received */
#define MINDEX_DRAFT     FLAG(7)  /* Mail is an unsent draft copy          */

/* Mail Flags - taken from mail.h */
#define MAIL_DELETED  1  /* Marks mail for deletion with next purge */
#define MAIL_URGENT   2  /* This mail is flagged as urgent?         */
#define MAIL_COD      3  /* Means coins amount is required payment  */
#define MAIL_FROM_MOB 4  /* Specifies that 'sender' is a mob vnum   */
#define MAIL_READ     5  /* Mail has been read by recipient         */
#define MAIL_DRAFT    6  /* Mail is a draft (not yet sent)          */

void walkdir(FILE* index_file, char *dir);
int get_line(FILE *fl, char *buf);
long asciiflag_conv(char *flag);
int sprintascii(char *out, long bits);

int main(int argc, char** argv)
{
  FILE *index_file;
  if ( argc == 1 )	{
    printf("Usage: %s indexfile\n",argv[0]);
    return 0;
  }
  if (!(index_file = fopen(argv[1], "w"))) {
    TB_REPORT_ERRNO("fopen", argv[1]);
    return 1;
  }

  if (fprintf(index_file, "# tbaMUD mail index\n") < 0) TB_REPORT_ERRNO("fprintf", argv[1]);
  if (fprintf(index_file, "# Format: <ID> <flags> <sender ID/vnum> <recipient ID> <time> <subject>\n") < 0) TB_REPORT_ERRNO("fprintf", argv[1]);
  if (fprintf(index_file, "# For 'No Subject', use (null)\n") < 0) TB_REPORT_ERRNO("fprintf", argv[1]);

  walkdir(index_file, ".");

  if (fprintf(index_file, "~\n") < 0) TB_REPORT_ERRNO("fprintf", argv[1]);
  if (fclose(index_file) != 0) TB_REPORT_ERRNO("fclose", argv[1]);
  return 0;
}

/* check that filename is a valid mail file, else return NULL */
char *parsefilename(char *filename) {
  static char copy[1024];
  char *extension;

  strlcpy(copy, filename, sizeof(copy));
  extension = strchr(copy, '.');
  if (extension == NULL) {
    return NULL;
  }
  if (strcmp(".ml", extension)) {
    return NULL;
  }
  *extension = '\0';
  return copy;
}

/* Search file for a specific tag line, return text after tag, or NULL if not found */
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
/* Search file for mail ID and convert to long */
long parse_mailid(FILE *plr_file) {
  char *value = findLine(plr_file, "MlID:");
  return value ? parse_long_or_zero(value) : 0;
}

/* Search file for sender ID and convert to long */
long parse_sender(FILE *plr_file) {
  char *value = findLine(plr_file, "Send:");
  return value ? parse_long_or_zero(value) : 0;
}

/* Search file for recipient ID and convert to long */
long parse_recipient(FILE *plr_file) {
  char *value = findLine(plr_file, "Reci:");
  return value ? parse_long_or_zero(value) : 0;
}

/* Search file for date/time mail was sent and convert to long */
long parse_send_time(FILE *plr_file) {
  char *value = findLine(plr_file, "Sent:");
  return value ? parse_long_or_zero(value) : 0;
}

/* Search file for mail subject and return as string */
char *parse_subject(FILE *plr_file) {
	static char subj[1000];
	char *txt = findLine(plr_file, "Subj:");
	snprintf(subj, sizeof(subj), "%s", (txt == NULL) ? "(null)" : txt);
	return (subj);
}

/* Search file for mail flags and return as bitvector */
int parse_mail_flags(FILE *plr_file) {
  int fl[4], ret=0;
  char *txt, f1[33], f2[33], f3[33], f4[33];

  if ((txt = findLine(plr_file, "Flag:")) != NULL) {
    /* Read the flags */
    if (sscanf(txt, "%s %s %s %s", f1, f2, f3, f4) == 4) {
      fl[0] = asciiflag_conv(f1);
      fl[1] = asciiflag_conv(f2);
      fl[2] = asciiflag_conv(f3);
      fl[3] = asciiflag_conv(f4);

      /* convert from mail flags to mail index flags */
      if (IS_SET_AR(fl, MAIL_DELETED))  SET_BIT(ret, MINDEX_DELETED);
      if (IS_SET_AR(fl, MAIL_URGENT))   SET_BIT(ret, MINDEX_URGENT);
      if (IS_SET_AR(fl, MAIL_COD))      SET_BIT(ret, MINDEX_IS_COD);
      if (IS_SET_AR(fl, MAIL_FROM_MOB)) SET_BIT(ret, MINDEX_FROM_MOB);
      if (IS_SET_AR(fl, MAIL_READ))     SET_BIT(ret, MINDEX_READ);
      if (IS_SET_AR(fl, MAIL_DRAFT))    SET_BIT(ret, MINDEX_DRAFT);
    }
  }
  if ((txt = findLine(plr_file, "Gold:")) != NULL) {
    if (parse_long_or_zero(txt) > 0) SET_BIT(ret, MINDEX_HAS_GOLD);
  }
  if ((txt = findLine(plr_file, "Objs:")) != NULL) {
    SET_BIT(ret, MINDEX_HAS_OBJ);
  }
  return (ret);
}

int parseadminlevel(FILE *plr_file, int level) {
	char *fromFile = findLine(plr_file, "Admn:");
	if (fromFile != NULL)
		return parse_int_or_zero(fromFile);

	if (level >= 30)
		return level-30;
	else
		return 0;
}

long parselast(FILE *plr_file) {
	return parse_long_or_zero(findLine(plr_file, "Last:"));
}


void walkdir(FILE *index_file, char *dir) {
  char filename_qfd[1000], *subject, bits[65];
  struct dirent *dp;
  long id, sender, recipient, sent_time;
  int flags;
  DIR *dfd;
  FILE *mail_file;

  if ((dfd = opendir(dir)) == NULL)
  {
    fprintf(stderr, "Can't open %s\n", dir);
    return;
  }
  while ((dp = readdir(dfd)) != NULL)
  {
    struct stat stbuf ;
    if (snprintf(filename_qfd, sizeof(filename_qfd), "%s/%s", dir, dp->d_name) >= (int)sizeof(filename_qfd)) {
    if (fprintf(stderr, "Path too long: %s/%s\n", dir, dp->d_name) < 0)
      TB_REPORT_ERRNO("fprintf", "stderr");
    continue;
  }

    if( stat(filename_qfd,&stbuf ) == -1 ) {
      if (fprintf(stdout, "Unable to stat file: %s\n", filename_qfd) < 0) TB_REPORT_ERRNO("fprintf", "stdout");
      continue ;
    }

    if ( ( stbuf.st_mode & S_IFMT ) == S_IFDIR ) {
      if (!strcmp(".", dp->d_name) || !strcmp("..", dp->d_name))
        continue;

      walkdir(index_file, filename_qfd);
  	} else {
      char *name = parsefilename(dp->d_name);

      if (name != NULL) {
        /* Grab the data from the mail file and throw it in the index */
        mail_file = fopen(filename_qfd, "r");
        if (!mail_file) {
          TB_REPORT_ERRNO("fopen", filename_qfd);
          continue;
        }

        id        = parse_mailid(mail_file);
        sender    = parse_sender(mail_file);
        recipient = parse_recipient(mail_file);
        sent_time = parse_send_time(mail_file);
        flags     = parse_mail_flags(mail_file);
        subject   = parse_subject(mail_file);

        sprintascii(bits, flags);

        if (fprintf(index_file, "%ld %s %ld %ld %ld %s\n", id, *bits ? bits : "0", sender, recipient, sent_time, subject) < 0) TB_REPORT_ERRNO("fprintf", argv[1]);

        if (fclose(mail_file) != 0) TB_REPORT_ERRNO("fclose", filename_qfd);
      }
    }
  }
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

long asciiflag_conv(char *flag)
{
  long flags = 0;
  int is_num = TRUE;
  char *p;

  for (p = flag; *p; p++) {
    if (islower(*p))
      flags |= 1 << (*p - 'a');
    else if (isupper(*p))
      flags |= 1 << (26 + (*p - 'A'));

    /* Allow the first character to be a minus sign */
    if (!isdigit(*p) && (*p != '-' || p != flag))
      is_num = FALSE;
  }

  if (is_num)
    flags = parse_long_or_zero(flag);

  return (flags);
}

int sprintascii(char *out, long bits)
{
  int i, j = 0;
  /* 32 bits, don't just add letters to try to get more unless your bitvector_t is also as large. */
  char *flags = "abcdefghijklmnopqrstuvwxyzABCDEF";

  for (i = 0; flags[i] != '\0'; i++)
    if (bits & (1 << i))
      out[j++] = flags[i];

  if (j == 0) /* Didn't write anything. */
    out[j++] = '0';

  /* NUL terminate the output string. */
  out[j++] = '\0';
  return j;
}
