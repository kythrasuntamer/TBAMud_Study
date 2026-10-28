/**************************************************************************
*  File: db.c                                              Part of tbaMUD *
*  Usage: Loading/saving chars, booting/resetting world, internal funcs.  *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/* Purpose: boot the world (index_boot() and the per-record parsers), run zone
 * resets, create mobile/object instances from prototypes, load text files,
 * help, socials and the game configuration, and free it all at shutdown
 * (destroy_db()).
 *
 * Ownership and lifetime:
 *  - world[], mob_proto[]/mob_index[], obj_proto[]/obj_index[], zone_table[],
 *    help_table[] and soc_mess_list[] are allocated once at boot, sized by a
 *    counting pass, and freed by destroy_db(). The per-record parsers keep
 *    static cursors and may run only during that one sequential boot pass.
 *  - Instances from read_mobile()/read_object() are linked into
 *    character_list/object_list and share string pointers with their
 *    prototype; free_char()/free_obj() compare against the prototype before
 *    freeing a string.
 *  - fread_line(), fread_word() and parse_object() return static buffers that
 *    the next call overwrites.
 *
 * Who controls the input: everything under lib/ (world, zone, help, text,
 * socials, config). OLC (redit/medit/oedit/zedit/hedit/cedit) writes
 * in-range values, but implementors edit these files by hand, so every
 * parser range-checks values that index a table and reports problems with
 * TB_ERROR_MSG/TB_WARN_MSG/TB_FATAL_MSG naming the file, line and vnum.
 * Boot-time format errors are fatal (exit(1)); runtime reloads that reuse
 * the same parsers (index_boot(DB_BOOT_HLP) from "reload xhelp") inherit
 * that, so a malformed help file ends the live server. */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "comm.h"
#include "handler.h"
#include "spells.h"
#include "mail.h"
#include "interpreter.h"
#include "house.h"
#include "constants.h"
#include "oasis.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include "act.h"
#include "ban.h"
#include "spec_procs.h"
#include "genzon.h"
#include "genolc.h"
#include "genobj.h" /* for free_object_strings */
#include "config.h" /* for the default config values. */
#include "fight.h"
#include "modify.h"
#include "shop.h"
#include "quest.h"
#include "ibt.h"
#include "mud_event.h"
#include "msgedit.h"
#include "prefedit.h"
#include "boards.h"
#include "screen.h"
#include <sys/stat.h>

/** fgets() window used by fread_string() and fread_clean_string(). Their
 * chunk buffer is FREAD_CHUNK + 3 bytes: a final chunk with no newline holds
 * FREAD_CHUNK - 1 characters, and the "\r\n" splice then writes three bytes
 * ('\r', '\n', NUL) after the last character. */
#define FREAD_CHUNK 512

/* Range predicates for file-derived values that index a table. The unsigned
 * cast folds "x < 0" into the upper-bound test, so each evaluates its
 * argument once. In the CircleMUD VALID_* style (see VALID_ROOM_RNUM). */
#define VALID_SECTOR(s)   ((unsigned int)(s) < NUM_ROOM_SECTORS)
#define VALID_DIR(d)      ((unsigned int)(d) < NUM_OF_DIRS)
#define VALID_LIQ(l)      ((unsigned int)(l) < NUM_LIQ_TYPES)
#define VALID_POS(p)      ((unsigned int)(p) < NUM_POSITIONS)
#define VALID_SEX(x)      ((unsigned int)(x) < NUM_GENDERS)
#define VALID_WEAR_POS(w) ((unsigned int)(w) < NUM_WEARS)
#define VALID_ATTACK(a)   ((unsigned int)(a) < NUM_ATTACK_TYPES)

/* TRUE if n dice of s sides fit the weapon limit oedit enforces
 * (MAX_WEAPON_NDICE d MAX_WEAPON_SDICE, oasis.h); no dice (0) is allowed.
 * Each argument is evaluated once. */
#define VALID_WEAPON_DICE(n, s) \
  ((unsigned int)(n) <= MAX_WEAPON_NDICE && (unsigned int)(s) <= MAX_WEAPON_SDICE)

/* TRUE if a parsed int/long can be stored in an IDXTYPE vnum field without
 * wrapping and is not the NOWHERE/NOTHING sentinel (IDXTYPE_MAX).
 * Evaluates n twice: pass a plain variable. */
#define VNUM_STORABLE(n)  ((long)(n) >= IDXTYPE_MIN && (long)(n) < IDXTYPE_MAX)

/* Report a fatal boot-time defect in the "db" subsystem with call-site
 * locality and stop: the structured replacement for the CircleMUD
 * log("SYSERR: ..."); exit(1); pair. Use only where continuing would index
 * past a table or corrupt the world. */
#define BOOT_FATAL(op, resource, ...) do { \
  TB_FATAL_MSG("db", (op), (resource), __VA_ARGS__); \
  exit(1); \
} while (0)

/*  declarations of most of the 'global' variables */
struct config_data config_info; /* Game configuration list.	 */

struct room_data *world = NULL;	/* array of rooms		 */
room_rnum top_of_world = 0;	/* ref to top element of world	 */

struct char_data *character_list = NULL; /* global linked list of chars	*/
struct index_data *mob_index;	/* index table for mobile file	 */
struct char_data *mob_proto;	/* prototypes for mobs		 */
mob_rnum top_of_mobt = 0;	/* top of mobile index table	 */

struct obj_data *object_list = NULL;	/* global linked list of objs	*/
struct index_data *obj_index;	/* index table for object file	 */
struct obj_data *obj_proto;	/* prototypes for objs		 */
obj_rnum top_of_objt = 0;	/* top of object index table	 */

struct zone_data *zone_table; /* zone table      */
zone_rnum top_of_zone_table = 0;/* top element of zone tab   */

/* begin previously located in players.c */
struct player_index_element *player_table = NULL; /* index to plr file   */
int top_of_p_table = 0;   /* ref to top of table     */
int top_of_p_file = 0;    /* ref of size of p file   */
long top_idnum = 0;       /* highest idnum in use    */
/* end previously located in players.c */

struct message_list fight_messages[MAX_MESSAGES];	/* fighting messages	 */

struct index_data **trig_index; /* index table for triggers      */
struct trig_data *trigger_list = NULL;  /* all attached triggers */
int top_of_trigt = 0;           /* top of trigger index table    */
long max_mob_id = MOB_ID_BASE;  /* for unique mob id's           */
long max_obj_id = OBJ_ID_BASE;  /* for unique obj id's           */

struct aq_data *aquest_table;   /* Autoquests table              */
qst_rnum total_quests = 0;      /* top of autoquest table        */

struct shop_data *shop_index;   /* index table for shops         */
int top_shop = -1;              /* top of shop table             */

int no_mail = 0;                /* mail disabled?		 */
int mini_mud = 0;               /* mini-mud mode?		 */
int no_rent_check = 0;          /* skip rent check on boot?	 */
time_t boot_time = 0;           /* time of mud boot		 */
int circle_restrict = 0;        /* level of game restriction	 */
room_rnum r_mortal_start_room;	/* rnum of mortal start room	 */
room_rnum r_immort_start_room;	/* rnum of immort start room	 */
room_rnum r_frozen_start_room;	/* rnum of frozen start room	 */

char *credits = NULL;		/* game credits			 */
char *news = NULL;		/* mud news			 */
char *motd = NULL;		/* message of the day - mortals  */
char *imotd = NULL;		/* message of the day - immorts  */
char *GREETINGS = NULL;		/* opening credits screen        */
char *help = NULL;		/* help screen			 */
char *ihelp = NULL;             /* help screen (immortals)       */
char *info = NULL;		/* info page			 */
char *wizlist = NULL;		/* list of higher gods		 */
char *immlist = NULL;		/* list of peon gods		 */
char *background = NULL;	/* background story		 */
char *handbook = NULL;		/* handbook for new immortals	 */
char *policies = NULL;		/* policies page		 */
char *bugs = NULL;              /* bugs file                     */
char *typos = NULL;             /* typos file                    */
char *ideas = NULL;             /* ideas file                    */

int top_of_helpt = 0;
struct help_index_element *help_table = NULL;

struct social_messg *soc_mess_list = NULL;      /* list of socials */
int top_of_socialt = -1;                        /* number of socials */

time_t newsmod; /* Time news file was last modified. */
time_t motdmod; /* Time motd file was last modified. */

struct time_info_data time_info;  /* the infomation about the time    */
struct weather_data weather_info;	/* the infomation about the weather */
struct player_special_data dummy_mob;	/* dummy spec area for mobs	*/
struct reset_q_type reset_q;	    /* queue of zones to be reset	 */

struct happyhour happy_data = {0,0,0,0};

/* declaration of local (file scope) variables */
static int converting = FALSE;

/* Local (file scope) utility functions */
static int check_bitvector_names(bitvector_t bits, size_t namecount, const char *whatami, const char *whatbits);
static int check_object_spell_number(struct obj_data *obj, int val);
static int check_object_level(struct obj_data *obj, int val);
static int check_object(struct obj_data *);
static void load_zones(FILE *fl, char *zonename);
static int file_to_string(const char *name, char *buf, size_t bufsize);
static int file_to_string_alloc(const char *name, char **buf);
static int count_alias_records(FILE *fl);
static void parse_simple_mob(FILE *mob_f, int i, int nr);
static void interpret_espec(const char *keyword, const char *value, int i, int nr);
static void parse_espec(char *buf, int i, int nr);
static void parse_enhanced_mob(FILE *mob_f, int i, int nr);
static void get_one_line(FILE *fl, char *buf);
static void check_start_rooms(void);
static void renum_zone_table(void);
static void log_zone_error(zone_rnum zone, int cmd_no, const char *message);
static void reset_time(void);
static char fread_letter(FILE *fp);
static void free_followers(struct follow_type *k);
static void load_default_config( void );
static void free_extra_descriptions(struct extra_descr_data *edesc);
static bitvector_t asciiflag_conv_aff(char *flag);
static int hsort(const void *a, const void *b);

static int parse_int_fields(const char *text, int *values, size_t max_fields)
{
  const char *p = text;
  char *end;
  size_t count = 0;
  long value;

  if (text == NULL || values == NULL)
    return -1;

  while (count < max_fields) {
    while (*p != '\0' && isspace((unsigned char)*p))
      p++;
    if (*p == '\0')
      break;

    errno = 0;
    value = strtol(p, &end, 10);
    if (p == end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
      return -1;

    values[count++] = (int)value;
    p = end;
  }

  while (*p != '\0' && isspace((unsigned char)*p))	/* NUL ends the scan */
    p++;
  if (*p != '\0')
    return -1;

  return (int)count;
}

static int parse_int_text_strict(const char *text, int *value)
{
  int parsed[1];
  int count = parse_int_fields(text, parsed, 1);

  if (count != 1)
    return 0;
  *value = parsed[0];
  return 1;
}

/* Parse a zone's four mandatory numeric fields, optionally followed by flags
 * and minimum/maximum levels. Keep legacy four-field zone headers valid. */
static int parse_reset_numbers(const char *text, int *values, int count,
                               const char **remainder)
{
  const char *p = text;
  char *end;
  long number;
  int i;

  if (p == NULL || values == NULL || count < 1)
    return 0;
  for (i = 0; i < count; i++) {
    while (isspace((unsigned char)*p))
      p++;
    errno = 0;
    number = strtol(p, &end, 10);
    if (p == end || errno == ERANGE || number < INT_MIN || number > INT_MAX)
      return 0;
    values[i] = (int)number;
    p = end;
  }
  if (remainder != NULL)
    *remainder = p;
  return 1;
}

static int parse_zone_header(const char *text, struct zone_data *zone,
                             char *f1, char *f2, char *f3, char *f4)
{
  const char *p = text;
  char *end;
  long values[4], levels[2];
  char *flags[4] = { f1, f2, f3, f4 };
  int i;

  if (p == NULL)
    return 0;
  for (i = 0; i < 4; i++) {
    while (isspace((unsigned char)*p))
      p++;
    errno = 0;
    values[i] = strtol(p, &end, 10);
    if (p == end || errno == ERANGE || values[i] < (i < 2 ? 0 : INT_MIN) ||
        values[i] > (i < 2 ? USHRT_MAX : INT_MAX))
      return 0;
    p = end;
  }
  zone->bot = (unsigned short)values[0];
  zone->top = (unsigned short)values[1];
  zone->lifespan = (int)values[2];
  zone->reset_mode = (int)values[3];

  while (isspace((unsigned char)*p))
    p++;
  if (*p == '\0')
    return 4;

  for (i = 0; i < 4; i++) {
    size_t len = 0;
    while (isspace((unsigned char)*p))
      p++;
    while (*p && !isspace((unsigned char)*p)) {
      if (len >= 79)
        return 4;
      flags[i][len++] = *p++;
    }
    if (len == 0)
      return 4;
    flags[i][len] = '\0';
  }
  for (i = 0; i < 2; i++) {
    while (isspace((unsigned char)*p))
      p++;
    errno = 0;
    levels[i] = strtol(p, &end, 10);
    if (p == end || errno == ERANGE || levels[i] < INT_MIN || levels[i] > INT_MAX)
      return 4;
    p = end;
  }
  zone->min_level = (int)levels[0];
  zone->max_level = (int)levels[1];
  return 10;
}

static int parse_hash_int(const char *text, int *value)
{
  return text != NULL && *text == '#' && parse_int_text_strict(text + 1, value);
}

static int parse_long_text_strict(const char *text, long *value)
{
  const char *p = text;
  char *end;
  long parsed;

  if (text == NULL || value == NULL)
    return 0;

  while ((*p == ' ' || (*p >= '\t' && *p <= '\r')))
    p++;
  errno = 0;
  parsed = strtol(p, &end, 10);
  if (p == end || errno == ERANGE)
    return 0;
  while ((*end == ' ' || (*end >= '\t' && *end <= '\r')))
    end++;
  if (*end != '\0')
    return 0;

  *value = parsed;
  return 1;
}

static int parse_dice_text(const char *text, int *count, int *size, int *bonus)
{
  char *end;
  long a, b, d;

  if (text == NULL || count == NULL || size == NULL || bonus == NULL)
    return 0;

  errno = 0;
  a = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || a < INT_MIN || a > INT_MAX ||
      (*end != 'd' && *end != 'D'))
    return 0;

  text = end + 1;
  errno = 0;
  b = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || b < INT_MIN || b > INT_MAX ||
      *end != '+')
    return 0;

  text = end + 1;
  errno = 0;
  d = strtol(text, &end, 10);
  if (text == end || errno == ERANGE || d < INT_MIN || d > INT_MAX ||
      *end != '\0')
    return 0;

  *count = (int)a;
  *size = (int)b;
  *bonus = (int)d;
  return 1;
}

/* routines for booting the system */
char *fread_action(FILE *fl, int nr)
{
  char buf[MAX_STRING_LENGTH];
  int i;

  if(fgets(buf, MAX_STRING_LENGTH, fl) == NULL) {
    if(feof(fl)) {
      log("SYSERR: fread_action: unexpected EOF near action #%d", nr);
      /* SYSERR_DESC: fread_action() will fail if it discovers an end of file
      * marker before it is able to read in the expected string.  This can be
      * caused by a truncated socials file. */
    } else {
      TB_REPORT_ERRNO("fgets", "social action text");
    }
    exit(1);
  }

  if (*buf == '#')
    return (NULL);

  parse_at(buf);

  /* Some clients interpret '\r' the same as { '\r' '\n' }, so the original way
   * of just replacing '\n' with '\0' would appear as 2 new lines following the
   * action */
  for (i = 0; buf[i] != '\0'; i++)
    if (buf[i] == '\r' || buf[i] == '\n') {
      buf[i] = '\0';
      break;
    }
  
  return (strdup(buf));
}

/** Report each social missing a message its command cannot do without. A
 * '#' line in the socials file (implementor-edited) leaves a field NULL.
 * do_action prints char_no_arg when the social is used alone, and do_gmote
 * formats others_found once char_found says the social takes a target;
 * both refuse such a social at run time (social-field-missing), so the
 * same report at boot names the file to fix before a player finds it. */
static void report_missing_social_messages(const char *socfile)
{
  int k;

  for (k = 0; k <= top_of_socialt; k++) {
    const struct social_messg *soc = &soc_mess_list[k];

    if (!soc->char_no_arg)
      TB_WARN_MSG("social", "social-field-missing", socfile,
                  "social '%s' has no char_no_arg message; using it without "
                  "a target will fail", soc->command);
    if (soc->char_found && !soc->others_found)
      TB_WARN_MSG("social", "social-field-missing", socfile,
                  "social '%s' has char_found but no others_found message; "
                  "gemote at a target will fail", soc->command);
  }
}

static void boot_social_messages(void)
{
  FILE *fl;
  int line_number, hide, min_char_pos, min_pos, min_lvl, curr_soc = -1;
  char next_soc[MAX_STRING_LENGTH], sorted[MAX_INPUT_LENGTH];
  char soc_name[256];	/* bounded by the " %255s " read below */
  const char *socfile = CONFIG_NEW_SOCIALS == TRUE ? SOCMESS_FILE_NEW : SOCMESS_FILE;
  char hide_text[32], min_char_text[32], min_pos_text[32], min_lvl_text[32];

  if (CONFIG_NEW_SOCIALS == TRUE) {
    /* open social file */
    if (!(fl = fopen(SOCMESS_FILE_NEW, "r"))) {
      TB_REPORT_ERRNO("fopen", SOCMESS_FILE_NEW);
      /* SYSERR_DESC: This error, from boot_social_messages(), occurs when the
       * server fails to open the file containing the social messages.  The
       * error at the end will indicate the reason why. */
      exit(1);
    }
    /* count socials */
    *next_soc = '\0';
    while (fgets(next_soc, MAX_STRING_LENGTH, fl))
      if (*next_soc == '~') top_of_socialt++;

    if(ferror(fl)) {
      TB_REPORT_ERRNO("fgets", SOCMESS_FILE_NEW);
      exit(1);
    }
  } else { /* old style */

    /* open social file */
    if (!(fl = fopen(SOCMESS_FILE, "r"))) {
      TB_REPORT_ERRNO("fopen", SOCMESS_FILE);
      /* SYSERR_DESC: This error, from boot_social_messages(), occurs when the
       * server fails to open the file containing the social messages.  The
       * error at the end will indicate the reason why. */
      exit(1);
    }
    /* count socials */
    while (fgets(next_soc, MAX_STRING_LENGTH, fl))
      if (*next_soc == '\n' || *next_soc == '\r') top_of_socialt++; /* all socials are followed by a blank line */

    if(ferror(fl)) {
      TB_REPORT_ERRNO("fgets", SOCMESS_FILE);
      exit(1);
    }
  }

  log("Social table contains %d socials.", top_of_socialt);
  if (fseek(fl, 0L, SEEK_SET) != 0) {
    TB_REPORT_ERRNO("fseek", CONFIG_NEW_SOCIALS == TRUE ? SOCMESS_FILE_NEW : SOCMESS_FILE);
    exit(1);
  }

  CREATE(soc_mess_list, struct social_messg, top_of_socialt + 1);

  /* now read 'em */
  for (line_number = 0;; ++line_number) {
    if (fscanf(fl, " %255s ", soc_name) != 1) {
      if(feof(fl))
        log("SYSERR: unexpected end of file encountered in socials file %s", socfile);
      else if(ferror(fl))
        TB_REPORT_ERRNO("fscanf", socfile);
      else
        log("SYSERR: format error in social file near line %d", line_number);
      exit(1);
    }
    if (*soc_name == '$') break;

    /* The count above sees only lines that start with '~' (new format) or
     * are empty (old format), but fscanf() skips leading whitespace, so a
     * social written as " ~name" or a separator line holding a space is
     * parsed without being counted. The socials file is the implementor's;
     * soc_mess_list[] has top_of_socialt + 1 slots (the spare one holds an
     * old-format last social with no blank line after it); a social past
     * them would write beyond the array. */
    if (curr_soc + 1 > top_of_socialt) {
      BOOT_FATAL("social-parse", socfile,
                 "social '%s' is past the %d slots allocated for %d counted "
                 "socials; check for leading whitespace on a name or "
                 "separator line", soc_name, top_of_socialt + 1,
                 top_of_socialt);
    }

    if (CONFIG_NEW_SOCIALS == TRUE) {
      if (fscanf(fl, " %255s %31s %31s %31s %31s \n",
                 sorted, hide_text, min_char_text, min_pos_text, min_lvl_text) != 5 ||
          !parse_int_text_strict(hide_text, &hide) ||
          !parse_int_text_strict(min_char_text, &min_char_pos) ||
          !parse_int_text_strict(min_pos_text, &min_pos) ||
          !parse_int_text_strict(min_lvl_text, &min_lvl)) {
      log("SYSERR: format error in social file near social '%s'", soc_name);
      /* SYSERR_DESC: From boot_social_messages(), this error is output when
       * the server is expecting to find the remainder of the first line of the
       * social ('hide' and 'minimum position').  These must follow the name of
       * the social with a single space such as: 'accuse 0 5\n'. This error
       * often occurs when one of the numbers is missing or the social name has
       * a space in it (i.e., 'bend over'). */
      exit(1);
    }
      curr_soc++;
      soc_mess_list[curr_soc].command = strdup(soc_name+1);
      soc_mess_list[curr_soc].sort_as = strdup(sorted);
      soc_mess_list[curr_soc].hide = hide;
      soc_mess_list[curr_soc].min_char_position = min_char_pos;
      soc_mess_list[curr_soc].min_victim_position = min_pos;
      soc_mess_list[curr_soc].min_level_char = min_lvl;
    } else {  /* old style */
      if (fscanf(fl, " %31s %31s \n", hide_text, min_pos_text) != 2 ||
          !parse_int_text_strict(hide_text, &hide) ||
          !parse_int_text_strict(min_pos_text, &min_pos)) {
        log("SYSERR: format error in social file near social '%s'", soc_name);
      /* SYSERR_DESC: From boot_social_messages(), this error is output when the
       * server is expecting to find the remainder of the first line of the
       * social ('hide' and 'minimum position').  These must follow the name of
       * the social with a single space such as: 'accuse 0 5\n'. This error
       * often occurs when one of the numbers is missing or the social name has
       * a space in it (i.e., 'bend over'). */
      exit(1);
      }
      curr_soc++;
      soc_mess_list[curr_soc].command = strdup(soc_name);
      soc_mess_list[curr_soc].sort_as = strdup(soc_name);
      soc_mess_list[curr_soc].hide = hide;
      soc_mess_list[curr_soc].min_char_position = POS_RESTING;
      soc_mess_list[curr_soc].min_victim_position = min_pos;
      soc_mess_list[curr_soc].min_level_char = 0;
    }

#ifdef CIRCLE_ACORN
    if (fgetc(fl) != '\n')
      log("SYSERR: Acorn bug workaround failed.");
      /* SYSERR_DESC: The only time that this error should ever arise is if you
       * are running your MUD on the Acorn platform.  The error arises when the
       * server cannot properly read a '\n' out of the file at the end of the
       * first line of the social (that with 'hide' and 'min position').  This
       * is in boot_social_messages(). */
#endif

    soc_mess_list[curr_soc].char_no_arg = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].others_no_arg = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].char_found = fread_action(fl, curr_soc);

    /* if no char_found, the rest is to be ignored */
    if (CONFIG_NEW_SOCIALS == FALSE && !soc_mess_list[curr_soc].char_found)
      continue;

    soc_mess_list[curr_soc].others_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].vict_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].not_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].char_auto = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].others_auto = fread_action(fl, curr_soc);

    if (CONFIG_NEW_SOCIALS == FALSE)
      continue;

    soc_mess_list[curr_soc].char_body_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].others_body_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].vict_body_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].char_obj_found = fread_action(fl, curr_soc);
    soc_mess_list[curr_soc].others_obj_found = fread_action(fl, curr_soc);
  }

  /* close file & set top */
  tb_fclose(fl);
  assert(curr_soc <= top_of_socialt);
  top_of_socialt = curr_soc;
  report_missing_social_messages(socfile);
}

/* this is necessary for the autowiz system */
void reboot_wizlists(void)
{
  file_to_string_alloc(WIZLIST_FILE, &wizlist);
  file_to_string_alloc(IMMLIST_FILE, &immlist);
}

/* Wipe out all the loaded text files, for shutting down. */
void free_text_files(void)
{
  char **textfiles[] = {
	&wizlist, &immlist, &news, &credits, &motd, &imotd, &help, &ihelp, &info,
	&policies, &handbook, &background, &GREETINGS, &bugs, &typos, &ideas, NULL
  };
  int rf;

  for (rf = 0; textfiles[rf]; rf++)
    if (*textfiles[rf]) {
      free(*textfiles[rf]);
      *textfiles[rf] = NULL;
    }
}

/* Too bad it doesn't check the return values to let the user know about -1
 * values.  This will result in an 'Okay.' to a 'reload' command even when the
 * string was not replaced. To fix later. */
