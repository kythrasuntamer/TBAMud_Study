/* ************************************************************************
*  file:  shopconv.c                                       Part of tbaMUD *
*  Usage: code to convert 2.20 shop files to 3.0 shop files               *
*  Written by Jeff Fink                                                   *
************************************************************************* */

#include "conf.h"
#include "sysdep.h"
#include <math.h>	/* isfinite */

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

  while (*text == ' ' || *text == '\t' || *text == '\n' ||
         *text == '\r' || *text == '\f' || *text == '\v')
    text++;

  errno = 0;
  value = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
    return 0;

  while (strchr(" \t\r\n\f\v", (unsigned char)*end) && *end)
    end++;
  if (*end != '\0')
    return 0;

  *out = (int)value;
  return 1;
}

/* One whitespace-delimited numeric token from the shop file (TOKEN_LEN
 * bytes at most; a longer one is split and then fails to parse). */
#define TOKEN_LEN 63

static int read_int_token(FILE *stream, int *out)
{
  char token[TOKEN_LEN + 1];

  if (fscanf(stream, TB_SCAN_STR(TOKEN_LEN), token) != 1)
    return 0;
  return parse_int_text(token, out);
}

static int read_float_token(FILE *stream, float *out)
{
  char token[TOKEN_LEN + 1], *end;
  float value;

  if (fscanf(stream, TB_SCAN_STR(TOKEN_LEN), token) != 1)
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
      TB_REPORT_DOMAIN_ERROR(TB_SEV_ERROR, TB_ERR_PARSER,
                             "fread_string",
                             "shop string exceeds MAX_STRING_LENGTH");
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
    /* Standalone converter: do not call the MUD-only allocation helper. */
    rslt = (char *)calloc(result_size, sizeof(*rslt));
    if (rslt == NULL) {
      TB_REPORT_DOMAIN_ERRNO(TB_SEV_ERROR, TB_ERR_GENERAL,
                             "calloc", "shopconv string");
      return NULL;
    }
    memcpy(rslt, buf, result_size);
  }
  return rslt;
}

/*
 * Field converters.  Each returns 1 on success, or 0 after reporting
 * "shop-field-parse" naming where (buf2: "shop #N in shop file F"), which
 * field and which entry of it failed.  They never exit: main() has already
 * moved the original to "<file>.tmp" and opened "<file>" for output, and
 * only its failure path (boot_the_shops_conv() returning 1) puts the
 * original back.  An exit() here used to leave a half-converted shop file
 * under the live name and the original where the server never reads it.
 */
/* Consume the whitespace (including the newline) after a numeric field, as
 * the original fscanf(shop_f, "%d \n") did.  Without it the next
 * fread_string() starts with the leftover "\n", so the following "#vnum~" or
 * "$~" line reads as "\n#vnum" and no 2.20 file converted.  Not used after
 * do_list()'s numbers, whose line remainder is read with fgets(). */
static void skip_field_space(FILE *stream)
{
  int ch;

  while ((ch = fgetc(stream)) != EOF && isspace(ch))
    ;
  if (ch != EOF)
    ungetc(ch, stream);
}

#define SHOP_FIELD_ERROR(where, field, ordinal) \
  TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "shop-field-parse", \
                          "%s field=%s entry=%d", (where), (field), (ordinal))

int do_list(FILE * shop_f, FILE * newshop_f, int max, const char *where,
            const char *field)
{
  int count, temp;
  char buf[MAX_STRING_LENGTH];

  for (count = 0; count < max; count++) {
    if (!read_int_token(shop_f, &temp) ||
        fgets(buf, MAX_STRING_LENGTH - 1, shop_f) == NULL) {
      SHOP_FIELD_ERROR(where, field, count + 1);
      return 0;
    }
    if (temp > 0)
      tb_fprintf(newshop_f, "%d%s", temp, buf);
  }

  tb_fprintf(newshop_f, "-1\n");
  return 1;
}


int do_float(FILE * shop_f, FILE * newshop_f, const char *where,
             const char *field)
{
  float f;
  char str[64];
  int written;

  if (!read_float_token(shop_f, &f)) {
    SHOP_FIELD_ERROR(where, field, 1);
    return 0;
  }
  skip_field_space(shop_f);
  /* A buy/sell ratio is a positive finite multiplier.  strtof() accepts
   * "inf", "nan" and 1e20 from the file; the old "%f" into char[20] then
   * truncated 1e20 silently to a different number. */
  if (!isfinite(f) || f <= 0.0f) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "shop-ratio-range",
                            "%s field=%s value=%g: not a positive finite "
                            "ratio", where, field, (double)f);
    return 0;
  }
  written = snprintf(str, sizeof(str), "%.6g", (double)f);
  if (written < 0 || (size_t)written >= sizeof(str)) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "shop-ratio-format",
                            "%s field=%s need=%d have=%zu", where, field,
                            written, sizeof(str));
    return 0;
  }
  tb_fprintf(newshop_f, "%s \n", str);
  return 1;
}


