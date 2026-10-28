/**************************************************************************
*  File: comm.c                                            Part of tbaMUD *
*  Usage: Communication, socket handling, main(), central game loop.      *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/**
 * @file comm.c
 * Network front end and main loop: sockets, descriptors, per-connection
 * input and output buffering, the act() message formatter, signals and
 * copyover recovery.
 *
 * Who controls the input:
 *  - Socket bytes (process_input(), via ProtocolInput()) come from remote,
 *    unauthenticated peers and are fully untrusted. So are reverse-DNS host
 *    names (new_descriptor()); log the numeric address instead.
 *  - Output text (write_to_output(), send_to_*(), act()) is built from
 *    builder data (room/object/mob strings), DG script text and player
 *    text (say, emote, '@' colour and unicode codes), all untrusted as
 *    format *arguments*; formats themselves are always literals.
 *  - copyover.dat and the command line come from the previous server
 *    process and the operator: checked, but not hostile.
 *
 * Ownership and lifetime:
 *  - A descriptor_data is created by new_descriptor() (or
 *    copyover_recover()) and freed only by close_socket(), which game_loop()
 *    calls for the descriptor it is processing or for descriptors marked
 *    CON_CLOSE/CON_DISCONNECT. Nothing may use a descriptor after
 *    close_socket() returns.
 *  - Large output buffers belong to the descriptor while it is in the large
 *    state and are returned to the file-static bufpool afterwards (see the
 *    output buffer state machine above outbuf_reset_small()).
 *  - Characters are extracted at the end of the pulse, so a char_data
 *    pointer stays allocated for the rest of the current pulse, but act
 *    triggers can move it to another room at any time (see act_body()).
 *
 * Diagnostics use TB_ERROR_MSG / TB_WARN_MSG with DIAG_DESC (descriptor)
 * and DIAG_CHAR (character) identities; conditions a peer can trigger at
 * will use the rate-limited TB_WARN_EVERY / TB_ERROR_EVERY (comm.h).
 */

#include "conf.h"
#include "sysdep.h"

/* Begin conf.h dependent includes */

#if CIRCLE_GNU_LIBC_MEMORY_TRACK
# include <mcheck.h>
#endif

#ifdef CIRCLE_MACINTOSH		/* Includes for the Macintosh */
# define SIGPIPE 13
# define SIGALRM 14
  /* GUSI headers */
# include <sys/ioctl.h>
  /* Codewarrior dependant */
# include <SIOUX.h>
# include <console.h>
#endif

#ifdef CIRCLE_WINDOWS		/* Includes for Win32 */
# ifdef __BORLANDC__
#  include <dir.h>
# else /* MSVC */
#  include <direct.h>
#  include <winsock.h>
# endif
# include <mmsystem.h>
#endif /* CIRCLE_WINDOWS */

#ifdef CIRCLE_AMIGA		/* Includes for the Amiga */
# include <sys/ioctl.h>
# include <clib/socket_protos.h>
#endif /* CIRCLE_AMIGA */

#ifdef CIRCLE_ACORN		/* Includes for the Acorn (RiscOS) */
# include <socklib.h>
# include <inetlib.h>
# include <sys/ioctl.h>
#endif

#ifdef HAVE_ARPA_TELNET_H
#include <arpa/telnet.h>
#else
#include "telnet.h"
#endif

/* end conf.h dependent includes */

/* Note, most includes for all platforms are in sysdep.h.  The list of
 * files that is included is controlled by conf.h for that platform. */

#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "house.h"
#include "oasis.h"
#include "genolc.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include "screen.h" /* to support the gemote act type command */
#include "constants.h" /* For mud versions */
#include "boards.h"
#include "act.h"
#include "ban.h"
#include "msgedit.h"
#include "fight.h"
#include "spells.h" /* for affect_update */
#include "modify.h"
#include "quest.h"
#include "ibt.h" /* for free_ibt_lists */
#include "mud_event.h"

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif

/* TRUE when cur is below a quarter of max (the auto-prompt threshold).
 * Multiplies in long instead of 'cur << 2': hit points are negative while a
 * player is incapacitated or dying, and shifting a negative value is
 * undefined behaviour. */
#define BELOW_QUARTER(cur, max) ((long) (cur) * 4L < (long) (max))

extern time_t motdmod;
extern time_t newsmod;

/* locally defined globals, used externally */
struct descriptor_data *descriptor_list = NULL;   /* master desc list */
int buf_largecount = 0;   /* # of large buffers which exist */
int buf_overflows = 0;    /* # of overflows of output */
int buf_switches = 0;     /* # of switches from small to large buf */
int circle_shutdown = 0;  /* clean shutdown */
int circle_reboot = 0;    /* reboot the game after a shutdown */
int no_specials = 0;      /* Suppress ass. of special routines */
int scheck = 0;           /* for syntax checking mode */
FILE *logfile = NULL;     /* Where to send the log messages. */
unsigned long pulse = 0;  /* number of pulses since game start */
ush_int port;
socket_t mother_desc;
int next_tick = SECS_PER_MUD_HOUR;  /* Tick countdown */


/* static local global variable declarations (current file scope only) */
static struct txt_block *bufpool = 0;  /* pool of large output buffers */
static int max_players = 0;   /* max descriptors available */
static int tics_passed = 0;     /* for extern checkpointing */
static struct timeval null_time; /* zero-valued time structure */
/* Set by signal handlers, acted on by game_loop(). Handlers only store to
 * these; everything else (logging, freeing, exiting) happens in the loop. */
static volatile sig_atomic_t reread_wizlist;   /* signal: SIGUSR1 */
static volatile sig_atomic_t emergency_unban;  /* signal: SIGUSR2 */
static volatile sig_atomic_t shutdown_signal;  /* SIGHUP/SIGINT/SIGTERM */

static int dg_act_check;         /* toggle for act_trigger */
static bool fCopyOver;          /* Are we booting in copyover mode? */
static char *last_act_message = NULL;
/* Call site of the act() being formatted (NULL for direct perform_act()
 * calls). act_at() saves and restores it because act triggers can send
 * further act() messages while one is being delivered. */
static const char *act_site_file = NULL;
static int act_site_line = 0;
static const char *act_site_func = NULL;

/* static local function prototypes (current file scope only) */
static RETSIGTYPE reread_wizlists(int sig);
/* Appears to be orphaned right now...
static RETSIGTYPE unrestrict_game(int sig);
*/
static RETSIGTYPE reap(int sig);
static RETSIGTYPE checkpointing(int sig);
static RETSIGTYPE hupsig(int sig);
static ssize_t perform_socket_read(socket_t desc, char *read_point,size_t space_left);
static ssize_t perform_socket_write(socket_t desc, const char *txt,size_t length);
static void circle_sleep(struct timeval *timeout);
static int get_from_q(struct txt_q *queue, char *dest, int *aliased);
static void init_game(ush_int port);
static void signal_setup(void);
static socket_t init_socket(ush_int port);
static int new_descriptor(socket_t s);
static int get_max_players(void);
static int process_output(struct descriptor_data *t);
static int process_input(struct descriptor_data *t);
static void timediff(struct timeval *diff, struct timeval *a, struct timeval *b);
static void timeadd(struct timeval *sum, struct timeval *a, struct timeval *b);
static void flush_queues(struct descriptor_data *d);
static void flush_input_queue(struct descriptor_data *d);
static void nonblock(socket_t s);
static int perform_subst(struct descriptor_data *t, char *orig, char *subst);
static void record_usage(void);
static char *make_prompt(struct descriptor_data *point);
static void check_idle_passwords(void);
static void init_descriptor (struct descriptor_data *newd, int desc);

static struct in_addr *get_bind_addr(void);
static int parse_ip(const char *addr, struct in_addr *inaddr);
static int set_sendbuf(socket_t s);
static void free_bufpool(void);
static void setup_log(const char *filename, int fd);
static const char *act_format_excerpt(char *dst, size_t dst_size,
                                      const char *format);
static int open_logfile(const char *filename, FILE *stderr_fp);
#if defined(POSIX)
static sigfunc *my_signal(int signo, sigfunc *func);
#endif

static void msdp_update(void); /* KaVir plugin*/

/* externally defined functions, used locally */
#ifdef __CXREF__
#undef FD_ZERO
#undef FD_SET
#undef FD_ISSET
#undef FD_CLR
#define FD_ZERO(x)
#define FD_SET(x, y) 0
#define FD_ISSET(x, y) 0
#define FD_CLR(x, y)
#endif

/* Parse a whole decimal command-line argument into [min, max]. FALSE for
 * empty text, trailing characters, overflow or a value out of range; the
 * caller reports it. */
static bool parse_long_arg(const char *text, long min, long max, long *out)
{
  char *end;
  long value;

  if (text == NULL || *text == '\0')
    return FALSE;
  errno = 0;
  value = strtol(text, &end, 10);
  if (*end != '\0' || errno == ERANGE || value < min || value > max)
    return FALSE;
  *out = value;
  return TRUE;
}

/* Highest descriptor accepted for the copyover control socket. */
#ifdef CIRCLE_WINDOWS
#define MOTHER_DESC_MAX LONG_MAX
#else
#define MOTHER_DESC_MAX ((long) FD_SETSIZE - 1)
#endif

/*  main game loop and related stuff */

#if defined(CIRCLE_WINDOWS) || defined(CIRCLE_MACINTOSH)
/* Windows and Mac do not have gettimeofday, so we'll simulate it. Borland C++
 * warns: "Undefined structure 'timezone'" */
void gettimeofday(struct timeval *t, struct timezone *dummy)
{
#if defined(CIRCLE_WINDOWS)
  DWORD millisec = GetTickCount();
#elif defined(CIRCLE_MACINTOSH)
  unsigned long int millisec;
  millisec = (int)((float)TickCount() * 1000.0 / 60.0);
#endif

  t->tv_sec = (int) (millisec / 1000);
  t->tv_usec = (millisec % 1000) * 1000;
}

#endif	/* CIRCLE_WINDOWS || CIRCLE_MACINTOSH */

int main(int argc, char **argv)
{
  int pos = 1;
  const char *dir;

#ifdef MEMORY_DEBUG
  zmalloc_init();
#endif

#if CIRCLE_GNU_LIBC_MEMORY_TRACK
  mtrace();	/* This must come before any use of malloc(). */
#endif

#ifdef CIRCLE_MACINTOSH
  /* ccommand() calls the command line/io redirection dialog box from
   * Codewarriors's SIOUX library. */
  argc = ccommand(&argv);
  /* Initialize the GUSI library calls.  */
  GUSIDefaultSetup();
#endif

  /* Load the game configuration. We must load BEFORE we use any of the
   * constants stored in constants.c.  Otherwise, there will be no variables
   * set to set the rest of the vars to, which will mean trouble --> Mythran */
  CONFIG_CONFFILE = NULL;
  while ((pos < argc) && (*(argv[pos]) == '-')) {
    if (*(argv[pos] + 1) == 'f') {
      if (*(argv[pos] + 2))
	CONFIG_CONFFILE = argv[pos] + 2;
      else if (++pos < argc)
	CONFIG_CONFFILE = argv[pos];
      else {
	puts("SYSERR: File name to read from expected after option -f.");
	exit(1);
      }
    }
    pos++;
  }
  pos = 1;

  if (!CONFIG_CONFFILE)
    CONFIG_CONFFILE = strdup(CONFIG_FILE);

  load_config();

  port = CONFIG_DFLT_PORT;
  dir = CONFIG_DFLT_DIR;

  while ((pos < argc) && (*(argv[pos]) == '-')) {
    switch (*(argv[pos] + 1)) {
    case 'f':
      if (! *(argv[pos] + 2))
	++pos;
      break;
    case 'o':
      if (*(argv[pos] + 2))
	CONFIG_LOGNAME = argv[pos] + 2;
      else if (++pos < argc)
	CONFIG_LOGNAME = argv[pos];
      else {
	puts("SYSERR: File name to log to expected after option -o.");
	exit(1);
      }
      break;
    case 'C': /* -C<socket number> - recover from copyover, this is the control socket */
      {
       /* Written by do_copyover() when it exec()s us. A bad value used to
        * become 0 (stdin) silently; the mother socket must be an open
        * descriptor select() can watch. */
       long fd;
       if (!parse_long_arg(argv[pos] + 2, 0, MOTHER_DESC_MAX, &fd)) {
         char fd_context[96];
         tb_snprintf(fd_context, sizeof(fd_context),
                     "illegal copyover control socket '%.40s' (want 0..%ld)",
                     argv[pos] + 2, (long) MOTHER_DESC_MAX);
         TB_REPORT_DOMAIN_ERROR(TB_SEV_FATAL, TB_ERR_BOOT,
                                "parse-copyover-fd", fd_context);
         exit(1);
       }
       fCopyOver = TRUE;
       mother_desc = (socket_t) fd;
      }
      break;
    case 'd':
      if (*(argv[pos] + 2))
	dir = argv[pos] + 2;
      else if (++pos < argc)
	dir = argv[pos];
      else {
	puts("SYSERR: Directory arg expected after option -d.");
	exit(1);
      }
      break;
    case 'm':
      mini_mud = 1;
      no_rent_check = 1;
      puts("Running in minimized mode & with no rent check.");
      break;
    case 'c':
      scheck = 1;
      puts("Syntax check mode enabled.");
      break;
    case 'q':
      no_rent_check = 1;
      puts("Quick boot mode -- rent check supressed.");
      break;
    case 'r':
      circle_restrict = 1;
      puts("Restricting game -- no new players allowed.");
      break;
    case 's':
      no_specials = 1;
      puts("Suppressing assignment of special routines.");
      break;
    case 'h':
      /* From: Anil Mahajan. Do NOT use -C, this is the copyover mode and
       * without the proper copyover.dat file, the game will go nuts! */
      printf("Usage: %s [-c] [-m] [-q] [-r] [-s] [-d pathname] [port #]\n"
              "  -c             Enable syntax check mode.\n"
              "  -d <directory> Specify library directory (defaults to 'lib').\n"
              "  -h             Print this command line argument help.\n"
              "  -m             Start in mini-MUD mode.\n"
	      "  -f<file>       Use <file> for configuration.\n"
	      "  -o <file>      Write log to <file> instead of stderr.\n"
              "  -q             Quick boot (doesn't scan rent for object limits)\n"
              "  -r             Restrict MUD -- no new players allowed.\n"
              "  -s             Suppress special procedure assignments.\n"
              " Note:		These arguments are 'CaSe SeNsItIvE!!!'\n",
		 argv[0]
      );
      exit(0);
    default: {
      char option_context[96];
      tb_snprintf(option_context, sizeof(option_context),
                  "unknown command-line option -%c", *(argv[pos] + 1));
      TB_REPORT_DOMAIN_ERROR(TB_SEV_ERROR, TB_ERR_BOOT,
                             "parse-options", option_context);
      break;
    }
    }
    pos++;
  }

  if (pos < argc) {
    long parsed_port;

    if (!isdigit((unsigned char) *argv[pos])) {
      printf("Usage: %s [-c] [-m] [-q] [-r] [-s] [-d pathname] [port #]\n", argv[0]);
      exit(1);
    }
    /* Range-checked before narrowing: 'port' is a ush_int, so a value such
     * as 70000 used to wrap to 4464 and pass the old '> 1024' test. */
    if (!parse_long_arg(argv[pos], 1025, 65535, &parsed_port)) {
      char port_context[96];
      tb_snprintf(port_context, sizeof(port_context),
                  "illegal port number '%.40s' (want 1025..65535)", argv[pos]);
      TB_REPORT_DOMAIN_ERROR(TB_SEV_FATAL, TB_ERR_BOOT,
                             "parse-port", port_context);
      exit(1);
    }
    port = (ush_int) parsed_port;
  }

  /* All arguments have been parsed, try to open log file. */
  setup_log(CONFIG_LOGNAME, STDERR_FILENO);

  /* Moved here to distinguish command line options and to show up
   * in the log if stderr is redirected to a file. */
  log("Loading configuration.");
  log("%s", tbamud_version);

  if (chdir(dir) < 0) {
    TB_REPORT_ERRNO("chdir", "data directory");
    exit(1);
  }
  log("Using %s as data directory.", dir);

  if (scheck)
    boot_world();
  else {
    log("Running game on port %d.", port);
    init_game(port);
  }

  log("Clearing game world.");
  destroy_db();

  if (!scheck) {
    log("Clearing other memory.");
    free_bufpool();         /* comm.c */
    free_player_index();    /* players.c */
    free_messages();        /* fight.c */
    free_text_files();      /* db.c */
    board_clear_all();      /* boards.c */
    free(cmd_sort_info);    /* act.informative.c */
    free_command_list();    /* act.informative.c */
    free_social_messages(); /* act.social.c */
    free_help_table();      /* db.c */
    free_invalid_list();    /* ban.c */
    free_save_list();       /* genolc.c */
    free_strings(&config_info, OASIS_CFG); /* oasis_delete.c */
    free_ibt_lists();       /* ibt.c */
    free_recent_players();  /* act.informative.c */
    free_list(world_events); /* free up our global lists */
    free_list(global_lists);
  }

  if (last_act_message)
    free(last_act_message);

  /* probably should free the entire config here.. */
  free(CONFIG_CONFFILE);
  
  log("Done.");

#ifdef MEMORY_DEBUG
  zmalloc_check();
#endif

  return (0);
}

