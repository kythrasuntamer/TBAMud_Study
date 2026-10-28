/* ************************************************************************
*  file:  asciipasswd.c (derived from mudpasswd.c)         Part of tbaMUD *
*  Usage: generating hashed passwords for an ascii playerfile.            *
*  Copyright (C) 1990, 1991 - see 'license.doc' for complete information. *
*  All Rights Reserved                                                    *
************************************************************************* */

/*
 * asciipasswd <name> <password>
 *
 * Prints the "Name:" and "Pass:" lines for an ASCII pfile, hashing the
 * password the way the server does at login: CRYPT(password, Name), with
 * the capitalised player name as the salt.  Run by an implementor from a
 * shell; both arguments come from that operator.
 *
 * Exit status 0 only when a usable hash was printed.  An invalid name, a
 * crypt() failure (NULL, or libxcrypt's "*0"/"*1" failure tokens, which no
 * password matches) or a build without crypt support (where CRYPT() would
 * return the plaintext) is reported and exits 1, with nothing on stdout.
 */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"


int main(int argc, char **argv) {
  char name[MAX_NAME_LENGTH + 1];
  const char *hash;
  size_t len, i;

  if (argc != 3) {
    tb_fprintf(stderr, "Usage: %s name password\n", argv[0]);
    return 1;
  }

#if defined(NOCRYPT) || !defined(CIRCLE_CRYPT)
  /* CRYPT(a, b) is (a) here: "hashing" would print the plaintext password
   * into the pfile. */
  TB_REPORT_ERROR("crypt", "asciipasswd built without crypt support "
                  "(CIRCLE_CRYPT undefined): refusing to print the plaintext");
  return 1;
#endif

  /* The name is the salt and must be a player name the server would accept
   * (letters only, 1..MAX_NAME_LENGTH): libxcrypt rejects a salt with other
   * bytes, and a name the server cannot create is useless anyway. */
  len = strlen(argv[1]);
  if (len == 0 || len > MAX_NAME_LENGTH) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "player-name",
                            "name length %zu outside 1..%d", len,
                            MAX_NAME_LENGTH);
    return 1;
  }
  for (i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)argv[1][i];

    if (ch > 127 || !isalpha(ch)) {
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "player-name",
                              "byte %zu of the name is 0x%02x, not a letter",
                              i, (unsigned int)ch);
      return 1;
    }
    name[i] = (char)(i == 0 ? toupper(ch) : ch);
  }
  name[len] = '\0';

  errno = 0;
  hash = CRYPT(argv[2], name);
  if (hash == NULL || *hash == '*' || *hash == '\0') {
    TB_REPORT_DOMAIN_ERRNOF(TB_SEV_ERROR, TB_ERR_GENERAL, "crypt",
                            "asciipasswd salt from name=%s: crypt() returned "
                            "%s", name, hash == NULL ? "NULL" :
                            "a failure token");
    return 1;
  }

  if (printf("Name: %s\nPass: %s\n", name, hash) < 0)
    return 1;
  return 0;
}
