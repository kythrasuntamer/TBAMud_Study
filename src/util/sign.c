/* ************************************************************************
*  file: sign.c                                            Part of tbaMUD *
*  Usage: A program to present text on a TCP port.                        *
*         sign <port> <filename | port>                                   *
*  Written by Jeremy Elson                                                *
************************************************************************* */

#define MAX_FILESIZE	8192
#define LINEBUF_SIZE	128

#include "conf.h"
#include "sysdep.h"


/*
 * init_socket sets up the mother descriptor - creates the socket, sets
 * its options up, binds it, and listens.
 */
int init_socket(int port)
{
  int s, opt;
  struct sockaddr_in sa;

  /*
   * Should the first argument to socket() be AF_INET or PF_INET?  I don't
   * know, take your pick.  PF_INET seems to be more widely adopted, and
   * Comer (_Internetworking with TCP/IP_) even makes a point to say that
   * people erroneously use AF_INET with socket() when they should be using
   * PF_INET.  However, the man pages of some systems indicate that AF_INET
   * is correct; some such as ConvexOS even say that you can use either one.
   * All implementations I've seen define AF_INET and PF_INET to be the same
   * number anyway, so ths point is (hopefully) moot.
   */

  if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    TB_REPORT_ERRNO("socket", "sign listener");
    exit(1);
  }
#if defined(SO_REUSEADDR)
  opt = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt)) < 0) {
    TB_REPORT_ERRNO("setsockopt", "SO_REUSEADDR sign listener");
    exit(1);
  }
#endif

#if defined(SO_LINGER)
  {
    struct linger ld;

    ld.l_onoff = 0;
    ld.l_linger = 0;
    if (setsockopt(s, SOL_SOCKET, SO_LINGER, (char *) &ld, sizeof(ld)) < 0) {
      TB_REPORT_ERRNO("setsockopt", "SO_LINGER sign listener");
      exit(1);
    }
  }
#endif

  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(s, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
    TB_REPORT_ERRNO("bind", "sign listener");
    close(s);
    exit(1);
  }
  if (listen(s, 5) < 0) {
    TB_REPORT_ERRNO("listen", "sign listener backlog=5");
    (void)close(s);
    exit(1);
  }
  return (s);
}


char *get_text(char *fname)
{
  static char t[MAX_FILESIZE];
  char tmp[LINEBUF_SIZE + 2];
  FILE *fl = NULL;
  int close_stream = 0;

  *t = '\0';

  if (!strcmp(fname, "-")) {
    fl = stdin;
    if (isatty(STDIN_FILENO))
      tb_fprintf(stderr, "Enter sign text; terminate with Ctrl-D.\n");
  } else {
    if (!(fl = fopen(fname, "r"))) {
      TB_REPORT_ERRNO("fopen", fname);
      exit(1);
    }
    close_stream = 1;
  }

  while (fgets(tmp, LINEBUF_SIZE, fl)) {
    size_t used = strlen(t);
    size_t incoming = strlen(tmp);

    if (used + incoming + 1 < MAX_FILESIZE) {
      strlcat(t, tmp, MAX_FILESIZE);
      strlcat(t, "\r", MAX_FILESIZE);
    } else {
      tb_fprintf(stderr, "String too long.  Truncated.\n");
      break;
    }
  }

  if (close_stream)
    tb_fclose(fl);

  return (t);
}


/* clean up our zombie kids to avoid defunct processes */
RETSIGTYPE reap(int sig)
{
  (void)sig;

  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
}


int main(int argc, char *argv[])
{
  char *txt;
  int desc, remaining, len, s, port;
  pid_t child;
  ssize_t bytes_written;
  struct sigaction sa;

  if (argc != 3) {
    tb_fprintf(stderr, "usage: %s <portnum> <\"-\" | filename>\n", argv[0]);
    exit(1);
  }
  /* The port goes through htons() (16 bits): 70000 used to be accepted,
   * announced as 70000 and bound to 4464, and "4000x" bound 4000. */
  if (!tb_parse_int_checked(argv[1], &port) || port < 1024 || port > 65535) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PARSER, "port-parse",
                            "port=%.32s: need a whole number in 1024..65535",
                            argv[1]);
    tb_fprintf(stderr, "usage: %s <portnum> <\"-\" | filename>\n", argv[0]);
    exit(1);
  }
  s = init_socket(port);
  len = strlen(txt = get_text(argv[2]));

  child = fork();
  if (child < 0) {
    TB_REPORT_ERRNO("fork", "sign daemon");
    return 1;
  }
  if (child > 0) {
    tb_fprintf(stderr, "Sign started on port %d (pid %d).\n", port, (int)child);
    return 0;
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = reap;
  if (sigemptyset(&sa.sa_mask) != 0 || sigaction(SIGCHLD, &sa, NULL) != 0) {
    TB_REPORT_ERRNO("sigaction", "SIGCHLD");
    return 1;
  }

  for (;;) {
    if ((desc = accept(s, (struct sockaddr *)NULL, NULL)) < 0) {
      if (errno == EINTR)
        continue;
      TB_REPORT_ERRNO("accept", "sign listener");
      continue;
    }

    child = fork();
    if (child < 0) {
      TB_REPORT_ERRNO("fork", "sign client handler");
      (void)close(desc);
      continue;
    }
    if (child == 0) {
      char *out = txt;

      remaining = len;
      while (remaining > 0) {
        bytes_written = write(desc, out, (size_t)remaining);
        if (bytes_written < 0) {
          if (errno == EINTR)
            continue;
          TB_REPORT_ERRNO("write", "sign client socket");
          (void)close(desc);
          _exit(1);
        }
        if (bytes_written == 0)
          break;
        out += bytes_written;
        remaining -= (int)bytes_written;
      }
      (void)close(desc);
      _exit(0);
    }
    if (close(desc) != 0)
      TB_REPORT_ERRNO("close", "sign client socket");
  }
}