/**
 * copyover.dat format (written by do_copyover() in act.wizard.c just before
 * it exec()s the new binary; read once here, then unlinked):
 *   line 1:   boot time (decimal time_t)
 *   then one line per player: "<fd> <pref> <name> <host> <guiopt>"
 *   terminated by a line whose fd is -1.
 * Every field is parsed with bounds; each fd must be open and below
 * FD_SETSIZE. A player who cannot be restored (bad fd, failed write, no
 * pfile, deleted) is reported as copyover/bad-fd or copyover/restore-player
 * and skipped; a malformed file aborts the boot.
 */
/* Reload players after a copyover */
void copyover_recover()
{
  struct descriptor_data *d;
  FILE *fp;
  char host[1024], guiopt[1024];
  int desc, i, player_i;
  bool fOld;
  char name[MAX_INPUT_LENGTH], name_excerpt[48];
  long pref;

  log ("Copyover recovery initiated");

  fp = fopen (COPYOVER_FILE, "r");
  /* there are some descriptors open which will hang forever then ? */
  if (!fp) {
    TB_REPORT_ERRNO("fopen", COPYOVER_FILE);
    log ("Copyover file not found. Exitting.\n\r");
    exit (1);
  }

  /* In case something crashes - doesn't prevent reading  */
  unlink (COPYOVER_FILE);

  /* read boot_time - first line in file */
  {
    char timestamp[128], *end;
    long parsed;
    if (!fgets(timestamp, sizeof(timestamp), fp)) {
      log("SYSERR: Missing copyover timestamp in %s.", COPYOVER_FILE);
      tb_fclose(fp);
      exit(1);
    }
    errno = 0;
    parsed = strtol(timestamp, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\n' ||
           *end == '\r' || *end == '\f' || *end == '\v')
      end++;
    i = (end != timestamp && *end == '\0' && errno != ERANGE) ? 1 : 0;
    if (i == 1)
      boot_time = (time_t)parsed;
  }

  if (i != 1) {
    if (ferror(fp))
      TB_REPORT_ERRNO("fscanf", COPYOVER_FILE);
    else
      log("SYSERR: Invalid or truncated boot time in %s.", COPYOVER_FILE);
    tb_fclose(fp);
    exit(1);
  }

  for (;;) {
    fOld = TRUE;
    /* Field widths reserve a byte for the terminating NUL in each array. */
    {
      char desc_text[64], pref_text[64], *end;
      long parsed;
      int valid = 1;
      if (fscanf(fp, "%63s %63s %511s %1023s %1023s",
                 desc_text, pref_text, name, host, guiopt) != 5)
        valid = 0;
      if (valid) {
        errno = 0;
        parsed = strtol(desc_text, &end, 10);
        if (end == desc_text || *end || errno == ERANGE ||
            parsed < INT_MIN || parsed > INT_MAX)
          valid = 0;
        else
          desc = (int)parsed;
      }
      if (valid) {
        errno = 0;
        parsed = strtol(pref_text, &end, 10);
        if (end == pref_text || *end || errno == ERANGE)
          valid = 0;
        else
          pref = parsed;
      }
      if (!valid) {
      if (feof(fp)) {
        log("SYSERR: unexpected EOF in copyover file %s.", COPYOVER_FILE);
        break;
      }

      if (ferror(fp))
        TB_REPORT_ERRNO("fscanf", COPYOVER_FILE);
      else
        log("SYSERR: could not scan line in copyover file %s.", COPYOVER_FILE);
      tb_fclose(fp);
      exit(1);
      }
    }

    if (desc == -1)
      break;

    /* copyover.dat is written by do_copyover() of the previous process and
     * names sockets it left open across exec(). Trust it only as far as it
     * can be checked: the descriptor must be one select() can watch and
     * must actually be open, or FD_SET in game_loop() writes out of bounds
     * and every later write goes to the wrong file. Each player who cannot
     * be restored is reported with the reason. */
    act_format_excerpt(name_excerpt, sizeof(name_excerpt), name);
#ifndef CIRCLE_WINDOWS
    if (desc < 0 || desc >= FD_SETSIZE || fcntl(desc, F_GETFD) == -1) {
      TB_ERROR_MSG("copyover", "bad-fd", "copyover_recover",
                   "fd %d for player '%s' is %s; player not restored", desc,
                   name_excerpt, (desc < 0 || desc >= FD_SETSIZE)
                   ? "outside 0..FD_SETSIZE-1" : "not open");
      if (desc >= FD_SETSIZE && fcntl(desc, F_GETFD) != -1)
        close(desc);
      continue;
    }
#endif

    /* Write something, and check if it goes error-free */
    if (write_to_descriptor (desc, "\n\rRestoring from copyover...\n\r") < 0) {
      TB_ERROR_MSG("copyover", "restore-player", "copyover_recover",
                   "fd %d name '%s' reason write-failed; player not restored",
                   desc, name_excerpt);
      close (desc); /* nope */
      continue;
    }

    /* create a new descriptor */
    CREATE (d, struct descriptor_data, 1);
    memset ((char *) d, 0, sizeof (struct descriptor_data));
    init_descriptor (d,desc); /* set up various stuff */

    strlcpy(d->host, host, sizeof(d->host));
    d->next = descriptor_list;
    descriptor_list = d;

    d->connected = CON_CLOSE;

    CopyoverSet(d,guiopt);

    /* Now, find the pfile */
    CREATE(d->character, struct char_data, 1);
    clear_char(d->character);
    CREATE(d->character->player_specials, struct player_special_data, 1);
    
    new_mobile_data(d->character);
    
    d->character->desc = d;

    if ((player_i = load_char(name, d->character)) >= 0) {
      GET_PFILEPOS(d->character) = player_i;
      if (!PLR_FLAGGED(d->character, PLR_DELETED)) {
        REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_WRITING);
        REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_MAILING);
        REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_CRYO);
      } else
        fOld = FALSE;
    } else
      fOld = FALSE;

    /* Player file not found?! */
    if (!fOld) {
      TB_ERROR_MSG("copyover", "restore-player", "copyover_recover",
                   "fd %d name '%s' reason %s; connection closed", desc,
                   name_excerpt, player_i < 0 ? "load_char-failed"
                                              : "player-deleted");
      write_to_descriptor (desc, "\n\rSomehow, your character was lost in the copyover. Sorry.\n\r");
      close_socket (d);
    } else {
      write_to_descriptor (desc, "\n\rCopyover recovery complete.\n\r");
      GET_PREF(d->character) = pref;
    
      enter_player_game(d);

      /* Clear their load room if it's not persistant. */
      if (!PLR_FLAGGED(d->character, PLR_LOADROOM))
        GET_LOADROOM(d->character) = NOWHERE;

      d->connected = CON_PLAYING;
      look_at_room(d->character, 0);

      /* Add to the list of 'recent' players (since last reboot) with copyover flag */
      if (AddRecentPlayer(GET_NAME(d->character), d->host, FALSE, TRUE) == FALSE)
      {
        mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "Failure to AddRecentPlayer (returned FALSE).");
      }
    }
  }
  tb_fclose(fp);
}

/* Init sockets, run game, and cleanup sockets */
static void init_game(ush_int local_port)
{
  /* We don't want to restart if we crash before we get up. */
  touch(KILLSCRIPT_FILE);

  circle_srandom(time(0));

  log("Finding player limit.");
  max_players = get_max_players();

  /* If copyover mother_desc is already set up */
  if (!fCopyOver) {
     log ("Opening mother connection.");
     mother_desc = init_socket (local_port);
  }

  event_init();

  /* set up hash table for find_char() */
  init_lookup_table();

  boot_db();

#if defined(CIRCLE_UNIX) || defined(CIRCLE_MACINTOSH)
  log("Signal trapping.");
  signal_setup();
#endif

  /* If we made it this far, we will be able to restart without problem. */
  if (remove(KILLSCRIPT_FILE) != 0 && errno != ENOENT)
    TB_REPORT_ERRNO("remove", KILLSCRIPT_FILE);

  if (fCopyOver) /* reload players */
  copyover_recover();

  log("Entering game loop.");

  game_loop(mother_desc);

  Crash_save_all();

  log("Closing all sockets.");
  while (descriptor_list)
    close_socket(descriptor_list);

  CLOSE_SOCKET(mother_desc);

  if (circle_reboot != 2)
    save_all();

  log("Saving current MUD time.");
  save_mud_time(&time_info);

  if (circle_reboot) {
    log("Rebooting.");
    exit(52);			/* what's so great about HHGTTG, anyhow? */
  }
  log("Normal termination of game.");
}

/* init_socket sets up the mother descriptor - creates the socket, sets
 * its options up, binds it, and listens. */
static socket_t init_socket(ush_int local_port)
{
  socket_t s;
  struct sockaddr_in sa;
  int opt;

#ifdef CIRCLE_WINDOWS
  {
    WORD wVersionRequested;
    WSADATA wsaData;

    wVersionRequested = MAKEWORD(1, 1);

    if (WSAStartup(wVersionRequested, &wsaData) != 0) {
      log("SYSERR: WinSock not available!");
      exit(1);
    }

    /* 4 = stdin, stdout, stderr, mother_desc.  Windows might keep sockets and
     * files separate, in which case this isn't necessary, but we will err on
     * the side of caution. */
    if ((wsaData.iMaxSockets - 4) < max_players) {
      max_players = wsaData.iMaxSockets - 4;
    }
    log("Max players set to %d", max_players);

    if ((s = socket(PF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
      log("SYSERR: Error opening network connection: Winsock error #%d",
	  WSAGetLastError());
      exit(1);
    }
  }
#else
  /* Should the first argument to socket() be AF_INET or PF_INET?  I don't
   * know, take your pick.  PF_INET seems to be more widely adopted, and
   * Comer (_Internetworking with TCP/IP_) even makes a point to say that
   * people erroneously use AF_INET with socket() when they should be using
   * PF_INET.  However, the man pages of some systems indicate that AF_INET
   * is correct; some such as ConvexOS even say that you can use either one.
   * All implementations I've seen define AF_INET and PF_INET to be the same
   * number anyway, so the point is (hopefully) moot. */

  if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    TB_REPORT_ERRNO("socket", "mother descriptor");
    exit(1);
  }
#endif				/* CIRCLE_WINDOWS */

#if defined(SO_REUSEADDR) && !defined(CIRCLE_MACINTOSH)
  opt = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt)) < 0){
    TB_REPORT_ERRNO("setsockopt", "SO_REUSEADDR on mother descriptor");
    exit(1);
  }
#endif

  set_sendbuf(s);

/* The GUSI sockets library is derived from BSD, so it defines SO_LINGER, even
 * though setsockopt() is unimplimented. (from Dean Takemori) */
#if defined(SO_LINGER) && !defined(CIRCLE_MACINTOSH)
  {
    struct linger ld;

    ld.l_onoff = 0;
    ld.l_linger = 0;
    if (setsockopt(s, SOL_SOCKET, SO_LINGER, (char *) &ld, sizeof(ld)) < 0)
      TB_REPORT_ERRNO("setsockopt", "SO_LINGER on mother descriptor"); /* non-fatal */
  }
#endif

  /* Clear the structure */
  memset((char *)&sa, 0, sizeof(sa));

  sa.sin_family = AF_INET;
  sa.sin_port = htons(local_port);
  sa.sin_addr = *(get_bind_addr());

  if (bind(s, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
    TB_REPORT_ERRNO("bind", "mother descriptor");
    CLOSE_SOCKET(s);
    exit(1);
  }
  nonblock(s);
  if (listen(s, 5) < 0) {
    TB_REPORT_ERRNO("listen", "mother descriptor backlog=5");
    CLOSE_SOCKET(s);
    exit(1);
  }
  return (s);
}

static int get_max_players(void)
{
#ifndef CIRCLE_UNIX
  return (CONFIG_MAX_PLAYING);
#else

  int max_descs = 0;
  const char *method;

/* First, we'll try using getrlimit/setrlimit.  This will probably work
 * on most systems.  HAS_RLIMIT is defined in sysdep.h. */
#ifdef HAS_RLIMIT
  {
    struct rlimit limit;

    /* find the limit of file descs */
    method = "rlimit";
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) {
      TB_REPORT_ERRNO("getrlimit", "RLIMIT_NOFILE");
      exit(1);
    }

    /* set the current to the maximum */
    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
#if defined(__APPLE__) && defined(RLIMIT_NOFILE) && defined(OPEN_MAX)
        /* On OS X, setting rlim_cur above OPEN_MAX fails, so try again
         * with OPEN_MAX. */
      limit.rlim_cur = OPEN_MAX;
#endif
      if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
        TB_REPORT_ERRNO("setrlimit", "RLIMIT_NOFILE");
        exit(1);
      }
    }
#ifdef RLIM_INFINITY
    if (limit.rlim_max == RLIM_INFINITY)
      max_descs = CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS;
    else
      max_descs = MIN(CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS, limit.rlim_max);
#else
    max_descs = MIN(CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS, limit.rlim_max);
#endif
  }

#elif defined (OPEN_MAX) || defined(FOPEN_MAX)
#if !defined(OPEN_MAX)
#define OPEN_MAX FOPEN_MAX
#endif
  method = "OPEN_MAX";
  max_descs = OPEN_MAX;		/* Uh oh.. rlimit didn't work, but we have
				 * OPEN_MAX */
#elif defined (_SC_OPEN_MAX)
  /* Okay, you don't have getrlimit() and you don't have OPEN_MAX.  Time to
   * try the POSIX sysconf() function.  (See Stevens' _Advanced Programming
   * in the UNIX Environment_). */
  method = "POSIX sysconf";
  errno = 0;
  if ((max_descs = sysconf(_SC_OPEN_MAX)) < 0) {
    if (errno == 0)
      max_descs = CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS;
    else {
      TB_REPORT_ERRNO("sysconf", "_SC_OPEN_MAX");
      exit(1);
    }
  }
#else
  /* if everything has failed, we'll just take a guess */
  method = "random guess";
  max_descs = CONFIG_MAX_PLAYING + NUM_RESERVED_DESCS;
#endif

  /* now calculate max _players_ based on max descs */
  max_descs = MIN(CONFIG_MAX_PLAYING, max_descs - NUM_RESERVED_DESCS);

  /* game_loop() polls every socket with select(), which cannot watch a
   * descriptor >= FD_SETSIZE; new_descriptor() refuses those too. */
  if (max_descs > FD_SETSIZE - NUM_RESERVED_DESCS) {
    TB_WARN_MSG("config", "max-playing", "get_max_players",
                "player limit %d (max_playing %d, by %s) exceeds what select()"
                " can poll; using %d (FD_SETSIZE %d less %d reserved)",
                max_descs, CONFIG_MAX_PLAYING, method,
                FD_SETSIZE - NUM_RESERVED_DESCS, (int) FD_SETSIZE,
                NUM_RESERVED_DESCS);
    max_descs = FD_SETSIZE - NUM_RESERVED_DESCS;
  }

  if (max_descs <= 0) {
    log("SYSERR: Non-positive max player limit!  (Set at %d using %s).",
	    max_descs, method);
    exit(1);
  }
  log("   Setting player limit to %d using %s.", max_descs, method);
  return (max_descs);
#endif /* CIRCLE_UNIX */
}

/* game_loop contains the main loop which drives the entire MUD.  It
 * cycles once every 0.10 seconds and is responsible for accepting new
 * new connections, polling existing connections for input, dequeueing
 * output and sending it out to players, and calling "heartbeat" functions
 * such as mobile_activity(). */
