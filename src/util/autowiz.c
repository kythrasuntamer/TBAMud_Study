/* ************************************************************************
*  file:  autowiz.c                                        Part of tbaMUD *
*  Usage: self-updating wizlists                                          *
*  Written by Jeremy Elson                                                *
*  All Rights Reserved                                                    *
*  Copyright (C) 1993 The Trustees of The Johns Hopkins University        *
************************************************************************* */

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
    tmp->names = 0;
    tmp->params = &(level_params[i++]);
    tmp->next = levels;
    levels = tmp;
  }
}

void read_file(void)
{
  void add_name(byte level, char *name);
  char *CAP(char *txt);
  int get_line(FILE * fl, char *buf);
  bitvector_t asciiflag_conv(char *flag);

  FILE *fl;
  int recs, i, level = 0, flags = 0;
  char index_name[40], line[256], bits[64];
  char name[MAX_NAME_LENGTH];

  tb_snprintf(index_name, sizeof(index_name), "%s%s", LIB_PLRFILES, INDEX_FILE);
  if (!(fl = fopen(index_name, "r"))) {
    TB_REPORT_ERRNO("fopen", index_name);
    exit(1);
  }
  /* count the number of players in the index */
  recs = 0;
  while (get_line(fl, line))
    if (*line != '~')
      recs++;
  if (fseek(fl, 0L, SEEK_SET) != 0) {
    TB_REPORT_ERRNO("fseek", index_name);
    tb_fclose(fl);
    exit(1);
  }

  for (i = 0; i < recs; i++) {
    get_line(fl, line);
    {
      char id_text[64], level_text[64], last_text[64], name_text[128], extra[2], *end;
      long parsed;
      if (sscanf(line, "%63s %127s %63s %63s %63s %1s",
                 id_text, name_text, level_text, bits, last_text, extra) != 5)
        continue;
      if (strlen(name_text) >= sizeof(name))
        continue;
      tb_snprintf(name, sizeof(name), "%s", name_text);
      errno = 0;
      parsed = strtol(id_text, &end, 10);
      if (end == id_text || *end || errno == ERANGE)
        continue;
      errno = 0;
      parsed = strtol(level_text, &end, 10);
      if (end == level_text || *end || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
        continue;
      level = (int)parsed;
      errno = 0;
      parsed = strtol(last_text, &end, 10);
      if (end == last_text || *end || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
        continue;
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

void add_name(byte level, char *name)
{
  struct name_rec *tmp;
  struct level_rec *curr_level;
  char *ptr;

  if (!*name)
    return;

  for (ptr = name; *ptr; ptr++)
    if (!isalpha(*ptr))
      return;

  tmp = (struct name_rec *) malloc(sizeof(struct name_rec));
  strlcpy(tmp->name, name, sizeof(tmp->name));
  tmp->next = 0;

  curr_level = levels;
  while (curr_level->params->level > level)
    curr_level = curr_level->next;

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
  wizlevel = tb_parse_int_or_zero(argv[1]);
  immlevel = tb_parse_int_or_zero(argv[3]);

#ifdef CIRCLE_UNIX	/* Perhaps #ifndef CIRCLE_WINDOWS but ... */
  if (argc == 6)
    pid = tb_parse_int_or_zero(argv[5]);
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

  return (0);
}

char *CAP(char *txt)
{
  *txt = UPPER(*txt);
  return (txt);
}

/* get_line reads the next non-blank line off of the input stream. The newline 
 * character is removed from the input.  Lines which begin with '*' are 
 * considered to be comments. Returns the number of lines advanced in the 
 * file. */
int get_line(FILE * fl, char *buf)
{
  char temp[256];
  int lines = 0;

  do {
    if (fgets(temp, sizeof(temp), fl) == NULL)
      return (0);
    lines++;
  } while (*temp == '*' || *temp == '\n');

  temp[strlen(temp) - 1] = '\0';
  strlcpy(buf, temp, sizeof(buf));
  return (lines);
}

bitvector_t asciiflag_conv(char *flag)
{
  bitvector_t flags = 0;
  int is_number = 1;
  register char *p;

  for (p = flag; *p; p++) {
    if (islower(*p))
      flags |= 1 << (*p - 'a');
    else if (isupper(*p))
     flags |= 1 << (26 + (*p - 'A'));

    if (!isdigit(*p))
      is_number = 0;
  }

  if (is_number)
    flags = tb_parse_long_or_zero(flag);

  return (flags);
}