ACMD(do_reboot)
{
  char arg[MAX_INPUT_LENGTH];

  one_argument(argument, arg);

  if (!str_cmp(arg, "all") || *arg == '*') {
    if (file_to_string_alloc(GREETINGS_FILE, &GREETINGS) == 0)
      prune_crlf(GREETINGS);
    if (file_to_string_alloc(WIZLIST_FILE, &wizlist) < 0)
      send_to_char(ch, "Cannot read wizlist\r\n");
    if (file_to_string_alloc(IMMLIST_FILE, &immlist) < 0)
      send_to_char(ch, "Cannot read immlist\r\n");
    if (file_to_string_alloc(NEWS_FILE, &news) < 0)
      send_to_char(ch, "Cannot read news\r\n");
    if (file_to_string_alloc(CREDITS_FILE, &credits) < 0)
      send_to_char(ch, "Cannot read credits\r\n");
    if (file_to_string_alloc(MOTD_FILE, &motd) < 0)
      send_to_char(ch, "Cannot read motd\r\n");
    if (file_to_string_alloc(IMOTD_FILE, &imotd) < 0)
      send_to_char(ch, "Cannot read imotd\r\n");
    if (file_to_string_alloc(HELP_PAGE_FILE, &help) < 0)
      send_to_char(ch, "Cannot read help front page\r\n");
    if (file_to_string_alloc(IHELP_PAGE_FILE, &ihelp) < 0)
      send_to_char(ch, "Cannot read help front page\r\n");
    if (file_to_string_alloc(INFO_FILE, &info) < 0)
      send_to_char(ch, "Cannot read info file\r\n");
    if (file_to_string_alloc(POLICIES_FILE, &policies) < 0)
      send_to_char(ch, "Cannot read policies\r\n");
    if (file_to_string_alloc(HANDBOOK_FILE, &handbook) < 0)
      send_to_char(ch, "Cannot read handbook\r\n");
    if (file_to_string_alloc(BACKGROUND_FILE, &background) < 0)
      send_to_char(ch, "Cannot read background\r\n");
    if (help_table) {
      free_help_table();
    index_boot(DB_BOOT_HLP);
    }
  } else if (!str_cmp(arg, "wizlist")) {
    if (file_to_string_alloc(WIZLIST_FILE, &wizlist) < 0)
      send_to_char(ch, "Cannot read wizlist\r\n");
  } else if (!str_cmp(arg, "immlist")) {
    if (file_to_string_alloc(IMMLIST_FILE, &immlist) < 0)
      send_to_char(ch, "Cannot read immlist\r\n");
  } else if (!str_cmp(arg, "news")) {
    if (file_to_string_alloc(NEWS_FILE, &news) < 0)
      send_to_char(ch, "Cannot read news\r\n");
  } else if (!str_cmp(arg, "credits")) {
    if (file_to_string_alloc(CREDITS_FILE, &credits) < 0)
      send_to_char(ch, "Cannot read credits\r\n");
  } else if (!str_cmp(arg, "motd")) {
    if (file_to_string_alloc(MOTD_FILE, &motd) < 0)
      send_to_char(ch, "Cannot read motd\r\n");
  } else if (!str_cmp(arg, "imotd")) {
    if (file_to_string_alloc(IMOTD_FILE, &imotd) < 0)
      send_to_char(ch, "Cannot read imotd\r\n");
  } else if (!str_cmp(arg, "help")) {
    if (file_to_string_alloc(HELP_PAGE_FILE, &help) < 0)
      send_to_char(ch, "Cannot read help front page\r\n");
  } else if (!str_cmp(arg, "ihelp")) {
    if (file_to_string_alloc(IHELP_PAGE_FILE, &ihelp) < 0)
      send_to_char(ch, "Cannot read help front page\r\n");
  } else if (!str_cmp(arg, "info")) {
    if (file_to_string_alloc(INFO_FILE, &info) < 0)
      send_to_char(ch, "Cannot read info\r\n");
  } else if (!str_cmp(arg, "policy")) {
    if (file_to_string_alloc(POLICIES_FILE, &policies) < 0)
      send_to_char(ch, "Cannot read policy\r\n");
  } else if (!str_cmp(arg, "handbook")) {
    if (file_to_string_alloc(HANDBOOK_FILE, &handbook) < 0)
      send_to_char(ch, "Cannot read handbook\r\n");
  } else if (!str_cmp(arg, "background")) {
    if (file_to_string_alloc(BACKGROUND_FILE, &background) < 0)
      send_to_char(ch, "Cannot read background\r\n");
  } else if (!str_cmp(arg, "greetings")) {
    if (file_to_string_alloc(GREETINGS_FILE, &GREETINGS) == 0)
      prune_crlf(GREETINGS);
    else
      send_to_char(ch, "Cannot read greetings.\r\n");
  } else if (!str_cmp(arg, "xhelp")) {
    if (help_table) {
      free_help_table();
    index_boot(DB_BOOT_HLP);
    }
  } else {
    send_to_char(ch, "Unknown reload option.\r\n");
    return;
  }

  send_to_char(ch, "%s", CONFIG_OK);
}

void boot_world(void)
{
  log("Loading zone table.");
  index_boot(DB_BOOT_ZON);

  log("Loading triggers and generating index.");
  index_boot(DB_BOOT_TRG);

  log("Loading rooms.");
  index_boot(DB_BOOT_WLD);

  log("Renumbering rooms.");
  renum_world();

  log("Checking start rooms.");
  check_start_rooms();

  log("Loading mobs and generating index.");
  index_boot(DB_BOOT_MOB);

  log("Loading objs and generating index.");
  index_boot(DB_BOOT_OBJ);

  log("Renumbering zone table.");
  renum_zone_table();

  if(converting) {
    log("Saving 128bit world files to disk.");
    save_all();
  }

  if (!no_specials) {
    log("Loading shops.");
    index_boot(DB_BOOT_SHP);
  }

  log("Loading quests.");
  index_boot(DB_BOOT_QST);

}

static void free_extra_descriptions(struct extra_descr_data *edesc)
{
  struct extra_descr_data *enext;

  for (; edesc; edesc = enext) {
    enext = edesc->next;

    free(edesc->keyword);
    free(edesc->description);
    free(edesc);
  }
}

/** Free the world, in a memory allocation sense.
 *
 * Call only at shutdown, after every descriptor has been closed: every
 * follower is detached (stop_follower()) before any character is freed, so
 * no follow list points at a freed character, and the freed tables
 * (world, prototypes, indexes, zone_table, help_table, socials) are left as
 * dangling globals rather than reset, so nothing may touch them afterwards.
 * Zone command strings (sarg1/sarg2) are freed whatever the command letter. */
void destroy_db(void)
{
  ssize_t cnt, itr;
  struct char_data *chtmp, *i = character_list;
  struct obj_data *objtmp;

  /* Active Mobiles & Players */
  while (i) {
    chtmp = i;
    i = i->next;

    if (chtmp->master)
      stop_follower(chtmp);
  }

  while (character_list) {
    chtmp = character_list;
    character_list = character_list->next;

    free_char(chtmp);
  }

  /* Active Objects */
  while (object_list) {
    objtmp = object_list;
    object_list = object_list->next;
    free_obj(objtmp);
  }

  /* Rooms */
  for (cnt = 0; cnt <= top_of_world; cnt++) {
    if (world[cnt].name)
      free(world[cnt].name);
    if (world[cnt].description)
      free(world[cnt].description);
    free_extra_descriptions(world[cnt].ex_description);

  if (world[cnt].events != NULL) {
	  if (world[cnt].events->iSize > 0) {
		struct event * pEvent;

		while ((pEvent = simple_list(world[cnt].events)) != NULL)
		  event_cancel(pEvent);
	  }
	  free_list(world[cnt].events);
    world[cnt].events = NULL;
  }

    /* free any assigned scripts */
    if (SCRIPT(&world[cnt]))
      extract_script(&world[cnt], WLD_TRIGGER);
    /* free script proto list */
    free_proto_script(&world[cnt], WLD_TRIGGER);

    for (itr = 0; itr < NUM_OF_DIRS; itr++) { /* NUM_OF_DIRS here, not DIR_COUNT */
      if (!world[cnt].dir_option[itr])
        continue;

      if (world[cnt].dir_option[itr]->general_description)
        free(world[cnt].dir_option[itr]->general_description);
      if (world[cnt].dir_option[itr]->keyword)
        free(world[cnt].dir_option[itr]->keyword);
      free(world[cnt].dir_option[itr]);
    }
  }
  free(world);
  /* Leave no dangling pointer: top_of_world == 0 still means "one room,
   * world[0]", so code that runs during the rest of teardown (the script
   * ownership scans in dg_handler.c) must be able to see the world is gone. */
  world = NULL;
  top_of_world = 0;

  /* Objects */
  for (cnt = 0; cnt <= top_of_objt; cnt++) {
    if (obj_proto[cnt].name)
      free(obj_proto[cnt].name);
    if (obj_proto[cnt].description)
      free(obj_proto[cnt].description);
    if (obj_proto[cnt].short_description)
      free(obj_proto[cnt].short_description);
    if (obj_proto[cnt].action_description)
      free(obj_proto[cnt].action_description);
    free_extra_descriptions(obj_proto[cnt].ex_description);

    /* free script proto list */
    free_proto_script(&obj_proto[cnt], OBJ_TRIGGER);
  }
  free(obj_proto);
  free(obj_index);

  /* Mobiles */
  for (cnt = 0; cnt <= top_of_mobt; cnt++) {
    if (mob_proto[cnt].player.name)
      free(mob_proto[cnt].player.name);
    if (mob_proto[cnt].player.title)
      free(mob_proto[cnt].player.title);
    if (mob_proto[cnt].player.short_descr)
      free(mob_proto[cnt].player.short_descr);
    if (mob_proto[cnt].player.long_descr)
      free(mob_proto[cnt].player.long_descr);
    if (mob_proto[cnt].player.description)
      free(mob_proto[cnt].player.description);

    /* free script proto list */
    free_proto_script(&mob_proto[cnt], MOB_TRIGGER);

    while (mob_proto[cnt].affected)
      affect_remove(&mob_proto[cnt], mob_proto[cnt].affected);
  }
  free(mob_proto);
  free(mob_index);

  /* Shops */
  destroy_shops();

  /* Quests */
  destroy_quests();

  /* Zones */
#define THIS_CMD zone_table[cnt].cmd[itr]

  for (cnt = 0; cnt <= top_of_zone_table; cnt++) {
    if (zone_table[cnt].name)
      free(zone_table[cnt].name);
    if (zone_table[cnt].builders)
      free(zone_table[cnt].builders);
    if (zone_table[cnt].cmd) {
      /* Free every command's variable strings, not only those of a 'V':
       * renum_zone_table() and load_zones() disable a bad 'V' by rewriting
       * it to '*', and it still owns sarg1/sarg2. Commands that never had
       * strings hold NULL (the table is CREATE()d zeroed). */
      for (itr = 0;THIS_CMD.command != 'S';itr++) {
        free(THIS_CMD.sarg1);
        free(THIS_CMD.sarg2);
      }
      /* then free the command list */
      free(zone_table[cnt].cmd);
    }
  }
  free(zone_table);

#undef THIS_CMD

  /* zone table reset queue */
  if (reset_q.head) {
    struct reset_q_element *ftemp=reset_q.head, *temp;
    while (ftemp) {
      temp = ftemp->next;
      free(ftemp);
      ftemp = temp;
    }
  }

  /* Triggers */
  for (cnt=0; cnt < top_of_trigt; cnt++) {
    if (trig_index[cnt]->proto) {
      /* make sure to nuke the command list (memory leak) */
      /* free_trigger() doesn't free the command list */
      if (trig_index[cnt]->proto->cmdlist) {
        struct cmdlist_element *i, *j;
        i = trig_index[cnt]->proto->cmdlist;
        while (i) {
          j = i->next;
          if (i->cmd)
            free(i->cmd);
          free(i);
          i = j;
        }
      }
      free_trigger(trig_index[cnt]->proto);
    }
    free(trig_index[cnt]);
  }
  free(trig_index);

  /* Events */
  event_free_all();

}

/* body of the booting system */
void boot_db(void)
{
  zone_rnum i;

  log("Boot db -- BEGIN.");

  log("Resetting the game time:");
  reset_time();
  
  log("Initialize Global Lists");
  global_lists = create_list();
  group_list   = create_list();

  log("Initializing Events");
  init_events();

  log("Reading news, credits, help, ihelp, bground, info & motds.");
  file_to_string_alloc(NEWS_FILE, &news);
  file_to_string_alloc(CREDITS_FILE, &credits);
  file_to_string_alloc(MOTD_FILE, &motd);
  file_to_string_alloc(IMOTD_FILE, &imotd);
  file_to_string_alloc(HELP_PAGE_FILE, &help);
  file_to_string_alloc(IHELP_PAGE_FILE, &ihelp);
  file_to_string_alloc(INFO_FILE, &info);
  file_to_string_alloc(WIZLIST_FILE, &wizlist);
  file_to_string_alloc(IMMLIST_FILE, &immlist);
  file_to_string_alloc(POLICIES_FILE, &policies);
  file_to_string_alloc(HANDBOOK_FILE, &handbook);
  file_to_string_alloc(BACKGROUND_FILE, &background);
  if (file_to_string_alloc(GREETINGS_FILE, &GREETINGS) == 0)
    prune_crlf(GREETINGS);

  log("Loading spell definitions.");
  mag_assign_spells();

  boot_world();

  log("Loading help entries.");
  index_boot(DB_BOOT_HLP);

  log("Generating player index.");
  build_player_index();

  if (auto_pwipe) {
    log("Cleaning out inactive pfiles.");
    clean_pfiles();
  }

  log("Loading fight messages.");
  load_messages();

  log("Loading social messages.");
  boot_social_messages();

  log("Building command list.");
  create_command_list(); /* aedit patch -- M. Scott */

  log("Assigning function pointers:");

  if (!no_specials) {
    log("   Mobiles.");
    assign_mobiles();
    log("   Shopkeepers.");
    assign_the_shopkeepers();
    log("   Objects.");
    assign_objects();
    log("   Rooms.");
    assign_rooms();
    log("   Questmasters.");
    assign_the_quests();

    /* gen_board is now live on the board objects; its message index must
     * exist before the first player can touch one. */
    log("Booting bulletin boards.");
    if (init_boards() > 0) {
      TB_FATAL_MSG("board", "board-init", "boot_db",
                   TB_ERRID_BOOT_FATAL " board_info names object vnums that do not exist");
      exit(1);
    }
  }

  log("Assigning spell and skill levels.");
  init_spell_levels();

  log("Sorting command list and spells.");
  sort_commands();
  sort_spells();

  log("Booting mail system.");
  if (!scan_file()) {
    log("    Mail boot failed -- Mail system disabled");
    no_mail = 1;
  }
  log("Reading banned site and invalid-name list.");
  load_banned();
  read_invalid_list();

  log("Loading Ideas.");
  load_ibt_file(SCMD_IDEA);

  log("Loading Bugs.");
  load_ibt_file(SCMD_BUG);

  log("Loading Typos.");
  load_ibt_file(SCMD_TYPO);

  if (!no_rent_check) {
    log("Deleting timed-out crash and rent files:");
    update_obj_file();
    log("   Done.");
  }

  /* Moved here so the object limit code works. -gg 6/24/98 */
  if (!mini_mud)  {
    log("Booting houses.");
    House_boot();
  }

  log("Cleaning up last log.");
  clean_llog_entries();

#if 1
{
  int j;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
  for (j = 0 ; j < top_of_objt; j++) {
    if (obj_proto[j].script == (struct script_data *)&shop_keeper) {
      log("Item %d (%s) had shopkeeper trouble.", obj_index[j].vnum, obj_proto[j].short_description);
      obj_proto[j].script = NULL;
    }
  }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}
#endif

  for (i = 0; i <= top_of_zone_table; i++) {
    log("Resetting #%d: %s (rooms %d-%d).", zone_table[i].number,
	zone_table[i].name, zone_table[i].bot, zone_table[i].top);
    reset_zone(i);
  }

  reset_q.head = reset_q.tail = NULL;

  if (!boot_time)
    boot_time = time(0);

  log("Boot db -- DONE.");
}

/* reset the time in the game from file */
static void reset_time(void)
{
  time_t beginning_of_time = 0;
  FILE *bgtime;
  char time_text[64];
  long parsed_time;

  if ((bgtime = fopen(TIME_FILE, "r")) == NULL)
    log("No time file '%s' starting from the beginning.", TIME_FILE);
  else {
    if (fscanf(bgtime, "%63s", time_text) != 1 ||
        !parse_long_text_strict(time_text, &parsed_time)) {
      if (feof(bgtime))
        log("SYSERR: reset_time: unexpected end of file encountered reading %s.", TIME_FILE);
      else
        log("SYSERR: reset_time: invalid time value in %s.", TIME_FILE);
    } else {
      beginning_of_time = (time_t)parsed_time;
    }
    tb_fclose(bgtime);
  }

  if (beginning_of_time == 0)
    beginning_of_time = 650336715;

time_info = *mud_time_passed(time(0), beginning_of_time);

  if (time_info.hours <= 4)
    weather_info.sunlight = SUN_DARK;
  else if (time_info.hours == 5)
    weather_info.sunlight = SUN_RISE;
  else if (time_info.hours <= 20)
    weather_info.sunlight = SUN_LIGHT;
  else if (time_info.hours == 21)
    weather_info.sunlight = SUN_SET;
  else
    weather_info.sunlight = SUN_DARK;

  log("   Current Gametime: %dH %dD %dM %dY.", time_info.hours,
	  time_info.day, time_info.month, time_info.year);

  weather_info.pressure = 960;
  if ((time_info.month >= 7) && (time_info.month <= 12))
    weather_info.pressure += dice(1, 50);
  else
    weather_info.pressure += dice(1, 80);

  weather_info.change = 0;

  if (weather_info.pressure <= 980)
    weather_info.sky = SKY_LIGHTNING;
  else if (weather_info.pressure <= 1000)
    weather_info.sky = SKY_RAINING;
  else if (weather_info.pressure <= 1020)
    weather_info.sky = SKY_CLOUDY;
  else
    weather_info.sky = SKY_CLOUDLESS;
}

/* Write the time in 'when' to the MUD-time file. */
void save_mud_time(struct time_info_data *when)
{
  FILE *bgtime;

  if ((bgtime = fopen(TIME_FILE, "w")) == NULL)
    log("SYSERR: Can't write to '%s' time file.", TIME_FILE);
  else {
    tb_fprintf(bgtime, "%ld\n", (long)mud_time_to_secs(when));
    tb_fclose(bgtime);
  }
}

/* Thanks to Andrey (andrey@alex-ua.com) for this bit of code, although I did
 * add the 'goto' and changed some "while()" into "do { } while()". -gg */
static int count_alias_records(FILE *fl)
{
  char key[READ_SIZE], next_key[READ_SIZE];
  char line[READ_SIZE], *scan;
  int total_keywords = 0;

  /* get the first keyword line */
  get_one_line(fl, key);

  while (*key != '$') {
    /* skip the text */
    do {
      get_one_line(fl, line);
      if (feof(fl))
	goto ackeof;
    } while (*line != '#');

    /* now count keywords */
    scan = key;
    do {
      scan = one_word(scan, next_key);
      if (*next_key)
        ++total_keywords;
    } while (*next_key);

    /* get next keyword line (or $) */
    get_one_line(fl, key);

    if (feof(fl))
      goto ackeof;
  }

  return (total_keywords);

  /* No, they are not evil. -gg 6/24/98 */
ackeof:
  log("SYSERR: Unexpected end of help file.");
  exit(1);	/* Some day we hope to handle these things better... */
}

/* Prototype slots index_boot() allocated for the current DB_BOOT_* mode,
 * and how many '#' records discrete_load() has handed to a parser so far.
 * count_hash_records() only estimates the record count (a '#' inside a long
 * line or a string can make the two passes disagree), so discrete_load()
 * checks every record against the real allocation. */
static int boot_capacity, boot_records;
/* The previous record number in this DB_BOOT_* mode, across all its files.
 * real_room(), real_mobile() and real_object() binary-search the prototype
 * arrays, so the vnums must ascend through the whole index. */
static int boot_last_vnum;
/* Set by discrete_load() when a record number did not ascend; index_boot()
 * then sorts the table with sort_boot_records(). */
static int boot_disordered;

/** A loaded record's vnum and table slot, for sort_boot_records(). */
struct boot_vnum_slot {
  int vnum;
  int slot;
};

/* Ascending vnum; records sharing a vnum keep their file order. */
static int boot_vnum_slot_cmp(const void *a, const void *b)
{
  const struct boot_vnum_slot *x = a, *y = b;

  if (x->vnum != y->vnum)
    return x->vnum < y->vnum ? -1 : 1;
  return (x->slot > y->slot) - (x->slot < y->slot);
}

/** Reorder the n elements of size bytes at base so that element k becomes
 * the old element order[k].slot. */
static void boot_permute(void *base, size_t size,
                         const struct boot_vnum_slot *order, int n)
{
  char *copy;
  int k;

  CREATE(copy, char, (size_t)n * size);
  memcpy(copy, base, (size_t)n * size);
  for (k = 0; k < n; k++)
    memcpy((char *)base + (size_t)k * size,
           copy + (size_t)order[k].slot * size, size);
  free(copy);
}

/** Put the table index_boot() just loaded back in ascending vnum order.
 *
 * real_room(), real_mobile(), real_object() and real_trigger() binary-search
 * these tables, so one record out of place also hides records that are in
 * place: the stock 301.wld lists #30170 after #30172, and real_room() then
 * found neither 30171 nor 30172, so the exits to them silently led nowhere.
 * The world files are the implementor's, and discrete_load() has reported
 * each record that did not ascend. Nothing holds an rnum into the table yet
 * (renum_world(), renum_zone_table(), shops, quests and spec procs come
 * later in boot_world() and boot_db()), so the records are moved and their
 * own rnum fields renumbered. Records sharing a vnum stay in file order and
 * are reported: a lookup finds only one of them. */
static void sort_boot_records(int mode, const char *index_path)
{
  struct boot_vnum_slot *order;
  int n, k;

  switch (mode) {
  case DB_BOOT_WLD: n = top_of_world + 1; break;
  case DB_BOOT_MOB: n = top_of_mobt + 1; break;
  case DB_BOOT_OBJ: n = top_of_objt + 1; break;
  case DB_BOOT_TRG: n = top_of_trigt; break;
  case DB_BOOT_QST: n = total_quests; break;
  default: return;
  }
  if (n < 2)
    return;

  CREATE(order, struct boot_vnum_slot, n);
  for (k = 0; k < n; k++) {
    order[k].slot = k;
    switch (mode) {
    case DB_BOOT_WLD: order[k].vnum = world[k].number; break;
    case DB_BOOT_MOB: order[k].vnum = mob_index[k].vnum; break;
    case DB_BOOT_OBJ: order[k].vnum = obj_index[k].vnum; break;
    case DB_BOOT_TRG: order[k].vnum = trig_index[k]->vnum; break;
    default:          order[k].vnum = aquest_table[k].vnum; break;
    }
  }
  qsort(order, n, sizeof(*order), boot_vnum_slot_cmp);

  switch (mode) {
  case DB_BOOT_WLD:
    boot_permute(world, sizeof(*world), order, n);
    break;
  case DB_BOOT_MOB:
    boot_permute(mob_proto, sizeof(*mob_proto), order, n);
    boot_permute(mob_index, sizeof(*mob_index), order, n);
    for (k = 0; k < n; k++)
      mob_proto[k].nr = k;
    break;
  case DB_BOOT_OBJ:
    boot_permute(obj_proto, sizeof(*obj_proto), order, n);
    boot_permute(obj_index, sizeof(*obj_index), order, n);
    for (k = 0; k < n; k++)
      obj_proto[k].item_number = k;
    break;
  case DB_BOOT_TRG:
    boot_permute(trig_index, sizeof(*trig_index), order, n);
    for (k = 0; k < n; k++)
      trig_index[k]->proto->nr = k;
    break;
  default:
    boot_permute(aquest_table, sizeof(*aquest_table), order, n);
    break;
  }

  for (k = 1; k < n; k++)
    if (order[k].vnum == order[k - 1].vnum)
      TB_ERROR_MSG("db", "record-number", index_path,
                   "vnum %d is used by more than one record; lookups find "
                   "only one of them", order[k].vnum);
  free(order);
}

/** Count the '#'-delimited records in a world file, to size the prototype
 * arrays. Reads in READ_SIZE chunks, as get_line() does, so a '#' that
 * get_line() would return at the start of a chunk is counted. The count may
 * still be high (a '#' at the start of a string line), and is never trusted
 * as exact: discrete_load() enforces the allocation. */
int count_hash_records(FILE *fl)
{
  char buf[READ_SIZE];
  int count = 0;

  while (fgets(buf, sizeof(buf), fl))
    if (*buf == '#')
      count++;

  return (count);
}

/* Longest data-file name an index line may hold, and the matching scanf
 * width ("%1023s"); the two-level stringify turns the number into text. */
#define INDEX_NAME_MAX 1023
#define INDEX_NAME_STR_(n) #n
#define INDEX_NAME_STR(n) INDEX_NAME_STR_(n)
#define INDEX_NAME_FMT "%" INDEX_NAME_STR(INDEX_NAME_MAX) "s\n"