void game_loop(socket_t local_mother_desc)
{
  fd_set input_set, output_set, exc_set, null_set;
  struct timeval last_time, opt_time, process_time, temp_time;
  struct timeval before_sleep, now, timeout;
  char comm[MAX_INPUT_LENGTH];
  struct descriptor_data *d, *next_d;
  int missed_pulses, maxdesc, aliased;

  /* initialize various time values */
  null_time.tv_sec = 0;
  null_time.tv_usec = 0;
  opt_time.tv_usec = OPT_USEC;
  opt_time.tv_sec = 0;
  FD_ZERO(&null_set);

  gettimeofday(&last_time, (struct timezone *) 0);

  /* The Main Loop.  The Big Cheese.  The Top Dog.  The Head Honcho.  The.. */
  while (!circle_shutdown) {

    /* Sleep if we don't have any connections */
    if (descriptor_list == NULL) {
      log("No connections.  Going to sleep.");
      FD_ZERO(&input_set);
      FD_SET(local_mother_desc, &input_set);
      if (select(local_mother_desc + 1, &input_set, (fd_set *) 0, (fd_set *) 0, NULL) < 0) {
	if (errno == EINTR)
	  log("Waking up to process signal.");
	else
	  TB_REPORT_ERRNO("select", "idle mother descriptor wait");
      } else
	log("New connection.  Waking up.");
      gettimeofday(&last_time, (struct timezone *) 0);
    }
    /* Set up the input, output, and exception sets for select(). */
    FD_ZERO(&input_set);
    FD_ZERO(&output_set);
    FD_ZERO(&exc_set);
    FD_SET(local_mother_desc, &input_set);

    maxdesc = local_mother_desc;
    for (d = descriptor_list; d; d = d->next) {
#ifndef CIRCLE_WINDOWS
      if (d->descriptor > maxdesc)
	maxdesc = d->descriptor;
#endif
      FD_SET(d->descriptor, &input_set);
      FD_SET(d->descriptor, &output_set);
      FD_SET(d->descriptor, &exc_set);
    }

    /* At this point, we have completed all input, output and heartbeat
     * activity from the previous iteration, so we have to put ourselves
     * to sleep until the next 0.1 second tick.  The first step is to
     * calculate how long we took processing the previous iteration. */

    gettimeofday(&before_sleep, (struct timezone *) 0); /* current time */
    timediff(&process_time, &before_sleep, &last_time);

    /* If we were asleep for more than one pass, count missed pulses and sleep
     * until we're resynchronized with the next upcoming pulse. */
    if (process_time.tv_sec == 0 && process_time.tv_usec < OPT_USEC) {
      missed_pulses = 0;
    } else {
      missed_pulses = process_time.tv_sec * PASSES_PER_SEC;
      missed_pulses += process_time.tv_usec / OPT_USEC;
      process_time.tv_sec = 0;
      process_time.tv_usec = process_time.tv_usec % OPT_USEC;
    }

    /* Calculate the time we should wake up */
    timediff(&temp_time, &opt_time, &process_time);
    timeadd(&last_time, &before_sleep, &temp_time);

    /* Now keep sleeping until that time has come */
    gettimeofday(&now, (struct timezone *) 0);
    timediff(&timeout, &last_time, &now);

    /* Go to sleep */
    do {
      circle_sleep(&timeout);
      gettimeofday(&now, (struct timezone *) 0);
      timediff(&timeout, &last_time, &now);
    } while (timeout.tv_usec || timeout.tv_sec);

    /* Poll (without blocking) for new input, output, and exceptions. Signal
     * handlers are installed without SA_RESTART, so a signal arriving here
     * (SIGCHLD from reap(), SIGUSR1 ...) is not a failure: poll again. */
    if (select(maxdesc + 1, &input_set, &output_set, &exc_set, &null_time) < 0) {
      if (errno == EINTR)
        continue;
      TB_REPORT_ERRNO("select", "main descriptor poll");
      return;
    }
    /* If there are new connections waiting, accept them. */
    if (FD_ISSET(local_mother_desc, &input_set))
      new_descriptor(local_mother_desc);

    /* Kick out the freaky folks in the exception set and marked for close */
    for (d = descriptor_list; d; d = next_d) {
      next_d = d->next;
      if (FD_ISSET(d->descriptor, &exc_set)) {
        FD_CLR(d->descriptor, &input_set);
	      FD_CLR(d->descriptor, &output_set);
	      close_socket(d);
      }
    }

    /* Process descriptors with input pending */
    for (d = descriptor_list; d; d = next_d) {
      next_d = d->next;
      if (FD_ISSET(d->descriptor, &input_set))
       {
        if ( d->pProtocol != NULL )      /* KaVir's plugin */
          d->pProtocol->WriteOOB = 0;    /* KaVir's plugin */
	      if (process_input(d) < 0)
	        close_socket(d);
       }
    }

    /* Process commands we just read from process_input */
    for (d = descriptor_list; d; d = next_d) {
      next_d = d->next;

      /* Not combined to retain --(d->wait) behavior. -gg 2/20/98 If no wait
       * state, no subtraction.  If there is a wait state then 1 is subtracted.
       * Therefore we don't go less than 0 ever and don't require an 'if'
       * bracket. -gg 2/27/99 */
      if (d->character) {
        GET_WAIT_STATE(d->character) -= (GET_WAIT_STATE(d->character) > 0);

        if (GET_WAIT_STATE(d->character))
          continue;
      }

      if (!get_from_q(&d->input, comm, &aliased))
        continue;

      if (d->character) {
	/* Reset the idle timer & pull char back from void if necessary */
	d->character->char_specials.timer = 0;
	if (STATE(d) == CON_PLAYING && GET_WAS_IN(d->character) != NOWHERE) {
	  if (IN_ROOM(d->character) != NOWHERE)
	    char_from_room(d->character);
	  char_to_room(d->character, GET_WAS_IN(d->character));
	  GET_WAS_IN(d->character) = NOWHERE;
	  act("$n has returned.", TRUE, d->character, 0, 0, TO_ROOM);
	}
        GET_WAIT_STATE(d->character) = 1;
      }
      d->has_prompt = FALSE;

      if (d->showstr_count) /* Reading something w/ pager */
	show_string(d, comm);
      else if (d->str)		/* Writing boards, mail, etc. */
	string_add(d, comm);
      else if (STATE(d) != CON_PLAYING) /* In menus, etc. */
	nanny(d, comm);
      else {			/* else: we're playing normally. */
	if (aliased)		/* To prevent recursive aliases. */
	  d->has_prompt = TRUE;	/* To get newline before next cmd output. */
	else if (perform_alias(d, comm, sizeof(comm)))    /* Run it through aliasing system */
	  get_from_q(&d->input, comm, &aliased);
	command_interpreter(d->character, comm); /* Send it to interpreter */
      }
    }

    /* Send queued output out to the operating system (ultimately to user). */
    for (d = descriptor_list; d; d = next_d) {
      next_d = d->next;
      if (*(d->output) && FD_ISSET(d->descriptor, &output_set)) {
	/* Output for this player is ready */
	if (process_output(d) < 0)
	  close_socket(d);
	else
	  d->has_prompt = 1;
      }
    }

    /* Print prompts for other descriptors who had no other output */
    for (d = descriptor_list; d; d = d->next) {
      if (!d->has_prompt) {
	      /* A failed write (already logged with errno by
	       * write_to_descriptor) means the peer is gone: close it below
	       * instead of writing to it again every pulse. */
	      if (write_to_descriptor(d->descriptor, make_prompt(d)) < 0) {
	        TB_WARN_MSG("network", "prompt-write", "game_loop",
	                    DIAG_DESC_FMT " prompt write failed; closing",
	                    DIAG_DESC(d));
	        STATE(d) = CON_CLOSE;
	      }
	      d->has_prompt = TRUE;
      }
    }

    /* Kick out folks in the CON_CLOSE or CON_DISCONNECT state */
    for (d = descriptor_list; d; d = next_d) {
      next_d = d->next;
      if (STATE(d) == CON_CLOSE || STATE(d) == CON_DISCONNECT)
	close_socket(d);
    }

    /* Now, we execute as many pulses as necessary--just one if we haven't
     * missed any pulses, or make up for lost time if we missed a few
     * pulses by sleeping for too long. */
    missed_pulses++;

    if (missed_pulses <= 0) {
      log("SYSERR: **BAD** MISSED_PULSES NONPOSITIVE (%d), TIME GOING BACKWARDS!!", missed_pulses);
      missed_pulses = 1;
    }

    /* If we missed more than 30 seconds worth of pulses, just do 30 secs */
    if (missed_pulses > 30 RL_SEC) {
      log("SYSERR: Missed %d seconds worth of pulses.", missed_pulses / PASSES_PER_SEC);
      missed_pulses = 30 RL_SEC;
    }

    /* Now execute the heartbeat functions */
    while (missed_pulses--)
      heartbeat(++pulse);

    /* Check for any signals we may have received. */
    if (shutdown_signal) {
      log("SYSERR: Received signal %d (SIGHUP, SIGINT or SIGTERM).  Shutting down...",
          (int) shutdown_signal);
      circle_shutdown = 1;
    }

    if (reread_wizlist) {
      reread_wizlist = FALSE;
      mudlog(CMP, LVL_IMMORT, TRUE, "Signal received - rereading wizlists.");
      reboot_wizlists();
    }

    if (emergency_unban) {
      emergency_unban = FALSE;
      int bans = 0, invalid_names = num_invalid;

      mudlog(BRF, LVL_IMMORT, TRUE, "Received SIGUSR2 - completely unrestricting game (emergent)");
      /* Free what is cleared: both lists used to be dropped by resetting
       * the pointer and count, leaking every node and name string. */
      while (ban_list) {
        struct ban_list_element *next_ban = ban_list->next;
        free(ban_list);
        ban_list = next_ban;
        bans++;
      }
      free_invalid_list();
      circle_restrict = 0;
      TB_WARN_MSG("ban", "emergency-unban", "game_loop",
                  "SIGUSR2: cleared %d site bans and %d invalid-name"
                  " entries; game unrestricted", bans, invalid_names);
    }


#ifdef CIRCLE_UNIX
    /* Update tics_passed for deadlock protection (UNIX only) */
    tics_passed++;
#endif
  }
}

void heartbeat(int heart_pulse)
{
  static int mins_since_crashsave = 0;

  event_process();

  if (!(heart_pulse % PULSE_DG_SCRIPT))
    script_trigger_check();

  if (!(heart_pulse % PASSES_PER_SEC)) {    /* EVERY second */
    msdp_update();
    next_tick--;
  }

  if (!(heart_pulse % PULSE_ZONE))
    zone_update();

  if (!(heart_pulse % PULSE_IDLEPWD))		/* 15 seconds */
    check_idle_passwords();

  if (!(heart_pulse % PULSE_MOBILE))
    mobile_activity();

  if (!(heart_pulse % PULSE_VIOLENCE))
    perform_violence();

  if (!(heart_pulse % (SECS_PER_MUD_HOUR * PASSES_PER_SEC))) {  /* Tick ! */
    next_tick = SECS_PER_MUD_HOUR;  /* Reset tick coundown */
    weather_and_time(1);
    check_time_triggers();
    affect_update();
    point_update();
    check_timed_quests();
  }

  if (CONFIG_AUTO_SAVE && !(heart_pulse % PULSE_AUTOSAVE)) {	/* 1 minute */
    if (++mins_since_crashsave >= CONFIG_AUTOSAVE_TIME) {
      mins_since_crashsave = 0;
      Crash_save_all();
      House_save_all();
    }
  }

  if (!(heart_pulse % PULSE_USAGE))
    record_usage();

  if (!(heart_pulse % PULSE_TIMESAVE))
  save_mud_time(&time_info);

  /* Every pulse! Don't want them to stink the place up... */
  extract_pending_chars();
}

/* new code to calculate time differences, which works on systems for which
 * tv_usec is unsigned (and thus comparisons for something being < 0 fail).
 * Based on code submitted by ss@sirocco.cup.hp.com. Code to return the time
 * difference between a and b (a-b). Always returns a nonnegative value
 * (floors at 0). */
static void timediff(struct timeval *rslt, struct timeval *a, struct timeval *b)
{
  if (a->tv_sec < b->tv_sec)
    *rslt = null_time;
  else if (a->tv_sec == b->tv_sec) {
    if (a->tv_usec < b->tv_usec)
      *rslt = null_time;
    else {
      rslt->tv_sec = 0;
      rslt->tv_usec = a->tv_usec - b->tv_usec;
    }
  } else {			/* a->tv_sec > b->tv_sec */
    rslt->tv_sec = a->tv_sec - b->tv_sec;
    if (a->tv_usec < b->tv_usec) {
      rslt->tv_usec = a->tv_usec + 1000000 - b->tv_usec;
      rslt->tv_sec--;
    } else
      rslt->tv_usec = a->tv_usec - b->tv_usec;
  }
}

/* Add 2 time values.  Patch sent by "d. hall" to fix 'static' usage. */
static void timeadd(struct timeval *rslt, struct timeval *a, struct timeval *b)
{
  rslt->tv_sec = a->tv_sec + b->tv_sec;
  rslt->tv_usec = a->tv_usec + b->tv_usec;

  while (rslt->tv_usec >= 1000000) {
    rslt->tv_usec -= 1000000;
    rslt->tv_sec++;
  }
}

static void record_usage(void)
{
  int sockets_connected = 0, sockets_playing = 0;
  struct descriptor_data *d;

  for (d = descriptor_list; d; d = d->next) {
    sockets_connected++;
    if (IS_PLAYING(d))
      sockets_playing++;
  }

  log("nusage: %-3d sockets connected, %-3d sockets playing",
	  sockets_connected, sockets_playing);

#ifdef RUSAGE	/* Not RUSAGE_SELF because it doesn't guarantee prototype. */
  {
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    log("rusage: user time: %ld sec, system time: %ld sec, max res size: %ld",
	    ru.ru_utime.tv_sec, ru.ru_stime.tv_sec, ru.ru_maxrss);
  }
#endif
}

/* Turn off echoing (specific to telnet client) */
void echo_off(struct descriptor_data *d)
{
  char off_string[] =
  {
    (char) IAC,
    (char) WILL,
    (char) TELOPT_ECHO,
    (char) 0,
  };

  write_to_output(d, "%s", off_string);
}

/* Turn on echoing (specific to telnet client) */
void echo_on(struct descriptor_data *d)
{
  char on_string[] =
  {
    (char) IAC,
    (char) WONT,
    (char) TELOPT_ECHO,
    (char) 0
  };

  write_to_output(d, "%s", on_string);
}

static char *make_prompt(struct descriptor_data *d)
{
  static char prompt[MAX_PROMPT_LENGTH];

  /* Note, prompt is truncated at MAX_PROMPT_LENGTH chars (structs.h) */

  if (d->showstr_count)
    tb_snprintf(prompt, sizeof(prompt),
      "[ Return to continue, (q)uit, (r)efresh, (b)ack, or page number (%d/%d) ]",
      d->showstr_page, d->showstr_count);
  else if (d->str)
    strlcpy(prompt, "] ", sizeof(prompt));	/* strcpy: OK (for 'MAX_PROMPT_LENGTH >= 3') */
  else if (STATE(d) == CON_PLAYING && !IS_NPC(d->character)) {
    int count;
    size_t len = 0;

    *prompt = '\0';

    if (GET_INVIS_LEV(d->character) && len < sizeof(prompt)) {
      count = snprintf(prompt + len, sizeof(prompt) - len, "i%d ", GET_INVIS_LEV(d->character));
      if (count >= 0)
        len += count;
    }
    /* show only when below 25% */
    if (PRF_FLAGGED(d->character, PRF_DISPAUTO) && len < sizeof(prompt)) {
      struct char_data *ch = d->character;
      if (BELOW_QUARTER(GET_HIT(ch), GET_MAX_HIT(ch))) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dH ", GET_HIT(ch));
        if (count >= 0)
          len += count;
      }
      if (BELOW_QUARTER(GET_MANA(ch), GET_MAX_MANA(ch)) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dM ", GET_MANA(ch));
        if (count >= 0)
          len += count;
      }
      if (BELOW_QUARTER(GET_MOVE(ch), GET_MAX_MOVE(ch)) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dV ", GET_MOVE(ch));
        if (count >= 0)
          len += count;
      }
    } else { /* not auto prompt */
      if (PRF_FLAGGED(d->character, PRF_DISPHP) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dH ", GET_HIT(d->character));
        if (count >= 0)
          len += count;
      }

      if (PRF_FLAGGED(d->character, PRF_DISPMANA) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dM ", GET_MANA(d->character));
        if (count >= 0)
          len += count;
      }

      if (PRF_FLAGGED(d->character, PRF_DISPMOVE) && len < sizeof(prompt)) {
        count = snprintf(prompt + len, sizeof(prompt) - len, "%dV ", GET_MOVE(d->character));
        if (count >= 0)
          len += count;
      }
    }

    if (PRF_FLAGGED(d->character, PRF_BUILDWALK) && len < sizeof(prompt)) {
      count = snprintf(prompt + len, sizeof(prompt) - len, "BUILDWALKING ");
      if (count >= 0)
        len += count;
    }

    if (PRF_FLAGGED(d->character, PRF_AFK) && len < sizeof(prompt)) {
      count = snprintf(prompt + len, sizeof(prompt) - len, "AFK ");
      if (count >= 0)
        len += count;
    }

     if (GET_LAST_NEWS(d->character) < newsmod)
     {
       count = snprintf(prompt + len, sizeof(prompt) - len, "(news) ");
       if (count >= 0)
         len += count;
     }

     if (GET_LAST_MOTD(d->character) < motdmod)
     {
       count = snprintf(prompt + len, sizeof(prompt) - len, "(motd) ");
       if (count >= 0)
         len += count;
     }

    if (len < sizeof(prompt))
      strncat(prompt, "> ", sizeof(prompt) - len - 1);	/* strncat: OK */
  } else if (STATE(d) == CON_PLAYING && IS_NPC(d->character))
    tb_snprintf(prompt, sizeof(prompt), "%s> ", GET_NAME(d->character));
  else
    *prompt = '\0';

  return (prompt);
}

