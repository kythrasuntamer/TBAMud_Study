/* ************************************************************************
*  file:  split.c                                          Part of tbaMUD *
*  Usage: split one large file into multiple smaller ones, with index     *
*  Written by Jeremy Elson                                                *
*  All Rights Reserved                                                    *
*  Copyright (C) 1993 The Trustees of The Johns Hopkins University        *
************************************************************************* */

/*
 * This utility is meant to split a large file into multiple smaller ones,
 * mainly to help break huge world files (ala Diku) into zone-sized files
 * that are easier to manage.
 *
 * At each point in the original file where you want a break, insert a line
 * containng "=filename" at the break point.
 */

#define INDEX_NAME "index"
#define BSZ 256
#define MAGIC_CHAR '='

#include "conf.h"
#include "sysdep.h"

int main(void)
{
  char line[BSZ + 1], ctx[BSZ + 128];
  FILE *index = 0, *outfile = 0;
  long line_nr = 0;		/* 1-based stdin line of the current chunk */
  int at_line_start = 1;	/* next chunk begins a new physical line   */

  if (!(index = fopen(INDEX_NAME, "w"))) {
    TB_REPORT_ERRNO("fopen", INDEX_NAME);
    exit(1);
  }
  while (fgets(line, BSZ, stdin)) {
    char *nl = strchr(line, '\n');
    int starts_line = at_line_start;

    /*
     * fgets() returns at most BSZ - 1 bytes, so a chunk without '\n' is
     * the start or middle of a longer line and the next chunk continues
     * it.  Only a chunk that starts a physical line may be a "=filename"
     * marker; an '=' that happens to fall at a chunk boundary inside a
     * long data line is data.
     */
    at_line_start = (nl != NULL);
    if (starts_line)
      line_nr++;

    if (starts_line && *line == MAGIC_CHAR) {
      /*
       * The marker line comes from the operator's stdin and must fit in
       * one chunk.  The old code stored through strchr(line, '\n') without
       * a NULL check, which crashed on a longer name (or a NUL byte).
       * Only the last line of input may lack its newline.
       */
      if (nl != NULL) {
        *nl = '\0';
        if (nl > line && nl[-1] == '\r')
          nl[-1] = '\0';
      } else if (!feof(stdin)) {
        tb_snprintf(ctx, sizeof(ctx),
                    "stdin line %ld: '=' filename line is longer than %d "
                    "bytes or contains a NUL byte", line_nr, BSZ - 2);
        TB_REPORT_DOMAIN_ERROR(TB_SEV_ERROR, TB_ERR_PARSER,
                               "split-name-line", ctx);
        exit(1);
      }
      if (outfile) {
/*	fputs("$\n", outfile);*/
	tb_fclose(outfile);
      }
      if (!(outfile = fopen((line + 1), "a"))) {
	TB_REPORT_ERRNO("fopen", line + 1);
	exit(0);
      }
      tb_fputs(line + 1, index);
      tb_fputs("\n", index);
    } else if (outfile)
      tb_fputs(line, outfile);
  }

  tb_fputs("$\r\n", index);
  tb_fclose(index);
  if (outfile)
    tb_fclose(outfile);

  return (0);
}