void index_boot(int mode)
{
  const char *index_filename, *prefix = NULL;	/* NULL or egcs 1.1 complains */
  FILE *db_index, *db_file;
  int line_number, rec_count = 0, size[2];
  /* buf1 holds one index line (a file name, bounded by INDEX_NAME_FMT);
   * buf2 is prefix + that name; index_path names the index file itself. */
  char buf2[PATH_MAX], buf1[INDEX_NAME_MAX + 1], index_path[PATH_MAX];

  switch (mode) {
  case DB_BOOT_WLD:
    prefix = WLD_PREFIX;
    break;
  case DB_BOOT_MOB:
    prefix = MOB_PREFIX;
    break;
  case DB_BOOT_OBJ:
    prefix = OBJ_PREFIX;
    break;
  case DB_BOOT_ZON:
    prefix = ZON_PREFIX;
    break;
  case DB_BOOT_SHP:
    prefix = SHP_PREFIX;
    break;
  case DB_BOOT_HLP:
    prefix = HLP_PREFIX;
    break;
  case DB_BOOT_TRG:
    prefix = TRG_PREFIX;
    break;
  case DB_BOOT_QST:
    prefix = QST_PREFIX;
    break;
  default:
    log("SYSERR: Unknown subcommand %d to index_boot!", mode);
    exit(1);
  }

  if (mini_mud)
    index_filename = MINDEX_FILE;
  else
    index_filename = INDEX_FILE;

  tb_snprintf(index_path, sizeof(index_path), "%s%s", prefix, index_filename);
  if (!(db_index = fopen(index_path, "r"))) {
    TB_REPORT_ERRNO("fopen", index_path);
    exit(1);
  }

  for (line_number = 0;; ++line_number) {
    /* first, count the number of records in the file so we can malloc */
    if (fscanf(db_index, INDEX_NAME_FMT, buf1) != 1) {
      if (feof(db_index))
        log("SYSERR: boot error -- unexpected end of file encountered in index file ./%s%s. "
            "Ensure that the last line of the file starts with the character '$'.",
            prefix, index_filename);
      else if (ferror(db_index))
        TB_REPORT_ERRNO("fscanf", index_path);
      else
        log("SYSERR: boot error -- error parsing index file %s%s on line %d",
            prefix, index_filename, line_number);
      exit(1);
    }

    if (*buf1 == '$')
      break;

    tb_snprintf(buf2, sizeof(buf2), "%s%s", prefix, buf1);
    if (!(db_file = fopen(buf2, "r"))) {
      TB_REPORT_ERRNO("fopen", buf2);
    } else {
      if (mode == DB_BOOT_ZON)
        rec_count++;
      else if (mode == DB_BOOT_HLP)
        rec_count += count_alias_records(db_file);
      else
        rec_count += count_hash_records(db_file);
      tb_fclose(db_file);
    }
  }

  /* Exit if 0 records, unless this is shops */
  if (!rec_count) {
    if (mode == DB_BOOT_SHP || mode == DB_BOOT_QST) {
      tb_fclose(db_index);
      return;
    }
    log("SYSERR: boot error - 0 records counted in %s/%s.", prefix,
	index_filename);
    tb_fclose(db_index);
    exit(1);
  }

  /* "bytes" does _not_ include strings or other later malloc'd things. */
  switch (mode) {
  case DB_BOOT_TRG:
    CREATE(trig_index, struct index_data *, rec_count);
    break;
  case DB_BOOT_WLD:
    CREATE(world, struct room_data, rec_count);
    size[0] = sizeof(struct room_data) * rec_count;
    log("   %d rooms, %d bytes.", rec_count, size[0]);
    break;
  case DB_BOOT_MOB:
    CREATE(mob_proto, struct char_data, rec_count);
    CREATE(mob_index, struct index_data, rec_count);
    size[0] = sizeof(struct index_data) * rec_count;
    size[1] = sizeof(struct char_data) * rec_count;
    log("   %d mobs, %d bytes in index, %d bytes in prototypes.", rec_count, size[0], size[1]);
    break;
  case DB_BOOT_OBJ:
    CREATE(obj_proto, struct obj_data, rec_count);
    CREATE(obj_index, struct index_data, rec_count);
    size[0] = sizeof(struct index_data) * rec_count;
    size[1] = sizeof(struct obj_data) * rec_count;
    log("   %d objs, %d bytes in index, %d bytes in prototypes.", rec_count, size[0], size[1]);
    break;
  case DB_BOOT_ZON:
    CREATE(zone_table, struct zone_data, rec_count);
    size[0] = sizeof(struct zone_data) * rec_count;
    log("   %d zones, %d bytes.", rec_count, size[0]);
    break;
  case DB_BOOT_HLP:
    CREATE(help_table, struct help_index_element, rec_count);
    size[0] = sizeof(struct help_index_element) * rec_count;
    log("   %d entries, %d bytes.", rec_count, size[0]);
    break;
  case DB_BOOT_QST:
    CREATE(aquest_table, struct aq_data, rec_count);
    size[0] = sizeof(struct aq_data) * rec_count;
    log("   %d entries, %d bytes.", rec_count, size[0]);
    break;
  }

  if (fseek(db_index, 0L, SEEK_SET) != 0) {
    TB_REPORT_ERRNO("fseek", index_path);
    exit(1);
  }

  boot_capacity = rec_count;
  boot_records = 0;
  boot_last_vnum = -1;
  boot_disordered = FALSE;

  for (line_number = 1;; ++line_number) {
    if (fscanf(db_index, INDEX_NAME_FMT, buf1) != 1) {
      if (feof(db_index))
        log("SYSERR: boot error -- unexpected end of file encountered in index file ./%s%s",
            prefix, index_filename);
      else if (ferror(db_index))
        TB_REPORT_ERRNO("fscanf", index_path);
      else
        log("SYSERR: boot error -- error parsing index file ./%s%s on line %d",
            prefix, index_filename, line_number);
      exit(1);
    }

    if (*buf1 == '$')
      break;

    tb_snprintf(buf2, sizeof(buf2), "%s%s", prefix, buf1);
    if (!(db_file = fopen(buf2, "r"))) {
      TB_REPORT_ERRNO("fopen", buf2);
      exit(1);
    }
    switch (mode) {
    case DB_BOOT_WLD:
    case DB_BOOT_OBJ:
    case DB_BOOT_MOB:
    case DB_BOOT_TRG:
    case DB_BOOT_QST:
      discrete_load(db_file, mode, buf2);
      break;
    case DB_BOOT_ZON:
      load_zones(db_file, buf2);
      break;
    case DB_BOOT_HLP:
      load_help(db_file, buf2);
      break;
    case DB_BOOT_SHP:
      boot_the_shops(db_file, buf2, rec_count);
      break;
    }

    tb_fclose(db_file);
  }
  tb_fclose(db_index);

  /* discrete_load() reported each record number that did not ascend. */
  if (boot_disordered)
    sort_boot_records(mode, index_path);

  /* Sort the help index. */
  if (mode == DB_BOOT_HLP) {
    qsort(help_table, top_of_helpt, sizeof(struct help_index_element), hsort);
  }
}

void discrete_load(FILE *fl, int mode, char *filename)
{
  int nr = -1, last;
  char line[READ_SIZE];

  const char *modes[] = {"world", "mob", "obj", "ZON", "SHP", "HLP", "trg", "qst"};
  /* modes positions correspond to DB_BOOT_xxx in db.h */

  for (;;) {
    /* We have to do special processing with the obj files because they have no
     * end-of-record marker. */
    if (mode != DB_BOOT_OBJ || nr < 0)
      if (!get_line(fl, line)) {
	if (nr == -1) {
	  log("SYSERR: %s file %s is empty!", modes[mode], filename);
	} else {
	  log("SYSERR: Format error in %s after %s #%d\n"
	      "...expecting a new %s, but file ended!\n"
	      "(maybe the file is not terminated with '$'?)", filename,
	      modes[mode], nr, modes[mode]);
	}
	exit(1);
      }
    if (*line == '$')
      return;

    if (*line == '#') {
      last = nr;
      if (!parse_int_text_strict(line + 1, &nr)) {
	log("SYSERR: Format error after %s #%d", modes[mode], last);
	exit(1);
      }
      if (nr >= 99999) {
	/* Old CircleMUD files ended with "#99999"; anything after it in the
	 * file is ignored, so say so. */
	TB_WARN_MSG("db", "record-number", filename,
	            "%s #%d ends the file; any later records are skipped",
	            modes[mode], nr);
	return;
      }
      /* The record number becomes an IDXTYPE vnum (ush_int): #70000 would
       * silently become vnum 4464, and IDXTYPE_MAX is NOWHERE/NOTHING. */
      if (!VNUM_STORABLE(nr)) {
	BOOT_FATAL("record-number", filename,
	             "%s #%d out of range 0..%d (previous #%d)",
	             modes[mode], nr, IDXTYPE_MAX - 1, boot_last_vnum);
      }
      /* A number that does not ascend breaks the binary search in
       * real_room()/real_mobile()/real_object(): lookups of it, or of its
       * neighbours, fail. The stock world has such a record (301.wld
       * #30170), so it is reported, not fatal, and index_boot() sorts the
       * table once the whole index is loaded. */
      if (nr <= boot_last_vnum) {
	TB_ERROR_MSG("db", "record-number", filename,
	             "%s #%d does not ascend (previous #%d); the table is "
	             "sorted after loading", modes[mode], nr, boot_last_vnum);
	boot_disordered = TRUE;
      }
      boot_last_vnum = nr;
      /* Each record fills the next prototype slot (parse_room(),
       * parse_mobile(), parse_object()... keep their own cursor). The
       * world files are the implementor's; a record the counting pass
       * missed would be written past the array index_boot() allocated. */
      if (boot_records >= boot_capacity) {
	BOOT_FATAL("proto-slot", filename,
	             "%s #%d exceeds the %d records counted at boot",
	             modes[mode], nr, boot_capacity);
      }
      boot_records++;
      switch (mode) {
	case DB_BOOT_WLD:
	  parse_room(fl, nr);
	  break;
	case DB_BOOT_MOB:
	  parse_mobile(fl, nr);
	  break;
        case DB_BOOT_TRG:
          parse_trigger(fl, nr);
          break;
	case DB_BOOT_OBJ:
	  strlcpy(line, parse_object(fl, nr), sizeof(line));
	  break;
  case DB_BOOT_QST:
    parse_quest(fl, nr);
    break;
	}
    } else {
      log("SYSERR: Format error in %s file %s near %s #%d", modes[mode],
	  filename, modes[mode], nr);
      log("SYSERR: ... offending line: '%s'", line);
      exit(1);
    }
  }
}

static char fread_letter(FILE *fp)
{
  char c;
  do {
    c = getc(fp);
  } while (isspace(c));
  return c;
}

/** Convert a flag word from a world file to bits: 'a'..'z' are bits 0..25
 * and 'A'..'Z' bits 26..51; an all-digit word (optionally with a leading
 * '-') is taken as a number, so "-1" sets every bit. asciiflag_conv_aff()
 * is the same with every letter one bit higher. Most callers store the
 * result in a 32-bit int slot; db.c passes it through flag_slot(), which
 * reports bits that do not fit. */
bitvector_t asciiflag_conv(char *flag)
{
  bitvector_t flags = 0;
  int is_num = TRUE;
  char *p;

  for (p = flag; *p; p++) {
    if (islower(*p)) {
      unsigned int bit = (unsigned int)(*p - 'a');
      if (bit < sizeof(bitvector_t) * CHAR_BIT)
        flags |= (bitvector_t)1 << bit;
    } else if (isupper(*p)) {
      unsigned int bit = (unsigned int)(26 + (*p - 'A'));
      if (bit < sizeof(bitvector_t) * CHAR_BIT)
        flags |= (bitvector_t)1 << bit;
    }

    /* Allow the first character to be a minus sign */
    if (!isdigit(*p) && (*p != '-' || p != flag))
      is_num = FALSE;
  }

  if (is_num)
    flags = tb_parse_long_or_zero(flag);

  return (flags);
}

/** Narrow a parsed flag word to the 32-bit int slot it is stored in (room,
 * mob, object and zone flag arrays are int[4]). asciiflag_conv() yields a
 * 64-bit bitvector_t, so letters past 'F' (bits 32 and up) would be dropped
 * without a word, and bit 31 would make the int negative. The world files
 * are the implementor's; report the dropped bits with the entity. */
static int flag_slot(bitvector_t bits, const char *flag, const char *what,
                     int vnum)
{
  bitvector_t high = bits >> 32;

  if (high) {
    int bit = 32;
    while (!(high & 1)) {
      high >>= 1;
      bit++;
    }
    TB_ERROR_MSG("db", "flag-parse", what,
                 "%s vnum %d: flags '%s' set bit %d, which does not fit the "
                 "32-bit flag slot; high bits dropped", what, vnum, flag, bit);
  }
  return (int)(unsigned int)(bits & 0xFFFFFFFFUL);
}

static bitvector_t asciiflag_conv_aff(char *flag)
{
  bitvector_t flags = 0;
  int is_num = TRUE;
  char *p;

  for (p = flag; *p; p++) {
    if (islower(*p)) {
      unsigned int bit = (unsigned int)(*p - 'a' + 1);
      if (bit < sizeof(bitvector_t) * CHAR_BIT)
        flags |= (bitvector_t)1 << bit;
    } else if (isupper(*p)) {
      unsigned int bit = (unsigned int)(26 + (*p - 'A' + 1));
      if (bit < sizeof(bitvector_t) * CHAR_BIT)
        flags |= (bitvector_t)1 << bit;
    }

    /* Allow the first character to be a minus sign */
    if (!isdigit(*p) && (*p != '-' || p != flag))
      is_num = FALSE;
  }

  if (is_num)
    flags = tb_parse_long_or_zero(flag);

  return (flags);
}

/* Fix for crashes in the editor when formatting. E-descs are assumed to
  * end with a \r\n. -Welcor */
void ensure_newline_terminated(struct extra_descr_data* new_descr) {
  char *with_term, *end;

  if (new_descr->description == NULL) {
    return;
  }

  end = strchr(new_descr->description, '\0');
  if (end > new_descr->description && *(end - 1) != '\n') {
    CREATE(with_term, char, strlen(new_descr->description) + 3);
    tb_snprintf(with_term, strlen(new_descr->description) + 3,
                "%s\r\n", new_descr->description); /* snprintf ok : size checked above*/
    free(new_descr->description);
    new_descr->description = with_term;
  }
}

/** Return sector, or SECT_INSIDE with a diagnostic when it is outside
 * 0..NUM_ROOM_SECTORS-1. The sector comes from a hand-edited world file
 * (redit only offers valid sectors) and later indexes movement_loss[] and
 * sector_types[], so an out-of-range value must never reach a room. */
static int room_sector_or_inside(int vnum, int sector)
{
  if (VALID_SECTOR(sector))
    return sector;
  TB_ERROR_MSG("db", "room-parse", "sector",
               "room vnum %d: sector %d out of range 0..%d; using SECT_INSIDE",
               vnum, sector, NUM_ROOM_SECTORS - 1);
  return SECT_INSIDE;
}

/** Placeholder for a world string the file left empty; OLC's str_udup()
 * stores the same text for an empty entry. */
#define WORLD_STRING_UNDEFINED "undefined"

/** Return str, or a new WORLD_STRING_UNDEFINED with a diagnostic when it is
 * NULL. fread_string() returns NULL for an empty ("~") string, but room
 * names and descriptions, mob alias lists, mob and object short
 * descriptions, object long descriptions and extra descriptions are printed
 * and dereferenced without a NULL check ('look' crashed on an object with
 * no long description). OLC
 * never saves one empty, so a NULL comes from a hand-edited world file;
 * record and vnum name it in the report. */
static char *world_string(char *str, const char *record, int vnum,
                          const char *field)
{
  if (str)
    return str;
  TB_ERROR_MSG("db", "world-string-missing", record,
               "%s vnum %d: empty %s; using \"" WORLD_STRING_UNDEFINED "\"",
               record, vnum, field);
  CREATE(str, char, sizeof(WORLD_STRING_UNDEFINED));
  strlcpy(str, WORLD_STRING_UNDEFINED, sizeof(WORLD_STRING_UNDEFINED));
  return str;
}

/** Move parse_room()'s zone cursor *zone forward to the zone whose
 * bot..top holds room vnum, and return it. Rooms (the .wld files) must
 * ascend through the zones (the .zon headers), both hand-editable; redit
 * and zedit keep them consistent. A room below the cursor's zone, between
 * two zones or above the last zone has no zone that holds it: attached to a
 * neighbour, the zone's reset and redit's save (both walk bot..top) would
 * skip it, so each case is fatal. */
static int room_zone(int vnum, int *zone)
{
  if (vnum < zone_table[*zone].bot) {
    BOOT_FATAL("room-parse", "zone", "room vnum %d is below zone %d (%d-%d)",
               vnum, zone_table[*zone].number, zone_table[*zone].bot,
               zone_table[*zone].top);
  }
  while (vnum > zone_table[*zone].top)
    if (*zone == top_of_zone_table) {
      BOOT_FATAL("room-parse", "zone",
                 "room vnum %d is above the last zone %d (%d-%d)", vnum,
                 zone_table[*zone].number, zone_table[*zone].bot,
                 zone_table[*zone].top);
    } else
      ++*zone;
  /* The loop stops at the first zone that does not end below the room;
   * the room is in a gap if it also starts above it. */
  if (vnum < zone_table[*zone].bot) {
    BOOT_FATAL("room-parse", "zone",
               "room vnum %d lies between zone %d (%d-%d) and zone %d (%d-%d)",
               vnum, zone_table[*zone - 1].number, zone_table[*zone - 1].bot,
               zone_table[*zone - 1].top, zone_table[*zone].number,
               zone_table[*zone].bot, zone_table[*zone].top);
  }
  return *zone;
}

/** Parse room #virtual_nr into the next world[] slot.
 *
 * Boot-only, one sequential pass: the room cursor and the zone cursor are
 * statics that only move forward, so rooms must ascend through zones that
 * were loaded first (a room no zone holds is fatal: room_zone()). Capacity is
 * checked by discrete_load(). Calling this after boot (from OLC) would write
 * past the table. */
void parse_room(FILE *fl, int virtual_nr)
{
  static int room_nr = 0, zone = 0;
  int t[10], i, retval;
  char line[READ_SIZE], flags[128], flags2[128], flags3[128];
  char flags4[128], buf2[MAX_STRING_LENGTH], buf[128];
  char room_num_text[32], sector_text[32];
  struct extra_descr_data *new_descr;
  char letter;

  /* This really had better fit or there are other problems. */
  tb_snprintf(buf2, sizeof(buf2), "room #%d", virtual_nr);

  world[room_nr].zone = room_zone(virtual_nr, &zone);
  world[room_nr].number = virtual_nr;
  world[room_nr].name =
    world_string(fread_string(fl, buf2), "room", virtual_nr, "name");
  world[room_nr].description =
    world_string(fread_string(fl, buf2), "room", virtual_nr, "description");

  if (!get_line(fl, line)) {
    log("SYSERR: Expecting roomflags/sector type of room #%d but file ended!",
	virtual_nr);
    exit(1);
  }

  retval = sscanf(line, " %31s %127s %127s %127s %127s %31s ",
                  room_num_text, flags, flags2, flags3, flags4, sector_text);
  if (retval >= 1 && !parse_int_text_strict(room_num_text, &t[0]))
    retval = -1;
  if (retval == 3 && !parse_int_text_strict(flags2, &t[2]))
    retval = -1;
  if (retval == 6 && !parse_int_text_strict(sector_text, &t[2]))
    retval = -1;

  if ((retval == 3) && (bitwarning == TRUE)) {
    log("WARNING: Conventional world files detected. See config.c.");
    exit(1);
  } else if ((retval == 3) && (bitwarning == FALSE)) {
    /* Looks like the implementor is ready, so let's load the world files. We
     * load the extra three flags as 0, since they won't be anything anyway. We
     * will save the entire world later on, when every room, mobile, and object
     * is converted. */
    log("Converting room #%d to 128bits..", virtual_nr);
    world[room_nr].room_flags[0] = flag_slot(asciiflag_conv(flags), flags, "room", virtual_nr);
    world[room_nr].room_flags[1] = 0;
    world[room_nr].room_flags[2] = 0;
    world[room_nr].room_flags[3] = 0;

    /* In the old-style files, the 3rd item was the sector-type */
    world[room_nr].sector_type = room_sector_or_inside(virtual_nr, t[2]);

   tb_snprintf(flags, sizeof(flags), "room #%d", virtual_nr);	/* sprintf: OK (until 399-bit integers) */

    /* No need to scan the other three sections; they're 0 anyway. */
    check_bitvector_names((unsigned int)world[room_nr].room_flags[0], room_bits_count, flags, "room");

    if(bitsavetodisk) { /* Maybe the implementor just wants to look at the 128bit files */
      add_to_save_list(zone_table[real_zone_by_thing(virtual_nr)].number, 3);
      converting = TRUE;
    }

  log("   done.");
  } else if (retval == 6) {
    int taeller;

    world[room_nr].room_flags[0] = flag_slot(asciiflag_conv(flags), flags, "room", virtual_nr);
    world[room_nr].room_flags[1] = flag_slot(asciiflag_conv(flags2), flags2, "room", virtual_nr);
    world[room_nr].room_flags[2] = flag_slot(asciiflag_conv(flags3), flags3, "room", virtual_nr);
    world[room_nr].room_flags[3] = flag_slot(asciiflag_conv(flags4), flags4, "room", virtual_nr);

    tb_snprintf(flags, sizeof(flags), "room #%d", virtual_nr);	/* sprintf: OK (until 399-bit integers) */
    for(taeller=0; taeller < AF_ARRAY_MAX; taeller++)
      check_bitvector_names((unsigned int)world[room_nr].room_flags[taeller], room_bits_count, flags, "room");

    world[room_nr].sector_type = room_sector_or_inside(virtual_nr, t[2]);
    } else {
      log("SYSERR: Format error in roomflags/sector type of room #%d", virtual_nr);
    exit(1);
  }

  world[room_nr].func = NULL;
  world[room_nr].contents = NULL;
  world[room_nr].people = NULL;
  world[room_nr].light = 0;	/* Zero light sources */

  for (i = 0; i < NUM_OF_DIRS; i++) /* NUM_OF_DIRS here, not DIR_COUNT */
    world[room_nr].dir_option[i] = NULL;

  world[room_nr].ex_description = NULL;

  tb_snprintf(buf, sizeof(buf), "SYSERR: Format error in room #%d (expecting D/E/S)", virtual_nr);

  for (;;) {
    if (!get_line(fl, line)) {
      log("%s", buf);
      exit(1);
    }
    switch (*line) {
    case 'D': {
      int dir;

      /* The direction indexes dir_option[]; setup_dir() range-checks it. */
      if (!parse_int_text_strict(line + 1, &dir)) {
        BOOT_FATAL("room-parse", "setup_dir",
                   "room vnum %d: malformed exit direction line", virtual_nr);
      }
      setup_dir(fl, room_nr, dir);
      break;
    }
    case 'E':
      CREATE(new_descr, struct extra_descr_data, 1);
      new_descr->keyword = world_string(fread_string(fl, buf2), "room",
                                        virtual_nr, "extra description keyword");
      new_descr->description = world_string(fread_string(fl, buf2), "room",
                                            virtual_nr, "extra description");
      ensure_newline_terminated(new_descr);

      new_descr->next = world[room_nr].ex_description;
      world[room_nr].ex_description = new_descr;
      break;
    case 'S':			/* end of room */
      /* DG triggers -- script is defined after the end of the room */
      letter = fread_letter(fl);
      if (ungetc(letter, fl) == EOF) {
        log("SYSERR: parse_room: unable to push back trigger marker");
        exit(1);
      }
      while (letter=='T') {
        dg_read_trigger(fl, &world[room_nr], WLD_TRIGGER);
        letter = fread_letter(fl);
        if (ungetc(letter, fl) == EOF) {
          log("SYSERR: parse_room: unable to push back trigger marker");
          exit(1);
        }
      }
      top_of_world = room_nr++;
      return;
    default:
      log("%s", buf);
      exit(1);
    }
  }
}

/* read direction data */
void setup_dir(FILE *fl, int room, int dir)
{
  int t[5];
  char line[READ_SIZE], buf2[128];

  /* world[room].number, not GET_ROOM_VNUM(): during boot room is above
   * top_of_world (set only at the room's 'S'), so the macro gave NOWHERE. */
  tb_snprintf(buf2, sizeof(buf2), "room #%d, direction D%d", world[room].number, dir);

  /* dir comes from the world file and indexes dir_option[NUM_OF_DIRS]. An
   * index past the end lands on other room_data fields (func, script...)
   * inside the same allocation, which no sanitizer can see. */
  if (!VALID_DIR(dir)) {
    BOOT_FATAL("room-parse", "setup_dir",
               "room vnum %d: exit direction %d out of range 0..%d",
               world[room].number, dir, NUM_OF_DIRS - 1);
  }

  /* With diagonal directions off the exit is dropped, but its two strings
   * and numeric line are still part of the record: consume them, or
   * parse_room() would read the description as the next D/E/S line. */
  if (!CONFIG_DIAGONAL_DIRS && IS_DIAGONAL(dir)) {
    free(fread_string(fl, buf2));
    free(fread_string(fl, buf2));
    if (!get_line(fl, line) || parse_int_fields(line, t, 3) != 3) {
      log("SYSERR: Format error, %s", buf2);
      exit(1);
    }
    TB_WARN_MSG("db", "room-parse", "diagonal-exit",
                "room vnum %d: D%d skipped (diagonal_dirs off)",
                world[room].number, dir);
    return;
  }

  /* A second D record for the same direction replaces the first, as it always
   * did; free the earlier exit instead of leaking it, and say so. */
  if (world[room].dir_option[dir]) {
    TB_ERROR_MSG("db", "room-parse", "setup_dir",
                 "room vnum %d: duplicate exit D%d; the later one replaces the earlier",
                 world[room].number, dir);
    free(world[room].dir_option[dir]->general_description);
    free(world[room].dir_option[dir]->keyword);
    free(world[room].dir_option[dir]);
    world[room].dir_option[dir] = NULL;
  }

  CREATE(world[room].dir_option[dir], struct room_direction_data, 1);
  world[room].dir_option[dir]->general_description = fread_string(fl, buf2);
  world[room].dir_option[dir]->keyword = fread_string(fl, buf2);

  if (!get_line(fl, line)) {
    log("SYSERR: Format error, %s", buf2);
    exit(1);
  }
  if (parse_int_fields(line, t, 3) != 3) {
    log("SYSERR: Format error, %s", buf2);
    exit(1);
  }
  if (t[0] == 1)
    world[room].dir_option[dir]->exit_info = EX_ISDOOR;
  else if (t[0] == 2)
    world[room].dir_option[dir]->exit_info = EX_ISDOOR | EX_PICKPROOF;
  else if (t[0] == 3)
    world[room].dir_option[dir]->exit_info = EX_ISDOOR | EX_HIDDEN;
  else if (t[0] == 4)
    world[room].dir_option[dir]->exit_info = EX_ISDOOR | EX_PICKPROOF | EX_HIDDEN;
  else
    world[room].dir_option[dir]->exit_info = 0;

  world[room].dir_option[dir]->key = ((t[1] == -1 || t[1] == 65535) ? NOTHING : t[1]);
  world[room].dir_option[dir]->to_room = ((t[2] == -1  || t[2] == 0) ? NOWHERE : t[2]);
}

/* make sure the start rooms exist & resolve their vnums to rnums */
static void check_start_rooms(void)
{
  if ((r_mortal_start_room = real_room(CONFIG_MORTAL_START)) == NOWHERE) {
    log("SYSERR:  Mortal start room does not exist.  Change in config.c.");
    exit(1);
  }
  if ((r_immort_start_room = real_room(CONFIG_IMMORTAL_START)) == NOWHERE) {
    if (!mini_mud)
      log("SYSERR:  Warning: Immort start room does not exist.  Change in config.c.");
    r_immort_start_room = r_mortal_start_room;
  }
  if ((r_frozen_start_room = real_room(CONFIG_FROZEN_START)) == NOWHERE) {
    if (!mini_mud)
      log("SYSERR:  Warning: Frozen start room does not exist.  Change in config.c.");
    r_frozen_start_room = r_mortal_start_room;
  }
}