/* NOTE: 'txt' must be at most MAX_INPUT_LENGTH big. */
/* Bound pending commands per descriptor to prevent input floods from
 * growing an unbounded linked list while the game loop is busy. */
#define TB_MAX_PENDING_COMMANDS 64

/* Append a command to an input queue. Returns 1 if it was queued, 0 if it
 * was dropped: the queue already holds TB_MAX_PENDING_COMMANDS entries
 * (the remote player is typing faster than the game executes), allocation
 * failed, or an argument was NULL. Every drop is reported here; the flood
 * case is left to the caller, which knows the descriptor (process_input()
 * reports it with DIAG_DESC and tells the player). */
int write_to_q(const char *txt, struct txt_q *queue, int aliased)
{
  struct txt_block *newt;
  struct txt_block *scan;
  size_t pending = 0;

  if (queue == NULL || txt == NULL) {
    TB_ERROR_MSG("network", "input-queue", "write_to_q",
                 "NULL %s; command dropped", queue ? "text" : "queue");
    return (0);
  }

  for (scan = queue->head; scan != NULL; scan = scan->next) {
    if (++pending >= TB_MAX_PENDING_COMMANDS)
      return (0);
  }

  CREATE(newt, struct txt_block, 1);
  newt->text = strdup(txt);
  if (newt->text == NULL) {
    int saved_errno = errno;
    free(newt);
    errno = saved_errno;
    TB_REPORT_ERRNO("strdup", "input command queue");
    return (0);
  }
  newt->aliased = aliased;

  /* queue empty? */
  if (!queue->head) {
    newt->next = NULL;
    queue->head = queue->tail = newt;
  } else {
    queue->tail->next = newt;
    queue->tail = newt;
    newt->next = NULL;
  }
  return (1);
}

/* NOTE: 'dest' must be at least MAX_INPUT_LENGTH big. */
static int get_from_q(struct txt_q *queue, char *dest, int *aliased)
{
  struct txt_block *tmp;

  /* queue empty? */
  if (!queue->head)
    return (0);

  strlcpy(dest, queue->head->text, MAX_INPUT_LENGTH);	/* strcpy: OK (mutual MAX_INPUT_LENGTH) */
  *aliased = queue->head->aliased;

  tmp = queue->head;
  queue->head = queue->head->next;
  if (queue->head == NULL)
    queue->tail = NULL;
  free(tmp->text);
  free(tmp);

  return (1);
}

/* Discard queued, not-yet-executed input lines (the "--" command). Output
 * state is untouched: the descriptor keeps using its current buffer. */
static void flush_input_queue(struct descriptor_data *d)
{
  while (d->input.head) {
    struct txt_block *tmp = d->input.head;
    d->input.head = d->input.head->next;
    free(tmp->text);
    free(tmp);
  }
  d->input.tail = NULL;
}

/* Output buffer state machine (vwrite_to_output() fills it, process_output()
 * drains it). Exactly one buffer is live:
 *   small: large_outbuf == NULL, output == small_outbuf;
 *   large: output == large_outbuf->text (taken from bufpool or allocated).
 * In both states 0 <= bufptr, 0 <= bufspace, bufptr + bufspace equals the
 * live buffer's capacity - 1, and output[bufptr] is the terminating NUL.
 * bufspace == 0 is the overflow state: further output is discarded until
 * process_output() empties the buffer. Both macros evaluate d several times. */
#define OUTBUF_CAPACITY(d) ((d)->large_outbuf ? LARGE_BUFSIZE : SMALL_BUFSIZE)
#define OUTBUF_INVARIANT_OK(d) \
  ((d)->bufptr >= 0 && (d)->bufspace >= 0 && \
   (d)->output == ((d)->large_outbuf ? (d)->large_outbuf->text \
                                     : (d)->small_outbuf) && \
   (d)->bufptr + (d)->bufspace == OUTBUF_CAPACITY(d) - 1)

/* Return d's large output buffer (if any) to the pool and leave d in the
 * empty small-buffer state. */
static void outbuf_reset_small(struct descriptor_data *d)
{
  if (d->large_outbuf) {
    d->large_outbuf->next = bufpool;
    bufpool = d->large_outbuf;
    d->large_outbuf = NULL;
  }
  d->output = d->small_outbuf;
  *d->output = '\0';
  d->bufptr = 0;
  d->bufspace = SMALL_BUFSIZE - 1;
}

/* Verify the output buffer invariant above at a state-machine boundary.
 * 'full' also checks that the text length equals bufptr (an O(n) scan, so
 * it is used once per flush, not on every write). A violation means earlier
 * bookkeeping wrote outside the live buffer or will do so next; it is
 * reported with the descriptor identity and the buffer is reset, discarding
 * the pending output, rather than trusting a corrupt offset. */
static bool outbuf_check(struct descriptor_data *d, const char *where, bool full)
{
  long len = -1;

  if (OUTBUF_INVARIANT_OK(d)) {
    if (full)
      len = (long) strlen(d->output);
    else if (d->output[d->bufptr] == '\0')
      len = d->bufptr; /* terminator in place; length not rescanned */
    if (len == d->bufptr)
      return TRUE;
  }

  TB_ERROR_MSG("network", "outbuf-invariant", where,
               DIAG_DESC_FMT " bufptr=%d bufspace=%d large=%d text-len=%ld:"
               " output buffer bookkeeping is inconsistent; pending output"
               " discarded", DIAG_DESC(d), d->bufptr, d->bufspace,
               d->large_outbuf != NULL, len);
  outbuf_reset_small(d);
  return FALSE;
}

/* Release a closing connection's queues. The large output buffer goes back
 * to the shared pool, so this descriptor must never write output again:
 * only close_socket() may call this. The pointer is cleared so a stray later
 * use cannot write into a buffer another descriptor now owns. */
static void flush_queues(struct descriptor_data *d)
{
  outbuf_reset_small(d);
  flush_input_queue(d);
}

/**
 * Queue printf-style output for a descriptor (for use outside comm.c).
 * @param t   Destination; must be non-NULL with a live pProtocol.
 * @param txt Literal printf format; arguments may be untrusted text.
 * @return Space left in the output buffer after queuing (0 = the buffer is
 *         in the overflow state and further output is being discarded).
 */
size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{
  va_list args;
  size_t left;

  va_start(args, txt);
  left = vwrite_to_output(t, txt, args);
  va_end(args);

  return left;
}

/**
 * va_list form of write_to_output(). Formats into a static buffer, runs the
 * result through ProtocolOutput() (colour, MXP and unicode escapes) and
 * appends it to the descriptor's output buffer, switching from the small
 * to a large buffer when needed and into the overflow state when even that
 * is full (see the state machine above outbuf_reset_small()).
 *
 * Not reentrant: the format buffer is static. Every truncation is reported
 * (network/output-truncate, network/output-overflow); a broken buffer
 * invariant is reported as network/outbuf-invariant and the buffer reset.
 * @return Space left in the output buffer; 0 means overflow state.
 */
size_t vwrite_to_output(struct descriptor_data *t, const char *format, va_list args)
{
  const char *text_overflow = "\r\nOVERFLOW\r\n";
  static char txt[MAX_STRING_LENGTH];
  const char *translated;
  int size, proto_len;
  bool fmt_truncated;

  /* The offsets below index t->output directly; never write through a
   * broken bufptr (it would land in neighbouring descriptor fields). */
  outbuf_check(t, "vwrite_to_output", FALSE);

  /* if we're in the overflow state already, ignore this new output */
  if (t->bufspace == 0)
    return (0);

  size = vsnprintf(txt, sizeof(txt), format, args);
  if (size < 0) {
    TB_ERROR_ERRNO("network", "output-format", "vwrite_to_output",
                   DIAG_DESC_FMT " vsnprintf failed; message dropped",
                   DIAG_DESC(t));
    return (t->bufspace);
  }
  fmt_truncated = ((size_t) size >= sizeof(txt));
  if (fmt_truncated) {
    TB_WARN_EVERY(60, "network", "output-truncate", "vwrite_to_output",
                  DIAG_DESC_FMT " formatted text needs %d bytes; cut to %d",
                  DIAG_DESC(t), size, (int) sizeof(txt) - 1);
    size = (int) sizeof(txt) - 1;
  }

  /* KaVir's protocol layer translates colour/MXP/unicode escapes. Its
   * length argument is an int in/out parameter (it used to be handed a
   * size_t through an int *). The result is measured with strlen() so
   * bufptr always matches the text actually copied. ProtocolOutput()
   * returns its argument unchanged when there is no protocol state, so
   * never strlcpy() txt onto itself. */
  proto_len = size;
  translated = ProtocolOutput(t, txt, &proto_len);
  if (translated != txt)
    strlcpy(txt, translated, sizeof(txt));
  size = (int) strlen(txt);
  if (t->pProtocol && t->pProtocol->WriteOOB > 0)
    --t->pProtocol->WriteOOB;

  /* Mark output that was cut above, making room for the marker if the
   * translated text still fills txt. */
  if (fmt_truncated) {
    int room = (int) (sizeof(txt) - 1 - strlen(text_overflow));
    if (size > room) {
      size = room;
      txt[size] = '\0';
    }
    strlcat(txt, text_overflow, sizeof(txt));
    size = (int) strlen(txt);
  }

  /* If the text is too big to fit into even a large buffer, truncate
   * the new text to make it fit.  (This will switch to the overflow
   * state automatically because t->bufspace will end up 0.) */
  if (size + t->bufptr + 1 > LARGE_BUFSIZE) {
    int dropped = size - (LARGE_BUFSIZE - t->bufptr - 1);

    size = LARGE_BUFSIZE - t->bufptr - 1;
    txt[size] = '\0';
    buf_overflows++;
    /* Once per overflow episode: afterwards bufspace is 0 and further
     * output is discarded at the top of this function until the buffer is
     * flushed (the player sees **OVERFLOW**). Rate-limited because any
     * player can cause it by requesting enough output. */
    TB_WARN_EVERY(60, "network", "output-overflow", "vwrite_to_output",
                  DIAG_DESC_FMT " output buffer full: dropped %d bytes"
                  " (bufptr=%d)", DIAG_DESC(t), dropped, t->bufptr);
  }

  /* If we have enough space, just write to buffer and that's it! If the
   * text just barely fits, then it's switched to a large buffer instead. */
  if (t->bufspace > size) {
    strlcpy(t->output + t->bufptr, txt, (size_t)t->bufspace);	/* strcpy: OK (size checked above) */
    t->bufspace -= size;
    t->bufptr += size;
    return (t->bufspace);
  }

  /* Already in the large buffer: the clamp above guarantees the text fits
   * exactly (size == bufspace), leaving the overflow state. Never switch
   * again here; that would replace large_outbuf and leak the live one. */
  if (t->large_outbuf) {
    strlcpy(t->output + t->bufptr, txt, (size_t)t->bufspace + 1);	/* strcpy: OK (bufptr + bufspace + 1 == LARGE_BUFSIZE) */
    t->bufptr += size;
    t->bufspace -= size;
    return (t->bufspace);
  }

  buf_switches++;

  /* if the pool has a buffer in it, grab it */
  if (bufpool != NULL) {
    t->large_outbuf = bufpool;
    bufpool = bufpool->next;
  } else {			/* else create a new one */
    CREATE(t->large_outbuf, struct txt_block, 1);
    CREATE(t->large_outbuf->text, char, LARGE_BUFSIZE);
    buf_largecount++;
  }

  strlcpy(t->large_outbuf->text, t->output, LARGE_BUFSIZE);	/* strcpy: OK (size checked previously) */
  t->output = t->large_outbuf->text;	/* make big buffer primary */
  strlcat(t->output, txt, LARGE_BUFSIZE);	/* strcat: OK (size checked) */

  /* set the pointer for the next write */
  t->bufptr = strlen(t->output);

  /* calculate how much space is left in the buffer */
  t->bufspace = LARGE_BUFSIZE - 1 - t->bufptr;

  return (t->bufspace);
}

static void free_bufpool(void)
{
  struct txt_block *tmp;

  while (bufpool) {
    tmp = bufpool->next;
    if (bufpool->text)
      free(bufpool->text);
    free(bufpool);
    bufpool = tmp;
  }
}

/*  socket handling */
/* get_bind_addr: Return a struct in_addr that should be used in our
 * call to bind().  If the user has specified a desired binding
 * address, we try to bind to it; otherwise, we bind to INADDR_ANY.
 * Note that inet_aton() is preferred over inet_addr() so we use it if
 * we can.  If neither is available, we always bind to INADDR_ANY. */
static struct in_addr *get_bind_addr()
{
  static struct in_addr bind_addr;

  /* Clear the structure */
  memset((char *) &bind_addr, 0, sizeof(bind_addr));

  /* If DLFT_IP is unspecified, use INADDR_ANY */
  if (CONFIG_DFLT_IP == NULL) {
    bind_addr.s_addr = htonl(INADDR_ANY);
  } else {
    /* If the parsing fails, use INADDR_ANY */
    if (!parse_ip(CONFIG_DFLT_IP, &bind_addr)) {
      log("SYSERR: DFLT_IP of %s appears to be an invalid IP address",
          CONFIG_DFLT_IP);
      bind_addr.s_addr = htonl(INADDR_ANY);
    }
  }

  /* Put the address that we've finally decided on into the logs */
  if (bind_addr.s_addr == htonl(INADDR_ANY))
    log("Binding to all IP interfaces on this host.");
  else
    log("Binding only to IP address %s", inet_ntoa(bind_addr));

  return (&bind_addr);
}

#ifdef HAVE_INET_ATON
/* inet_aton's interface is the same as parse_ip's: 0 on failure, non-0 if
 * successful. */
static int parse_ip(const char *addr, struct in_addr *inaddr)
{
  return (inet_aton(addr, inaddr));
}

#elif HAVE_INET_ADDR

/* inet_addr has a different interface, so we emulate inet_aton's */
int parse_ip(const char *addr, struct in_addr *inaddr)
{
  long ip;

  if ((ip = inet_addr(addr)) == -1) {
    return (0);
  } else {
    inaddr->s_addr = (unsigned long) ip;
    return (1);
  }
}

#else
/* If you have neither function - sorry, you can't do specific binding. */
int parse_ip(const char *addr, struct in_addr *inaddr)
{
  log("SYSERR: warning: you're trying to set DFLT_IP but your system has no "
      "functions to parse IP addresses (how bizarre!)");
  return (0);
}
#endif /* INET_ATON and INET_ADDR */

/* Sets the kernel's send buffer size for the descriptor */
static int set_sendbuf(socket_t s)
{
#if defined(SO_SNDBUF) && !defined(CIRCLE_MACINTOSH)
  int opt = MAX_SOCK_BUF;

  if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &opt, sizeof(opt)) < 0) {
    TB_REPORT_ERRNO("setsockopt", "SO_SNDBUF");
    return (-1);
  }
#endif

  return (0);
}

/* Initialize a descriptor */
static void init_descriptor (struct descriptor_data *newd, int desc)
{
  static int last_desc = 0;	/* last descriptor number */

  newd->descriptor = desc;
  newd->idle_tics = 0;
  newd->output = newd->small_outbuf;
  newd->bufspace = SMALL_BUFSIZE - 1;
  newd->login_time = time(0);
  *newd->output = '\0';
  newd->bufptr = 0;
  newd->has_prompt = 1;  /* prompt is part of greetings */
  STATE(newd) = CONFIG_PROTOCOL_NEGOTIATION ? CON_GET_PROTOCOL : CON_GET_NAME;
  CREATE(newd->history, char *, HISTORY_SIZE);
  if (++last_desc == 1000)
    last_desc = 1;
  newd->desc_num = last_desc;
  newd->pProtocol = ProtocolCreate(); /* KaVir's plugin*/
  newd->events = create_list();
  
}

