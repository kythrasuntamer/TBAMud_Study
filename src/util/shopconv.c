/* ************************************************************************
*  file:  shopconv.c                                       Part of tbaMUD *
*  Usage: code to convert 2.20 shop files to 3.0 shop files               *
*  Written by Jeff Fink                                                   *
************************************************************************* */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "db.h"
#include "shop.h"

static int parse_int_text(const char *text, int *out)
{
  char *end;
  long value;

  if (text == NULL || out == NULL)
    return 0;

  while (isspace((unsigned char)*text))
    text++;

  errno = 0;
  value = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
    return 0;

  while (isspace((unsigned char)*end))
    end++;
  if (*end != '\0')
    return 0;

  *out = (int)value;
  return 1;
}

static int read_int_token(FILE *stream, int *out)
{
  char token[64];

  if (fscanf(stream, "%63s", token) != 1)
    return 0;
  return parse_int_text(token, out);
}

static int read_float_token(FILE *stream, float *out)
{
  char token[64], *end;
  float value;

  if (fscanf(stream, "%63s", token) != 1)
    return 0;

  errno = 0;
  value = strtof(token, &end);
  if (end == token || errno == ERANGE || *end != '\0')
    return 0;

  *out = value;
  return 1;
}


void basic_mud_log(const char *x, ...)
{
  puts(x);
}

char *fread_string(FILE * fl, const char *error)
{
  char buf[MAX_STRING_LENGTH], tmp[512], *rslt, *point;
  int flag;

  *buf = '\0';

  do {
    if (!fgets(tmp, sizeof(tmp), fl)) {
      printf("fread_string: format error at or near %s\n", error);
      return NULL;
    }
    if (strlen(tmp) + strlen(buf) >= sizeof(buf)) {
      printf("SYSERR: fread_string: string too large (shopconv.c)\n");
      return NULL;
    }
    strlcat(buf, tmp, sizeof(buf));

    /* Walk backward from the end without ever forming a pointer before buf. */
    point = buf + strlen(buf);
    while (point > buf && isspace((unsigned char)*(point - 1)))
      point--;

    flag = point > buf && *(point - 1) == '~';
    if (flag)
      *(point - 1) = '\0';
  } while (!flag);

  {
    size_t result_size = strlen(buf) + 1;
    CREATE(rslt, char, result_size);
    memcpy(rslt, buf, result_size);
  }
  return rslt;
}

void do_list(FILE * shop_f, FILE * newshop_f, int max)
{
  int count, temp;
  char buf[MAX_STRING_LENGTH];

  for (count = 0; count < max; count++) {
    if (!read_int_token(shop_f, &temp) ||
        fgets(buf, MAX_STRING_LENGTH - 1, shop_f) == NULL) {
      tb_fprintf(stderr, "shopconv: malformed product list\n");
      exit(1);
    }
    if (temp > 0)
      tb_fprintf(newshop_f, "%d%s", temp, buf);
  }

  tb_fprintf(newshop_f, "-1\n");
}


void do_float(FILE * shop_f, FILE * newshop_f)
{
  float f;
  char str[20];
  if (!read_float_token(shop_f, &f)) {
    tb_fprintf(stderr, "shopconv: malformed floating-point field\n");
    exit(1);
  }
  tb_snprintf(str, sizeof(str), "%f", f);
  while ((str[strlen(str) - 1] == '0') && (str[strlen(str) - 2] != '.'))
    str[strlen(str) - 1] = 0;
  tb_fprintf(newshop_f, "%s \n", str);
}


void do_int(FILE * shop_f, FILE * newshop_f)
{
  int i;

  if (!read_int_token(shop_f, &i)) {
    tb_fprintf(stderr, "shopconv: malformed integer field\n");
    exit(1);
  }
  tb_fprintf(newshop_f, "%d \n", i);
}


void do_string(FILE * shop_f, FILE * newshop_f, char *msg)
{
  char *ptr;

  ptr = fread_string(shop_f, msg);
  tb_fprintf(newshop_f, "%s~\n", ptr);
  free(ptr);
}


