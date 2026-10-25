/* ************************************************************************
*  file:  rebuildAsciiIndex.c                              Part of tbaMUD *
*  Copyright (C) 1990, 2010 - see 'license.doc' for complete information. *
*  All Rights Reserved                                                    *
************************************************************************* */

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

#define READ_SIZE 256

static void util_report_errno_at(const char *operation, const char *context,
                                 int errnum, const char *source_file,
                                 int source_line, const char *source_func)
{
  int caller_errno = errno;
  fprintf(stderr,
          "SYSERR: operation=%s context=%s error=\"%s\" errno=%d source=%s:%d function=%s\n",
          operation ? operation : "operation",
          context ? context : "(no context)",
          strerror(errnum), errnum,
          source_file ? source_file : "(unknown)",
          source_line,
          source_func ? source_func : "(unknown)");
  (void)fflush(stderr);
  errno = caller_errno;
}

#define UTIL_REPORT_ERRNO(operation, context) do { \
  int util_saved_errno_ = errno; \
  util_report_errno_at((operation), (context), util_saved_errno_, \
                       __FILE__, __LINE__, __func__); \
} while (0)


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
  return (value < INT_MIN || value > INT_MAX) ? 0 : (int)value;
}


void walkdir(FILE* index_file, char *dir);
int get_line(FILE *fl, char *buf);

int main(int argc, char** argv)
{
 	FILE *index_file;
  if ( argc == 1 )	{
	  printf("Usage: %s indexfile\n",argv[0]);
	  return 0;
 	}
 	if (!(index_file = fopen(argv[1], "w"))) {
    UTIL_REPORT_ERRNO("fopen", argv[1]);
    return 1;
  }
	walkdir(index_file, ".");

  if (fprintf(index_file, "~\n") < 0) UTIL_REPORT_ERRNO("fprintf", argv[1]);
  if (fclose(index_file) != 0) UTIL_REPORT_ERRNO("fclose", argv[1]);
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

long parseid(FILE *plr_file) {
  char *value = findLine(plr_file, "Id  :");
  return value ? parse_long_or_zero(value) : 0;
}

int parselevel(FILE *plr_file) {
  char *value = findLine(plr_file, "Levl:");
  return value ? parse_int_or_zero(value) : 0;
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
  char *value = findLine(plr_file, "Last:");
  return value ? parse_long_or_zero(value) : 0;
}


void walkdir(FILE *index_file, char *dir) {
 	char filename_qfd[1000] ;
	struct dirent *dp;
 	DIR *dfd;

 	if ((dfd = opendir(dir)) == NULL)
 	{
	  fprintf(stderr, "Can't open %s\n", dir);
	  return;
 	}
 	while ((dp = readdir(dfd)) != NULL)
 	{
	  struct stat stbuf ;
	  snprintf(filename_qfd, sizeof(filename_qfd), "%s/%s",dir,dp->d_name);
	  if( stat(filename_qfd,&stbuf ) == -1 ) {
   		if (fprintf(stdout, "Unable to stat file: %s\n", filename_qfd) < 0) UTIL_REPORT_ERRNO("fprintf", "stdout");
   		continue ;
  	}

  	if ( ( stbuf.st_mode & S_IFMT ) == S_IFDIR ) {
			if (!strcmp(".", dp->d_name) || !strcmp("..", dp->d_name))
   			continue;

   		walkdir(index_file, filename_qfd);
  	} else {
			char *name = parsename(dp->d_name);

			if (name != NULL) {
  			FILE *plr_file = fopen(filename_qfd, "r");
        if (!plr_file) {
          UTIL_REPORT_ERRNO("fopen", filename_qfd);
          continue;
        }
 				long id = parseid(plr_file);

  			int level = parselevel(plr_file);
 				int adminlevel = parseadminlevel(plr_file, level);
 				if (level > 30)
 					level = 30;
 				long last = parselast(plr_file);

 				fprintf(index_file, "%ld %s %d %d 0 %ld\n", id, name, level, adminlevel, last);

        if (fclose(plr_file) != 0) UTIL_REPORT_ERRNO("fclose", filename_qfd);
  		}
  	}
 	}
  if (closedir(dfd) != 0)
    UTIL_REPORT_ERRNO("closedir", dir);
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