/* Fixed-size IPv4 connection admission table. Avoids allocating memory for
 * unauthenticated connection attempts. Counts are reset after each window;
 * denied attempts do not extend the window indefinitely.
 *
 * Admission policy (new_descriptor()), in order: at most
 * TB_RATE_MAX_ATTEMPTS per address per TB_RATE_WINDOW_SECONDS
 * (network/admit-deny); fd below FD_SETSIZE (network/accept-limit);
 * fewer than MIN(CONFIG_MAX_PLAYING, max_players) connections
 * (network/game-full); site not BAN_ALL (mudlog). All refusals are logged,
 * rate-limited, with the numeric address; the DNS name is peer-controlled
 * and is only used for ban matching. */
#define TB_RATE_SLOTS 256
#define TB_RATE_WINDOW_SECONDS 60
#define TB_RATE_MAX_ATTEMPTS 12

struct tb_rate_entry {
  unsigned long addr;
  time_t window_start;
  time_t last_seen;
  unsigned int attempts;
};

static struct tb_rate_entry tb_rate_entries[TB_RATE_SLOTS];

static int tb_admit_connection(unsigned long addr, time_t now)
{
  size_t i, slot = 0;
  int found = 0;

  for (i = 0; i < TB_RATE_SLOTS; ++i) {
    if (tb_rate_entries[i].attempts != 0 &&
        tb_rate_entries[i].addr == addr) {
      slot = i;
      found = 1;
      break;
    }
    if (tb_rate_entries[i].attempts == 0) {
      slot = i;
      found = 2;
      break;
    }
    if (tb_rate_entries[i].last_seen < tb_rate_entries[slot].last_seen)
      slot = i;
  }

  if (found != 1 ||
      now < tb_rate_entries[slot].window_start ||
      now - tb_rate_entries[slot].window_start >= TB_RATE_WINDOW_SECONDS) {
    tb_rate_entries[slot].addr = addr;
    tb_rate_entries[slot].window_start = now;
    tb_rate_entries[slot].attempts = 0;
  }
  tb_rate_entries[slot].last_seen = now;
  if (tb_rate_entries[slot].attempts >= TB_RATE_MAX_ATTEMPTS)
    return 0;
  ++tb_rate_entries[slot].attempts;
  return 1;
}

static int new_descriptor(socket_t s)
{
  socket_t desc;
  int sockets_connected = 0;
  int greetsize;
  socklen_t i;
  struct descriptor_data *newd;
  struct sockaddr_in peer;
  struct hostent *from;
  
  /* accept the new connection */
  i = sizeof(peer);
  if ((desc = accept(s, (struct sockaddr *) &peer, &i)) == INVALID_SOCKET) {
#ifndef CIRCLE_WINDOWS
    TB_REPORT_ERRNO("accept", "mother descriptor");
#else
    log("SYSERR: accept failed on mother descriptor: Winsock error #%d", WSAGetLastError());
#endif
    return (-1);
  }
  /* Drop bursts before hostname lookup, allocation, or protocol negotiation.
   * Denials are reported through TB_WARN_EVERY: the first one at once, then
   * a count per minute, so a connection storm is visible without letting
   * it amplify into the disk log. Only the numeric address is logged. */
  if (!tb_admit_connection((unsigned long)peer.sin_addr.s_addr, time(NULL))) {
    TB_WARN_EVERY(60, "network", "admit-deny", "new_descriptor",
                  "fd %ld from %s: more than %d connection attempts in %d s;"
                  " refused", (long) desc, inet_ntoa(peer.sin_addr),
                  TB_RATE_MAX_ATTEMPTS, TB_RATE_WINDOW_SECONDS);
    CLOSE_SOCKET(desc);
    return (0);
  }
  /* keep it from blocking */
  nonblock(desc);

  /* set the send buffer size */
  if (set_sendbuf(desc) < 0) {
    CLOSE_SOCKET(desc);
    return (0);
  }

  /* Make sure we have room for it. Three limits apply:
   *  - CONFIG_MAX_PLAYING: set by implementors (cedit, config file), any
   *    value is accepted there;
   *  - max_players: what the process descriptor limit allows, computed at
   *    boot by get_max_players();
   *  - FD_SETSIZE: game_loop() watches every socket with select(); FD_SET
   *    on a descriptor >= FD_SETSIZE writes past its stack fd_set.
   * A remote peer can hit these at will, so the reports are rate-limited. */
  for (newd = descriptor_list; newd; newd = newd->next)
    sockets_connected++;

#ifndef CIRCLE_WINDOWS /* Winsock's FD_SETSIZE counts sockets, not values */
  if (desc >= FD_SETSIZE) {
    TB_ERROR_EVERY(60, "network", "accept-limit", "new_descriptor",
                   "fd %ld >= FD_SETSIZE %d (connected %d, max_playing %d,"
                   " max_players %d): select() cannot watch it; connection"
                   " from %s refused", (long) desc, (int) FD_SETSIZE,
                   sockets_connected, CONFIG_MAX_PLAYING, max_players,
                   inet_ntoa(peer.sin_addr));
    write_to_descriptor(desc, "Sorry, the game is full right now... please try again later!\r\n");
    CLOSE_SOCKET(desc);
    return (0);
  }
#endif

  if (sockets_connected >= MIN(CONFIG_MAX_PLAYING, max_players)) {
    TB_WARN_EVERY(60, "network", "game-full", "new_descriptor",
                  "%d connections, limit %d (max_playing %d, max_players %d):"
                  " connection from %s refused", sockets_connected,
                  MIN(CONFIG_MAX_PLAYING, max_players), CONFIG_MAX_PLAYING,
                  max_players, inet_ntoa(peer.sin_addr));
    write_to_descriptor(desc, "Sorry, the game is full right now... please try again later!\r\n");
    CLOSE_SOCKET(desc);
    return (0);
  }
  /* create a new descriptor */
  CREATE(newd, struct descriptor_data, 1);

  /* find the sitename */
  if (CONFIG_NS_IS_SLOW ||
      !(from = gethostbyaddr((char *) &peer.sin_addr,
		             sizeof(peer.sin_addr), AF_INET))) {

    /* resolution failed */
    if (!CONFIG_NS_IS_SLOW) {
#ifdef CIRCLE_WINDOWS
      log("SYSERR: gethostbyaddr failed for %s (Winsock error #%d)",
          inet_ntoa(peer.sin_addr), WSAGetLastError());
#else
      log("SYSERR: gethostbyaddr failed for %s (h_errno=%d)",
          inet_ntoa(peer.sin_addr), h_errno);
#endif
    }

    /* find the numeric site address */
    strncpy(newd->host, (char *)inet_ntoa(peer.sin_addr), HOST_LENGTH);	/* strncpy: OK (n->host:HOST_LENGTH+1) */
    *(newd->host + HOST_LENGTH) = '\0';
  } else {
    strncpy(newd->host, from->h_name, HOST_LENGTH);	/* strncpy: OK (n->host:HOST_LENGTH+1) */
    *(newd->host + HOST_LENGTH) = '\0';
  }

  /* determine if the site is banned */
  if (isbanned(newd->host) == BAN_ALL) {
    CLOSE_SOCKET(desc);
    mudlog(CMP, LVL_GOD, TRUE, "Connection attempt denied from [%s]", newd->host);
    free(newd);
    return (0);
  }

  /* initialize descriptor data */
  init_descriptor(newd, desc);

  /* prepend to list */
  newd->next = descriptor_list;
  descriptor_list = newd;

  if (CONFIG_PROTOCOL_NEGOTIATION) {
    /* Attach Event */ 
    NEW_EVENT(ePROTOCOLS, newd, NULL, 1.5 * PASSES_PER_SEC);
    /* KaVir's plugin*/
    write_to_output(newd, "Attempting to Detect Client, Please Wait...\r\n");
    ProtocolNegotiate(newd);
  } else {
    greetsize = strlen(GREETINGS);
    write_to_output(newd, "%s", ProtocolOutput(newd, GREETINGS, &greetsize));
  }
  return (0);
}

/**
 * Send a descriptor's queued output to its socket. The text sent ("osb") is
 * t->output plus, when applicable, "**OVERFLOW**", an extra CRLF for
 * non-compact players and the prompt; if the player was sitting at a
 * prompt a CRLF is prepended (and not counted). 32 byte GARBAGE_SPACE in
 * MAX_SOCK_BUF covers: 2 bytes prepended CRLF, 14 bytes overflow message,
 * 2 bytes extra CRLF, 14 bytes unused.
 *
 * Partial writes: if the kernel took all of t->output but only part of the
 * trailer, the unsent trailer becomes the new (small) buffer contents; if
 * it took less than t->output, the rest is moved to the front of the
 * buffer. A snooper is sent exactly the bytes that went out.
 *
 * @return -1 on a fatal socket error (the caller closes the descriptor),
 *         0 if nothing could be sent (socket buffer full, try later),
 *         otherwise the number of bytes of osb sent.
 */
static int process_output(struct descriptor_data *t)
{
  char i[MAX_SOCK_BUF], *osb = i + 2;
  int result;

  /* we may need this \r\n for later -- see below */
  strlcpy(i, "\r\n", sizeof(i));	/* strcpy: OK (for 'MAX_SOCK_BUF >= 3') */

  /* now, append the 'real' output */
  strlcpy(osb, t->output, sizeof(i) - 2);	/* strcpy: OK (t->output:LARGE_BUFSIZE < osb:MAX_SOCK_BUF-2) */

  /* if we're in the overflow state, notify the user */
  if (t->bufspace == 0)
    strlcat(osb, "**OVERFLOW**\r\n", sizeof(i) - 2);	/* strcpy: OK (osb:MAX_SOCK_BUF-2 reserves space) */

  /* add the extra CRLF if the person isn't in compact mode */
  if (STATE(t) == CON_PLAYING && t->character && !IS_NPC(t->character) && !PRF_FLAGGED(t->character, PRF_COMPACT))
    if ( !t->pProtocol->WriteOOB ) 
      strlcat(osb, "\r\n", sizeof(i) - 2);	/* strcpy: OK (osb:MAX_SOCK_BUF-2 reserves space) */

  if (!t->pProtocol->WriteOOB) /* add a prompt */
    strlcat(i, make_prompt(t), sizeof(i));	/* strcpy: OK (i:MAX_SOCK_BUF reserves space) */

  /* now, send the output.  If this is an 'interruption', use the prepended
   * CRLF, otherwise send the straight output sans CRLF. */
  if (t->has_prompt && !t->pProtocol->WriteOOB) {
    t->has_prompt = FALSE;
    result = write_to_descriptor(t->descriptor, i);
    /* 'result' must count bytes of osb. If only part of the prepended CRLF
     * went out, none of osb did. */
    if (result >= 2)
      result -= 2;
    else if (result > 0)
      result = 0;
  } else
    result = write_to_descriptor(t->descriptor, osb);

  if (result < 0) {	/* Oops, fatal error. Bye! */
//    close_socket(t); // close_socket is called after return of negative result
    return (-1);
  } else if (result == 0)	/* Socket buffer full. Try later. */
    return (0);

  /* Handle snooping: show the snooper what was actually sent, as
   * "% <text>%". The precision (not a field width, as the old "%*s" was)
   * limits it to the 'result' bytes the kernel accepted; unsent text is
   * shown when it is sent. */
  if (t->snoop_by)
    write_to_output(t->snoop_by, "%% %.*s%%", result, t->output);

  /* The common case: all saved output was handed off to the kernel buffer. */
  if (result >= t->bufptr) {
    /* If we were using a large buffer, put the large buffer on the buffer pool
     * and switch back to the small one. */
    outbuf_reset_small(t);

    /* If the overflow message or prompt were partially written (the kernel
     * took all of t->output but not the whole of osb), carry the unsent
     * tail over as the start of the new small buffer. The tail is at most
     * GARBAGE_SPACE + MAX_PROMPT_LENGTH bytes, so it always fits; the
     * offsets are recomputed from the copied text rather than adjusted, so
     * bufptr can never go negative (it once did, and the next write
     * landed in d->last_input). */
    if ((size_t)result < strlen(osb)) {
      size_t savetextlen = strlen(osb + result);

      if (strlcpy(t->output, osb + result, SMALL_BUFSIZE) >= SMALL_BUFSIZE)
        TB_ERROR_MSG("network", "output-tail-truncate", "process_output",
                     DIAG_DESC_FMT " unsent tail of %lu bytes exceeds the"
                     " %d-byte small buffer; tail cut", DIAG_DESC(t),
                     (unsigned long) savetextlen, SMALL_BUFSIZE);
      t->bufptr = (int) strlen(t->output);
      t->bufspace = SMALL_BUFSIZE - 1 - t->bufptr;
    }

  } else {
    /* Not all data in buffer sent.  result < output buffersize. */
    memmove(t->output, t->output + result,
	    strlen(t->output + result) + 1);	/* memmove: the copy overlaps */
    t->bufptr   -= result;
    t->bufspace += result;
  }

  outbuf_check(t, "process_output", TRUE);
  return (result);
}

/* perform_socket_write: takes a descriptor, a pointer to text, and a
 * text length, and tries once to send that text to the OS.  This is
 * where we stuff all the platform-dependent stuff that used to be
 * ugly #ifdef's in write_to_descriptor(). This function must return:
 * -1  If a fatal error was encountered in writing to the descriptor.
 *  0  If a transient failure was encountered (e.g. socket buffer full).
 * >0  To indicate the number of bytes successfully written, possibly
 *     fewer than the number the caller requested be written.
 * Right now there are two versions of this function: one for Windows,
 * and one for all other platforms. */

#if defined(CIRCLE_WINDOWS)
ssize_t perform_socket_write(socket_t desc, const char *txt, size_t length)
{
  ssize_t result;

  result = send(desc, txt, length, 0);

  if (result > 0) {
    /* Write was successful */
    return (result);
  }

  if (result == 0) {
    /* This should never happen! */
    log("SYSERR: Huh??  write() returned 0???  Please report this!");
    return (-1);
  }

  /* result < 0: An error was encountered. */

  /* Transient error? */
  if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR)
    return (0);

  /* Must be a fatal error. */
  return (-1);
}

#else

#if defined(CIRCLE_ACORN)
#define write	socketwrite
#endif

/* perform_socket_write for all Non-Windows platforms */
static ssize_t perform_socket_write(socket_t desc, const char *txt, size_t length)
{
  ssize_t result;

  result = write(desc, txt, length);

  if (result > 0) {
    /* Write was successful. */
    return (result);
  }

  if (result == 0) {
    /* This should never happen! */
    log("SYSERR: Huh??  write() returned 0???  Please report this!");
    return (-1);
  }

  /* result < 0, so an error was encountered - is it transient? Unfortunately,
   * different systems use different constants to indicate this. */

#ifdef EAGAIN		/* POSIX */
  if (errno == EAGAIN)
    return (0);
#endif

#ifdef EWOULDBLOCK	/* BSD */
  if (errno == EWOULDBLOCK)
    return (0);
#endif

#ifdef EDEADLK		/* Macintosh */
  if (errno == EDEADLK)
    return (0);
#endif

  /* Looks like the error was fatal.  Too bad. */
  return (-1);
}
#endif /* CIRCLE_WINDOWS */

/* write_to_descriptor takes a descriptor, and text to write to the descriptor.
 * It keeps calling the system-level write() until all the text has been
 * delivered to the OS, or until an error is encountered. Returns:
 * >=0  If all is well and good.
 *  -1  If an error was encountered, so that the player should be cut off. */
int write_to_descriptor(socket_t desc, const char *txt)
{
  ssize_t bytes_written;
  size_t total = strlen(txt), write_total = 0;

  while (total > 0) {
    bytes_written = perform_socket_write(desc, txt, total);

    if (bytes_written < 0) {
      /* Fatal error.  Disconnect the player, preserving the originating descriptor. */
#ifndef CIRCLE_WINDOWS
      {
        int saved_errno = errno;
        log("SYSERR: socket write failed on descriptor %ld: %s (errno=%d)",
            (long)desc, strerror(saved_errno), saved_errno);
      }
#else
      log("SYSERR: socket write failed on descriptor %ld: Winsock error #%d",
          (long)desc, WSAGetLastError());
#endif
      return (-1);
    } else if (bytes_written == 0) {
      /* Temporary failure -- socket buffer full. */
      return (write_total);
    } else {
      txt += bytes_written;
      total -= bytes_written;
      write_total += bytes_written;
    }
  }

  return (write_total);
}

/* Same information about perform_socket_write applies here. I like
 * standards, there are so many of them. -gg 6/30/98 */