/* resolve all vnums into rnums in the world */
void renum_world(void)
{
  int room, door;

  for (room = 0; room <= top_of_world; room++)
    for (door = 0; door < NUM_OF_DIRS; door++)
      if (world[room].dir_option[door])
	if (world[room].dir_option[door]->to_room != NOWHERE)
	  world[room].dir_option[door]->to_room =
	    real_room(world[room].dir_option[door]->to_room);
}

/** This is not the same ZCMD as used elsewhere. GRUMBLE... namespace conflict
 * @todo refactor this particular ZCMD and remove this redefine. */
#ifdef ZCMD
#undef ZCMD
#endif
#define ZCMD zone_table[zone].cmd[cmd_no]

/* Resolve vnums into rnums in the zone reset tables. In English: Once all of
 * the zone reset tables have been loaded, we resolve the virtual numbers into
 * real numbers all at once so we don't have to do it repeatedly while the game
 * is running.  This does make adding any room, mobile, or object a little more
 * difficult while the game is running. Assumes NOWHERE == NOBODY == NOTHING.
 * Assumes sizeof(room_rnum) >= (sizeof(mob_rnum) and sizeof(obj_rnum)) */
static void renum_zone_table(void)
{
  int cmd_no;
  room_rnum a, b, c;
  int olda, oldb, oldc;	/* the file's vnums, untruncated, for the report */
  zone_rnum zone;

  for (zone = 0; zone <= top_of_zone_table; zone++)
    for (cmd_no = 0; ZCMD.command != 'S'; cmd_no++) {
      a = b = c = 0;
      olda = ZCMD.arg1;
      oldb = ZCMD.arg2;
      oldc = ZCMD.arg3;
      switch (ZCMD.command) {
      case 'M':
	a = ZCMD.arg1 = real_mobile(ZCMD.arg1);
	c = ZCMD.arg3 = real_room(ZCMD.arg3);
	break;
      case 'O':
	a = ZCMD.arg1 = real_object(ZCMD.arg1);
	if (ZCMD.arg3 != NOWHERE)
	  c = ZCMD.arg3 = real_room(ZCMD.arg3);
	break;
      case 'G':
	a = ZCMD.arg1 = real_object(ZCMD.arg1);
	break;
      case 'E':
	a = ZCMD.arg1 = real_object(ZCMD.arg1);
	break;
      case 'P':
	a = ZCMD.arg1 = real_object(ZCMD.arg1);
	c = ZCMD.arg3 = real_object(ZCMD.arg3);
	break;
      case 'D':
	a = ZCMD.arg1 = real_room(ZCMD.arg1);
	break;
      case 'R': /* rem obj from room */
        a = ZCMD.arg1 = real_room(ZCMD.arg1);
	b = ZCMD.arg2 = real_object(ZCMD.arg2);
        break;
      case 'T': /* a trigger */
        b = ZCMD.arg2 = real_trigger(ZCMD.arg2);
        c = ZCMD.arg3 = real_room(ZCMD.arg3);
        break;
      case 'V': /* trigger variable assignment */
        c = ZCMD.arg3 = real_room(ZCMD.arg3);
        break;
      }
      /* A vnum that names nothing loaded disables the command. In mini-mud
       * mode most of the world is absent, so only the count is noisy there
       * and the per-command report is skipped. */
      if (a == NOWHERE || b == NOWHERE || c == NOWHERE) {
	if (!mini_mud)
	  TB_ERROR_MSG("db", "zone-renum", "renum_zone_table",
	               "zone %d line %d cmd %c: arg%d vnum %d not found; cmd disabled",
	               zone_table[zone].number, ZCMD.line, ZCMD.command,
	               a == NOWHERE ? 1 : b == NOWHERE ? 2 : 3,
	               a == NOWHERE ? olda : b == NOWHERE ? oldb : oldc);
	ZCMD.command = '*';
      }
    }
}

/** TRUE if the first word of a short description is "a", "an" or "the"
 * (any case), the test parse_mobile() and parse_object() use to lower-case
 * the article. fname() did this by copying the whole leading alphabetic run
 * into a READ_SIZE static with no bound, and a short description comes from
 * fread_string() (up to MAX_STRING_LENGTH; medit accepts 511-byte lines), so
 * a 300-letter first word overflowed it. This only measures the run. */
static int starts_with_article(const char *text)
{
  size_t n = 0;

  while (isalpha((unsigned char)text[n]))
    n++;
  return (n == 1 && LOWER(text[0]) == 'a') ||
         (n == 2 && !strn_cmp(text, "an", 2)) ||
         (n == 3 && !strn_cmp(text, "the", 3));
}

/** TRUE if v is in lo..hi; otherwise report the mob vnum, field and value.
 * parse_simple_mob() stores its numbers into byte and sh_int fields, where
 * a value outside the field's range would wrap silently. */
static int mob_value_in_range(int nr, const char *field, long v, long lo,
                              long hi)
{
  if (v >= lo && v <= hi)
    return TRUE;
  TB_ERROR_MSG("db", "mob-parse", field,
               "mob vnum %d: %s %ld out of range %ld..%ld", nr, field, v, lo, hi);
  return FALSE;
}

static void parse_simple_mob(FILE *mob_f, int i, int nr)
{
  int j, t[10], bad;
  char line[READ_SIZE];
  char n0[32], n1[32], n2[32], dice1[64], dice2[64];

  mob_proto[i].real_abils.str = 11;
  mob_proto[i].real_abils.intel = 11;
  mob_proto[i].real_abils.wis = 11;
  mob_proto[i].real_abils.dex = 11;
  mob_proto[i].real_abils.con = 11;
  mob_proto[i].real_abils.cha = 11;

  if (!get_line(mob_f, line)) {
    log("SYSERR: Format error in mob #%d, file ended after S flag!", nr);
    exit(1);
  }

  if (sscanf(line, " %31s %31s %31s %63s %63s ",
             n0, n1, n2, dice1, dice2) != 5 ||
      !parse_int_text_strict(n0, &t[0]) ||
      !parse_int_text_strict(n1, &t[1]) ||
      !parse_int_text_strict(n2, &t[2]) ||
      !parse_dice_text(dice1, &t[3], &t[4], &t[5]) ||
      !parse_dice_text(dice2, &t[6], &t[7], &t[8])) {
    log("SYSERR: Format error in mob #%d, first line after S flag\n"
	"...expecting line of form '# # # #d#+# #d#+#'", nr);
    exit(1);
  }

  /* Each number below goes into a field of the type named in its bound:
   * level (unsigned char), hitroll = 20 - THAC0 and damroll (sbyte), armor
   * class * 10 and the hit dice (sh_int), damage dice (byte); read_mobile()
   * then rolls the hit dice into the sh_int max_hit. medit writes only
   * values that fit, so one that does not comes from a hand-edited .mob
   * file and would wrap silently (armor class * 10 could even overflow the
   * int). The stock world fits every range, so as for a bad position this
   * is fatal; every bad field of the mob is reported first. */
  bad = !mob_value_in_range(nr, "level", t[0], 0, UCHAR_MAX);
  bad += !mob_value_in_range(nr, "THAC0", t[1], 20 - SCHAR_MAX, 20 - SCHAR_MIN);
  bad += !mob_value_in_range(nr, "armor class", t[2], SHRT_MIN / 10, SHRT_MAX / 10);
  bad += !mob_value_in_range(nr, "hit dice count", t[3], 0, SHRT_MAX);
  bad += !mob_value_in_range(nr, "hit dice size", t[4], 0, SHRT_MAX);
  bad += !mob_value_in_range(nr, "hit point bonus", t[5], SHRT_MIN, SHRT_MAX);
  bad += !mob_value_in_range(nr, "damage dice count", t[6], 0, SCHAR_MAX);
  bad += !mob_value_in_range(nr, "damage dice size", t[7], 0, SCHAR_MAX);
  bad += !mob_value_in_range(nr, "damroll", t[8], SCHAR_MIN, SCHAR_MAX);
  if (!bad)
    bad = !mob_value_in_range(nr, "maximum hit points",
                              (long)t[3] * t[4] + t[5], SHRT_MIN, SHRT_MAX);
  if (bad) {
    BOOT_FATAL("mob-parse", "simple mob",
               "mob vnum %d: %d value(s) on the line after S out of range",
               nr, bad);
  }

  GET_LEVEL(mob_proto + i) = t[0];
  GET_HITROLL(mob_proto + i) = 20 - t[1];
  GET_AC(mob_proto + i) = 10 * t[2];

  /* max hit = 0 is a flag that H, M, V is xdy+z */
  GET_MAX_HIT(mob_proto + i) = 0;
  GET_HIT(mob_proto + i) = t[3];
  GET_MANA(mob_proto + i) = t[4];
  GET_MOVE(mob_proto + i) = t[5];

  GET_MAX_MANA(mob_proto + i) = 10;
  GET_MAX_MOVE(mob_proto + i) = 50;

  mob_proto[i].mob_specials.damnodice = t[6];
  mob_proto[i].mob_specials.damsizedice = t[7];
  GET_DAMROLL(mob_proto + i) = t[8];

  if (!get_line(mob_f, line)) {
      log("SYSERR: Format error in mob #%d, second line after S flag\n"
	  "...expecting line of form '# #', but file ended!", nr);
      exit(1);
    }

  if (parse_int_fields(line, t, 2) != 2) {
    log("SYSERR: Format error in mob #%d, second line after S flag\n"
	"...expecting line of form '# #'", nr);
    exit(1);
  }

  GET_GOLD(mob_proto + i) = t[0];
  GET_EXP(mob_proto + i) = t[1];

  if (!get_line(mob_f, line)) {
    log("SYSERR: Format error in last line of mob #%d\n"
	"...expecting line of form '# # #', but file ended!", nr);
    exit(1);
  }

  if (parse_int_fields(line, t, 3) != 3) {
    log("SYSERR: Format error in last line of mob #%d\n"
	"...expecting line of form '# # #'", nr);
    exit(1);
  }

  /* Position, default position and sex are stored in byte fields and index
   * position_types[], positions[] and the gender tables. medit only offers
   * valid values, so these come from a hand-edited .mob file; a value that
   * does not fit would silently truncate and index past those tables. */
  if (!VALID_POS(t[0]) || !VALID_POS(t[1]) || !VALID_SEX(t[2])) {
    BOOT_FATAL("mob-parse", "position",
               "mob vnum %d: position %d default %d sex %d out of range "
               "(positions 0..%d, sex 0..%d)", nr, t[0], t[1], t[2],
               NUM_POSITIONS - 1, NUM_GENDERS - 1);
  }

  GET_POS(mob_proto + i) = t[0];
  GET_DEFAULT_POS(mob_proto + i) = t[1];
  GET_SEX(mob_proto + i) = t[2];

  GET_CLASS(mob_proto + i) = 0;
  GET_WEIGHT(mob_proto + i) = 200;
  GET_HEIGHT(mob_proto + i) = 198;

  /* These are now save applies; base save numbers for MOBs are now from the
   * warrior save table. */
  for (j = 0; j < NUM_OF_SAVING_THROWS; j++)
    GET_SAVE(mob_proto + i, j) = 0;
}

/* interpret_espec is the function that takes espec keywords and values and
 * assigns the correct value to the mob as appropriate.  Adding new e-specs is
 * absurdly easy -- just add a new CASE statement to this function! No other
 * changes need to be made anywhere in the code.
 * CASE		: Requires a parameter through 'value'. */
#define CASE(test)	\
	if (value && !matched && !str_cmp(keyword, test) && (matched = TRUE))
#define RANGE(low, high)	\
	espec_range(&num_arg, (low), (high), keyword, value, nr)

/** Clamp an E-spec value to [low, high], reporting any change. The value
 * comes from a hand-edited .mob file (medit writes in-range values). */
static void espec_range(int *num_arg, int low, int high, const char *keyword,
                        const char *value, int nr)
{
  if (*num_arg >= low && *num_arg <= high)
    return;
  TB_WARN_MSG("db", "mob-espec", keyword,
              "mob vnum %d %s value '%s' clamped to [%d,%d]",
              nr, keyword, value, low, high);
  *num_arg = MAX(low, MIN(high, *num_arg));
}

static void interpret_espec(const char *keyword, const char *value, int i, int nr)
{
  int num_arg = 0, matched = FALSE;

  /* If there isn't a colon, there is no value.  While Boolean options are
   * possible, we don't actually have any.  Feel free to make some. */
  if (value && !parse_int_text_strict(value, &num_arg)) {
    TB_WARN_MSG("db", "mob-espec", keyword,
                "mob vnum %d %s value '%s' is not a number; using 0",
                nr, keyword, value);
    num_arg = 0;
  }

  CASE("BareHandAttack") {
    RANGE(0, NUM_ATTACK_TYPES - 1);
    mob_proto[i].mob_specials.attack_type = num_arg;
  }

  CASE("Str") {
    RANGE(3, 25);
    mob_proto[i].real_abils.str = num_arg;
  }

  CASE("StrAdd") {
    RANGE(0, 100);
    mob_proto[i].real_abils.str_add = num_arg;
  }

  CASE("Int") {
    RANGE(3, 25);
    mob_proto[i].real_abils.intel = num_arg;
  }

  CASE("Wis") {
    RANGE(3, 25);
    mob_proto[i].real_abils.wis = num_arg;
  }

  CASE("Dex") {
    RANGE(3, 25);
    mob_proto[i].real_abils.dex = num_arg;
  }

  CASE("Con") {
    RANGE(3, 25);
    mob_proto[i].real_abils.con = num_arg;
  }

  CASE("Cha") {
    RANGE(3, 25);
    mob_proto[i].real_abils.cha = num_arg;
  }

  CASE("SavingPara") {
    RANGE(0, 100);
    mob_proto[i].char_specials.saved.apply_saving_throw[SAVING_PARA] = num_arg;
  }

  CASE("SavingRod") {
    RANGE(0, 100);
    mob_proto[i].char_specials.saved.apply_saving_throw[SAVING_ROD] = num_arg;
  }

  CASE("SavingPetri") {
    RANGE(0, 100);
    mob_proto[i].char_specials.saved.apply_saving_throw[SAVING_PETRI] = num_arg;
  }

  CASE("SavingBreath") {
    RANGE(0, 100);
    mob_proto[i].char_specials.saved.apply_saving_throw[SAVING_BREATH] = num_arg;
  }

  CASE("SavingSpell") {
    RANGE(0, 100);
    mob_proto[i].char_specials.saved.apply_saving_throw[SAVING_SPELL] = num_arg;
  }

  if (!matched)
    TB_WARN_MSG("db", "mob-espec", keyword,
                "mob vnum %d: unrecognized espec keyword '%s'%s", nr, keyword,
                value ? "" : " (or a known keyword with no ': value')");
}

#undef CASE
#undef BOOL_CASE
#undef RANGE

static void parse_espec(char *buf, int i, int nr)
{
  char *ptr;

  if ((ptr = strchr(buf, ':')) != NULL) {
    *(ptr++) = '\0';
    while (isspace(*ptr))
      ptr++;
  }
  interpret_espec(buf, ptr, i, nr);
}

static void parse_enhanced_mob(FILE *mob_f, int i, int nr)
{
  char line[READ_SIZE];

  parse_simple_mob(mob_f, i, nr);

  while (get_line(mob_f, line)) {
    if (!strcmp(line, "E"))	/* end of the enhanced section */
      return;
    else if (*line == '#') {	/* we've hit the next mob, maybe? */
      log("SYSERR: Unterminated E section in mob #%d", nr);
      exit(1);
    } else
      parse_espec(line, i, nr);
  }

  log("SYSERR: Unexpected end of file reached after mob #%d", nr);
  exit(1);
}

/** Parse mobile #nr into the next mob_proto[]/mob_index[] slot. Boot-only,
 * like parse_room(): the static cursor only moves forward and
 * discrete_load() checks it against the counted capacity. */
void parse_mobile(FILE *mob_f, int nr)
{
  static int i = 0;
  int j, t[10], retval;
  char line[READ_SIZE], *tmpptr, letter;
  char f1[128], f2[128], f3[128], f4[128], f5[128], f6[128], f7[128], f8[128], buf2[128];
  char alignment_text[32];

  mob_index[i].vnum = nr;
  mob_index[i].number = 0;
  mob_index[i].func = NULL;

  clear_char(mob_proto + i);

  /* Mobiles should NEVER use anything in the 'player_specials' structure.
   * The only reason we have every mob in the game share this copy of the
   * structure is to save newbie coders from themselves. -gg */
  mob_proto[i].player_specials = &dummy_mob;
  tb_snprintf(buf2, sizeof(buf2), "mob vnum %d", nr);	/* sprintf: OK (for 'buf2 >= 19') */

  /* String data */
  /* The alias list is printed with %s by medit, DG name lookups and the pet
   * shop, not only matched by isname(). */
  mob_proto[i].player.name =
    world_string(fread_string(mob_f, buf2), "mob", nr, "alias list");
  /* The short description is the mob's name in act() and the room list. */
  tmpptr = mob_proto[i].player.short_descr =
    world_string(fread_string(mob_f, buf2), "mob", nr, "short description");
  if (tmpptr && *tmpptr)
    if (starts_with_article(tmpptr))
      *tmpptr = LOWER(*tmpptr);
  mob_proto[i].player.long_descr = fread_string(mob_f, buf2);
  mob_proto[i].player.description = fread_string(mob_f, buf2);
  GET_TITLE(mob_proto + i) = NULL;

  /* Numeric data */
  if (!get_line(mob_f, line)) {
    log("SYSERR: Format error after string section of mob #%d\n"
	"...expecting line of form '# # # {S | E}', but file ended!", nr);
    exit(1);
  }

  retval = sscanf(line, "%127s %127s %127s %127s %127s %127s %127s %127s %31s %c",
                  f1, f2, f3, f4, f5, f6, f7, f8, alignment_text, &letter);
  if (retval == 10 && !parse_int_text_strict(alignment_text, &t[2]))
    retval = -1;

  if ((retval != 10) && (bitwarning == TRUE)) {
    /* Let's make the implementor read some, before converting his world files. */
    log("WARNING: Conventional mobile files detected. See config.c.");
    exit(1);
  } else if ((retval == 4) && (bitwarning == FALSE)) {
    log("Converting mobile #%d to 128bits..", nr);
    MOB_FLAGS(mob_proto + i)[0] = flag_slot(asciiflag_conv(f1), f1, "mob", nr);
    MOB_FLAGS(mob_proto + i)[1] = 0;
    MOB_FLAGS(mob_proto + i)[2] = 0;
    MOB_FLAGS(mob_proto + i)[3] = 0;
    check_bitvector_names((unsigned int)MOB_FLAGS(mob_proto + i)[0], action_bits_count, buf2, "mobile");

    AFF_FLAGS(mob_proto + i)[0] = flag_slot(asciiflag_conv_aff(f2), f2, "mob", nr);
    AFF_FLAGS(mob_proto + i)[1] = 0;
    AFF_FLAGS(mob_proto + i)[2] = 0;
    AFF_FLAGS(mob_proto + i)[3] = 0;

    GET_ALIGNMENT(mob_proto + i) = tb_parse_int_or_zero(f3);

    /* Make some basic checks. */
    REMOVE_BIT_AR(AFF_FLAGS(mob_proto + i), AFF_CHARM);
    REMOVE_BIT_AR(AFF_FLAGS(mob_proto + i), AFF_POISON);
    REMOVE_BIT_AR(AFF_FLAGS(mob_proto + i), AFF_SLEEP);
    if (MOB_FLAGGED(mob_proto + i, MOB_AGGRESSIVE) && MOB_FLAGGED(mob_proto + i, MOB_AGGR_GOOD))
      REMOVE_BIT_AR(MOB_FLAGS(mob_proto + i), MOB_AGGR_GOOD);
    if (MOB_FLAGGED(mob_proto + i, MOB_AGGRESSIVE) && MOB_FLAGGED(mob_proto + i, MOB_AGGR_NEUTRAL))
      REMOVE_BIT_AR(MOB_FLAGS(mob_proto + i), MOB_AGGR_NEUTRAL);
    if (MOB_FLAGGED(mob_proto + i, MOB_AGGRESSIVE) && MOB_FLAGGED(mob_proto + i, MOB_AGGR_EVIL))
      REMOVE_BIT_AR(MOB_FLAGS(mob_proto + i), MOB_AGGR_EVIL);

    check_bitvector_names((unsigned int)AFF_FLAGS(mob_proto + i)[0], affected_bits_count, buf2, "mobile affect");

    /* This is necessary, since if we have conventional world files, &letter is
     * loaded into f4 instead of the letter characters. So what we do, is copy
     * f4 into letter. Disadvantage is that &letter cannot be longer then 128
     * characters, but this shouldn't occur anyway. */
    letter = *f4;

    if(bitsavetodisk) {
      add_to_save_list(zone_table[real_zone_by_thing(nr)].number, 0);
      converting =TRUE;
    }

  log("   done.");
  } else if (retval == 10) {
    int taeller;

    MOB_FLAGS(mob_proto + i)[0] = flag_slot(asciiflag_conv(f1), f1, "mob", nr);
    MOB_FLAGS(mob_proto + i)[1] = flag_slot(asciiflag_conv(f2), f2, "mob", nr);
    MOB_FLAGS(mob_proto + i)[2] = flag_slot(asciiflag_conv(f3), f3, "mob", nr);
    MOB_FLAGS(mob_proto + i)[3] = flag_slot(asciiflag_conv(f4), f4, "mob", nr);
    for(taeller=0; taeller < AF_ARRAY_MAX; taeller++)
      check_bitvector_names((unsigned int)MOB_FLAGS(mob_proto + i)[taeller], action_bits_count, buf2, "mobile");

    AFF_FLAGS(mob_proto + i)[0] = flag_slot(asciiflag_conv(f5), f5, "mob", nr);
    AFF_FLAGS(mob_proto + i)[1] = flag_slot(asciiflag_conv(f6), f6, "mob", nr);
    AFF_FLAGS(mob_proto + i)[2] = flag_slot(asciiflag_conv(f7), f7, "mob", nr);
    AFF_FLAGS(mob_proto + i)[3] = flag_slot(asciiflag_conv(f8), f8, "mob", nr);

    GET_ALIGNMENT(mob_proto + i) = t[2];

    for(taeller=0; taeller < AF_ARRAY_MAX; taeller++)
      check_bitvector_names((unsigned int)AFF_FLAGS(mob_proto + i)[taeller], affected_bits_count, buf2, "mobile affect");
  } else {
    log("SYSERR: Format error after string section of mob #%d\n ...expecting line of form '# # # {S | E}'", nr);
    exit(1);
  }

  SET_BIT_AR(MOB_FLAGS(mob_proto + i), MOB_ISNPC);
  if (MOB_FLAGGED(mob_proto + i, MOB_NOTDEADYET)) {
    /* Rather bad to load mobiles with this bit already set. */
    log("SYSERR: Mob #%d has reserved bit MOB_NOTDEADYET set.", nr);
    REMOVE_BIT_AR(MOB_FLAGS(mob_proto + i), MOB_NOTDEADYET);
  }

  switch (UPPER(letter)) {
  case 'S':	/* Simple monsters */
    parse_simple_mob(mob_f, i, nr);
    break;
  case 'E':	/* Circle3 Enhanced monsters */
    parse_enhanced_mob(mob_f, i, nr);
    break;
  /* add new mob types here.. */
  default:
    log("SYSERR: Unsupported mob type '%c' in mob #%d", letter, nr);
    exit(1);
  }

  /* DG triggers -- script info follows mob S/E section */
  letter = fread_letter(mob_f);
  if (ungetc(letter, mob_f) == EOF) {
    log("SYSERR: parse_mobile: unable to push back trigger marker");
    exit(1);
  }
  while (letter=='T') {
    dg_read_trigger(mob_f, &mob_proto[i], MOB_TRIGGER);
    letter = fread_letter(mob_f);
    if (ungetc(letter, mob_f) == EOF) {
      log("SYSERR: parse_mobile: unable to push back trigger marker");
      exit(1);
    }
  }

  mob_proto[i].aff_abils = mob_proto[i].real_abils;

  for (j = 0; j < NUM_WEARS; j++)
    mob_proto[i].equipment[j] = NULL;

  mob_proto[i].nr = i;
  mob_proto[i].desc = NULL;

  top_of_mobt = i++;
}

/** Parse object #nr into the next obj_proto[]/obj_index[] slot.
 *
 * Boot-only: the slot cursor is a static that only moves forward, and
 * discrete_load() has already checked it against index_boot()'s capacity.
 * Objects have no end marker, so the line that ended this record ("#..."
 * or "$") is returned in a static buffer; discrete_load() copies it at once
 * because the next call overwrites it. */