static int boot_the_shops_conv(FILE * shop_f, FILE * newshop_f, char *filename)
{
  char *buf, buf2[150];
  int temp, count;

  tb_snprintf(buf2, sizeof(buf2), "beginning of shop file %s", filename);
  tb_fprintf(newshop_f, "CircleMUD %s Shop File~\n", VERSION3_TAG);
  for (;;) {
    buf = fread_string(shop_f, buf2);
    if (*buf == '#') {		/* New shop */
      if (*buf != '#' || !parse_int_text(buf + 1, &temp)) {
        tb_fprintf(stderr, "shopconv: malformed shop header: %s\n", buf);
        free(buf);
        return 1;
      }
      tb_snprintf(buf2, sizeof(buf2), "shop #%d in shop file %s", temp, filename);
      tb_fprintf(newshop_f, "#%d~\n", temp);
      free(buf);		/* Plug memory leak! */
      printf("   #%d\n", temp);

      do_list(shop_f, newshop_f, MAX_PROD);	/* Produced Items */

      do_float(shop_f, newshop_f);	/* Ratios */
      do_float(shop_f, newshop_f);

      do_list(shop_f, newshop_f, MAX_TRADE);	/* Bought Items */

      for (count = 0; count < 7; count++)	/* Keeper msgs */
	do_string(shop_f, newshop_f, buf2);

      for (count = 0; count < 5; count++)	/* Misc   */
	do_int(shop_f, newshop_f);
      tb_fprintf(newshop_f, "-1\n");
      for (count = 0; count < 4; count++)	/* Open/Close     */
	do_int(shop_f, newshop_f);

    } else {
      if (*buf == '$') {	/* EOF */
	free(buf);		/* Plug memory leak! */
	tb_fprintf(newshop_f, "$~\n");
	break;
      } else if (strstr(buf, VERSION3_TAG)) {
	printf("%s: New format detected, conversion aborted!\n", filename);
	free(buf);		/* Plug memory leak! */
	return (1);
      } else {
        free(buf);
      }
    }
  }
  return (0);
}

int main(int argc, char *argv[])
{
  FILE *sfp, *nsfp;
  char fn[120], part[256], backup[256];
  int result, index;

  if (argc < 2) {
    printf("Usage: shopconv <file1> [file2] [file3] ...\n");
    exit(1);
  }
  for (index = 1; index < argc; index++) {
    if (snprintf(fn, sizeof(fn), "%s", argv[index]) >= (int)sizeof(fn)) {
      tb_fprintf(stderr, "shopconv: filename too long: %s\n", argv[index]);
      continue;
    }
    if (snprintf(part, sizeof(part), "%s.tmp", fn) >= (int)sizeof(part)) {
      tb_fprintf(stderr, "shopconv: temporary filename too long: %s\n", fn);
      continue;
    }
    if (rename(fn, part) != 0) {
      tb_fprintf(stderr, "shopconv: failed to rename %s to %s: %s\n",
              fn, part, strerror(errno));
      exit(1);
    }
    sfp = fopen(part, "r");
    if (sfp == NULL) {
      int saved_errno = errno;
      tb_fprintf(stderr, "shopconv: %s could not be opened: %s\n", part, strerror(saved_errno));
      if (rename(part, fn) != 0)
        tb_fprintf(stderr, "shopconv: failed to restore %s from temporary file: %s\n",
                fn, strerror(errno));
    } else {
      if ((nsfp = fopen(fn, "w")) == NULL) {
	      printf("Error writing to %s.\n", fn);
        tb_fclose(sfp);
        if (rename(part, fn) != 0)
          tb_fprintf(stderr, "shopconv: failed to restore %s from temporary file: %s\n",
                  fn, strerror(errno));
	      continue;
      }
      printf("%s:\n", fn);
      result = boot_the_shops_conv(sfp, nsfp, fn);
      tb_fclose(nsfp);
      tb_fclose(sfp);
      if (result) {
	      if (rename(part, fn) != 0) {
	        tb_fprintf(stderr, "shopconv: failed to restore %s from temporary file: %s\n",
	                fn, strerror(errno));
	        exit(1);
	      }
      } else {
	      if (snprintf(backup, sizeof(backup), "%s.bak", fn) >= (int)sizeof(backup)) {
	        tb_fprintf(stderr, "shopconv: backup filename too long: %s\n", fn);
	        exit(1);
	      }
	      if (rename(part, backup) != 0) {
	        tb_fprintf(stderr, "shopconv: failed to archive temporary file for %s: %s\n",
	                fn, strerror(errno));
	        exit(1);
	      }
	      printf("Done!\n");
      }
    }
  }

  return (0);
}