static ssize_t perform_socket_read(socket_t desc, char *read_point, size_t space_left)
{
  ssize_t ret;

  #if defined(CIRCLE_ACORN)
    ret = recv(desc, read_point, space_left, MSG_DONTWAIT);
  #elif defined(CIRCLE_WINDOWS)
    ret = recv(desc, read_point, space_left, 0);
  #else
    ret = read(desc, read_point, space_left);
  #endif

  /* Read was successful. */
  if (ret > 0)
    return (ret);

  /* read() returned 0, meaning we got an EOF. */
  if (ret == 0) {
    log("WARNING: EOF on socket read (connection broken by peer)");
    return (-1);
  }

  /* Read returned a value < 0: there was an error. */
#if defined(CIRCLE_WINDOWS)	/* Windows */
  if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR)
    return (0);
#else

#ifdef EINTR		/* Interrupted system call - various platforms */
  if (errno == EINTR)
    return (0);
#endif

#ifdef EAGAIN		/* POSIX */
  if (errno == EAGAIN)
    return (0);
#endif

#ifdef EWOULDBLOCK	/* BSD */
  if (errno == EWOULDBLOCK)
    return (0);
#endif /* EWOULDBLOCK */

#ifdef EDEADLK		/* Macintosh */
  if (errno == EDEADLK)
    return (0);
#endif

#ifdef ECONNRESET
  if (errno == ECONNRESET)
    return (-1);
#endif

#endif /* CIRCLE_WINDOWS */

  /* We don't know what happened, cut them off. This qualifies for
   * a SYSERR because we have no idea what happened at this point.*/
#ifndef CIRCLE_WINDOWS
  {
    int saved_errno = errno;
    log("SYSERR: socket read failed on descriptor %ld: %s (errno=%d); disconnecting",
        (long)desc, strerror(saved_errno), saved_errno);
  }
#else
  log("SYSERR: socket read failed on descriptor %ld: Winsock error #%d; disconnecting",
      (long)desc, WSAGetLastError());
#endif
  return (-1);
}

/**
 * Read from a descriptor's socket and split complete lines into commands.
 *
 * The bytes are untrusted network input. ProtocolInput() first strips
 * telnet/MXP negotiation and NUL bytes and appends the in-band text to
 * t->inbuf; it returns the number of bytes appended, which is how far
 * read_point advances. Each complete line (ended by CR or LF) is then
 * copied to a MAX_INPUT_LENGTH command buffer: non-printable bytes are
 * dropped, backspace/DEL erase, '$' is doubled (so act() and the
 * interpreter see it literally) and a line that does not fit is cut, the
 * player is told and network/input-truncate is logged. '!' and '^' use the
 * HISTORY_SIZE ring in t->history (history_pos is the next slot to fill);
 * "--" discards only queued input, never output. Commands beyond
 * TB_MAX_PENDING_COMMANDS are refused and reported (network/input-flood).
 *
 * @return -1 to close the connection (read error, EOF, input overflow),
 *         0 if no complete line has arrived yet, 1 otherwise.
 *
 * ASSUMPTION: There will be no newlines in the raw input buffer when this
 * function is called.  We must maintain that before returning.
 *
 * Ever wonder why 'tmp' had '+8' on it?  The crusty old code could write
 * MAX_INPUT_LENGTH+1 bytes to 'tmp' if there was a '$' as the final character
 * in the input buffer.  This would also cause 'space_left' to drop to -1,
 * which wasn't very happy in an unsigned variable.  Argh. So to fix the
 * above, 'tmp' lost the '+8' since it doesn't need it and the code has been
 * changed to reserve space by accepting one less character. (Do you really
 * need 256 characters on a line?) -gg 1/21/2000 */
static int process_input(struct descriptor_data *t)
{
  int buf_length, failed_subst;
  ssize_t bytes_read;
  size_t space_left;
  char *ptr, *read_point, *write_point, *nl_pos = NULL;
  char tmp[MAX_INPUT_LENGTH];
  static char read_buf[MAX_PROTOCOL_BUFFER] = { '\0' }; /* KaVir's plugin */
  
  /* first, find the point where we left off reading data */
  buf_length = strlen(t->inbuf);
  read_point = t->inbuf + buf_length;
  space_left = MAX_RAW_INPUT_LENGTH - buf_length - 1;

  do {
    if (space_left <= 0) {
      log("WARNING: process_input: about to close connection: input overflow");
      return (-1);
    }

    /* Read # of "bytes_read" from socket, and if we have something, mark the sizeof data
     * in the read_buf array as NULL */
    if ((bytes_read = perform_socket_read(t->descriptor, read_buf, space_left)) > 0)
      read_buf[bytes_read] = '\0';

    /* Since we have recieved atleast 1 byte of data from the socket, lets run it through
     * ProtocolInput() and rip out anything that is Out Of Band */ 
    if ( bytes_read > 0 )
      bytes_read = ProtocolInput(t, read_buf, bytes_read, t->inbuf, sizeof(t->inbuf));

    if (bytes_read < 0)	/* Error, disconnect them. */
      return (-1);
    else if (bytes_read == 0)	/* Just blocking, no problems. */
      return (0);

    /* at this point, we know we got some data from the read */
    *(read_point + bytes_read) = '\0';	/* terminate the string */

    /* search for a newline in the data we just read */
    for (ptr = read_point; *ptr && !nl_pos; ptr++)
      if (ISNEWL(*ptr))
	      nl_pos = ptr;

    read_point += bytes_read;
    space_left -= bytes_read;

/* on some systems such as AIX, POSIX-standard nonblocking I/O is broken,
 * causing the MUD to hang when it encounters input not terminated by a
 * newline.  This was causing hangs at the Password: prompt, for example.
 * I attempt to compensate by always returning after the _first_ read, instead
 * of looping forever until a read returns -1.  This simulates non-blocking
 * I/O because the result is we never call read unless we know from select()
 * that data is ready (process_input is only called if select indicates that
 * this descriptor is in the read set).  JE 2/23/95. */
#if !defined(POSIX_NONBLOCK_BROKEN)
  } while (nl_pos == NULL);
#else
  } while (0);

  if (nl_pos == NULL)
    return (0);
#endif /* POSIX_NONBLOCK_BROKEN */

  /* okay, at this point we have at least one newline in the string; now we
   * can copy the formatted data to a new array for further processing. */

  read_point = t->inbuf;

  while (nl_pos != NULL) {
    write_point = tmp;
    space_left = MAX_INPUT_LENGTH - 1;

    /* The '> 1' reserves room for a '$ => $$' expansion. */
    for (ptr = read_point; (space_left > 1) && (ptr < nl_pos); ptr++) {
      if (*ptr == '\b' || *ptr == 127) { /* handle backspacing or delete key */
	if (write_point > tmp) {
	  if (*(--write_point) == '$') {
	    write_point--;
	    space_left += 2;
	  } else
	    space_left++;
	}
      } else if (isascii(*ptr) && isprint(*ptr)) {
	if ((*(write_point++) = *ptr) == '$') {		/* copy one character */
	  *(write_point++) = '$';	/* if it's a $, double it */
	  space_left -= 2;
	} else
	  space_left--;
      }
    }

    *write_point = '\0';

    /* The copy loop stops early only when tmp is full. It exits with
     * space_left == 1 on ordinary text (0 only after a doubled '$'), so the
     * old 'space_left <= 0' test missed most cut lines. */
    if (ptr < nl_pos) {
      char buffer[MAX_INPUT_LENGTH + 64];

      TB_WARN_EVERY(60, "network", "input-truncate", "process_input",
                    DIAG_DESC_FMT " input line of %ld bytes cut to %lu",
                    DIAG_DESC(t), (long) (nl_pos - read_point),
                    (unsigned long) strlen(tmp));
      tb_snprintf(buffer, sizeof(buffer), "Line too long.  Truncated to:\r\n%s\r\n", tmp);
      if (write_to_descriptor(t->descriptor, buffer) < 0)
	return (-1);
    }
    if (t->snoop_by)
      write_to_output(t->snoop_by, "%% %s\r\n", tmp);
    failed_subst = 0;

    if (*tmp == '!' && !(*(tmp + 1)))	/* Redo last command. */
      strlcpy(tmp, t->last_input, sizeof(tmp));	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
    else if (*tmp == '!' && *(tmp + 1)) {
      char *commandln = (tmp + 1);
      int starting_pos = t->history_pos,
	  cnt = (t->history_pos == 0 ? HISTORY_SIZE - 1 : t->history_pos - 1);

      skip_spaces(&commandln);
      for (; cnt != starting_pos; cnt--) {
	if (t->history[cnt] && is_abbrev(commandln, t->history[cnt])) {
	  strlcpy(tmp, t->history[cnt], sizeof(tmp));	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
	  strlcpy(t->last_input, tmp, sizeof(t->last_input));	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
          write_to_output(t, "%s\r\n", tmp);
	  break;
	}
        if (cnt == 0)	/* At top, loop to bottom. */
	  cnt = HISTORY_SIZE;
      }
    } else if (*tmp == '^') {
      if (!(failed_subst = perform_subst(t, t->last_input, tmp)))
	strlcpy(t->last_input, tmp, sizeof(t->last_input));	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
    } else {
      strlcpy(t->last_input, tmp, sizeof(t->last_input));	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */
      if (t->history[t->history_pos])
	free(t->history[t->history_pos]);	/* Clear the old line. */
      t->history[t->history_pos] = strdup(tmp);	/* Save the new. */
      if (++t->history_pos >= HISTORY_SIZE)	/* Wrap to top. */
	t->history_pos = 0;
    }

   /* The '--' command flushes the queue. */
   if ( (*tmp == '-') && (*(tmp+1) == '-') && !(*(tmp+2)) )
   {
     write_to_output(t, "All queued commands cancelled.\r\n");
     /* Input only: flush_queues() would hand this descriptor's live output
      * buffer back to the shared pool while it is still in use. */
     flush_input_queue(t);
     failed_subst = 1;  /* Allow the read point to be moved, but don't add to queue */
   }

    if (!failed_subst && !write_to_q(tmp, &t->input, 0) && t->input.head) {
      /* Queue full: the peer is sending commands faster than they run.
       * Rate-limited so the flood cannot be amplified into the log. */
      TB_WARN_EVERY(60, "network", "input-flood", "process_input",
                    DIAG_DESC_FMT " %d commands pending; command dropped",
                    DIAG_DESC(t), TB_MAX_PENDING_COMMANDS);
      write_to_output(t, "Too many commands queued; command ignored.\r\n");
    }

    /* find the end of this line */
    while (ISNEWL(*nl_pos))
      nl_pos++;

    /* see if there's another newline in the input buffer */
    read_point = ptr = nl_pos;
    for (nl_pos = NULL; *ptr && !nl_pos; ptr++)
      if (ISNEWL(*ptr))
	nl_pos = ptr;
  }

  /* now move the rest of the buffer up to the beginning for the next pass */
  write_point = t->inbuf;
  while (*read_point)
    *(write_point++) = *(read_point++);
  *write_point = '\0';

  return (1);
}

/* Perform substitution for the '^..^' csh-esque syntax orig is the orig string,
 * i.e. the one being modified.  subst contains the substition string, i.e.
 * "^telm^tell" */
static int perform_subst(struct descriptor_data *t, char *orig, char *subst)
{
  char newsub[MAX_INPUT_LENGTH + 5];

  char *first, *second, *strpos;

  /* First is the position of the beginning of the first string (the one to be
   * replaced. */
  first = subst + 1;

  /* now find the second '^' */
  if (!(second = strchr(first, '^'))) {
    write_to_output(t, "Invalid substitution.\r\n");
    return (1);
  }
  /* terminate "first" at the position of the '^' and make 'second' point
   * to the beginning of the second string */
  *(second++) = '\0';

  /* now, see if the contents of the first string appear in the original */
  if (!(strpos = strstr(orig, first))) {
    write_to_output(t, "Invalid substitution.\r\n");
    return (1);
  }
  /* now, we construct the new string for output. */

  /* first, everything in the original, up to the string to be replaced */
  strncpy(newsub, orig, strpos - orig);	/* strncpy: OK (newsub:MAX_INPUT_LENGTH+5 > orig:MAX_INPUT_LENGTH) */
  newsub[strpos - orig] = '\0';

  /* now, the replacement string */
  strncat(newsub, second, MAX_INPUT_LENGTH - strlen(newsub) - 1);	/* strncpy: OK */

  /* now, if there's anything left in the original after the string to
   * replaced, copy that too. */
  if (((strpos - orig) + strlen(first)) < strlen(orig))
    strncat(newsub, strpos + strlen(first), MAX_INPUT_LENGTH - strlen(newsub) - 1);	/* strncpy: OK */

  /* terminate the string in case of an overflow from strncat */
  newsub[MAX_INPUT_LENGTH - 1] = '\0';
  strlcpy(subst, newsub, MAX_INPUT_LENGTH);	/* strcpy: OK (by mutual MAX_INPUT_LENGTH) */

  return (0);
}

/**
 * Close and free a descriptor. Teardown order: unlink d from
 * descriptor_list and close the socket; flush_queues() (the large output
 * buffer goes back to the pool, so d must not be written to after this);
 * end snooping in both directions; detach the character (players in game
 * are saved and stay linkless, others are freed); free the history and
 * pager; ProtocolDestroy(); cancel events; clean up OLC; free d. After this
 * returns d is freed: callers in game_loop() must already hold the next
 * pointer.
 */
void close_socket(struct descriptor_data *d)
{
  struct descriptor_data *temp;

  REMOVE_FROM_LIST(d, descriptor_list, next);
  CLOSE_SOCKET(d->descriptor);
  flush_queues(d);

  /* Forget snooping */
  if (d->snooping)
    d->snooping->snoop_by = NULL;

  if (d->snoop_by) {
    write_to_output(d->snoop_by, "Your victim is no longer among us.\r\n");
    d->snoop_by->snooping = NULL;
  }

  if (d->character) {
    /* If we're switched, this resets the mobile taken. */
    d->character->desc = NULL;

    /* Plug memory leak, from Eric Green. */
    if (!IS_NPC(d->character) && PLR_FLAGGED(d->character, PLR_MAILING) && d->str) {
      if (*(d->str))
        free(*(d->str));
      free(d->str);
      d->str = NULL;
    } else if (d->backstr && !IS_NPC(d->character) && !PLR_FLAGGED(d->character, PLR_WRITING)) {
      free(d->backstr);      /* editing description ... not olc */
      d->backstr = NULL;
    }

    add_llog_entry(d->character, LAST_DISCONNECT);

    if (IS_PLAYING(d) || STATE(d) == CON_DISCONNECT) {
      struct char_data *link_challenged = d->original ? d->original : d->character;

      /* We are guaranteed to have a person. */
      act("$n has lost $s link.", TRUE, link_challenged, 0, 0, TO_ROOM);
      save_char(link_challenged);
      mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(link_challenged)), TRUE, "Closing link to: %s.", GET_NAME(link_challenged));
    } else {
      mudlog(CMP, LVL_IMMORT, TRUE, "Losing player: %s.", GET_NAME(d->character) ? GET_NAME(d->character) : "<null>");
      free_char(d->character);
    }
  } else
    mudlog(CMP, LVL_IMMORT, TRUE, "Losing descriptor without char.");

  /* JE 2/22/95 -- part of my unending quest to make switch stable */
  if (d->original && d->original->desc)
    d->original->desc = NULL;

  /* Clear the command history. */
  if (d->history) {
    int cnt;
    for (cnt = 0; cnt < HISTORY_SIZE; cnt++)
      if (d->history[cnt])
	free(d->history[cnt]);
    free(d->history);
  }

  if (d->showstr_head)
    free(d->showstr_head);
  if (d->showstr_count)
    free(d->showstr_vector);
  
  /* KaVir's plugin*/
  ProtocolDestroy( d->pProtocol );
 
  /* Mud Events */
  if (d->events->iSize > 0) {
    struct event * pEvent;

    while ((pEvent = simple_list(d->events)) != NULL)
      event_cancel(pEvent);
  }

  free_list(d->events);

  /*. Kill any OLC stuff .*/
  switch (d->connected) {
    case CON_OEDIT:
    case CON_REDIT:
    case CON_ZEDIT:
    case CON_MEDIT:
    case CON_SEDIT:
    case CON_TEDIT:
    case CON_TRIGEDIT:
    case CON_AEDIT:
    case CON_HEDIT:
    case CON_QEDIT:
    case CON_MSGEDIT:
      cleanup_olc(d, CLEANUP_ALL);
      break;
    default:
      break;
  }

  free(d);
}