char *parse_object(FILE *obj_f, int nr)
{
  static int i = 0;
  static char line[READ_SIZE];
  int t[10], j, retval;
  char *tmpptr, buf2[128], f1[READ_SIZE], f2[READ_SIZE], f3[READ_SIZE], f4[READ_SIZE];
  char f5[READ_SIZE], f6[READ_SIZE], f7[READ_SIZE], f8[READ_SIZE];
  char f9[READ_SIZE], f10[READ_SIZE], f11[READ_SIZE], f12[READ_SIZE];
  char obj_type_text[32];
  struct extra_descr_data *new_descr;

  obj_index[i].vnum = nr;
  obj_index[i].number = 0;
  obj_index[i].func = NULL;

  clear_object(obj_proto + i);
  obj_proto[i].item_number = i;

  tb_snprintf(buf2, sizeof(buf2), "object #%d", nr);	/* sprintf: OK (for 'buf2 >= 19') */

  /* string data */
  if ((obj_proto[i].name = fread_string(obj_f, buf2)) == NULL) {
    log("SYSERR: Null obj name or format error at or near %s", buf2);
    exit(1);
  }
  tmpptr = obj_proto[i].short_description =
    world_string(fread_string(obj_f, buf2), "obj", nr, "short description");
  if (tmpptr && *tmpptr)
    if (starts_with_article(tmpptr))
      *tmpptr = LOWER(*tmpptr);

  tmpptr = obj_proto[i].description =
    world_string(fread_string(obj_f, buf2), "obj", nr, "long description");
  if (tmpptr && *tmpptr)
    CAP(tmpptr);
  obj_proto[i].action_description = fread_string(obj_f, buf2);

  /* numeric data */
  if (!get_line(obj_f, line)) {
    log("SYSERR: Expecting first numeric line of %s, but file ended!", buf2);
    exit(1);
  }

  retval = sscanf(line, " %31s %255s %255s %255s %255s %255s %255s %255s %255s %255s %255s %255s %255s",
                  obj_type_text, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12);
  if (retval >= 1 && !parse_int_text_strict(obj_type_text, &t[0]))
    retval = -1;

  if ((retval == 4) && (bitwarning == TRUE)) {
    /* Let's make the implementor read some, before converting his world files. */
    log("WARNING: Conventional object files detected. Please see config.c.");
    exit(1);
  } else if (((retval == 4) || (retval == 3)) && (bitwarning == FALSE)) {

    if (retval == 3)
      t[3] = 0;
    else if (retval == 4)
      t[3] = flag_slot(asciiflag_conv_aff(f3), f3, "obj", nr);

    log("Converting object #%d to 128bits..", nr);
    GET_OBJ_EXTRA(obj_proto + i)[0] = flag_slot(asciiflag_conv(f1), f1, "obj", nr);
    GET_OBJ_EXTRA(obj_proto + i)[1] = 0;
    GET_OBJ_EXTRA(obj_proto + i)[2] = 0;
    GET_OBJ_EXTRA(obj_proto + i)[3] = 0;
    GET_OBJ_WEAR(obj_proto + i)[0] = flag_slot(asciiflag_conv(f2), f2, "obj", nr);
    GET_OBJ_WEAR(obj_proto + i)[1] = 0;
    GET_OBJ_WEAR(obj_proto + i)[2] = 0;
    GET_OBJ_WEAR(obj_proto + i)[3] = 0;
    GET_OBJ_AFFECT(obj_proto + i)[0] = flag_slot(asciiflag_conv_aff(f3), f3, "obj", nr);
    GET_OBJ_AFFECT(obj_proto + i)[1] = 0;
    GET_OBJ_AFFECT(obj_proto + i)[2] = 0;
    GET_OBJ_AFFECT(obj_proto + i)[3] = 0;

    if(bitsavetodisk) {
      add_to_save_list(zone_table[real_zone_by_thing(nr)].number, 1);
      converting = TRUE;
    }

    log("   done.");
  } else if (retval == 13) {

    GET_OBJ_EXTRA(obj_proto + i)[0] = flag_slot(asciiflag_conv(f1), f1, "obj", nr);
    GET_OBJ_EXTRA(obj_proto + i)[1] = flag_slot(asciiflag_conv(f2), f2, "obj", nr);
    GET_OBJ_EXTRA(obj_proto + i)[2] = flag_slot(asciiflag_conv(f3), f3, "obj", nr);
    GET_OBJ_EXTRA(obj_proto + i)[3] = flag_slot(asciiflag_conv(f4), f4, "obj", nr);
    GET_OBJ_WEAR(obj_proto + i)[0] = flag_slot(asciiflag_conv(f5), f5, "obj", nr);
    GET_OBJ_WEAR(obj_proto + i)[1] = flag_slot(asciiflag_conv(f6), f6, "obj", nr);
    GET_OBJ_WEAR(obj_proto + i)[2] = flag_slot(asciiflag_conv(f7), f7, "obj", nr);
    GET_OBJ_WEAR(obj_proto + i)[3] = flag_slot(asciiflag_conv(f8), f8, "obj", nr);
    GET_OBJ_AFFECT(obj_proto + i)[0] = flag_slot(asciiflag_conv(f9), f9, "obj", nr);
    GET_OBJ_AFFECT(obj_proto + i)[1] = flag_slot(asciiflag_conv(f10), f10, "obj", nr);
    GET_OBJ_AFFECT(obj_proto + i)[2] = flag_slot(asciiflag_conv(f11), f11, "obj", nr);
    GET_OBJ_AFFECT(obj_proto + i)[3] = flag_slot(asciiflag_conv(f12), f12, "obj", nr);

  } else {
    log("SYSERR: Format error in first numeric line (expecting 13 args, got %d), %s", retval, buf2);
    exit(1);
  }

  /* Object flags checked in check_object(). */
  GET_OBJ_TYPE(obj_proto + i) = t[0];

  if (!get_line(obj_f, line)) {
    log("SYSERR: Expecting second numeric line of %s, but file ended!", buf2);
    exit(1);
  }
  if ((retval = parse_int_fields(line, t, 4)) != 4) {
    log("SYSERR: Format error in second numeric line (expecting 4 args, got %d), %s", retval, buf2);
    exit(1);
  }
  GET_OBJ_VAL(obj_proto + i, 0) = t[0];
  GET_OBJ_VAL(obj_proto + i, 1) = t[1];
  GET_OBJ_VAL(obj_proto + i, 2) = t[2];
  GET_OBJ_VAL(obj_proto + i, 3) = t[3];

  if (!get_line(obj_f, line)) {
    log("SYSERR: Expecting third numeric line of %s, but file ended!", buf2);
    exit(1);
  }
  if ((retval = parse_int_fields(line, t, 5)) != 5) {
    if (retval == 3) {
      t[3] = 0;
      t[4] = 0;
    } else if (retval == 4)
      t[4] = 0;
    else {
      log("SYSERR: Format error in third numeric line (expecting 5 args, got %d), %s", retval, buf2);
      exit(1);
    }
  }

  GET_OBJ_WEIGHT(obj_proto + i) = t[0];
  GET_OBJ_COST(obj_proto + i) = t[1];
  GET_OBJ_RENT(obj_proto + i) = t[2];
  GET_OBJ_LEVEL(obj_proto + i) = t[3];
  GET_OBJ_TIMER(obj_proto + i) = t[4];

  obj_proto[i].sitting_here = NULL;

  /* check to make sure that weight of containers exceeds curr. quantity */
  if (GET_OBJ_TYPE(obj_proto + i) == ITEM_DRINKCON ||
      GET_OBJ_TYPE(obj_proto + i) == ITEM_FOUNTAIN) {
    if (GET_OBJ_WEIGHT(obj_proto + i) < GET_OBJ_VAL(obj_proto + i, 1) && CAN_WEAR(obj_proto + i, ITEM_WEAR_TAKE))
      GET_OBJ_WEIGHT(obj_proto + i) = GET_OBJ_VAL(obj_proto + i, 1) + 5;
  }

  /* extra descriptions and affect fields */
  for (j = 0; j < MAX_OBJ_AFFECT; j++) {
    obj_proto[i].affected[j].location = APPLY_NONE;
    obj_proto[i].affected[j].modifier = 0;
  }

  strlcat(buf2, ", after numeric constants\n"
          "...expecting 'E', 'A', '$', or next object number", sizeof(buf2));
  j = 0;

  for (;;) {
    if (!get_line(obj_f, line)) {
      log("SYSERR: Format error in %s", buf2);
      exit(1);
    }
    switch (*line) {
    case 'E':
      CREATE(new_descr, struct extra_descr_data, 1);
      new_descr->keyword = world_string(fread_string(obj_f, buf2), "obj", nr,
                                        "extra description keyword");
      new_descr->description = world_string(fread_string(obj_f, buf2), "obj",
                                            nr, "extra description");
      new_descr->next = obj_proto[i].ex_description;
      obj_proto[i].ex_description = new_descr;
      break;
    case 'A':
      if (j >= MAX_OBJ_AFFECT) {
	log("SYSERR: Too many A fields (%d max), %s", MAX_OBJ_AFFECT, buf2);
	exit(1);
      }
      if (!get_line(obj_f, line)) {
	log("SYSERR: Format error in 'A' field, %s\n"
	    "...expecting 2 numeric constants but file ended!", buf2);
	exit(1);
      }

      if ((retval = parse_int_fields(line, t, 2)) != 2) {
	log("SYSERR: Format error in 'A' field, %s\n"
	    "...expecting 2 numeric arguments, got %d\n"
	    "...offending line: '%s'", buf2, retval, line);
	exit(1);
      }
      obj_proto[i].affected[j].location = t[0];
      obj_proto[i].affected[j].modifier = t[1];
      j++;
      break;
    case 'T':  /* DG triggers */
      dg_obj_trigger(line, &obj_proto[i]);
      break;
    case '$':
    case '#':
      top_of_objt = i;
      /* check_object() reports each problem itself and repairs the ones
       * that would index a table out of range; the rest (a missing drink
       * keyword, an odd weight) are legal in stock worlds, so a TRUE
       * result is not fatal. */
      (void)check_object(obj_proto + i);
      i++;
      return (line);
    default:
      log("SYSERR: Format error in (%c): %s", *line, buf2);
      exit(1);
    }
  }
}

/** Which of a zone command's arguments hold a vnum, as a bit mask (1 = arg1,
 * 2 = arg2, 4 = arg3). renum_zone_table() passes these to real_room(),
 * real_mobile(), real_object() and real_trigger(), which take IDXTYPE
 * (ush_int) vnums, so each must be checked before it is narrowed. */
static int zone_cmd_vnum_args(char command)
{
  switch (command) {
  case 'M': case 'O': case 'P': return 1 | 4;
  case 'G': case 'E': case 'D': return 1;
  case 'R': return 1 | 2;
  case 'T': return 2 | 4;
  case 'V': return 4;
  default:  return 0;
  }
}

/** TRUE if every vnum argument of a parsed zone command fits 0..IDXTYPE_MAX-1.
 * The .zon file is hand-editable (zedit only writes existing vnums); 70000
 * would otherwise wrap to vnum 4464 and reset the wrong thing. 'O' may name
 * NOWHERE as its room ("load nowhere"). Reports the first bad argument. */
static int zone_cmd_vnums_in_range(const struct reset_com *cmd, int zone_vnum,
                                   const char *zname, int line_num)
{
  int args[3], mask = zone_cmd_vnum_args(cmd->command), n;

  args[0] = cmd->arg1;
  args[1] = cmd->arg2;
  args[2] = cmd->arg3;
  for (n = 0; n < 3; n++) {
    if (!(mask & (1 << n)))
      continue;
    if (cmd->command == 'O' && n == 2 && args[n] == NOWHERE)
      continue;
    if (!VNUM_STORABLE(args[n])) {
      TB_ERROR_MSG("db", "zone-parse", zname,
                   "zone %d line %d cmd %c arg%d=%d out of vnum range 0..%d; "
                   "cmd disabled", zone_vnum, line_num, cmd->command, n + 1,
                   args[n], IDXTYPE_MAX - 1);
      return FALSE;
    }
  }
  return TRUE;
}

/** TRUE if a parsed zone command's table-index argument is in range: the
 * wear position of 'E' (arg3; equipment[] and equipment_types[]) and the
 * direction of 'D' (arg2; dir_option[] and dirs[]). zedit writes only valid
 * ones, so a bad value comes from a hand-edited .zon file, and zedit's
 * command list would index those name tables with it before any reset
 * could disable it. Reports the bad argument. */
static int zone_cmd_indexes_in_range(const struct reset_com *cmd, int zone_vnum,
                                     const char *zname, int line_num)
{
  if (cmd->command == 'E' && !VALID_WEAR_POS(cmd->arg3)) {
    TB_ERROR_MSG("db", "zone-parse", zname,
                 "zone %d line %d cmd E wear position %d out of range 0..%d; "
                 "cmd disabled", zone_vnum, line_num, cmd->arg3, NUM_WEARS - 1);
    return FALSE;
  }
  if (cmd->command == 'D' && !VALID_DIR(cmd->arg2)) {
    TB_ERROR_MSG("db", "zone-parse", zname,
                 "zone %d line %d cmd D direction %d out of range 0..%d; "
                 "cmd disabled", zone_vnum, line_num, cmd->arg2, NUM_OF_DIRS - 1);
    return FALSE;
  }
  return TRUE;
}

#define Z	zone_table[zone]
/* load the zone table and command tables */
static void load_zones(FILE *fl, char *zonename)
{
  static zone_rnum zone = 0;
  int i, cmd_no, num_of_cmds = 0, line_num = 0, tmp, error;
  char *ptr, buf[READ_SIZE], zname[READ_SIZE], buf2[MAX_STRING_LENGTH];
  int zone_fix = FALSE;
  char t1[80], t2[80];
  char zbuf1[MAX_STRING_LENGTH], zbuf2[MAX_STRING_LENGTH];
  char zbuf3[MAX_STRING_LENGTH], zbuf4[MAX_STRING_LENGTH];

  strlcpy(zname, zonename, sizeof(zname));

  /* Skip first 3 lines lest we mistake the zone name for a command. */
  for (tmp = 0; tmp < 3; tmp++)
    get_line(fl, buf);

  /* Count every non-comment line the parser below can consume.  The
   * allocation must never be smaller than the parser's command count. */
  while (get_line(fl, buf)) {
    ptr = buf;
    skip_spaces(&ptr);
    if (*ptr == '*')
      continue;
    num_of_cmds++;
    if (*ptr == 'S' || *ptr == '$')
      break;
  }

  if (fseek(fl, 0L, SEEK_SET) != 0) {
    TB_REPORT_ERRNO("fseek", zname);
    exit(1);
  }

  if (num_of_cmds == 0) {
    log("SYSERR: %s is empty!", zname);
    exit(1);
  } else
    CREATE(Z.cmd, struct reset_com, num_of_cmds);

  line_num += get_line(fl, buf);

  {
    int zone_number;
    if (!parse_hash_int(buf, &zone_number) || zone_number < 0 || zone_number > USHRT_MAX) {
    log("SYSERR: Format error in %s, line %d", zname, line_num);
    exit(1);
    }
    Z.number = (unsigned short)zone_number;
  }
  tb_snprintf(buf2, sizeof(buf2), "beginning of zone #%d", Z.number);

  line_num += get_line(fl, buf);
  if ((ptr = strchr(buf, '~')) != NULL) /* take off the '~' if it's there */
    *ptr = '\0';
  Z.builders = strdup(buf);

  line_num += get_line(fl, buf);
  if ((ptr = strchr(buf, '~')) != NULL)	/* take off the '~' if it's there */
    *ptr = '\0';
  Z.name = strdup(buf);
  parse_at(Z.name);

  /* Clear all the zone flags */
  for (i=0; i<ZN_ARRAY_MAX; i++)
    Z.zone_flags[i] = 0;

  line_num += get_line(fl, buf);
  /* Look for 10 items first (new tbaMUD), if not found, try 4 (old tbaMUD) */
  if (parse_zone_header(buf, &Z, zbuf1, zbuf2, zbuf3, zbuf4) != 10)
  {
    if (parse_zone_header(buf, &Z, zbuf1, zbuf2, zbuf3, zbuf4) != 4) {
      log("SYSERR: Format error in numeric constant line of %s, attempting to fix.", zname);
      if (parse_zone_header(Z.name, &Z, zbuf1, zbuf2, zbuf3, zbuf4) != 4) {
        log("SYSERR: Could not fix previous error, aborting game.");
        exit(1);
      } else {
        free(Z.name);
        Z.name = strdup(Z.builders);
        free(Z.builders);
        Z.builders = strdup("None.");
        zone_fix = TRUE;
      }
    }
    /* We only found 4 values, so set 'defaults' for the ones not found */
    Z.min_level = -1;
    Z.max_level = -1;
  }
  else
  {
    /* We found 10 values, so deal with the strings */
    Z.zone_flags[0] = flag_slot(asciiflag_conv(zbuf1), zbuf1, "zone", Z.number);
    Z.zone_flags[1] = flag_slot(asciiflag_conv(zbuf2), zbuf2, "zone", Z.number);
    Z.zone_flags[2] = flag_slot(asciiflag_conv(zbuf3), zbuf3, "zone", Z.number);
    Z.zone_flags[3] = flag_slot(asciiflag_conv(zbuf4), zbuf4, "zone", Z.number);
  }
  if (Z.bot > Z.top) {
    log("SYSERR: Zone %d bottom (%d) > top (%d).", Z.number, Z.bot, Z.top);
    exit(1);
  }
  /* top comes from the hand-edited .zon header (zedit caps it well below
   * this). IDXTYPE_MAX is NOWHERE, the "no room" sentinel, so no zone may
   * claim it; reset_zone() walks bot..top inclusive. */
  if (Z.top >= IDXTYPE_MAX) {
    BOOT_FATAL("zone-parse", zname,
               "zone %d top %d is the reserved NOWHERE vnum (max %d)",
               Z.number, Z.top, IDXTYPE_MAX - 1);
  }

  cmd_no = 0;

  for (;;) {
    /* skip reading one line if we fixed above (line is correct already) */
    if (zone_fix != TRUE) {
      if ((tmp = get_line(fl, buf)) == 0) {
        log("SYSERR: Format error in %s - premature end of file", zname);
        exit(1);
      }
    } else
      zone_fix = FALSE;

    line_num += tmp;
    ptr = buf;
    skip_spaces(&ptr);

    if (cmd_no >= num_of_cmds) {
      log("SYSERR: More commands in %s than the %d counted.", zname, num_of_cmds);
      exit(1);
    }

    if ((ZCMD.command = *ptr) == '*')
      continue;

    ptr++;

    if (ZCMD.command == 'S' || ZCMD.command == '$') {
      ZCMD.command = 'S';
      break;
    }
    error = 0;
    {
      int args[4];
      const char *rest = NULL;
      if (strchr("MOGEPDTV", ZCMD.command) == NULL) {
        if (!parse_reset_numbers(ptr, args, 3, NULL))
          error = 1;
        else {
          tmp = args[0];
          ZCMD.arg1 = args[1];
          ZCMD.arg2 = args[2];
        }
      } else if (ZCMD.command == 'V') {
        if (!parse_reset_numbers(ptr, args, 4, &rest) ||
            sscanf(rest, " %79s %79[^\f\n\r\t\v]", t1, t2) != 2)
          error = 1;
        else {
          tmp = args[0];
          ZCMD.arg1 = args[1];
          ZCMD.arg2 = args[2];
          ZCMD.arg3 = args[3];
          ZCMD.sarg1 = strdup(t1);
          ZCMD.sarg2 = strdup(t2);
        }
      } else {
        if (!parse_reset_numbers(ptr, args, 4, NULL))
          error = 1;
        else {
          tmp = args[0];
          ZCMD.arg1 = args[1];
          ZCMD.arg2 = args[2];
          ZCMD.arg3 = args[3];
        }
      }
    }

    ZCMD.if_flag = tmp;

    if (error) {
      log("SYSERR: Format error in %s, line %d: '%s'", zname, line_num, buf);
      exit(1);
    }
    ZCMD.line = line_num;
    if (!zone_cmd_vnums_in_range(&ZCMD, Z.number, zname, line_num) ||
        !zone_cmd_indexes_in_range(&ZCMD, Z.number, zname, line_num))
      ZCMD.command = '*';	/* a 'V' still owns its sarg1/sarg2 strings */
    cmd_no++;
  }

  top_of_zone_table = zone++;
}
#undef Z

/** Read one help-file line into buf (at least READ_SIZE bytes) without its
 * line ending. The help files are edited by implementors and through hedit.
 * A chunk that starts with NUL has strlen() 0, so the old "remove the last
 * byte" wrote buf[-1]; and a line longer than READ_SIZE - 1 lost a real
 * character. Only CR/LF are removed now, and both cases are reported. */
static void get_one_line(FILE *fl, char *buf)
{
  size_t n;

  if (fgets(buf, READ_SIZE, fl) == NULL) {
    log("SYSERR: error reading help file: not terminated with $?");
    exit(1);
  }

  n = strlen(buf);
  if (n == 0)
    TB_ERROR_MSG("db", "help-load", "get_one_line",
                 "NUL byte at the start of a help line; line read as empty");
  else if (buf[n - 1] != '\n' && !feof(fl))
    TB_WARN_MSG("db", "help-load", "get_one_line",
                "help line longer than %d bytes split after '%.40s...'",
                READ_SIZE - 1, buf);

  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = '\0';
}

void free_help(struct help_index_element *hentry)
{
  if (hentry->keywords)
    free(hentry->keywords);
  if (hentry->entry && !hentry->duplicate)
    free(hentry->entry);

  free(hentry);
}

void free_help_table(void)
{
  if (help_table) {
    int hp;
    for (hp = 0; hp < top_of_helpt; hp++) {
      if (help_table[hp].keywords)
        free(help_table[hp].keywords);
      if (help_table[hp].entry && !help_table[hp].duplicate)
        free(help_table[hp].entry);
    }
    free(help_table);
    help_table = NULL;
  }
  top_of_helpt = 0;
}

/** Load one help file into help_table[].
 *
 * An entry with several keywords is stored once per keyword; all copies
 * share one strdup()ed entry text, and only the copy with duplicate == 0 owns
 * it (free_help_table() frees only that one). help_table's capacity comes
 * from count_alias_records(), which must split keywords exactly as
 * one_word() does here. index_boot() sorts the table by keyword afterwards
 * with hsort(). Input: help files written by hedit or by hand. */
void load_help(FILE * fl, char *name)
{
  char key[READ_SIZE + 1], next_key[READ_SIZE + 1], entry[32384];
  size_t entrylen;
  char line[READ_SIZE + 1], hname[READ_SIZE + 1], *scan;
  struct help_index_element el;

  strlcpy(hname, name, sizeof(hname));

  get_one_line(fl, key);
  while (*key != '$') {
    strlcat(key, "\r\n", sizeof(key)); /* strcat: OK (READ_SIZE - "\n"  "\r\n" == READ_SIZE  1) */
    entrylen = strlcpy(entry, key, sizeof(entry));

    /* Read in the corresponding help entry. */
    get_one_line(fl, line);
    while (*line != '#' && entrylen < sizeof(entry) - 1) {
      entrylen += strlcpy(entry + entrylen, line, sizeof(entry) - entrylen);

      if (entrylen + 2 < sizeof(entry) - 1) {
        strlcpy(entry + entrylen, "\r\n", sizeof(entry) - entrylen); /* strcpy: OK (size checked above) */
        entrylen += 2;
      }
      get_one_line(fl, line);
    }

    if (entrylen >= sizeof(entry) - 1) {
      int keysize;
      const char *truncmsg = "\r\n*TRUNCATED*\r\n";

      memcpy(entry + sizeof(entry) - strlen(truncmsg) - 1, truncmsg, strlen(truncmsg) + 1); /* strcpy: OK (assuming sane 'entry' size) */

      keysize = strlen(key) - 2;
      log("SYSERR: Help entry exceeded buffer space: %.*s", keysize, key);

      /* If we ran out of buffer space, eat the rest of the entry. */
      while (*line != '#')
      get_one_line(fl, line);
    }

    if (*line == '#') {
      if (!parse_hash_int(line, &el.min_level)) {
        log("SYSERR: Help entry does not have a min level. %s", key);
        el.min_level = 0;
      }
    }

    el.duplicate = 0;
    el.entry = strdup(entry);
    parse_at(el.entry);
    scan = one_word(key, next_key);

    while (*next_key) {
      el.keywords = strdup(next_key);
      help_table[top_of_helpt++] = el;
      el.duplicate++;
      scan = one_word(scan, next_key);
    }
  get_one_line(fl, key);
  }
}

static int hsort(const void *a, const void *b)
{
  const struct help_index_element *a1, *b1;

  a1 = (const struct help_index_element *) a;
  b1 = (const struct help_index_element *) b;

  return (str_cmp(a1->keywords, b1->keywords));
}

int vnum_mobile(char *searchname, struct char_data *ch)
{
  int nr, found = 0;

  for (nr = 0; nr <= top_of_mobt; nr++)
    if (isname(searchname, mob_proto[nr].player.name))
      send_to_char(ch, "%3d. [%5d] %-40s %s\r\n",
                   ++found, mob_index[nr].vnum, mob_proto[nr].player.short_descr,
                   mob_proto[nr].proto_script ? "[TRIG]" : "" );

  return (found);
}

int vnum_object(char *searchname, struct char_data *ch)
{
  int nr, found = 0;

  for (nr = 0; nr <= top_of_objt; nr++)
    if (isname(searchname, obj_proto[nr].name))
      send_to_char(ch, "%3d. [%5d] %-40s %s\r\n",
                   ++found, obj_index[nr].vnum, obj_proto[nr].short_description,
                   obj_proto[nr].proto_script ? "[TRIG]" : "" );

  return (found);
}

int vnum_room(char *searchname, struct char_data *ch)
{
 int nr, found = 0;

 for (nr = 0; nr <= top_of_world; nr++)
   if (isname(searchname, world[nr].name))
   send_to_char(ch, "%3d. [%5d] %-40s %s\r\n",
   ++found, world[nr].number, world[nr].name,
  world[nr].proto_script ? "[TRIG]" : "" );
  return (found);
}

int vnum_trig(char *searchname, struct char_data *ch)
{
 int nr, found = 0;
  for (nr = 0; nr < top_of_trigt; nr++)
    if (isname(searchname, trig_index[nr]->proto->name))
    send_to_char(ch, "%3d. [%5d] %-40s\r\n",
    ++found, trig_index[nr]->vnum, trig_index[nr]->proto->name);
  return (found);
}

/* create a character, and add it to the char list */
struct char_data *create_char(void)
{
  struct char_data *ch;

  CREATE(ch, struct char_data, 1);
  clear_char(ch);
  
  new_mobile_data(ch);
  
  ch->next = character_list;
  character_list = ch;

  ch->script_id = 0;	// set later by char_script_id

  return (ch);
}

void new_mobile_data(struct char_data *ch)
{
  ch->events   = NULL;
  ch->group    = NULL;
}


/** Create a mobile from prototype nr and link it into character_list.
 *
 * type VIRTUAL looks nr up with real_mobile() and returns NULL if it does
 * not exist. type REAL trusts nr as an rnum and does not range-check it:
 * callers (reset_zone() after renum_zone_table(), OLC) must pass a valid
 * rnum. The new mobile shares its strings with the prototype. */
struct char_data *read_mobile(mob_vnum nr, int type) /* and mob_rnum */
{
  mob_rnum i;
  struct char_data *mob;

  if (type == VIRTUAL) {
    if ((i = real_mobile(nr)) == NOBODY) {
      log("WARNING: Mobile vnum %d does not exist in database.", nr);
      return (NULL);
    }
  } else
    i = nr;

  CREATE(mob, struct char_data, 1);
  clear_char(mob);
 
  *mob = mob_proto[i];
  mob->next = character_list;
  character_list = mob;
  
  new_mobile_data(mob);  
  