int do_int(FILE * shop_f, FILE * newshop_f, const char *where,
           const char *field, int ordinal)
{
  int i;

  if (!read_int_token(shop_f, &i)) {
    SHOP_FIELD_ERROR(where, field, ordinal);
    return 0;
  }
  skip_field_space(shop_f);
  tb_fprintf(newshop_f, "%d \n", i);
  return 1;
}


int do_string(FILE * shop_f, FILE * newshop_f, const char *where,
              const char *field, int ordinal)
{
  char *ptr;

  ptr = fread_string(shop_f, where);
  if (ptr == NULL) {
    SHOP_FIELD_ERROR(where, field, ordinal);
    return 0;
  }
  tb_fprintf(newshop_f, "%s~\n", ptr);
  free(ptr);
  return 1;
}


static int boot_the_shops_conv(FILE * shop_f, FILE * newshop_f, char *filename)
{
  char *buf, buf2[150];
  int temp, count;

  tb_snprintf(buf2, sizeof(buf2), "beginning of shop file %s", filename);
  tb_fprintf(newshop_f, "CircleMUD %s Shop File~\n", VERSION3_TAG);
  for (;;) {
    buf = fread_string(shop_f, buf2);
    if (buf == NULL) {
      tb_fprintf(stderr, "shopconv: failed reading %s\n", buf2);
      return 1;
    }
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

      /* Produced items, buy/sell ratios, bought items. */
      if (!do_list(shop_f, newshop_f, MAX_PROD, buf2, "produced") ||
          !do_float(shop_f, newshop_f, buf2, "profit-buy") ||
          !do_float(shop_f, newshop_f, buf2, "profit-sell") ||
          !do_list(shop_f, newshop_f, MAX_TRADE, buf2, "trade"))
        return 1;

      for (count = 0; count < 7; count++)	/* Keeper msgs */
	if (!do_string(shop_f, newshop_f, buf2, "message", count + 1))
	  return 1;

      for (count = 0; count < 5; count++)	/* Misc   */
	if (!do_int(shop_f, newshop_f, buf2, "misc", count + 1))
	  return 1;
      tb_fprintf(newshop_f, "-1\n");
      for (count = 0; count < 4; count++)	/* Open/Close     */
	if (!do_int(shop_f, newshop_f, buf2, "open-close", count + 1))
	  return 1;

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
  int result, index, failures = 0;

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
      TB_REPORT_ERRNO("rename", fn);
      exit(1);
    }
    sfp = fopen(part, "r");
    if (sfp == NULL) {
      int saved_errno = errno;
      tb_fprintf(stderr, "shopconv: %s could not be opened: %s\n", part, strerror(saved_errno));
      if (rename(part, fn) != 0)
        TB_REPORT_ERRNO("rename", part);
    } else {
      if ((nsfp = fopen(fn, "w")) == NULL) {
        int saved_errno = errno;
        tb_fprintf(stderr, "shopconv: fopen for output failed [%s]: %s (errno=%d)\n",
                   fn, strerror(saved_errno), saved_errno);
        tb_fclose(sfp);
        if (rename(part, fn) != 0)
          TB_REPORT_ERRNO("rename", part);
	      continue;
      }
      printf("%s:\n", fn);
      result = boot_the_shops_conv(sfp, nsfp, fn);
      tb_fclose(nsfp);
      tb_fclose(sfp);
      if (result) {
	      /* Put the original back over the partial output (rename replaces
	       * it atomically on POSIX). */
	      if (rename(part, fn) != 0) {
	        TB_REPORT_ERRNO("rename", part);
	        exit(1);
	      }
	      TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER,
	                              "shop-conversion-aborted",
	                              "file=%s: not converted; original restored",
	                              fn);
	      failures++;
      } else {
	      if (snprintf(backup, sizeof(backup), "%s.bak", fn) >= (int)sizeof(backup)) {
	        tb_fprintf(stderr, "shopconv: backup filename too long: %s\n", fn);
	        exit(1);
	      }
	      if (rename(part, backup) != 0) {
	        TB_REPORT_ERRNO("rename", part);
	        exit(1);
	      }
	      printf("Done!\n");
      }
    }
  }

  return (failures ? 1 : 0);
}