static void check_idle_passwords(void)
{
  struct descriptor_data *d, *next_d;

  for (d = descriptor_list; d; d = next_d) {
    next_d = d->next;
    if (STATE(d) != CON_PASSWORD && STATE(d) != CON_GET_NAME)
      continue;
    if (!d->idle_tics) {
      d->idle_tics++;
      continue;
    } else {
      echo_on(d);
      write_to_output(d, "\r\nTimed out... goodbye.\r\n");
      STATE(d) = CON_CLOSE;
    }
  }
}

/* I tried to universally convert Circle over to POSIX compliance, but
 * alas, some systems are still straggling behind and don't have all the
 * appropriate defines.  In particular, NeXT 2.x defines O_NDELAY but not
 * O_NONBLOCK.  Krusty old NeXT machines!  (Thanks to Michael Jones for
 * this and various other NeXT fixes.) */

#if defined(CIRCLE_WINDOWS)

void nonblock(socket_t s)
{
  unsigned long val = 1;
  ioctlsocket(s, FIONBIO, &val);
}

#elif defined(CIRCLE_AMIGA)

void nonblock(socket_t s)
{
  long val = 1;
  IoctlSocket(s, FIONBIO, &val);
}

#elif defined(CIRCLE_ACORN)

void nonblock(socket_t s)
{
  int val = 1;
  socket_ioctl(s, FIONBIO, &val);
}

#elif defined(CIRCLE_VMS)

void nonblock(socket_t s)
{
  int val = 1;

  if (ioctl(s, FIONBIO, &val) < 0) {
    TB_REPORT_ERRNO("ioctl", "FIONBIO nonblocking socket");
    exit(1);
  }
}

#elif defined(CIRCLE_UNIX) || defined(CIRCLE_OS2) || defined(CIRCLE_MACINTOSH)

#ifndef O_NONBLOCK
#define O_NONBLOCK O_NDELAY
#endif

static void nonblock(socket_t s)
{
  int flags;

  flags = fcntl(s, F_GETFL, 0);
  if (flags < 0) {
    TB_REPORT_ERRNO("fcntl", "F_GETFL for nonblocking socket");
    exit(1);
  }
  flags |= O_NONBLOCK;
  if (fcntl(s, F_SETFL, flags) < 0) {
    TB_REPORT_ERRNO("fcntl", "F_SETFL O_NONBLOCK");
    exit(1);
  }
}
#endif  /* CIRCLE_UNIX || CIRCLE_OS2 || CIRCLE_MACINTOSH */


/*  signal-handling functions (formerly signals.c).  UNIX only. */
#if defined(CIRCLE_UNIX) || defined(CIRCLE_MACINTOSH)
static RETSIGTYPE reread_wizlists(int sig)
{
  reread_wizlist = TRUE;
}

static RETSIGTYPE unrestrict_game(int sig)
{
  emergency_unban = TRUE;
}

#ifdef CIRCLE_UNIX

/* clean up our zombie kids to avoid defunct processes. waitpid() can
 * change errno, and this handler can interrupt code that is about to read
 * errno (perform_socket_read() checking for EAGAIN), so errno is saved and
 * restored. */
static RETSIGTYPE reap(int sig)
{
  int saved_errno = errno;

  while (waitpid(-1, NULL, WNOHANG) > 0);

  my_signal(SIGCHLD, reap);
  errno = saved_errno;
}

/* Deadlock protection (SIGVTALRM every 180 s of CPU time). The log()
 * before abort() is not async-signal-safe; it is kept because the process
 * is dying anyway and this is the only record of why. errno is preserved
 * for the code that was interrupted. */
static RETSIGTYPE checkpointing(int sig)
{
  int saved_errno = errno;
#ifndef MEMORY_DEBUG
  if (!tics_passed) {
    log("SYSERR: CHECKPOINT shutdown: tics not updated. (Infinite loop suspected)");
    abort();
  } else
    tics_passed = 0;
#endif
  errno = saved_errno;
}

/* SIGHUP/SIGINT/SIGTERM: only record the signal. log() and exit() are not
 * async-signal-safe (the signal may arrive inside malloc or stdio);
 * game_loop() logs it and shuts down cleanly, saving players. */
static RETSIGTYPE hupsig(int sig)
{
  shutdown_signal = sig;
}

#endif	/* CIRCLE_UNIX */

/* Signal handling rules for this file: handlers may only store to
 * volatile sig_atomic_t flags (or call async-signal-safe functions such as
 * waitpid and sigaction) and must save and restore errno. game_loop()
 * acts on the flags. Handlers are installed without SA_RESTART, so every
 * blocking system call in the loop must treat EINTR as "try again".
 *
 * This is an implementation of signal() using sigaction() for portability.
 * (sigaction() is POSIX; signal() is not.)  Taken from Stevens' _Advanced
 * Programming in the UNIX Environment_.  We are specifying that all system
 * calls _not_ be automatically restarted for uniformity, because BSD systems
 * do not restart select(), even if SA_RESTART is used.
 * Note that NeXT 2.x is not POSIX and does not have sigaction; therefore,
 * I just define it to be the old signal.  If your system doesn't have
 * sigaction either, you can use the same fix.
 * SunOS Release 4.0.2 (sun386) needs this too, according to Tim Aldric. */

#ifndef POSIX
#define my_signal(signo, func) signal(signo, func)
#else
static sigfunc *my_signal(int signo, sigfunc *func)
{
  struct sigaction sact, oact;

  sact.sa_handler = func;
  sigemptyset(&sact.sa_mask);
  sact.sa_flags = 0;
#ifdef SA_INTERRUPT
  sact.sa_flags |= SA_INTERRUPT;	/* SunOS */
#endif

  if (sigaction(signo, &sact, &oact) < 0)
    return (SIG_ERR);

  return (oact.sa_handler);
}
#endif				/* POSIX */

static void signal_setup(void)
{
#ifndef CIRCLE_MACINTOSH
  struct itimerval itime;
  struct timeval interval;

  /* user signal 1: reread wizlists.  Used by autowiz system. */
  my_signal(SIGUSR1, reread_wizlists);

  /* user signal 2: unrestrict game.  Used for emergencies if you lock
   * yourself out of the MUD somehow. */
  my_signal(SIGUSR2, unrestrict_game);

  /* set up the deadlock-protection so that the MUD aborts itself if it gets
   * caught in an infinite loop for more than 3 minutes. */
  interval.tv_sec = 180;
  interval.tv_usec = 0;
  itime.it_interval = interval;
  itime.it_value = interval;
  setitimer(ITIMER_VIRTUAL, &itime, NULL);
  my_signal(SIGVTALRM, checkpointing);

  /* just to be on the safe side: */
  my_signal(SIGHUP, hupsig);
  my_signal(SIGCHLD, reap);
#endif /* CIRCLE_MACINTOSH */
  my_signal(SIGINT, hupsig);
  my_signal(SIGTERM, hupsig);
  my_signal(SIGPIPE, SIG_IGN);
  my_signal(SIGALRM, SIG_IGN);
}

#endif	/* CIRCLE_UNIX || CIRCLE_MACINTOSH */
/* Public routines for system-to-player-communication. */
void game_info(const char *format, ...)
{
  struct descriptor_data *i;
  va_list args;
  char messg[MAX_STRING_LENGTH];
  if (format == NULL)
    return;
  tb_snprintf(messg, sizeof(messg), "\tcInfo: \ty");
  for (i = descriptor_list; i; i = i->next) {
    if (STATE(i) != CON_PLAYING)
      continue;
    if (!(i->character))
      continue;

    write_to_output(i, "%s", messg);
    va_start(args, format);
    vwrite_to_output(i, format, args);
    va_end(args);
    write_to_output(i, "\tn\r\n");
  }
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  if (ch->desc && messg && *messg) {
    size_t left;
    va_list args;

    va_start(args, messg);
    left = vwrite_to_output(ch->desc, messg, args);
    va_end(args);
    return left;
  }
  return 0;
}

void send_to_all(const char *messg, ...)
{
  struct descriptor_data *i;
  va_list args;

  if (messg == NULL)
    return;

  for (i = descriptor_list; i; i = i->next) {
    if (STATE(i) != CON_PLAYING)
      continue;

    va_start(args, messg);
    vwrite_to_output(i, messg, args);
    va_end(args);
  }
}

void send_to_outdoor(const char *messg, ...)
{
  struct descriptor_data *i;
  va_list args;

  if (!messg || !*messg)
    return;

  for (i = descriptor_list; i; i = i->next) {

    if (STATE(i) != CON_PLAYING || i->character == NULL)
      continue;
    if (!AWAKE(i->character) || !OUTSIDE(i->character))
      continue;

    va_start(args, messg);
    vwrite_to_output(i, messg, args);
    va_end(args);
  }
}

/** Send a message to every connected occupant of a room. 'room' must be a
 * valid rnum (0..top_of_world); 'messg' is a printf format. */
void send_to_room(room_rnum room, const char *messg, ...)
{
  struct char_data *i;
  va_list args;

  if (messg == NULL)
    return;

  for (i = world[room].people; i; i = i->next_in_room) {
    if (!i->desc)
      continue;

    va_start(args, messg);
    vwrite_to_output(i->desc, messg, args);
    va_end(args);
  }
}

/* Sends a message to the entire group, except for ch.
 * Send 'ch' as NULL, if you want to message to reach
 * everyone. -Vatiken
 * 'group' must be non-NULL. Iterates group->members with the shared
 * simple_list() iterator state, so nothing called from here may start
 * another simple_list() walk. */
void send_to_group(struct char_data *ch, struct group_data *group, const char * msg, ...)
{
	struct char_data *tch;
  va_list args;

  if (msg == NULL)
    return;
    	
  while ((tch = simple_list(group->members)) != NULL) {
    if (tch != ch && !IS_NPC(tch) && tch->desc && STATE(tch->desc) == CON_PLAYING) {
      write_to_output(tch->desc, "%s[%sGroup%s]%s ", 
      CCGRN(tch, C_NRM), CBGRN(tch, C_NRM), CCGRN(tch, C_NRM), CCNRM(tch, C_NRM));
      va_start(args, msg);
      vwrite_to_output(tch->desc, msg, args);
      va_end(args);
    }
  }
}


/* Thx to Jamie Nelson of 4D for this contribution */
/** Send a message to everyone connected in rooms whose vnums lie in
 * [start, finish] (both ends included). A reversed range or NULL message is
 * reported as comm/send-range and nothing is sent. */
void send_to_range(room_vnum start, room_vnum finish, const char *messg, ...)
{
  struct char_data *i;
  va_list args;
  int j;

  /* start and finish are room vnums, both included. DG scripts pass
   * script-supplied numbers (zoneecho and friends). */
  if (start > finish) {
    TB_ERROR_MSG("comm", "send-range", "send_to_range",
                 "start vnum %d > finish vnum %d; message not sent",
                 (int) start, (int) finish);
    return;
  }
  if (messg == NULL) {
    TB_ERROR_MSG("comm", "send-range", "send_to_range",
                 "NULL message for vnums %d-%d", (int) start, (int) finish);
    return;
  }

  /* top_of_world is the highest valid rnum, so it is included. */
  for (j = 0; j <= top_of_world; j++) {
    if (GET_ROOM_VNUM(j) >= start && GET_ROOM_VNUM(j) <= finish) {
      for (i = world[j].people; i; i = i->next_in_room) {
        if (!i->desc)
          continue;

        va_start(args, messg);
        vwrite_to_output(i->desc, messg, args);
        va_end(args);
      }
    }
  }
}

static const char *ACTNULL = "<NULL>";
#define CHECK_NULL(pointer, expression) \
  if ((pointer) == NULL) i = ACTNULL; else i = (expression);
/* Bounded, escaped copy of an act() format for diagnostics. The format is
 * usually a string literal, so it identifies the act() call site; it may also
 * embed player text (emote), so control characters, quotes and backslashes
 * are replaced to keep each log record one parseable line. */
static const char *act_format_excerpt(char *dst, size_t dst_size,
                                      const char *format)
{
  size_t n = 0;

  for (; format && *format && n + 4 < dst_size; format++) {
    unsigned char c = (unsigned char) *format;
    dst[n++] = (c < 0x20 || c == 0x7f || c == '"' || c == '\\') ? '?' : (char) c;
  }
  if (format && *format && n + 3 < dst_size)
    dst[n++] = '.', dst[n++] = '.', dst[n++] = '.';
  dst[n] = '\0';
  return dst;
}

/* "caller=file:line function" of the act() call, for every act diagnostic. */
#define ACT_DIAG_SITE \
  (act_site_file ? act_site_file : "(direct perform_act call)"), \
  act_site_line, (act_site_func ? act_site_func : "-")

/**
 * Format one act() message for one recipient and deliver it: written to
 * to->desc, and passed to to's act triggers (NPCs, unless DG_NO_TRIG).
 * $-codes are expanded with bounds; malformed or oversized formats are
 * reported with the act() caller's site (ACT_DIAG_SITE).
 *
 * Act triggers run synchronously inside this call and may move or mark
 * for extraction any character, including ch and to. The formatted text is
 * kept in last_act_message, which act() returns; that pointer is valid only
 * until the next act()/perform_act() call.
 */
void perform_act(const char *orig, struct char_data *ch, struct obj_data *obj,
    void *vict_obj, struct char_data *to)
{
  const char *const format = orig;
  char excerpt[96];
  const char *i = NULL;
  char lbuf[MAX_STRING_LENGTH], *buf, *j;
  /* Last position the terminating NUL may occupy; it is later overwritten
   * by "\r\n\0", which needs two more bytes. */
  char *const lbuf_end = lbuf + sizeof(lbuf) - 3;
  bool uppercasenext = FALSE, truncated = FALSE;
  struct char_data *dg_victim = (to == vict_obj) ? vict_obj : NULL;
  struct obj_data *dg_target = NULL;
  char *dg_arg = NULL;

  buf = lbuf;

  for (;;) {
    if (buf >= lbuf_end) {
      /* Substitutions such as $T carry script- and player-supplied text. */
      *(buf++) = '\0';
      truncated = TRUE;
      break;
    } else if (*orig == '$') {
      switch (*(++orig)) {
      case 'n':
	i = PERS(ch, to);
	break;
      case 'N':
	CHECK_NULL(vict_obj, PERS((const struct char_data *) vict_obj, to));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 'm':
	i = HMHR(ch);
	break;
      case 'M':
	CHECK_NULL(vict_obj, HMHR((const struct char_data *) vict_obj));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 's':
	i = HSHR(ch);
	break;
      case 'S':
	CHECK_NULL(vict_obj, HSHR((const struct char_data *) vict_obj));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 'e':
	i = HSSH(ch);
	break;
      case 'E':
	CHECK_NULL(vict_obj, HSSH((const struct char_data *) vict_obj));
	dg_victim = (struct char_data *) vict_obj;
	break;
      case 'o':
	CHECK_NULL(obj, OBJN(obj, to));
	break;
      case 'O':
	CHECK_NULL(vict_obj, OBJN((const struct obj_data *) vict_obj, to));
	dg_target = (struct obj_data *) vict_obj;
	break;
      case 'p':
	CHECK_NULL(obj, OBJS(obj, to));
	break;
      case 'P':
	CHECK_NULL(vict_obj, OBJS((const struct obj_data *) vict_obj, to));
	dg_target = (struct obj_data *) vict_obj;
	break;
      case 'a':
	CHECK_NULL(obj, SANA(obj));
	break;
      case 'A':
	CHECK_NULL(vict_obj, SANA((const struct obj_data *) vict_obj));
	dg_target = (struct obj_data *) vict_obj;
	break;
       case 'T':
 	CHECK_NULL(vict_obj, (const char *) vict_obj);
 	dg_arg = (char *) vict_obj;
	break;
      case 't':
 	CHECK_NULL(obj, (char *) obj);
	break;
      case 'F':
	CHECK_NULL(vict_obj, fname((const char *) vict_obj));
	break;
      /* uppercase previous word */
      case 'u':
        for (j=buf; j > lbuf && !isspace((int) *(j-1)); j--);
        if (j != buf)
          *j = UPPER(*j);
        i = "";
        break;
      /* uppercase next word */
      case 'U':
        uppercasenext = TRUE;
        i = "";
        break;
      case '$':
	i = "$";
	break;
      case '\0':
	TB_ERROR_MSG("act", "format-parse", "perform_act",
	             "format ends with a bare '$' caller=%s:%d %s (recipient " DIAG_CHAR_FMT ") "
	             "format=\"%s\"", ACT_DIAG_SITE, DIAG_CHAR(to),
	             act_format_excerpt(excerpt, sizeof(excerpt), format));
	i = "";
	break;
      default:
	TB_ERROR_MSG("act", "format-parse", "perform_act",
	             "illegal $-code 0x%02x at offset %ld caller=%s:%d %s (recipient "
	             DIAG_CHAR_FMT ") format=\"%s\"", (unsigned char) *orig,
	             (long) (orig - format), ACT_DIAG_SITE, DIAG_CHAR(to),
	             act_format_excerpt(excerpt, sizeof(excerpt), format));
	i = "";
	break;
      }
      while (buf < lbuf_end && (*buf = *(i++)))
        {
        if (uppercasenext && !isspace((int) *buf))
          {
          *buf = UPPER(*buf);
          uppercasenext = FALSE;
          }
	buf++;
        }
      /* A trailing '$' leaves orig on the terminator: do not step past it. */
      if (*orig)
        orig++;
    } else if (!(*(buf++) = *(orig++))) {
      break;
    } else if (uppercasenext && !isspace((int) *(buf-1))) {
      *(buf-1) = UPPER(*(buf-1));
      uppercasenext = FALSE;
    }
  }

  *(--buf) = '\r';
  *(++buf) = '\n';
  *(++buf) = '\0';

  if (truncated)
    TB_ERROR_MSG("act", "format-output", "perform_act",
                 "expanded output exceeded %d bytes and was truncated "
                 "caller=%s:%d %s (recipient " DIAG_CHAR_FMT ") format=\"%s\"",
                 (int) sizeof(lbuf), ACT_DIAG_SITE, DIAG_CHAR(to),
                 act_format_excerpt(excerpt, sizeof(excerpt), format));

  if (to->desc)
    write_to_output(to->desc, "%s", CAP(lbuf));

  if ((IS_NPC(to) && dg_act_check) && (to != ch))
    act_mtrigger(to, lbuf, ch, dg_victim, obj, dg_target, dg_arg);

  if (last_act_message)
    free(last_act_message);
  if ((last_act_message = strdup(lbuf)) == NULL)
    TB_ERROR_ERRNO("act", "allocate", "last_act_message",
                   "cannot copy %zu-byte act() result; act() will return NULL "
                   "caller=%s:%d %s", strlen(lbuf) + 1, ACT_DIAG_SITE);
}