  if (!mob->points.max_hit) {
    mob->points.max_hit = dice(mob->points.hit, mob->points.mana) +
      mob->points.move;
  } else
    mob->points.max_hit = rand_number(mob->points.hit, mob->points.mana);

  mob->points.hit = mob->points.max_hit;
  mob->points.mana = mob->points.max_mana;
  mob->points.move = mob->points.max_move;

  mob->player.time.birth = time(0);
  mob->player.time.played = 0;
  mob->player.time.logon = time(0);

  mob_index[i].number++;

  mob->script_id = 0;	// this is set later by char_script_id

  copy_proto_script(&mob_proto[i], mob, MOB_TRIGGER);
  assign_triggers(mob, MOB_TRIGGER);

  return (mob);
}

/* create an object, and add it to the object list */
struct obj_data *create_obj(void)
{
  struct obj_data *obj;

  CREATE(obj, struct obj_data, 1);
  clear_object(obj);
  obj->next = object_list;
  object_list = obj;
  
  obj->events = NULL;

  obj->script_id = 0;	// this is set later by obj_script_id

  return (obj);
}

/** Create an object from prototype nr and link it into object_list.
 *
 * Unlike read_mobile(), both VIRTUAL and REAL are range-checked: an unknown
 * vnum or an rnum past top_of_objt is logged and returns NULL. The new
 * object shares its strings with the prototype. */
struct obj_data *read_object(obj_vnum nr, int type) /* and obj_rnum */
{
  struct obj_data *obj;
  obj_rnum i = type == VIRTUAL ? real_object(nr) : nr;

  if (i == NOTHING || i > top_of_objt) {
    log("Object (%c) %d does not exist in database.", type == VIRTUAL ? 'V' : 'R', nr);
    return (NULL);
  }

  CREATE(obj, struct obj_data, 1);
  clear_object(obj);
  *obj = obj_proto[i];
  obj->next = object_list;
  object_list = obj;
  
  obj->events = NULL;

  obj_index[i].number++;

  obj->script_id = 0;	// this is set later by obj_script_id

  copy_proto_script(&obj_proto[i], obj, OBJ_TRIGGER);
  assign_triggers(obj, OBJ_TRIGGER);

  return (obj);
}

#define ZO_DEAD  999
/* update zone ages, queue for reset if necessary, and dequeue when possible */
void zone_update(void)
{
  int i;
  struct reset_q_element *update_u, *temp;
  static int timer = 0;

  /* jelson 10/22/92 */
  if (((++timer * PULSE_ZONE) / PASSES_PER_SEC) >= 60) {
    /* one minute has passed NOT accurate unless PULSE_ZONE is a multiple of
     * PASSES_PER_SEC or a factor of 60 */

    timer = 0;

    /* since one minute has passed, increment zone ages */
    for (i = 0; i <= top_of_zone_table; i++) {
      if (zone_table[i].age < zone_table[i].lifespan &&
	  zone_table[i].reset_mode)
	(zone_table[i].age)++;

      if (zone_table[i].age >= zone_table[i].lifespan &&
	  zone_table[i].age < ZO_DEAD && zone_table[i].reset_mode) {
	/* enqueue zone */

	CREATE(update_u, struct reset_q_element, 1);

	update_u->zone_to_reset = i;
	update_u->next = 0;

	if (!reset_q.head)
	  reset_q.head = reset_q.tail = update_u;
	else {
	  reset_q.tail->next = update_u;
	  reset_q.tail = update_u;
	}

	zone_table[i].age = ZO_DEAD;
      }
    }
  }	/* end - one minute has passed */

  /* Dequeue zones (if possible) and reset. This code is executed every 10
   * seconds (i.e. PULSE_ZONE). */
  for (update_u = reset_q.head; update_u; update_u = update_u->next)
    if (zone_table[update_u->zone_to_reset].reset_mode == 2 ||
	is_empty(update_u->zone_to_reset)) {
      reset_zone(update_u->zone_to_reset);
      mudlog(CMP, LVL_IMPL+1, FALSE, "Auto zone reset: %s (Zone %d)",
          zone_table[update_u->zone_to_reset].name, zone_table[update_u->zone_to_reset].number);
      struct descriptor_data *pt;
      for (pt = descriptor_list; pt; pt = pt->next)
        if (IS_PLAYING(pt) && pt->character && PRF_FLAGGED(pt->character, PRF_ZONERESETS))
          send_to_char(pt->character, "%s[Auto zone reset: %s (Zone %d)]%s", 
            CCGRN(pt->character, C_NRM), zone_table[update_u->zone_to_reset].name, 
            zone_table[update_u->zone_to_reset].number, CCNRM(pt->character, C_NRM));
      /* dequeue */
      if (update_u == reset_q.head)
	reset_q.head = reset_q.head->next;
      else {
	for (temp = reset_q.head; temp->next != update_u;
	     temp = temp->next);

	if (!update_u->next)
	  reset_q.tail = temp;

	temp->next = update_u->next;
      }

      free(update_u);
      break;
    }
}

static void log_zone_error(zone_rnum zone, int cmd_no, const char *message)
{
  mudlog(NRM, LVL_GOD, TRUE, "SYSERR: zone file: %s", message);
  mudlog(NRM, LVL_GOD, TRUE, "SYSERR: ...offending cmd: '%c' cmd in zone #%d, line %d",
	ZCMD.command, zone_table[zone].number, ZCMD.line);
}

#define ZONE_ERROR(message) \
	{ log_zone_error(zone, cmd_no, message); last_cmd = 0; }

/* Object triggers fired during a reset may purge the object they run on.
 * Unlike characters (which wait for extract_pending_chars()), extract_obj()
 * frees an object at once, and load_otrigger()/wear_otrigger() take the object
 * by value, so the caller cannot see the purge through its own pointer.
 *
 * Invariant: after these helpers return, the object is either live or the
 * caller's pointer is NULL. The UID is taken before dispatch, and
 * obj_uid_matches() compares the registered address without dereferencing
 * the (possibly freed) object. Who controls the input: builders, through the
 * zone file and the object's trigger scripts. */
static struct obj_data *zone_reset_load_otrigger(struct obj_data *obj,
                                                 zone_rnum zone, int cmd_no)
{
  obj_vnum vnum = GET_OBJ_VNUM(obj);
  long uid;

  if (!SCRIPT_CHECK(obj, OTRIG_LOAD))
    return obj;

  uid = obj_script_id(obj);
  load_otrigger(obj);
  if (obj_uid_matches(uid, obj))
    return obj;

  TB_WARN_MSG("db", "zone-reset", "load_otrigger",
              "zone %d cmd %d '%c' line %d: obj vnum %d (uid %ld) purged by "
              "its load trigger; dependent commands skipped",
              zone_table[zone].number, cmd_no, ZCMD.command, ZCMD.line,
              vnum, uid);
  return NULL;
}

/* As above, for the wear trigger of an 'E' command. Returns wear_otrigger()'s
 * verdict, and sets *objp to NULL when the trigger purged the object. */
static int zone_reset_wear_otrigger(struct obj_data **objp,
                                    struct char_data *mob, int pos,
                                    zone_rnum zone, int cmd_no)
{
  struct obj_data *obj = *objp;
  obj_vnum vnum = GET_OBJ_VNUM(obj);
  long uid;
  int worn;

  if (!SCRIPT_CHECK(obj, OTRIG_WEAR))
    return wear_otrigger(obj, mob, pos);

  uid = obj_script_id(obj);
  worn = wear_otrigger(obj, mob, pos);
  if (obj_uid_matches(uid, obj))
    return worn;

  TB_WARN_MSG("db", "zone-reset", "wear_otrigger",
              "zone %d cmd %d '%c' line %d: obj vnum %d (uid %ld) purged by "
              "its wear trigger on mob vnum %d; dependent commands skipped",
              zone_table[zone].number, cmd_no, ZCMD.command, ZCMD.line,
              vnum, uid, GET_MOB_VNUM(mob));
  *objp = NULL;
  return 0;
}

/** Run zone's reset command table (see struct reset_com for the meaning of
 * each command's arguments after renum_zone_table()).
 *
 * Pointer lifetime: mob is the last mobile an 'M' loaded and stays valid for
 * the commands that follow, because a character extracted by its load
 * trigger is only marked and freed later by extract_pending_chars(). An
 * object purged by its load or wear trigger is freed at once, so obj and
 * tobj are cleared through zone_reset_load_otrigger() and
 * zone_reset_wear_otrigger() and the dependent commands are skipped. tmob
 * and tobj name the target of following 'T'/'V' commands and are reset by
 * every command that loads something else.
 *
 * A command whose target cannot exist (missing container, bad wear slot,
 * missing door) is reported and disabled ('*'); a command that merely did
 * not run this time (max existing reached) only clears last_cmd. */
void reset_zone(zone_rnum zone)
{
  int cmd_no, last_cmd = 0;
  struct char_data *mob = NULL;
  struct obj_data *obj, *obj_to;
  int rvnum;	/* int, not room_vnum: top may be IDXTYPE_MAX - 1, and a
		 * room_vnum counter would wrap after it and never end */
  room_rnum rrnum;
  struct char_data *tmob=NULL; /* for trigger assignment */
  struct obj_data *tobj=NULL;  /* for trigger assignment */

  for (cmd_no = 0; ZCMD.command != 'S'; cmd_no++) {

    if (ZCMD.if_flag && !last_cmd)
      continue;

    /* This is the list of actual zone commands. If any new zone commands are
     * added to the game, be certain to update the list of commands in load_zone
     * () so that the counting will still be correct. - ae. */
    switch (ZCMD.command) {
    case '*':			/* ignore command */
      last_cmd = 0;
      break;

    case 'M':			/* read a mobile */
      if (mob_index[ZCMD.arg1].number < ZCMD.arg2) {
	mob = read_mobile(ZCMD.arg1, REAL);
	char_to_room(mob, ZCMD.arg3);
        load_mtrigger(mob);
        tmob = mob;
	last_cmd = 1;
      } else
	last_cmd = 0;
      tobj = NULL;
      break;

    case 'O':			/* read an object */
      if (obj_index[ZCMD.arg1].number < ZCMD.arg2) {
	if (ZCMD.arg3 != NOWHERE) {
	  obj = read_object(ZCMD.arg1, REAL);
	  obj_to_room(obj, ZCMD.arg3);
	  obj = zone_reset_load_otrigger(obj, zone, cmd_no);
	  last_cmd = obj != NULL;
          tobj = obj;
	} else {
	  obj = read_object(ZCMD.arg1, REAL);
	  IN_ROOM(obj) = NOWHERE;
	  last_cmd = 1;
          tobj = obj;
	}
      } else
	last_cmd = 0;
      tmob = NULL;
      break;

    case 'P':			/* object to object */
      if (obj_index[ZCMD.arg1].number < ZCMD.arg2) {
	/* Find the container before creating the object: an object created
	 * first had no location on this path and stayed in object_list,
	 * counted in obj_index[].number, for the rest of the boot. */
	if (!(obj_to = get_obj_num(ZCMD.arg3))) {
	  TB_ERROR_MSG("db", "zone-reset", "put",
	               "zone %d line %d: container obj vnum %d for obj vnum %d "
	               "is not loaded; cmd disabled",
	               zone_table[zone].number, ZCMD.line,
	               ZCMD.arg3 >= 0 && ZCMD.arg3 <= top_of_objt ?
	               obj_index[ZCMD.arg3].vnum : -1,
	               obj_index[ZCMD.arg1].vnum);
	  ZCMD.command = '*';
	  last_cmd = 0;
	  break;
	}
	obj = read_object(ZCMD.arg1, REAL);
	obj_to_obj(obj, obj_to);
	obj = zone_reset_load_otrigger(obj, zone, cmd_no);
	last_cmd = obj != NULL;
        tobj = obj;
      } else
	last_cmd = 0;
      tmob = NULL;
      break;

    case 'G':			/* obj_to_char */
      if (!mob) {
	char error[MAX_INPUT_LENGTH];
	tb_snprintf(error, sizeof(error), "attempt to give obj #%d to non-existant mob, command disabled", obj_index[ZCMD.arg1].vnum);
	ZONE_ERROR(error);
	ZCMD.command = '*';
	break;
      }
      if (obj_index[ZCMD.arg1].number < ZCMD.arg2) {
	obj = read_object(ZCMD.arg1, REAL);
	obj_to_char(obj, mob);
	obj = zone_reset_load_otrigger(obj, zone, cmd_no);
	last_cmd = obj != NULL;
        tobj = obj;
      } else
	last_cmd = 0;
      tmob = NULL;
      break;

    case 'E':			/* object to equipment list */
      if (!mob) {
	char error[MAX_INPUT_LENGTH];
	tb_snprintf(error, sizeof(error), "trying to equip non-existant mob with obj #%d, command disabled", obj_index[ZCMD.arg1].vnum);
	ZONE_ERROR(error);
	ZCMD.command = '*';
	break;
      }
      if (obj_index[ZCMD.arg1].number < ZCMD.arg2) {
	/* arg3 is the wear slot from the zone file and indexes equipment[].
	 * After renum_zone_table() the object rnum is arg1 (arg2 is the
	 * max-existing count), so name the object from arg1. A bad slot never
	 * becomes valid, so the command is disabled rather than re-reported on
	 * every reset. */
	if (!VALID_WEAR_POS(ZCMD.arg3)) {
	  TB_ERROR_MSG("db", "zone-reset", "equip",
	               "zone %d line %d: mob vnum %d obj vnum %d invalid wear pos %d "
	               "(0..%d); cmd disabled",
	               zone_table[zone].number, ZCMD.line, GET_MOB_VNUM(mob),
	               obj_index[ZCMD.arg1].vnum, ZCMD.arg3, NUM_WEARS - 1);
	  ZCMD.command = '*';
	  last_cmd = 0;
	} else {
	  int worn = 0;

	  obj = read_object(ZCMD.arg1, REAL);
          IN_ROOM(obj) = IN_ROOM(mob);
          obj = zone_reset_load_otrigger(obj, zone, cmd_no);
          if (obj != NULL)
            worn = zone_reset_wear_otrigger(&obj, mob, ZCMD.arg3, zone, cmd_no);
          if (obj == NULL) {
            tobj = NULL;
            last_cmd = 0;
            break;
          }
          if (worn) {
            IN_ROOM(obj) = NOWHERE;
            equip_char(mob, obj, ZCMD.arg3);
          } else
            obj_to_char(obj, mob);
          tobj = obj;
	  last_cmd = 1;
	}
      } else
	last_cmd = 0;
      tmob = NULL;
      break;

    case 'R': /* rem obj from room */
      if ((obj = get_obj_in_list_num(ZCMD.arg2, world[ZCMD.arg1].contents)) != NULL)
        extract_obj(obj);
      last_cmd = 1;
      tmob = NULL;
      tobj = NULL;
      break;


    case 'D':			/* set state of door */
      if (ZCMD.arg2 < 0 || ZCMD.arg2 >= DIR_COUNT ||
	  (world[ZCMD.arg1].dir_option[ZCMD.arg2] == NULL)) {
        char error[MAX_INPUT_LENGTH];
        tb_snprintf(error, sizeof(error), "door does not exist in room %d - dir %d, command disabled",  world[ZCMD.arg1].number, ZCMD.arg2);
	ZONE_ERROR(error);
	ZCMD.command = '*';
      } else
	switch (ZCMD.arg3) {
	case 0:
	  REMOVE_BIT(world[ZCMD.arg1].dir_option[ZCMD.arg2]->exit_info,
		     EX_LOCKED);
	  REMOVE_BIT(world[ZCMD.arg1].dir_option[ZCMD.arg2]->exit_info,
		     EX_CLOSED);
	  break;
	case 1:
	  SET_BIT(world[ZCMD.arg1].dir_option[ZCMD.arg2]->exit_info,
		  EX_CLOSED);
	  REMOVE_BIT(world[ZCMD.arg1].dir_option[ZCMD.arg2]->exit_info,
		     EX_LOCKED);
	  break;
	case 2:
	  SET_BIT(world[ZCMD.arg1].dir_option[ZCMD.arg2]->exit_info,
		  EX_LOCKED);
	  SET_BIT(world[ZCMD.arg1].dir_option[ZCMD.arg2]->exit_info,
		  EX_CLOSED);
	  break;
	}
      last_cmd = 1;
      tmob = NULL;
      tobj = NULL;
      break;

    case 'T': /* trigger command */
      if (ZCMD.arg1==MOB_TRIGGER && tmob) {
        if (!SCRIPT(tmob))
          CREATE(SCRIPT(tmob), struct script_data, 1);
        add_trigger(SCRIPT(tmob), read_trigger(ZCMD.arg2), -1);
        last_cmd = 1;
      } else if (ZCMD.arg1==OBJ_TRIGGER && tobj) {
        if (!SCRIPT(tobj))
          CREATE(SCRIPT(tobj), struct script_data, 1);
        add_trigger(SCRIPT(tobj), read_trigger(ZCMD.arg2), -1);
        last_cmd = 1;
      } else if (ZCMD.arg1==WLD_TRIGGER) {
        if (ZCMD.arg3 == NOWHERE || ZCMD.arg3 > top_of_world) {
          ZONE_ERROR("Invalid room number in trigger assignment");
        } else {
          if (!world[ZCMD.arg3].script)
            CREATE(world[ZCMD.arg3].script, struct script_data, 1);
          add_trigger(world[ZCMD.arg3].script, read_trigger(ZCMD.arg2), -1);
          last_cmd = 1;
        }
      }

      break;

    case 'V':
      /* 'V' <type> <context> <room> <name> <value>: arg2 is the variable
       * context for every target type (zedit and save_zone() write it
       * there); arg3 is the room rnum after renum_zone_table(). */
      if (ZCMD.arg1==MOB_TRIGGER && tmob) {
        if (!SCRIPT(tmob)) {
          ZONE_ERROR("Attempt to give variable to scriptless mobile");
        } else
          add_var(&(SCRIPT(tmob)->global_vars), ZCMD.sarg1, ZCMD.sarg2,
                  ZCMD.arg2);
        last_cmd = 1;
      } else if (ZCMD.arg1==OBJ_TRIGGER && tobj) {
        if (!SCRIPT(tobj)) {
          ZONE_ERROR("Attempt to give variable to scriptless object");
        } else
          add_var(&(SCRIPT(tobj)->global_vars), ZCMD.sarg1, ZCMD.sarg2,
                  ZCMD.arg2);
        last_cmd = 1;
      } else if (ZCMD.arg1==WLD_TRIGGER) {
        if (ZCMD.arg3 == NOWHERE || ZCMD.arg3>top_of_world) {
          ZONE_ERROR("Invalid room number in variable assignment");
        } else {
          if (!(world[ZCMD.arg3].script)) {
            ZONE_ERROR("Attempt to give variable to scriptless object");
          } else
            add_var(&(world[ZCMD.arg3].script->global_vars),
                    ZCMD.sarg1, ZCMD.sarg2, ZCMD.arg2);
          last_cmd = 1;
        }
      }
      break;

    default:
      ZONE_ERROR("unknown cmd in reset table; cmd disabled");
      ZCMD.command = '*';
      break;
    }
  }

  zone_table[zone].age = 0;

  /* handle reset_wtrigger's */
  rvnum = zone_table[zone].bot;
  while (rvnum <= zone_table[zone].top) {
    rrnum = real_room(rvnum);
    if (rrnum != NOWHERE) reset_wtrigger(&world[rrnum]);
    rvnum++;
  }
}

/* for use in reset_zone; return TRUE if zone 'nr' is free of PC's  */
int is_empty(zone_rnum zone_nr)
{
  struct descriptor_data *i;

  for (i = descriptor_list; i; i = i->next) {
    if (STATE(i) != CON_PLAYING)
      continue;
    if (IN_ROOM(i->character) == NOWHERE)
      continue;
    if (world[IN_ROOM(i->character)].zone != zone_nr)
      continue;
    /* If an immortal has nohassle off, he counts as present. Added for testing
     * zone reset triggers -Welcor */
    if ((!IS_NPC(i->character)) && (GET_LEVEL(i->character) >= LVL_IMMORT) && (PRF_FLAGGED(i->character, PRF_NOHASSLE)))
      continue;

    return (0);
  }

  return (1);
}

/* Functions of a general utility nature. */

/** Read a '~'-terminated string from fl, converting each line ending to
 * "\r\n".
 *
 * @param error Context for diagnostics (usually "room #3001" or similar).
 * @return A malloc()ed string the caller frees, or NULL when the string is
 * empty. parse_at() has already been applied; callers must not apply it
 * again.
 *
 * Lines are read in FREAD_CHUNK - 1 byte windows; a longer physical line is
 * rejoined, not split. A NUL byte at the start of a window is reported and
 * that window dropped. The whole string is limited to MAX_STRING_LENGTH - 1;
 * a longer one, EOF or a read error ends the process (exit(1)), so this is
 * for boot-time and trusted files. Input: world, mob, object, trigger,
 * config and other lib/ files. */
char *fread_string(FILE *fl, const char *error)
{
  char buf[MAX_STRING_LENGTH], tmp[FREAD_CHUNK + 3];
  char *point;
  int done = 0, length = 0, templength;

  *buf = '\0';

  do {
    if (!fgets(tmp, FREAD_CHUNK, fl)) {
      log("SYSERR: fread_string: format error at or near %s", error);
      exit(1);
    }
    /* A NUL byte at the start of a chunk (a corrupt or partly written file)
     * makes the chunk look empty; the scan below would then start at tmp[-1]. */
    if (*tmp == '\0') {
      TB_ERROR_MSG("db", "fread-string", error,
                   "NUL byte in string data; rest of that line dropped");
      continue;
    }
    /* If there is a '~', end the string; else put an "\r\n" over the '\n'. */
    /* now only removes trailing ~'s -- Welcor */
    point = strchr(tmp, '\0');
    /* A chunk with no '\n' that is not the end of the file is the front of a
     * line longer than the fgets() window (the OLC and IBT editors accept
     * 511-byte lines). Append it unchanged so the line is rejoined instead of
     * being split by an inserted "\r\n". */
    if (point[-1] != '\n' && point[-1] != '~' && !feof(fl)) {
      templength = point - tmp;
      if (length + templength >= MAX_STRING_LENGTH) {
        log("SYSERR: %s: string too large (db.c)", __func__);
        log("%s", error);
        exit(1);
      }
      strlcat(buf, tmp, sizeof(buf));	/* size checked above */
      length += templength;
      continue;
    }
    for (point-- ; (*point=='\r' || *point=='\n') && point > tmp; point--);
    if (*point=='~') {
      *point='\0';
      done = 1;
    } else {
      if (*point == '\n' || *point == '\r')
        *point = '\r';
      else
        *(++point) = '\r';

      *(++point) = '\n';
      *(++point) = '\0';
    }

    templength = point - tmp;

    if (length + templength >= MAX_STRING_LENGTH) {
      log("SYSERR: fread_string: string too large (db.c)");
      log("%s", error);
      exit(1);
    } else {
      strlcat(buf, tmp, sizeof(buf));	/* strcat: OK (size checked above) */
      length += templength;
    }
  } while (!done);
  
  parse_at(buf);
  /* allocate space for the new string and copy it */
  return (strlen(buf) ? strdup(buf) : NULL);
}

/* fread_clean_string is the same as fread_string, but skips preceding spaces.
 *
 * Contract (its only caller is the IBT loader, read_ibt() in ibt.c, whose
 * files hold player text and can be cut short by a crash or a full disk):
 * returns a newly allocated string -- "" for an empty one -- or NULL when
 * the string is missing, unterminated or too long.  NULL is always
 * reported here with 'error' (the caller's file/record/field context) and
 * the caller drops the record.  It never exits: a truncated bug list must
 * not make the server unbootable. */
char *fread_clean_string(FILE *fl, const char *error)
{
  char buf[MAX_STRING_LENGTH], tmp[FREAD_CHUNK + 3];
  char *point;
  int c;
  int done = 0, length = 0, templength;

  *buf = '\0';

  do
  {
    if (feof(fl) || (c = getc(fl)) == EOF) {
      TB_ERROR_MSG("db", "fread-clean-string", error,
                   TB_ERRID_PARSER_EOF " file ends before the string starts");
      return NULL;
    }
  }
  while (isspace((unsigned char)c));
  if (ungetc(c, fl) == EOF) {
    TB_ERROR_ERRNO("db", "fread-clean-string", error,
                   TB_ERRID_PARSER_EOF " could not restore lookahead");
    return NULL;
  }

  do {
    if (!fgets(tmp, FREAD_CHUNK, fl)) {
      TB_ERROR_MSG("db", "fread-clean-string", error,
                   TB_ERRID_PARSER_EOF " file ends after %d bytes with no closing '~'",
                   length);
      return NULL;
    }
    /* See fread_string(): a chunk starting with NUL would scan from tmp[-1]. */
    if (*tmp == '\0') {
      TB_ERROR_MSG("db", "fread-string", error,
                   "NUL byte in string data; rest of that line dropped");
      continue;
    }
    /* If there is a '~', end the string; else put an "\r\n" over the '\n'. */
    /* now only removes trailing ~'s -- Welcor */
    point = strchr(tmp, '\0');
    /* A chunk with no '\n' that is not the end of the file is the front of a
     * line longer than the fgets() window (the OLC and IBT editors accept
     * 511-byte lines). Append it unchanged so the line is rejoined instead of
     * being split by an inserted "\r\n". */
    if (point[-1] != '\n' && point[-1] != '~' && !feof(fl)) {
      templength = point - tmp;
      if (length + templength >= MAX_STRING_LENGTH) {
        TB_ERROR_MSG("db", "fread-clean-string", error,
                     TB_ERRID_PARSER_RANGE " string exceeds %d bytes", MAX_STRING_LENGTH - 1);
        return NULL;	/* never exit: see the contract above */
      }
      strlcat(buf, tmp, sizeof(buf));	/* size checked above */
      length += templength;
      continue;
    }
    /* Stop at tmp[0]: a body line that is only "\r\n" (a blank line typed in
     * the IBT editor) used to walk below tmp while stack bytes were CR/LF. */
    for (point-- ; (*point=='\r' || *point=='\n') && point > tmp; point--);
    if (*point=='~') {
      *point='\0';
      done = 1;
    } else {
      if (*point == '\n' || *point == '\r')
        *point = '\r';	/* all-newline line: keep it as one empty line */
      else
        *(++point) = '\r';
      *(++point) = '\n';
      *(++point) = '\0';
    }

    templength = point - tmp;

    if (length + templength >= MAX_STRING_LENGTH) {
      TB_ERROR_MSG("db", "fread-clean-string", error,
                   TB_ERRID_PARSER_RANGE " string exceeds %d bytes", MAX_STRING_LENGTH - 1);
      return NULL;
    } else {
      strlcat(buf, tmp, sizeof(buf));	/* strcat: OK (size checked above) */
      length += templength;
    }
  } while (!done);
  
  parse_at(buf);
  /* allocate space for the new string and copy it */
  return strdup(buf);
}