static char *act_body(const char *str, int hide_invisible, struct char_data *ch,
	 struct obj_data *obj, void *vict_obj, int type);

/* TO_* preconditions: TO_CHAR needs ch; TO_VICT needs vict_obj to be a
 * char_data; TO_ROOM/TO_NOTVICT need ch or obj placed in a room;
 * TO_GMOTE needs a player ch. TO_SLEEP and DG_NO_TRIG may be OR-ed in.
 * dg_act_check is file-global state for the act() being delivered; see
 * act_at() for how nesting is handled. */

/* Target of the act() macro in comm.h: record the caller for diagnostics,
 * deliver the message, then restore the enclosing act()'s state. An act
 * trigger can send further act() messages (do_say from a script uses
 * DG_NO_TRIG) while an outer TO_ROOM act() is still delivering; the nested
 * call sets dg_act_check for itself, so it is restored here too. Otherwise
 * the outer act's remaining recipients silently lost (or gained) their act
 * triggers. */
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  const char *saved_file = act_site_file, *saved_func = act_site_func;
  int saved_line = act_site_line;
  int saved_act_check = dg_act_check;
  char *result;

  act_site_file = src_file;
  act_site_line = src_line;
  act_site_func = src_func;
  result = act_body(str, hide_invisible, ch, obj, vict_obj, type);
  act_site_file = saved_file;
  act_site_line = saved_line;
  act_site_func = saved_func;
  dg_act_check = saved_act_check;
  return result;
}

/* One snapshotted TO_ROOM/TO_NOTVICT recipient (see act_body()). */
struct act_target {
  struct char_data *ch;
  long uid;
};

/* Still the same character, still registered, still in the act's room.
 * A character pending extraction keeps its UID and room until the end of
 * the pulse, so it still counts as present (as before the snapshot). */
static bool act_target_live(struct char_data *ch, long uid, room_rnum room)
{
  return find_char(uid) == ch && IN_ROOM(ch) == room;
}

static char *act_body(const char *str, int hide_invisible, struct char_data *ch,
	 struct obj_data *obj, void *vict_obj, int type)
{
  struct char_data *to;
  int to_sleeping;
  room_rnum room;
  struct act_target local_targets[32], *targets;
  size_t count = 0, n = 0, k;

  if (!str || !*str)
    return NULL;

  /* Warning: the following TO_SLEEP code is a hack. I wanted to be able to tell
   * act to deliver a message regardless of sleep without adding an additional
   * argument.  TO_SLEEP is 128 (a single bit high up).  It's ONLY legal to
   * combine TO_SLEEP with one other TO_x command.  It's not legal to combine
   * TO_x's with each other otherwise. TO_SLEEP only works because its value
   * "happens to be" a single bit; do not change it to something else.  In
   * short, it is a hack. */

  /* check if TO_SLEEP is there, and remove it if it is. */
  if ((to_sleeping = (type & TO_SLEEP)))
    type &= ~TO_SLEEP;

  /* this is a hack as well - DG_NO_TRIG is 256 -- Welcor */
  /* If the bit is set, unset dg_act_check, thus the ! below */
  if (!(dg_act_check = !IS_SET(type, DG_NO_TRIG)))
    REMOVE_BIT(type, DG_NO_TRIG);

  if (type == TO_CHAR) {
    if (ch && SENDOK(ch)) {
      perform_act(str, ch, obj, vict_obj, ch);
      return last_act_message;
    }
    return NULL;
  }

  if (type == TO_VICT) {
    if ((to = vict_obj) != NULL && SENDOK(to)) {
      perform_act(str, ch, obj, vict_obj, to);
      return last_act_message;
    }
    return NULL;
  }

  if (type == TO_GMOTE && !IS_NPC(ch)) {
    struct descriptor_data *i;
    char buf[MAX_STRING_LENGTH];

    for (i = descriptor_list; i; i = i->next) {
      if (!i->connected && i->character &&
          !PRF_FLAGGED(i->character, PRF_NOGOSS) &&
          !PLR_FLAGGED(i->character, PLR_WRITING) &&
          !ROOM_FLAGGED(IN_ROOM(i->character), ROOM_SOUNDPROOF)) {

        tb_snprintf(buf, sizeof(buf), "%s%s%s", CCYEL(i->character, C_NRM), str, CCNRM(i->character, C_NRM));
        perform_act(buf, ch, obj, vict_obj, i->character);
      }
    }
    return last_act_message;
  }
  /* ASSUMPTION: at this point we know type must be TO_NOTVICT or TO_ROOM */

  if (ch && IN_ROOM(ch) != NOWHERE)
    room = IN_ROOM(ch);
  else if (obj && IN_ROOM(obj) != NOWHERE)
    room = IN_ROOM(obj);
  else {
    /* TO_ROOM/TO_NOTVICT need an actor or object placed in a room. */
    TB_ERROR_MSG("act", "no-target", "act_body",
                 "caller=%s:%d %s: TO_ROOM/TO_NOTVICT act() with no placed"
                 " actor or object (actor %s %ld, obj vnum %ld); message"
                 " dropped", ACT_DIAG_SITE,
                 ch ? (IS_NPC(ch) ? "mob vnum" : "player idnum") : "none",
                 ch ? (IS_NPC(ch) ? (long) GET_MOB_VNUM(ch) : (long) GET_IDNUM(ch)) : -1L,
                 obj ? (long) GET_OBJ_VNUM(obj) : (long) NOTHING);
    return NULL;
  }

  /* perform_act() runs act triggers synchronously, and a trigger can move
   * characters (mteleport, mforce ... ) in or out of this room: char_to_room
   * prepends to the room list, so following to->next_in_room after a
   * delivery could skip, repeat or loop forever over recipients. Snapshot
   * the occupants as (pointer, UID) pairs first and re-validate each one
   * before delivery. Extraction is deferred to the end of the pulse, so the
   * pointers stay allocated for the whole call. */
  for (to = world[room].people; to; to = to->next_in_room)
    count++;
  if (count > sizeof(local_targets) / sizeof(local_targets[0]))
    CREATE(targets, struct act_target, count);
  else
    targets = local_targets;
  for (to = world[room].people; to && n < count; to = to->next_in_room) {
    targets[n].ch = to;
    targets[n].uid = char_script_id(to);
    n++;
  }

  for (k = 0; k < n; k++) {
    to = targets[k].ch;
    if (!act_target_live(to, targets[k].uid, room)) {
      TB_WARN_EVERY(60, "act", "recipient-moved", "act_body",
                    "caller=%s:%d %s recipient " DIAG_CHAR_FMT " left room"
                    " vnum %d or was extracted during delivery; skipped",
                    ACT_DIAG_SITE, DIAG_CHAR(to), GET_ROOM_VNUM(room));
      continue;
    }
    if (!SENDOK(to) || (to == ch))
      continue;
    if (hide_invisible && ch && !CAN_SEE(to, ch))
      continue;
    if (type != TO_ROOM && to == vict_obj)
      continue;
    perform_act(str, ch, obj, vict_obj, to);
  }
  if (targets != local_targets)
    free(targets);
  return last_act_message;
}

/* Prefer the file over the descriptor. */
static void setup_log(const char *filename, int fd)
{
  FILE *s_fp;

#if defined(__MWERKS__) || defined(__GNUC__)
  s_fp = stderr;
#else
  if ((s_fp = fdopen(STDERR_FILENO, "w")) == NULL) {
    puts("SYSERR: Error opening stderr, trying stdout.");

    if ((s_fp = fdopen(STDOUT_FILENO, "w")) == NULL) {
      puts("SYSERR: Error opening stdout, trying a file.");

      /* If we don't have a file, try a default. */
      if (filename == NULL || *filename == '\0')
        filename = "log/syslog";
    }
  }
#endif

  if (filename == NULL || *filename == '\0') {
    /* No filename, set us up with the descriptor we just opened. */
    logfile = s_fp;
    puts("Using file descriptor for logging.");
    return;
  }

  /* We honor the default filename first. */
  if (open_logfile(filename, s_fp))
    return;

  /* Well, that failed but we want it logged to a file so try a default. */
  if (open_logfile("log/syslog", s_fp))
    return;

  /* Ok, one last shot at a file. */
  if (open_logfile("syslog", s_fp))
    return;

  /* Erp, that didn't work either, just die. */
  TB_REPORT_FATAL(TB_ERR_BOOT, "setup-log",
                  "could not open any logging destination; giving up");
  exit(1);
}

/* Open 'filename' as the log. With stderr_fp, the file is placed on that
 * stream's descriptor so stray stderr output lands in the log too.
 *
 * This used freopen(filename, "w", stderr_fp), but freopen closes the
 * stream even when opening the new file fails (C11 7.21.5.4), and
 * setup_log() then retried the next fallback name on the closed FILE *
 * (undefined behaviour). The file is now opened first and moved onto the
 * stream's descriptor with dup2() only once that has succeeded, so a failed
 * attempt leaves stderr untouched and still usable for the report. */
static int open_logfile(const char *filename, FILE *stderr_fp)
{
  FILE *fp = fopen(filename, "w");

  if (fp == NULL) {
    TB_REPORT_DOMAIN_ERRNO(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                           "fopen", filename);
    return (FALSE);
  }

  if (stderr_fp) {
#if defined(CIRCLE_WINDOWS)
    fclose(fp);
    if ((logfile = freopen(filename, "w", stderr_fp)) == NULL) {
      TB_REPORT_DOMAIN_ERRNO(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                             "freopen", filename);
      return (FALSE);
    }
#else
    (void)fflush(stderr_fp);  /* best effort: pending stderr text */
    if (dup2(fileno(fp), fileno(stderr_fp)) < 0) {
      TB_REPORT_DOMAIN_ERRNO(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                             "dup2", filename);
      (void)fclose(fp);  /* read-only use so far; nothing to lose */
      return (FALSE);
    }
    /* fp only supplied the descriptor; stderr_fp now owns the file. */
    (void)fclose(fp);
    logfile = stderr_fp;
#endif
  } else
    logfile = fp;

  printf("Using log file '%s'%s.\n",
         filename, stderr_fp ? " with redirection" : "");
  return (TRUE);
}

/* This may not be pretty but it keeps game_loop() neater than if it was inline. */
#if defined(CIRCLE_WINDOWS)
void circle_sleep(struct timeval *timeout)
{
  Sleep(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
}

#else
static void circle_sleep(struct timeval *timeout)
{
  if (select(0, (fd_set *) 0, (fd_set *) 0, (fd_set *) 0, timeout) < 0) {
    if (errno != EINTR) {
      TB_REPORT_ERRNO("select", "game-loop sleep");
      exit(1);
    }
  }
}

#endif /* CIRCLE_WINDOWS */

/* Opponent health as a percentage for MSDP. max_hit comes from the mob
 * prototype's hit dice (builder/OLC data): "0d0+0" yields max_hit 0, which
 * would be an integer division by zero (SIGFPE, taking down the server)
 * once per second for every player fighting that mob. Such an opponent is
 * reported and shown as 0%. The product is computed in long so hit * 100
 * cannot overflow. */
static int msdp_opponent_health_percent(struct descriptor_data *d,
                                        struct char_data *opponent)
{
  if (GET_MAX_HIT(opponent) <= 0) {
    TB_ERROR_EVERY(60, "msdp", "opponent-health", "msdp_update",
                   DIAG_DESC_FMT " opponent " DIAG_CHAR_FMT " has max_hit %d;"
                   " health percentage needs max_hit > 0, sent 0",
                   DIAG_DESC(d), DIAG_CHAR(opponent), GET_MAX_HIT(opponent));
    return 0;
  }
  return (int) (((long) GET_HIT(opponent) * 100L) / (long) GET_MAX_HIT(opponent));
}

/* KaVir's plugin*/
static void msdp_update( void )
{
  struct descriptor_data *d;
  int PlayerCount = 0;
  char buf[MAX_STRING_LENGTH];
  extern const char *pc_class_types[];

  for (d = descriptor_list; d; d = d->next)
  {
    struct char_data *ch = d->character;
    if ( ch && !IS_NPC(ch) && d->connected == CON_PLAYING )
    {
      struct char_data *pOpponent = FIGHTING(ch);
      ++PlayerCount;

      MSDPSetString( d, eMSDP_CHARACTER_NAME, GET_NAME(ch) );
      MSDPSetNumber( d, eMSDP_ALIGNMENT, GET_ALIGNMENT(ch) );
      MSDPSetNumber( d, eMSDP_EXPERIENCE, GET_EXP(ch) );

      MSDPSetNumber( d, eMSDP_HEALTH, GET_HIT(ch) );
      MSDPSetNumber( d, eMSDP_HEALTH_MAX, GET_MAX_HIT(ch) );
      MSDPSetNumber( d, eMSDP_LEVEL, GET_LEVEL(ch) );

      sprinttype( ch->player.chclass, pc_class_types, buf, sizeof(buf) );
      MSDPSetString( d, eMSDP_CLASS, buf );

      MSDPSetNumber( d, eMSDP_MANA, GET_MANA(ch) );
      MSDPSetNumber( d, eMSDP_MANA_MAX, GET_MAX_MANA(ch) );
      MSDPSetNumber( d, eMSDP_WIMPY, GET_WIMP_LEV(ch) );
      MSDPSetNumber( d, eMSDP_MONEY, GET_GOLD(ch) );
      MSDPSetNumber( d, eMSDP_MOVEMENT, GET_MOVE(ch) );
      MSDPSetNumber( d, eMSDP_MOVEMENT_MAX, GET_MAX_MOVE(ch) );
      MSDPSetNumber( d, eMSDP_AC, compute_armor_class(ch) );

      /* This would be better moved elsewhere */
      if ( pOpponent != NULL )
      {
          MSDPSetNumber( d, eMSDP_OPPONENT_HEALTH,
                         msdp_opponent_health_percent(d, pOpponent) );
          MSDPSetNumber( d, eMSDP_OPPONENT_HEALTH_MAX, 100 );
          MSDPSetNumber( d, eMSDP_OPPONENT_LEVEL, GET_LEVEL(pOpponent) );
          MSDPSetString( d, eMSDP_OPPONENT_NAME, PERS(pOpponent, ch) );
      }
      else /* Clear the values */
      {
          MSDPSetNumber( d, eMSDP_OPPONENT_HEALTH, 0 );
          MSDPSetNumber( d, eMSDP_OPPONENT_LEVEL, 0 ); 
          MSDPSetString( d, eMSDP_OPPONENT_NAME, "" ); 
      }

      MSDPUpdate( d );
    }

    /* Ideally this should be called once at startup, and again whenever
     * someone leaves or joins the mud.  But this works, and it keeps the
     * snippet simple.  Optimise as you see fit.
     */
    MSSPSetPlayers( PlayerCount );
  }
}