/** Read a decimal number (optionally signed, optionally "a|b|..." summed)
 * from fp. Used by the IBT loader (ibt.c) for Level and Room, so the input
 * is the bug/idea/typo file the game writes, possibly hand-edited.
 *
 * Returns the value, or 0 after a "fread-number" diagnostic naming the file
 * line on EOF, a non-digit, or a value outside int: the digits are
 * accumulated in long long and checked, because "number * 10 + digit" on an
 * int overflowed (undefined behaviour) for long digit strings. */
int fread_number(FILE *fp)
{
  long long number = 0, rest;
  bool sign = FALSE, overflow = FALSE;
  int c;

  do
    c = getc( fp );
  while( c != EOF && isspace( c ) );

  if (c == EOF) {
    TB_ERROR_MSG("db", "fread-number", "eof",
                 "EOF before a number at file line %lu",
                 tb_parse_context_line(fp));
    return 0;
  }

  if( c == '+' )
    c = getc( fp );
  else if( c == '-' ) {
    sign = TRUE;
    c = getc( fp );
  }

  if(!isdigit(c)) {
    TB_ERROR_MSG("db", "fread-number", "format",
                 "expected a digit, found %s0x%02x at file line %lu",
                 c == EOF ? "EOF " : "", c == EOF ? 0 : (unsigned)c,
                 tb_parse_context_line(fp));
    return 0;	/* the bad character stays consumed, as it always was */
  }

  while(isdigit(c)) {
    if (!overflow) {
      number = number * 10 + (c - '0');
      if (number > (long long)INT_MAX + 1)	/* INT_MIN's magnitude fits */
        overflow = TRUE;
    }
    /* getc() returns EOF on error or end of file, which ends the loop. */
    c = getc( fp ); /* NOLINT(clang-analyzer-unix.Stream) */
  }

  if( sign )
    number = -number;
  if (overflow || number > INT_MAX || number < INT_MIN) {
    TB_ERROR_MSG("db", "fread-number", "range",
                 "number out of int range at file line %lu; using 0",
                 tb_parse_context_line(fp));
    number = 0;
  }

  if( c == '|' ) {
    rest = fread_number( fp );
    if (number + rest > INT_MAX || number + rest < INT_MIN) {
      TB_ERROR_MSG("db", "fread-number", "range",
                   "sum %lld + %lld out of int range at file line %lu; using 0",
                   number, rest, tb_parse_context_line(fp));
      return 0;
    }
    number += rest;
  } else if (c != ' ' && c != EOF && ungetc(c, fp) == EOF) { /* NOLINT(clang-analyzer-unix.Stream): c is a real character here */
    log("SYSERR: fread_number: cannot restore lookahead");
    return 0;
  }

  return (int)number;
}

/** Read to end of line from fp into a static buffer and return it.
 * The buffer is overwritten by the next call (not reentrant); callers copy
 * what they keep. */
char *fread_line(FILE *fp)
{
  static char line[MAX_STRING_LENGTH];
  char *pline;
  int c;
  int ln;

  pline = line;
  line[0] = '\0';
  ln = 0;

  /* Skip blanks.     */
  /* Read first char. */
  do {
    if(feof(fp)) {
      log("fread_line: EOF encountered on read.");
      *pline = '\0';
      return (line);
    }
    c = getc(fp);
    if (c == EOF) {
      log("SYSERR: fread_line: unexpected EOF");
      return line;
    }
  }
  while (isspace((unsigned char)c));

  /* Un-Read first char */
  if (ungetc(c, fp) == EOF) {
    log("SYSERR: fread_line: failed to restore lookahead");
    return line;
  }

  do {
    if(feof(fp)) {
      log("fread_line: EOF encountered on read.");
      *pline = '\0';
      return ( line );
    }
    c = getc( fp );
    *pline++ = c;
    ln++;
    if( ln >= ( MAX_STRING_LENGTH - 1 ) ) {
      log("fread_line: line too long");
      break;
    }
  }
  while( (c != '\n') && (c != '\r') );

  do
  {
    c = getc(fp);
  }
  while( c == '\n' || c == '\r' );

  if (c != EOF && ungetc(c, fp) == EOF) {
    log("SYSERR: fread_line: failed to restore lookahead");
    return line;
  }
  pline--;
  *pline = '\0';

  /* Since tildes generally aren't found at the end of lines, this seems workable. Will enable reading old configs. */
  if( line[strlen(line) - 1] == '~' )
    line[strlen(line) - 1] = '\0';

  return (line);
}

/* Read to end of line from a given file and convert to flag values, then return number of ints */
int fread_flags(FILE *fp, int *fg, int fg_size)
{
  char line[MAX_STRING_LENGTH];
  char *pline, *tmp_txt, val_txt[MAX_INPUT_LENGTH];
  int c;
  int ln,i;

  pline = line;
  line[0] = '\0';
  ln = 0;

  /* Skip blanks.     */
  /* Read first char. */
  do {
    if(feof(fp)) {
      log("fread_flags: EOF encountered on read.");
      *pline = '\0';
      return (0);
    }
    c = getc(fp);
    if (c == EOF) {
      log("SYSERR: fread_flags: unexpected EOF");
      return 0;
    }
  }
  while (isspace((unsigned char)c));

  /* Un-Read first char */
  if (ungetc(c, fp) == EOF) {
    log("SYSERR: fread_flags: failed to restore lookahead");
    return 0;
  }

  do {
    if(feof(fp)) {
      log("fread_flags: EOF encountered on read.");
      *pline = '\0';
      return (0);
    }
    c = getc( fp );
    *pline++ = c;
    ln++;
    if( ln >= ( MAX_STRING_LENGTH - 1 ) ) {
      log("fread_flags: line too long");
      break;
    }
  }
  while( (c != '\n') && (c != '\r') );

  do
  {
    c = getc(fp);
  }
  while( c == '\n' || c == '\r' );

  if (c != EOF && ungetc(c, fp) == EOF) {
    log("SYSERR: fread_flags: failed to restore lookahead");
    return 0;
  }
  pline--;
  *pline = '\0';

  /* Since tildes generally aren't found at the end of lines, this seems workable. Will enable reading old configs. */
  if( line[strlen(line) - 1] == '~' )
    line[strlen(line) - 1] = '\0';

  /* We now have a line of text with all the flags on it - let's convert it */
  for (i=0,tmp_txt=line;tmp_txt && *tmp_txt && i<fg_size;i++) {
    tmp_txt = one_argument(tmp_txt,val_txt);  /* Grab a number  */
    fg[i]   = tb_parse_int_or_zero(val_txt);                  /* Convert to int */
  }

  return (i);
}

/** Read one whitespace- or quote-delimited word from fp into a static
 * buffer. May return NULL at EOF; the buffer is overwritten by the next call
 * (not reentrant). */
char *fread_word(FILE *fp)
{
   static char word[MAX_STRING_LENGTH];
   char *pword;
   int cEnd;

   do
   {
      if( feof( fp ) )
      {
         log( "fread_word: EOF encountered on read.");
         word[0] = '\0';
         return word;
      }
      cEnd = getc( fp );
   }
   while( isspace( cEnd ) );

   if( cEnd == '\'' || cEnd == '"' )
   {
      pword = word;
   }
   else
   {
      word[0] = cEnd;
      pword = word + 1;
      cEnd = ' ';
   }

   for( ; pword < word + MAX_STRING_LENGTH; pword++ )
   {
      if( feof( fp ) )
      {
         log( "fread_word: EOF encountered on read.");
         *pword = '\0';
         return word;
      }
      int next = getc(fp);
      if (next == EOF) {
         *pword = '\0';
         return word;
      }
      *pword = (char)next;
      if (cEnd == ' ' ? isspace((unsigned char)next) : next == cEnd)
      {
         if (cEnd == ' ' && ungetc(next, fp) == EOF) {
            log("SYSERR: fread_word: failed to restore lookahead");
            return NULL;
         }
         *pword = '\0';
         return word;
      }
   }
   log( "fread_word: word too long");
   return NULL;
}

/* Read to end of line in a given file (for comments) */
void fread_to_eol(FILE *fp)
{
  int c;

  do {
    if(feof(fp)) {
      log( "%s", "fread_to_eol: EOF encountered on read." );
      return;
    }
    c = getc( fp );
  }
  while( c != '\n' && c != '\r' );

  do {
    c = getc( fp );
  }
  while( c == '\n' || c == '\r' );

  if (c != EOF && ungetc(c, fp) == EOF)
    log("SYSERR: fread_to_eol: failed to restore lookahead");
}

/* Called to free all allocated follow_type structs */
static void free_followers(struct follow_type *k)
{
   if (!k)
    return;

  if (k->next)
    free_followers(k->next);

  k->follower = NULL;
  free(k);
}

/* release memory allocated for a char struct */
void free_char(struct char_data *ch)
{
  int i;
  struct alias_data *a;

  /* No editor may keep pointing at this character. */
  if (!IS_NPC(ch))
    prefedit_forget_char(ch);

  if (ch->player_specials != NULL && ch->player_specials != &dummy_mob) {
    while ((a = GET_ALIASES(ch)) != NULL) {
      GET_ALIASES(ch) = (GET_ALIASES(ch))->next;
      free_alias(a);
    }
    if (ch->player_specials->poofin)
      free(ch->player_specials->poofin);
    if (ch->player_specials->poofout)
      free(ch->player_specials->poofout);
    if (ch->player_specials->saved.completed_quests)
      free(ch->player_specials->saved.completed_quests);
    if (GET_HOST(ch))
      free(GET_HOST(ch));
    if (IS_NPC(ch))
      log("SYSERR: Mob %s (#%d) had player_specials allocated!", GET_NAME(ch), GET_MOB_VNUM(ch));
  }
  if (!IS_NPC(ch) || (IS_NPC(ch) && GET_MOB_RNUM(ch) == NOBODY)) {
    /* if this is a player, or a non-prototyped non-player, free all */
    if (ch->player.name)
      free(ch->player.name);
    if (ch->player.title)
      free(ch->player.title);
    if (ch->player.short_descr)
      free(ch->player.short_descr);
    if (ch->player.long_descr)
      free(ch->player.long_descr);
    if (ch->player.description)
      free(ch->player.description);
    if (ch->player_specials != NULL && ch->player_specials != &dummy_mob) {
      for (i = 0; i < NUM_HIST; i++)
        if (GET_HISTORY(ch, i))
          free_history(ch, i);

      free(ch->player_specials);
      ch->player_specials = NULL;
    }

    /* free script proto list */
    free_proto_script(ch, MOB_TRIGGER);

  } else if ((i = GET_MOB_RNUM(ch)) != NOBODY) {
    /* otherwise, free strings only if the string is not pointing at proto */
    if (ch->player.name && ch->player.name != mob_proto[i].player.name)
      free(ch->player.name);
    if (ch->player.title && ch->player.title != mob_proto[i].player.title)
      free(ch->player.title);
    if (ch->player.short_descr && ch->player.short_descr != mob_proto[i].player.short_descr)
      free(ch->player.short_descr);
    if (ch->player.long_descr && ch->player.long_descr != mob_proto[i].player.long_descr)
      free(ch->player.long_descr);
    if (ch->player.description && ch->player.description != mob_proto[i].player.description)
      free(ch->player.description);
    /* free script proto list if it's not the prototype */
    if (ch->proto_script && ch->proto_script != mob_proto[i].proto_script)
      free_proto_script(ch, MOB_TRIGGER);
  }
  while (ch->affected)
    affect_remove(ch, ch->affected);

  /* free any assigned scripts */
  if (SCRIPT(ch))
    extract_script(ch, MOB_TRIGGER);

    /* Mud Events */
  if (ch->events != NULL) {
	  if (ch->events->iSize > 0) {
		struct event * pEvent;

		while ((pEvent = simple_list(ch->events)) != NULL)
		  event_cancel(pEvent);
	  }
	  free_list(ch->events);
    ch->events = NULL;
  }

  /* new version of free_followers take the followers pointer as arg */
  free_followers(ch->followers);

  if (ch->desc)
    ch->desc->character = NULL;

  /* find_char helper, when free_char is called with a blank character struct,
   * ID is set to 0, and has not yet been added to the lookup table. */
  if (ch->script_id != 0) {
    remove_from_lookup_table(ch->script_id, ch);
  }

  free(ch);
}

/* release memory allocated for an obj struct */
void free_obj(struct obj_data *obj)
{
  if (GET_OBJ_RNUM(obj) == NOWHERE) {
    free_object_strings(obj);
    /* free script proto list */
    free_proto_script(obj, OBJ_TRIGGER);
  } else {
    free_object_strings_proto(obj);
    if (obj->proto_script != obj_proto[GET_OBJ_RNUM(obj)].proto_script)
      free_proto_script(obj, OBJ_TRIGGER);
  }

  /* free any assigned scripts */
  if (SCRIPT(obj))
    extract_script(obj, OBJ_TRIGGER);

  /* find_obj helper (0 is not-yet-added to the table) */
  if (obj->script_id != 0) {
    remove_from_lookup_table(obj->script_id, obj);
  }

  free(obj);
}

/* Steps: 1: Read contents of a text file. 2: Make sure no one is using the
 * pointer in paging. 3: Allocate space. 4: Point 'buf' to it.
 * We don't want to free() the string that someone may be viewing in the pager.
 * page_string() keeps the internal strdup()'d copy on ->showstr_head and it
 * won't care if we delete the original.  Otherwise, strings are kept on
 * ->showstr_vector but we'll only match if the pointer is to the string we're
 * interested in and not a copy. If someone is reading a global copy we're
 * trying to replace, give everybody using it a different copy so as to avoid
 * special cases. */
static int file_to_string_alloc(const char *name, char **buf)
{
  int temppage;
  char temp[MAX_STRING_LENGTH];
  struct descriptor_data *in_use;

  /* A pager reading *buf in place (page_string() with keep_internal 0) is
   * given its own copy below before *buf is freed. This used to return -1
   * first whenever such a pager existed, which made that copy dead code and
   * failed "reload" with a misleading "Cannot read" while anyone paged the
   * file. */

  /* Lets not free() what used to be there unless we succeeded. */
  if (file_to_string(name, temp, sizeof(temp)) < 0)
    return (-1);

  for (in_use = descriptor_list; in_use; in_use = in_use->next) {
    if (!in_use->showstr_count || !in_use->showstr_vector ||
        *in_use->showstr_vector != *buf)
      continue;

    /* Let's be nice and leave them at the page they were on. */
    temppage = in_use->showstr_page;
    paginate_string((in_use->showstr_head = strdup(*in_use->showstr_vector)), in_use);
    in_use->showstr_page = temppage;
  }

  if (*buf)
    free(*buf);
  
  parse_at(temp);   
    
  *buf = strdup(temp);
  return (0);
}

/* Read the text file 'name' into buf, which holds bufsize bytes, converting
 * every line ending to "\r\n". A line longer than one fgets() chunk is joined,
 * not split. Returns 0, or -1 with buf empty and a diagnostic: a file that
 * does not fit is rejected whole rather than shown truncated.
 *
 * The capacity is an explicit parameter because buf is a pointer here:
 * sizeof(buf) would be the pointer size, which once limited every text file
 * to its first 7 bytes. Input is implementor-controlled (lib/text/). */
static int file_to_string(const char *name, char *buf, size_t bufsize)
{
  FILE *fl;
  char tmp[READ_SIZE];
  size_t used = 0, len;
  int partial = FALSE;
  struct stat statbuf;

  if (bufsize == 0)                 /* no room even for the terminator */
    return (-1);
  *buf = '\0';

  if (!(fl = fopen(name, "r"))) {
    TB_REPORT_ERRNO("fopen", name);
    return (-1);
  }

  /* Grab the date/time the file was last edited */
  if (!strcmp(name, NEWS_FILE) || !strcmp(name, MOTD_FILE)) {
    if (fstat(fileno(fl), &statbuf) != 0)
      TB_ERROR_ERRNO("db", "text-file-load", name,
                     "fstat failed; modification time not updated");
    else if (!strcmp(name, NEWS_FILE))
      newsmod = statbuf.st_mtime;
    else
      motdmod = statbuf.st_mtime;
  }

  while (fgets(tmp, sizeof(tmp), fl)) {
    int line_end;

    len = strlen(tmp);
    line_end = len > 0 && tmp[len - 1] == '\n';
    if (!line_end && len + 1 < sizeof(tmp) && !feof(fl)) {
      /* fgets() stopped early without a newline: a NUL byte hid the rest. */
      TB_WARN_MSG("db", "text-file-load", name,
                  "NUL byte after %zu bytes; rest of that line dropped",
                  used + len);
      line_end = TRUE;
    }
    if (line_end)
      while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r'))
        len--;

    if (used + len + (line_end ? 2 : 0) >= bufsize) {
      TB_ERROR_MSG("db", "text-file-load", name,
                   "text exceeds the %zu-byte buffer at %zu bytes; file not loaded",
                   bufsize, used + len);
      *buf = '\0';
      tb_fclose(fl);
      return (-1);
    }
    memcpy(buf + used, tmp, len);
    used += len;
    if (line_end) {
      buf[used++] = '\r';
      buf[used++] = '\n';
    }
    buf[used] = '\0';
    partial = !line_end;
  }

  if (ferror(fl)) {
    TB_ERROR_ERRNO("db", "text-file-load", name,
                   "read failed after %zu bytes; file not loaded", used);
    *buf = '\0';
    tb_fclose(fl);
    return (-1);
  }

  /* Last line without a newline: terminate it like the others. */
  if (partial) {
    if (used + 2 >= bufsize) {
      TB_ERROR_MSG("db", "text-file-load", name,
                   "text exceeds the %zu-byte buffer at %zu bytes; file not loaded",
                   bufsize, used + 2);
      *buf = '\0';
      tb_fclose(fl);
      return (-1);
    }
    buf[used++] = '\r';
    buf[used++] = '\n';
    buf[used] = '\0';
  }

  tb_fclose(fl);

  return (0);
}

/* clear some of the the working variables of a char */
void reset_char(struct char_data *ch)
{
  int i;

  for (i = 0; i < NUM_WEARS; i++)
    GET_EQ(ch, i) = NULL;

  ch->followers = NULL;
  ch->master = NULL;
  IN_ROOM(ch) = NOWHERE;
  ch->carrying = NULL;
  ch->next = NULL;
  ch->next_fighting = NULL;
  ch->next_in_room = NULL;
  FIGHTING(ch) = NULL;
  char_from_furniture(ch);
  ch->char_specials.position = POS_STANDING;
  ch->mob_specials.default_pos = POS_STANDING;
  ch->char_specials.carry_weight = 0;
  ch->char_specials.carry_items = 0;

  if (GET_HIT(ch) <= 0)
    GET_HIT(ch) = 1;
  if (GET_MOVE(ch) <= 0)
    GET_MOVE(ch) = 1;
  if (GET_MANA(ch) <= 0)
    GET_MANA(ch) = 1;

  GET_LAST_TELL(ch) = NOBODY;
}

/* clear ALL the working variables of a char; do NOT free any space alloc'ed */
void clear_char(struct char_data *ch)
{
  memset((char *) ch, 0, sizeof(struct char_data));

  IN_ROOM(ch) = NOWHERE;
  GET_PFILEPOS(ch) = -1;
  GET_MOB_RNUM(ch) = NOBODY;
  GET_WAS_IN(ch) = NOWHERE;
  GET_POS(ch) = POS_STANDING;
  ch->mob_specials.default_pos = POS_STANDING;
  ch->events = NULL;
  
  GET_AC(ch) = 100;		/* Basic Armor */
  if (ch->points.max_mana < 100)
    ch->points.max_mana = 100;
}

void clear_object(struct obj_data *obj)
{
  memset((char *) obj, 0, sizeof(struct obj_data));

  obj->item_number = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  obj->worn_on = NOWHERE;
}

/* Called during character creation after picking character class (and then
 * never again for that character). */
void init_char(struct char_data *ch)
{
  int i;

  /* create a player_special structure */
  if (ch->player_specials == NULL)
    CREATE(ch->player_specials, struct player_special_data, 1);

  /* If this is our first player make him IMPL. */
  if (top_of_p_table == 0) {
    GET_LEVEL(ch) = LVL_IMPL;
    GET_EXP(ch) = 7000000;

    /* The implementor never goes through do_start(). */
    GET_MAX_HIT(ch) = 500;
    GET_MAX_MANA(ch) = 100;
    GET_MAX_MOVE(ch) = 82;
    GET_HIT(ch) = GET_MAX_HIT(ch);
    GET_MANA(ch) = GET_MAX_MANA(ch);
    GET_MOVE(ch) = GET_MAX_MOVE(ch);
  }

  set_title(ch, NULL);
  ch->player.short_descr = NULL;
  ch->player.long_descr = NULL;
  ch->player.description = NULL;

  GET_NUM_QUESTS(ch) = 0;
  ch->player_specials->saved.completed_quests = NULL;
  GET_QUEST(ch) = NOTHING;

  ch->player.time.birth = time(0);
  ch->player.time.logon = time(0);
  ch->player.time.played = 0;

  GET_AC(ch) = 100;

  /* Bias the height and weight of the character depending on what gender
   * they have chosen. While it is possible to have a tall, heavy female it's
   * not as likely as a male. Height is in centimeters. Weight is in pounds.
   * The only place they're ever printed (in stock code) is SPELL_IDENTIFY. */
  if (GET_SEX(ch) == SEX_MALE) {
    GET_WEIGHT(ch) = rand_number(120, 180);
    GET_HEIGHT(ch) = rand_number(160, 200); /* 5'4" - 6'8" */
  } else {
    GET_WEIGHT(ch) = rand_number(100, 160);
    GET_HEIGHT(ch) = rand_number(150, 180); /* 5'0" - 6'0" */
  }

  if ((i = get_ptable_by_name(GET_NAME(ch))) != -1)
    player_table[i].id = GET_IDNUM(ch) = ++top_idnum;
  else
    log("SYSERR: init_char: Character '%s' not found in player table.", GET_NAME(ch));

  for (i = 1; i <= MAX_SKILLS; i++) {
    if (GET_LEVEL(ch) < LVL_IMPL)
      SET_SKILL(ch, i, 0);
    else
      SET_SKILL(ch, i, 100);
  }

  for (i = 0; i < AF_ARRAY_MAX; i++)
    AFF_FLAGS(ch)[i] = 0;

  for (i = 0; i < 5; i++)
    GET_SAVE(ch, i) = 0;

  ch->real_abils.intel = 25;
  ch->real_abils.wis = 25;
  ch->real_abils.dex = 25;
  ch->real_abils.str = 25;
  ch->real_abils.str_add = 100;
  ch->real_abils.con = 25;
  ch->real_abils.cha = 25;

  for (i = 0; i < 3; i++)
    GET_COND(ch, i) = (GET_LEVEL(ch) == LVL_IMPL ? -1 : 24);

  GET_LOADROOM(ch) = NOWHERE;
  GET_SCREEN_WIDTH(ch) = PAGE_WIDTH;
  
  /* Set Beginning Toggles Here */
  SET_BIT_AR(PRF_FLAGS(ch), PRF_AUTOEXIT);
  if (ch->desc)
    if (ch->desc->pProtocol->pVariables[eMSDP_ANSI_COLORS] || 
      ch->desc->pProtocol->pVariables[eMSDP_XTERM_256_COLORS]) {
      SET_BIT_AR(PRF_FLAGS(ch), PRF_COLOR_1);
      SET_BIT_AR(PRF_FLAGS(ch), PRF_COLOR_2);
    } 
  SET_BIT_AR(PRF_FLAGS(ch), PRF_DISPHP);  
  SET_BIT_AR(PRF_FLAGS(ch), PRF_DISPMANA);
  SET_BIT_AR(PRF_FLAGS(ch), PRF_DISPMOVE);
}

/* returns the real number of the room with given virtual number */
room_rnum real_room(room_vnum vnum)
{
  room_rnum bot, top, mid;

  bot = 0;
  top = top_of_world;

  if (world[bot].number > vnum || world[top].number < vnum)
    return (NOWHERE);

  /* perform binary search on world-table */
  while (bot<= top) {
    mid = (bot + top) / 2;

    if ((world + mid)->number == vnum)
      return (mid);
    if ((world + mid)->number > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOWHERE);
}

/* returns the real number of the monster with given virtual number */
mob_rnum real_mobile(mob_vnum vnum)
{
  mob_rnum bot, top, mid;

  bot = 0;
  top = top_of_mobt;

  /* quickly reject out-of-range vnums */
  if (mob_index[bot].vnum > vnum || mob_index[top].vnum < vnum)
    return (NOBODY);

  /* perform binary search on mob-table */
  while (bot <= top) {
    mid = (bot + top) / 2;

    if ((mob_index + mid)->vnum == vnum)
      return (mid);
    if ((mob_index + mid)->vnum > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOBODY);
}

/* returns the real number of the object with given virtual number */
obj_rnum real_object(obj_vnum vnum)
{
  obj_rnum bot, top, mid;

  bot = 0;
  top = top_of_objt;

  /* quickly reject out-of-range vnums */
  if (obj_index[bot].vnum > vnum || obj_index[top].vnum < vnum)
    return (NOTHING);

  /* perform binary search on obj-table */
  while (bot <= top) {
    mid = (bot + top) / 2;

    if ((obj_index + mid)->vnum == vnum)
      return (mid);
    if ((obj_index + mid)->vnum > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOTHING);
}

/* returns the real number of the zone with given virtual number */
zone_rnum real_zone(zone_vnum vnum)
{
  zone_rnum bot, top, mid;

  bot = 0;
  top = top_of_zone_table;

  if (zone_table[bot].number > vnum || zone_table[top].number < vnum)
    return (NOWHERE);

  /* perform binary search on zone-table */
  while (bot <= top) {
    mid = (bot + top) / 2;

    if ((zone_table + mid)->number == vnum)
      return (mid);
    if ((zone_table + mid)->number > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOWHERE);
}

/* Extend later to include more checks and add checks for unknown bitvectors. */
static int check_object(struct obj_data *obj)
{
  char objname[MAX_INPUT_LENGTH + 32];
  int error = FALSE, y;

  if (GET_OBJ_WEIGHT(obj) < 0 && (error = TRUE))
    log("SYSERR: Object #%d (%s) has negative weight (%d).",
	GET_OBJ_VNUM(obj), obj->short_description, GET_OBJ_WEIGHT(obj));

  if (GET_OBJ_RENT(obj) < 0 && (error = TRUE))
    log("SYSERR: Object #%d (%s) has negative cost/day (%d).",
	GET_OBJ_VNUM(obj), obj->short_description, GET_OBJ_RENT(obj));

  tb_snprintf(objname, sizeof(objname), "Object #%d (%s)", GET_OBJ_VNUM(obj), obj->short_description);
  for(y = 0; y < TW_ARRAY_MAX; y++) {
    error |= check_bitvector_names((unsigned int)GET_OBJ_WEAR(obj)[y], wear_bits_count, objname, "object wear");
    error |= check_bitvector_names((unsigned int)GET_OBJ_EXTRA(obj)[y], extra_bits_count, objname, "object extra");
    error |= check_bitvector_names((unsigned int)GET_OBJ_AFFECT(obj)[y], affected_bits_count, objname, "object affect");
  }

  switch (GET_OBJ_TYPE(obj)) {
  case ITEM_DRINKCON:
  {
    char onealias[MAX_INPUT_LENGTH], *space = strrchr(obj->name, ' ');

    strlcpy(onealias, space ? space + 1 : obj->name, sizeof(onealias));
    if (search_block(onealias, drinknames, TRUE) < 0 && (error = TRUE))
      log("SYSERR: Object #%d (%s) doesn't have drink type as last keyword. (%s)",
		GET_OBJ_VNUM(obj), obj->short_description, obj->name);
  }
  /* Fall through. */
  case ITEM_FOUNTAIN:
    /* Value 2 is the liquid type and indexes drinks[], drink_aff[] and
     * drinknames[] (do_drink, shops). oedit only offers valid liquids, so
     * an out-of-range value comes from a hand-edited .obj file: report it
     * and make the container hold water. */
    if (!VALID_LIQ(GET_OBJ_VAL(obj, 2))) {
      TB_ERROR_MSG("db", "object-check", "liquid",
                   "obj vnum %d liquid %d out of range 0..%d; using LIQ_WATER",
                   GET_OBJ_VNUM(obj), GET_OBJ_VAL(obj, 2), NUM_LIQ_TYPES - 1);
      GET_OBJ_VAL(obj, 2) = LIQ_WATER;
      error = TRUE;
    }
    if ((GET_OBJ_VAL(obj,0) > 0) && (GET_OBJ_VAL(obj, 1) > GET_OBJ_VAL(obj, 0)
        && (error = TRUE)))
      log("SYSERR: Object #%d (%s) contains (%d) more than maximum (%d).",
		GET_OBJ_VNUM(obj), obj->short_description,
		GET_OBJ_VAL(obj, 1), GET_OBJ_VAL(obj, 0));
    break;
  case ITEM_SCROLL:
  case ITEM_POTION:
    error |= check_object_level(obj, 0);
    error |= check_object_spell_number(obj, 1);
    error |= check_object_spell_number(obj, 2);
    error |= check_object_spell_number(obj, 3);
    break;
  case ITEM_WAND:
  case ITEM_STAFF:
    error |= check_object_level(obj, 0);
    error |= check_object_spell_number(obj, 3);
    if (GET_OBJ_VAL(obj, 2) > GET_OBJ_VAL(obj, 1) && (error = TRUE))
      log("SYSERR: Object #%d (%s) has more charges (%d) than maximum (%d).",
		GET_OBJ_VNUM(obj), obj->short_description,
		GET_OBJ_VAL(obj, 2), GET_OBJ_VAL(obj, 1));
    break;
  case ITEM_NOTE:
    if (obj->ex_description) {
      char onealias[MAX_INPUT_LENGTH],*next_name;
      next_name = any_one_arg(obj->name, onealias);
      do {
  	if (find_exdesc(onealias, obj->ex_description) && (error = TRUE)) {
	log("SYSERR: Object #%d (%s) is type NOTE and has extra description with same name. (%s)",
	  GET_OBJ_VNUM(obj), obj->short_description, obj->name);
        }
  	next_name = any_one_arg(next_name, onealias);
      } while (*onealias);
    }
  break;
  case ITEM_FURNITURE:
    if (GET_OBJ_VAL(obj, 1) > GET_OBJ_VAL(obj, 0) && (error = TRUE))
      log("SYSERR: Object #%d (%s) contains (%d) more than maximum (%d).",
          GET_OBJ_VNUM(obj), obj->short_description, GET_OBJ_VAL(obj, 1),
	  GET_OBJ_VAL(obj, 0));
    break;
  case ITEM_WEAPON:
    /* Value 3 is the attack type: hit()/dam_message() and 'stat' index
     * attack_hit_text[] with it. oedit only offers valid types, so an
     * out-of-range one comes from a hand-edited .obj file: report it and
     * make the weapon hit (type 0). */
    if (!VALID_ATTACK(GET_OBJ_VAL(obj, 3))) {
      TB_ERROR_MSG("db", "object-check", "attack type",
                   "obj vnum %d attack type %d out of range 0..%d; using 0 (hit)",
                   GET_OBJ_VNUM(obj), GET_OBJ_VAL(obj, 3), NUM_ATTACK_TYPES - 1);
      GET_OBJ_VAL(obj, 3) = 0;
      error = TRUE;
    }
    /* Values 1 and 2 are the damage dice hit() rolls on every blow: dice()
     * loops value 1 times and sums into an int, so a huge count stalls the
     * game loop and a huge product overflows. A value past oedit's limit
     * comes from a hand-edited .obj file: clamp it and report it. */
    if (!VALID_WEAPON_DICE(GET_OBJ_VAL(obj, 1), GET_OBJ_VAL(obj, 2))) {
      int ndice = LIMIT(GET_OBJ_VAL(obj, 1), 0, MAX_WEAPON_NDICE);
      int sdice = LIMIT(GET_OBJ_VAL(obj, 2), 0, MAX_WEAPON_SDICE);

      TB_ERROR_MSG("db", "object-check", "damage dice",
                   "obj vnum %d damage dice %dd%d outside 0..%dd0..%d; using %dd%d",
                   GET_OBJ_VNUM(obj), GET_OBJ_VAL(obj, 1), GET_OBJ_VAL(obj, 2),
                   MAX_WEAPON_NDICE, MAX_WEAPON_SDICE, ndice, sdice);
      GET_OBJ_VAL(obj, 1) = ndice;
      GET_OBJ_VAL(obj, 2) = sdice;
      error = TRUE;
    }
    break;
  }

  return (error);
}

static int check_object_spell_number(struct obj_data *obj, int val)
{
  int error = FALSE;
  const char *spellname;

  if (GET_OBJ_VAL(obj, val) == -1 || GET_OBJ_VAL(obj, val) == 0) /* no spell */
    return (error);

  /* Check for negative spells, spells beyond the top define, and any spell
   * which is actually a skill. */
  if (GET_OBJ_VAL(obj, val) < 0)
    error = TRUE;
  if (GET_OBJ_VAL(obj, val) > TOP_SPELL_DEFINE)
    error = TRUE;
  if (GET_OBJ_VAL(obj, val) > MAX_SPELLS && GET_OBJ_VAL(obj, val) <= MAX_SKILLS)
    error = TRUE;
  if (error)
    log("SYSERR: Object #%d (%s) has out of range spell #%d.",
	GET_OBJ_VNUM(obj), obj->short_description, GET_OBJ_VAL(obj, val));

  if (scheck)		/* Spell names don't exist in syntax check mode. */
    return (error);

  /* Now check for unnamed spells. */
  spellname = skill_name(GET_OBJ_VAL(obj, val));

  if ((spellname == unused_spellname || !str_cmp("UNDEFINED", spellname)) && (error = TRUE))
    log("SYSERR: Object #%d (%s) uses '%s' spell #%d.",
		GET_OBJ_VNUM(obj), obj->short_description, spellname,
		GET_OBJ_VAL(obj, val));

  return (error);
}

static int check_object_level(struct obj_data *obj, int val)
{
  int error = FALSE;

  if ((GET_OBJ_VAL(obj, val) < 0 || GET_OBJ_VAL(obj, val) > LVL_IMPL) && (error = TRUE))
    log("SYSERR: Object #%d (%s) has out of range level #%d.",
	GET_OBJ_VNUM(obj), obj->short_description, GET_OBJ_VAL(obj, val));

  return (error);
}

static int check_bitvector_names(bitvector_t bits, size_t namecount, const char *whatami, const char *whatbits)
{
  const size_t width = sizeof(bitvector_t) * CHAR_BIT;
  bitvector_t known_mask;
  size_t flagnum;
  bool error = FALSE;

  /*
   * Avoid shifts by the width of the type.  If namecount covers the whole
   * bitvector there cannot be an unknown flag; if it is zero, no bits are
   * known and the mask is therefore zero.
   */
  if (namecount >= width)
    return (FALSE);

  known_mask = (namecount == 0)
                 ? (bitvector_t)0
                 : (~(bitvector_t)0 >> (width - namecount));

  if ((bits & ~known_mask) == 0)
    return (FALSE);

  for (flagnum = namecount; flagnum < width; flagnum++) {
    if ((((bitvector_t)1u << flagnum) & bits) != 0) {
      log("SYSERR: %s has unknown %s flag, bit %zu (0 through %d known).",
          whatami, whatbits, flagnum, (int)namecount - 1);
      error = TRUE;
    }
  }

  return (error);
}

static void load_default_config( void )
{
  /* This function is called only once, at boot-time. We assume config_info is
   * empty. -Welcor */
  /* Game play options. */
  CONFIG_PK_SETTING 	          = pk_setting;
  CONFIG_PT_SETTING             = pt_setting;
  CONFIG_LEVEL_CAN_SHOUT 	    = level_can_shout;
  CONFIG_HOLLER_MOVE_COST 	    = holler_move_cost;
  CONFIG_TUNNEL_SIZE 	        = tunnel_size;
  CONFIG_MAX_EXP_GAIN	        = max_exp_gain;
  CONFIG_MAX_EXP_LOSS 	        = max_exp_loss;
  CONFIG_MAX_NPC_CORPSE_TIME    = max_npc_corpse_time;
  CONFIG_MAX_PC_CORPSE_TIME	    = max_pc_corpse_time;
  CONFIG_IDLE_VOID		        = idle_void;
  CONFIG_IDLE_RENT_TIME	        = idle_rent_time;
  CONFIG_IDLE_MAX_LEVEL	        = idle_max_level;
  CONFIG_DTS_ARE_DUMPS	        = dts_are_dumps;
  CONFIG_LOAD_INVENTORY         = load_into_inventory;
  CONFIG_OK			= strdup(OK);
  CONFIG_HUH			= strdup(HUH);
  CONFIG_NOPERSON	        = strdup(NOPERSON);
  CONFIG_NOEFFECT	        = strdup(NOEFFECT);
  CONFIG_TRACK_T_DOORS          = track_through_doors;
  CONFIG_NO_MORT_TO_IMMORT	    = no_mort_to_immort;
  CONFIG_DISP_CLOSED_DOORS      = display_closed_doors;
  CONFIG_PROTOCOL_NEGOTIATION   = protocol_negotiation;
  CONFIG_SPECIAL_IN_COMM        = special_in_comm;
  CONFIG_DIAGONAL_DIRS          = diagonal_dirs;
  CONFIG_MAP                    = map_option;
  CONFIG_MAP_SIZE               = default_map_size;
  CONFIG_MINIMAP_SIZE           = default_minimap_size;
  CONFIG_SCRIPT_PLAYERS         = script_players;
  CONFIG_DEBUG_MODE             = debug_mode;

  /* Rent / crashsave options. */
  CONFIG_FREE_RENT              = free_rent;
  CONFIG_MAX_OBJ_SAVE           = max_obj_save;
  CONFIG_MIN_RENT_COST	        = min_rent_cost;
  CONFIG_AUTO_SAVE		        = auto_save;
  CONFIG_AUTOSAVE_TIME	        = autosave_time;
  CONFIG_CRASH_TIMEOUT          = crash_file_timeout;
  CONFIG_RENT_TIMEOUT	        = rent_file_timeout;

  /* Room numbers. */
  CONFIG_MORTAL_START           = mortal_start_room;
  CONFIG_IMMORTAL_START         = immort_start_room;
  CONFIG_FROZEN_START           = frozen_start_room;
  CONFIG_DON_ROOM_1             = donation_room_1;
  CONFIG_DON_ROOM_2             = donation_room_2;
  CONFIG_DON_ROOM_3             = donation_room_3;

  /* Game operation options. */
  CONFIG_DFLT_PORT              = DFLT_PORT;

  if (DFLT_IP)
    CONFIG_DFLT_IP              = strdup(DFLT_IP);
  else
    CONFIG_DFLT_IP              = NULL;

  CONFIG_DFLT_DIR               = strdup(DFLT_DIR);

  if (LOGNAME)
    CONFIG_LOGNAME              = strdup(LOGNAME);
  else
    CONFIG_LOGNAME              = NULL;

  CONFIG_MAX_PLAYING            = max_playing;
  CONFIG_MAX_FILESIZE           = max_filesize;
  CONFIG_MAX_BAD_PWS            = max_bad_pws;
  CONFIG_SITEOK_ALL             = siteok_everyone;
  CONFIG_NS_IS_SLOW             = nameserver_is_slow;
  CONFIG_NEW_SOCIALS            = use_new_socials;
  CONFIG_OLC_SAVE               = auto_save_olc;
  CONFIG_MENU                   = strdup(MENU);
  CONFIG_WELC_MESSG             = strdup(WELC_MESSG);
  CONFIG_START_MESSG            = strdup(START_MESSG);
  CONFIG_MEDIT_ADVANCED         = medit_advanced_stats;
  CONFIG_IBT_AUTOSAVE           = ibt_autosave;
  /* Autowiz options. */
  CONFIG_USE_AUTOWIZ            = use_autowiz;
  CONFIG_MIN_WIZLIST_LEV        = min_wizlist_lev;
}

/** Load the game configuration from CONFIG_CONFFILE (or the default
 * directory), over the built-in defaults from load_default_config().
 *
 * Each line is "tag = value" read with get_line() into a READ_SIZE buffer.
 * Numeric tags use tb_parse_int_or_zero(), so a non-numeric value becomes 0,
 * and unknown tags are ignored. "menu", "start_messg" and "welc_messg" read
 * a '~'-terminated string on the following lines; an empty menu or start
 * message falls back to the built-in default (welc_messg may stay NULL).
 * The file is written by cedit, so its contents are immortal-controlled. */
void load_config( void )
{
  FILE *fl;
  /* get_line() writes up to READ_SIZE bytes (READ_SIZE - 1 characters and
   * the NUL). The values that get "\r\n" re-added are copied into their own
   * READ_SIZE + 2 buffers below. */
  char line[READ_SIZE];
  char tag[MAX_INPUT_LENGTH];
  int  num;
  char buf[MAX_INPUT_LENGTH];

  load_default_config();

  tb_snprintf(buf, sizeof(buf), "%s/%s", CONFIG_DFLT_DIR, CONFIG_CONFFILE);
  if ( !(fl = fopen(CONFIG_CONFFILE, "r")) && !(fl = fopen(buf, "r")) ) {
    TB_REPORT_ERRNO("fopen", buf);
    log("SYSERR: No %s file found; using defaults.", CONFIG_CONFFILE);
    return;
  }

  /* Load the game configuration file. */
  while (get_line(fl, line)) {
    split_argument(line, tag);
    num = tb_parse_int_or_zero(line);

    switch (LOWER(*tag)) {
      case 'a':
        if (!str_cmp(tag, "auto_save"))
          CONFIG_AUTO_SAVE = num;
        else if (!str_cmp(tag, "autosave_time"))
          CONFIG_AUTOSAVE_TIME = num;
        else if (!str_cmp(tag, "auto_save_olc"))
          CONFIG_OLC_SAVE = num;
        break;

      case 'c':
        if (!str_cmp(tag, "crash_file_timeout"))
          CONFIG_CRASH_TIMEOUT = num;
        break;

      case 'd':
        if (!str_cmp(tag, "debug_mode"))
          CONFIG_DEBUG_MODE = num;
        else if (!str_cmp(tag, "display_closed_doors"))
          CONFIG_DISP_CLOSED_DOORS = num;
        else if (!str_cmp(tag, "diagonal_dirs"))
          CONFIG_DIAGONAL_DIRS = num;
        else if (!str_cmp(tag, "dts_are_dumps"))
          CONFIG_DTS_ARE_DUMPS = num;
        else if (!str_cmp(tag, "donation_room_1"))
          if (num == -1)
            CONFIG_DON_ROOM_1 = NOWHERE;
          else
            CONFIG_DON_ROOM_1 = num;
        else if (!str_cmp(tag, "donation_room_2"))
          if (num == -1)
            CONFIG_DON_ROOM_2 = NOWHERE;
          else
            CONFIG_DON_ROOM_2 = num;
        else if (!str_cmp(tag, "donation_room_3"))
          if (num == -1)
            CONFIG_DON_ROOM_3 = NOWHERE;
          else
            CONFIG_DON_ROOM_3 = num;
        else if (!str_cmp(tag, "dflt_dir")) {
          if (CONFIG_DFLT_DIR)
            free(CONFIG_DFLT_DIR);
          if (*line)
            CONFIG_DFLT_DIR = strdup(line);
          else
            CONFIG_DFLT_DIR = strdup(DFLT_DIR);
        } else if (!str_cmp(tag, "dflt_ip")) {
          if (CONFIG_DFLT_IP)
            free(CONFIG_DFLT_IP);
          if (*line)
            CONFIG_DFLT_IP = strdup(line);
          else
            CONFIG_DFLT_IP = NULL;
        } else if (!str_cmp(tag, "dflt_port"))
          CONFIG_DFLT_PORT = num;
        else if (!str_cmp(tag, "default_map_size"))
          CONFIG_MAP_SIZE = num;
        else if (!str_cmp(tag, "default_minimap_size"))
          CONFIG_MINIMAP_SIZE = num;
        break;

      case 'f':
        if (!str_cmp(tag, "free_rent"))
          CONFIG_FREE_RENT = num;
        else if (!str_cmp(tag, "frozen_start_room"))
          CONFIG_FROZEN_START = num;
        break;

      case 'h':
        if (!str_cmp(tag, "holler_move_cost"))
          CONFIG_HOLLER_MOVE_COST = num;
        else if (!str_cmp(tag, "huh")) {
          char tmp[READ_SIZE + 2];	/* line plus "\r\n" */
          if (CONFIG_HUH)
            free(CONFIG_HUH);
          tb_snprintf(tmp, sizeof(tmp), "%s\r\n", line);
          CONFIG_HUH = strdup(tmp);
        }
        break;

      case 'i':
        if (!str_cmp(tag, "idle_void"))
          CONFIG_IDLE_VOID = num;
        else if (!str_cmp(tag, "idle_rent_time"))
          CONFIG_IDLE_RENT_TIME = num;
        else if (!str_cmp(tag, "idle_max_level"))
          CONFIG_IDLE_MAX_LEVEL = num;
        else if (!str_cmp(tag, "immort_start_room"))
          CONFIG_IMMORTAL_START = num;
        else if (!str_cmp(tag, "ibt_autosave"))
		  CONFIG_IBT_AUTOSAVE = num;
        break;

      case 'l':
        if (!str_cmp(tag, "level_can_shout"))
          CONFIG_LEVEL_CAN_SHOUT = num;
        else if (!str_cmp(tag, "load_into_inventory"))
          CONFIG_LOAD_INVENTORY = num;
        else if (!str_cmp(tag, "logname")) {
          if (CONFIG_LOGNAME)
            free(CONFIG_LOGNAME);
          if (*line)
            CONFIG_LOGNAME = strdup(line);
          else
            CONFIG_LOGNAME = NULL;
        }
        break;

      case 'm':
        if (!str_cmp(tag, "max_bad_pws"))
          CONFIG_MAX_BAD_PWS = num;
        else if (!str_cmp(tag, "max_exp_gain"))
          CONFIG_MAX_EXP_GAIN = num;
        else if (!str_cmp(tag, "max_exp_loss"))
          CONFIG_MAX_EXP_LOSS = num;
        else if (!str_cmp(tag, "max_filesize"))
          CONFIG_MAX_FILESIZE = num;
        else if (!str_cmp(tag, "max_npc_corpse_time"))
          CONFIG_MAX_NPC_CORPSE_TIME = num;
        else if (!str_cmp(tag, "max_obj_save"))
          CONFIG_MAX_OBJ_SAVE = num;
        else if (!str_cmp(tag, "max_pc_corpse_time"))
          CONFIG_MAX_PC_CORPSE_TIME = num;
        else if (!str_cmp(tag, "max_playing"))
          CONFIG_MAX_PLAYING = num;
        else if (!str_cmp(tag, "menu")) {
          if (CONFIG_MENU)
            free(CONFIG_MENU);
          strncpy(buf, "Reading menu in load_config()", sizeof(buf));
          /* fread_string() already ran parse_at(), and returns NULL for an
           * empty value, which every reader of CONFIG_MENU dereferences. */
          CONFIG_MENU = fread_string(fl, buf);
          if (!CONFIG_MENU) {
            TB_ERROR_MSG("db", "config-load", "menu",
                         "empty 'menu' value in %s; using built-in default",
                         CONFIG_CONFFILE);
            CONFIG_MENU = strdup(MENU);
          }
        } else if (!str_cmp(tag, "min_rent_cost"))
          CONFIG_MIN_RENT_COST = num;
        else if (!str_cmp(tag, "min_wizlist_lev"))
          CONFIG_MIN_WIZLIST_LEV = num;
        else if (!str_cmp(tag, "mortal_start_room"))
          CONFIG_MORTAL_START = num;
        else if (!str_cmp(tag, "map_option"))
          CONFIG_MAP = num;
        else if (!str_cmp(tag, "medit_advanced_stats"))
          CONFIG_MEDIT_ADVANCED = num;
        break;

      case 'n':
        if (!str_cmp(tag, "nameserver_is_slow"))
          CONFIG_NS_IS_SLOW = num;
        else if (!str_cmp(tag, "no_mort_to_immort"))
          CONFIG_NO_MORT_TO_IMMORT = num;
        else if (!str_cmp(tag, "noperson")) {
          char tmp[READ_SIZE + 2];	/* line plus "\r\n" */
          if (CONFIG_NOPERSON)
            free(CONFIG_NOPERSON);
          tb_snprintf(tmp, sizeof(tmp), "%s\r\n", line);
          CONFIG_NOPERSON = strdup(tmp);
        } else if (!str_cmp(tag, "noeffect")) {
          char tmp[READ_SIZE + 2];	/* line plus "\r\n" */
          if (CONFIG_NOEFFECT)
            free(CONFIG_NOEFFECT);
          tb_snprintf(tmp, sizeof(tmp), "%s\r\n", line);
          CONFIG_NOEFFECT = strdup(tmp);
        }
        break;

      case 'o':
        if (!str_cmp(tag, "ok")) {
          char tmp[READ_SIZE + 2];	/* line plus "\r\n" */
          if (CONFIG_OK)
            free(CONFIG_OK);
          tb_snprintf(tmp, sizeof(tmp), "%s\r\n", line);
          CONFIG_OK = strdup(tmp);
        }
        break;

      case 'p':
        if (!str_cmp(tag, "pk_setting"))
          CONFIG_PK_SETTING = num;
        else if (!str_cmp(tag, "protocol_negotiation"))
          CONFIG_PROTOCOL_NEGOTIATION = num;
        else if (!str_cmp(tag, "pt_setting"))
          CONFIG_PT_SETTING = num;
        break;

      case 'r':
        if (!str_cmp(tag, "rent_file_timeout"))
          CONFIG_RENT_TIMEOUT = num;
        break;

      case 's':
        if (!str_cmp(tag, "siteok_everyone"))
          CONFIG_SITEOK_ALL = num;
        else if (!str_cmp(tag, "script_players"))
          CONFIG_SCRIPT_PLAYERS = num;
        else if (!str_cmp(tag, "special_in_comm"))
          CONFIG_SPECIAL_IN_COMM = num;
        else if (!str_cmp(tag, "start_messg")) {
          strncpy(buf, "Reading start message in load_config()", sizeof(buf));
          if (CONFIG_START_MESSG)
            free(CONFIG_START_MESSG);
          /* See "menu": no second parse_at(), and never a NULL message. */
          CONFIG_START_MESSG = fread_string(fl, buf);
          if (!CONFIG_START_MESSG) {
            TB_ERROR_MSG("db", "config-load", "start_messg",
                         "empty 'start_messg' value in %s; using built-in default",
                         CONFIG_CONFFILE);
            CONFIG_START_MESSG = strdup(START_MESSG);
          }
        }
        break;

      case 't':
        if (!str_cmp(tag, "tunnel_size"))
          CONFIG_TUNNEL_SIZE = num;
        else if (!str_cmp(tag, "track_through_doors"))
          CONFIG_TRACK_T_DOORS = num;
        break;

      case 'u':
        if (!str_cmp(tag, "use_autowiz"))
          CONFIG_USE_AUTOWIZ = num;
        else if (!str_cmp(tag, "use_new_socials"))
          CONFIG_NEW_SOCIALS = num;
        break;

      case 'w':
        if (!str_cmp(tag, "welc_messg")) {
          strncpy(buf, "Reading welcome message in load_config()", sizeof(buf));
          if (CONFIG_WELC_MESSG)
            free(CONFIG_WELC_MESSG);
          CONFIG_WELC_MESSG = fread_string(fl, buf);
        }
        break;

      default:
        break;
    }
  }

  tb_fclose(fl);
}
