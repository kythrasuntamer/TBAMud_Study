/**************************************************************************
*  File: interpreter.c                                     Part of tbaMUD *
*  Usage: Parse user commands, search for specials, call ACMD functions.  *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

/**
 * @file interpreter.c
 * The command interpreter (command_interpreter(), aliases, argument
 * splitters) and the login state machine nanny().
 *
 * Who controls the input: every argument string here is a line typed by a
 * remote client, bounded to MAX_INPUT_LENGTH by comm.c, and nanny() runs
 * before the client has authenticated. Other descriptors seen while logging
 * in can be at any CON_* state, so their characters may have no name or
 * idnum yet. Argument splitters write at most MAX_INPUT_LENGTH bytes to each
 * output buffer.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "db.h"
#include "spells.h"
#include "handler.h"
#include "mail.h"
#include "screen.h"
#include "genolc.h"
#include "oasis.h"
#include "improved-edit.h"
#include "dg_scripts.h"
#include "constants.h"
#include "act.h" /* ACMDs located within the act*.c files */
#include "ban.h"
#include "class.h"
#include "graph.h"
#include "hedit.h"
#include "house.h"
#include "config.h"
#include "modify.h" /* for do_skillset... */
#include "quest.h"
#include "asciimap.h"
#include "prefedit.h"
#include "ibt.h"
#include "mud_event.h"

/* libsodium's encoded Argon2id string includes algorithm parameters and salt.
 * Require libsodium for new hashes; never fall back to plaintext. */
#include <sodium.h>

/* Recognize the stored Argon2id string prefix.  This is format selection,
 * not password verification or validation of the encoded hash.
 */
static int tb_password_is_argon2id(const char *hash)
{
  return (hash != NULL && strncmp(hash, "$argon2id$", 10) == 0);
}

/* tb_password_verify() results.  TB_PW_UNVERIFIABLE means this build
 * cannot check the stored hash at all (a legacy hash without crypt(), or
 * libsodium failed to start): the server's fault, not the player's, so it
 * is reported and never counted as a wrong password. */
enum tb_pw_result { TB_PW_MISMATCH = 0, TB_PW_MATCH = 1, TB_PW_UNVERIFIABLE = 2 };

/* Reports an unverifiable password check, rate limited (defined below,
 * after the login rate table it shares). */
static void tb_pw_unverifiable_at(const char *resource, long idnum,
                                  const char *name, const char *why,
                                  const char *file, int line,
                                  const char *func);

/* What a player is told when the server cannot check a password. */
#define TB_PW_CANNOT_CHECK \
  "\r\nYour password cannot be checked on this server right now.\r\n" \
  "Please contact an administrator.\r\n"

/* Verify a password against the saved hash of player idnum / name.
 * New hashes use libsodium; existing crypt hashes are accepted only when
 * that compatibility path is enabled.  Never falls back to comparing
 * plaintext.  A hash this build cannot check is reported (password-verify,
 * rate limited) with the idnum and name at the caller's location, and the
 * caller must not treat it as a match.
 */
static enum tb_pw_result tb_password_verify_at(const char *password,
                                               const char *hash, long idnum,
                                               const char *name,
                                               const char *file, int line,
                                               const char *func)
{
  if (password == NULL || hash == NULL || !*hash)
    return (TB_PW_MISMATCH);
  if (sodium_init() < 0) {
    tb_pw_unverifiable_at("libsodium", idnum, name, "sodium_init failed",
                          file, line, func);
    return (TB_PW_UNVERIFIABLE);
  }
  if (tb_password_is_argon2id(hash))
    return (crypto_pwhash_str_verify(hash, password, strlen(password)) == 0
            ? TB_PW_MATCH : TB_PW_MISMATCH);
#if defined(CIRCLE_CRYPT) && !defined(NOCRYPT)
  {
    /* Reject invalid crypt results; do not allow a plaintext fallback. */
    const char *legacy = crypt(password, hash);

    return (legacy != NULL && strcmp(legacy, hash) == 0
            ? TB_PW_MATCH : TB_PW_MISMATCH);
  }
#else
  {
    /* Spend the same Argon2id work as a real mismatch, so the response time
     * does not reveal which accounts still hold a legacy hash.  The dummy
     * hash uses the production parameters and is computed once. */
    static char dummy[crypto_pwhash_STRBYTES];
    static int dummy_ready;
    static const char dummy_secret[] = "tbamud-legacy-timing-dummy";

    if (!dummy_ready)
      dummy_ready = crypto_pwhash_str_alg(dummy, dummy_secret,
                                          sizeof(dummy_secret) - 1,
                                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                          crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                          crypto_pwhash_ALG_ARGON2ID13) == 0;
    if (dummy_ready) {
      /* The result is irrelevant (a constant secret); only the work counts. */
      int dummy_result = crypto_pwhash_str_verify(dummy, password,
                                                  strlen(password));
      (void)dummy_result;
    }
  }
  tb_pw_unverifiable_at("crypt", idnum, name,
                        "legacy (non-Argon2id) hash but this build has no crypt()",
                        file, line, func);
  return (TB_PW_UNVERIFIABLE);
#endif
}
#define tb_password_verify(password, hash, idnum, name) \
  tb_password_verify_at((password), (hash), (idnum), (name), \
                        __FILE__, __LINE__, __func__)

/* Write a new Argon2id hash into caller-owned storage.
 * The buffer needs crypto_pwhash_STRBYTES bytes.  A false result means the
 * output must not be saved as a valid password hash.
 */
static int tb_password_hash(char *output, size_t capacity, const char *password)
{
  if (output == NULL || password == NULL ||
      capacity < crypto_pwhash_STRBYTES || sodium_init() < 0)
    return (0);
  return (crypto_pwhash_str_alg(output, password, strlen(password),
                               crypto_pwhash_OPSLIMIT_INTERACTIVE,
                               crypto_pwhash_MEMLIMIT_INTERACTIVE,
                               crypto_pwhash_ALG_ARGON2ID13) == 0);
}

/* local (file scope) functions */
static int perform_dupe_check(struct descriptor_data *d);
static struct alias_data *find_alias(struct alias_data *alias_list, char *str);
static int perform_complex_alias(struct txt_q *input_q, char *orig, struct alias_data *a);
static int parse_name_internal(char *arg, char *name);
static bool perform_new_char_dupe_check(struct descriptor_data *d);
/* sort_commands utility */
static int sort_commands_helper(const void *a, const void *b);

/* Shared across login descriptors: switching IP addresses cannot reset the
 * failed-password budget for a character. Bounded memory, no allocations.
 * Limits can also affect legitimate users during an attack on their account. */
#define TB_LOGIN_RATE_SLOTS 512
#define TB_LOGIN_WINDOW 60
#define TB_LOGIN_MAX_FAILURES 8

struct tb_login_rate_entry {
  char account[MAX_NAME_LENGTH + 1];
  time_t window_start;
  time_t last_seen;
  unsigned int failures;
  unsigned int pw_reports;     /* password-verify reports this window */
  unsigned int pw_suppressed;  /* reports withheld since the last one */
};

static struct tb_login_rate_entry tb_login_rates[TB_LOGIN_RATE_SLOTS];

static struct tb_login_rate_entry *tb_login_rate_entry_for(const char *name,
                                                           time_t now)
{
  size_t i, slot = 0;
  struct tb_login_rate_entry *entry;

  if (name == NULL || *name == '\0')
    return NULL;

  for (i = 0; i < TB_LOGIN_RATE_SLOTS; ++i) {
    if (strcmp(tb_login_rates[i].account, name) == 0) {
      slot = i;
      break;
    }
    if (tb_login_rates[i].account[0] == '\0') {
      slot = i;
      break;
    }
    if (tb_login_rates[i].last_seen < tb_login_rates[slot].last_seen)
      slot = i;
  }

  entry = &tb_login_rates[slot];
  if (strcmp(entry->account, name) != 0)
    entry->pw_suppressed = 0;          /* a different account's slot */
  if (strcmp(entry->account, name) != 0 ||
      now < entry->window_start ||
      now - entry->window_start >= TB_LOGIN_WINDOW) {
    strlcpy(entry->account, name, sizeof(entry->account));
    entry->window_start = now;
    entry->failures = 0;
    entry->pw_reports = 0;
  }
  entry->last_seen = now;
  return entry;
}

/* password-verify reports allowed per TB_LOGIN_WINDOW: per account (a
 * reconnect loop against one legacy-hash account) and from all accounts. */
#define TB_PW_REPORT_BURST 5
#define TB_PW_REPORT_GLOBAL 20

static void tb_pw_unverifiable_at(const char *resource, long idnum,
                                  const char *name, const char *why,
                                  const char *file, int line,
                                  const char *func)
{
  static time_t global_start;
  static unsigned int global_reports, global_suppressed;
  time_t now = time(NULL);
  struct tb_login_rate_entry *rate = tb_login_rate_entry_for(name, now);
  int over_account, over_global;
  unsigned int earlier;

  if (now < global_start || now - global_start >= TB_LOGIN_WINDOW) {
    global_start = now;
    global_reports = 0;
  }
  over_account = rate != NULL && rate->pw_reports >= TB_PW_REPORT_BURST;
  over_global = global_reports >= TB_PW_REPORT_GLOBAL;
  if (over_account || over_global) {
    unsigned int *withheld = over_account ? &rate->pw_suppressed
                                          : &global_suppressed;

    if ((*withheld)++ == 0)
      tb_error_log_at("WARNING", "login", "password-verify-suppressed",
                      resource, 0, file, line, func,
                      "player idnum %ld name %s: further password-verify "
                      "reports %s are withheld for up to %d s",
                      idnum, name ? name : "(none)",
                      over_account ? "for this account" : "from all accounts",
                      TB_LOGIN_WINDOW);
    return;
  }
  earlier = global_suppressed + (rate != NULL ? rate->pw_suppressed : 0);
  if (rate != NULL) {
    rate->pw_reports++;
    rate->pw_suppressed = 0;
  }
  global_reports++;
  global_suppressed = 0;
  tb_error_log_at("ERROR", "login", "password-verify", resource, 0,
                  file, line, func,
                  "player idnum %ld name %s: %s; password not checked "
                  "(%u earlier report(s) withheld)",
                  idnum, name ? name : "(none)", why, earlier);
}

/* globals defined here, used here and elsewhere */
int *cmd_sort_info = NULL;

struct command_info *complete_cmd_info;

/* This is the Master Command List. You can put new commands in, take commands
 * out, change the order they appear in, etc.  You can adjust the "priority"
 * of commands simply by changing the order they appear in the command list.
 * (For example, if you want "as" to mean "assist" instead of "ask", just put
 * "assist" above "ask" in the Master Command List. In general, utility
 * commands such as "at" should have high priority; infrequently used and
 * dangerously destructive commands should have low priority. */

cpp_extern const struct command_info cmd_info[] = {
  { "RESERVED", "", 0, 0, 0, 0 }, /* this must be first -- for specprocs */

  /* directions must come before other commands but after RESERVED */
  { "north"    , "n"       , POS_STANDING, do_move     , 0, SCMD_NORTH },
  { "east"     , "e"       , POS_STANDING, do_move     , 0, SCMD_EAST },
  { "south"    , "s"       , POS_STANDING, do_move     , 0, SCMD_SOUTH },
  { "west"     , "w"       , POS_STANDING, do_move     , 0, SCMD_WEST },
  { "up"       , "u"       , POS_STANDING, do_move     , 0, SCMD_UP },
  { "down"     , "d"       , POS_STANDING, do_move     , 0, SCMD_DOWN },
  { "northwest", "northw"  , POS_STANDING, do_move     , 0, SCMD_NW },
  { "nw"       , "nw"      , POS_STANDING, do_move     , 0, SCMD_NW },
  { "northeast", "northe"  , POS_STANDING, do_move     , 0, SCMD_NE },
  { "ne"       , "ne"      , POS_STANDING, do_move     , 0,  SCMD_NE },
  { "southeast", "southe"  , POS_STANDING, do_move     , 0, SCMD_SE },
  { "se"       , "se"      , POS_STANDING, do_move     , 0, SCMD_SE },
  { "southwest", "southw"  , POS_STANDING, do_move     , 0, SCMD_SW },
  { "sw"       , "sw"      , POS_STANDING, do_move     , 0, SCMD_SW },
  
  /* now, the main list */
  { "at"       , "at"      , POS_DEAD    , do_at       , LVL_IMMORT, 0 },
  { "advance"  , "adv"     , POS_DEAD    , do_advance  , LVL_GRGOD, 0 },
  { "aedit"    , "aed"     , POS_DEAD    , do_oasis_aedit, LVL_GOD, 0 },
  { "alias"    , "ali"     , POS_DEAD    , do_alias    , 0, 0 },
  { "afk"      , "afk"     , POS_DEAD    , do_gen_tog  , 0, SCMD_AFK },
  { "areas"    , "are"     , POS_DEAD    , do_areas    , 0, 0 },
  { "assist"   , "as"      , POS_FIGHTING, do_assist   , 1, 0 },
  { "ask"      , "ask"     , POS_RESTING , do_spec_comm, 0, SCMD_ASK },
  { "astat"    , "ast"     , POS_DEAD    , do_astat    , 0, 0 },
  { "attach"   , "attach"  , POS_DEAD    , do_attach   , LVL_BUILDER, 0 },
  { "auction"  , "auc"     , POS_SLEEPING, do_gen_comm , 0, SCMD_AUCTION },
  { "autoexits" , "autoex"  , POS_DEAD    , do_gen_tog , 0, SCMD_AUTOEXIT },
  { "autoassist","autoass" , POS_DEAD    , do_gen_tog , 0, SCMD_AUTOASSIST },
  { "autodoor" , "autodoor", POS_DEAD    , do_gen_tog , 0, SCMD_AUTODOOR },
  { "autogold" , "autogold", POS_DEAD    , do_gen_tog , 0, SCMD_AUTOGOLD },
  { "autokey"  , "autokey" , POS_DEAD    , do_gen_tog , 0, SCMD_AUTOKEY },
  { "autoloot" , "autoloot", POS_DEAD    , do_gen_tog , 0, SCMD_AUTOLOOT },
  { "automap"  , "automap" , POS_DEAD    , do_gen_tog , 0, SCMD_AUTOMAP },
  { "autosac"  , "autosac" , POS_DEAD    , do_gen_tog , 0, SCMD_AUTOSAC },
  { "autosplit", "autospl" , POS_DEAD    , do_gen_tog , 0, SCMD_AUTOSPLIT },
  { "alive" , "alive" , POS_DEAD , do_alive , 0, 0 },
  { "backstab" , "ba"      , POS_STANDING, do_backstab , 1, 0 },
  { "ban"      , "ban"     , POS_DEAD    , do_ban      , LVL_GRGOD, 0 },
  { "bandage"  , "band"    , POS_RESTING , do_bandage  , 1, 0 },
  { "balance"  , "bal"     , POS_STANDING, do_not_here , 1, 0 },
  { "bash"     , "bas"     , POS_FIGHTING, do_bash     , 1, 0 },
  { "brief"    , "br"      , POS_DEAD    , do_gen_tog  , 0, SCMD_BRIEF },
  { "buildwalk", "buildwalk", POS_STANDING, do_gen_tog , LVL_BUILDER, SCMD_BUILDWALK },
  { "buy"      , "bu"      , POS_STANDING, do_not_here , 0, 0 },
  { "bug"      , "bug"     , POS_DEAD    , do_ibt      , 0, SCMD_BUG },

  { "cast"     , "c"       , POS_SITTING , do_cast     , 1, 0 },
  { "cedit"    , "cedit"   , POS_DEAD    , do_oasis_cedit, LVL_IMPL, 0 },
  { "changelog", "cha"     , POS_DEAD    , do_changelog, LVL_IMPL, 0 },
  { "check"    , "ch"      , POS_STANDING, do_not_here , 1, 0 },
  { "checkload", "checkl"  , POS_DEAD    , do_checkloadstatus, LVL_GOD, 0 },
  { "close"    , "cl"      , POS_SITTING , do_gen_door , 0, SCMD_CLOSE },
  { "clear"    , "cle"     , POS_DEAD    , do_gen_ps   , 0, SCMD_CLEAR },
  { "cls"      , "cls"     , POS_DEAD    , do_gen_ps   , 0, SCMD_CLEAR },
  { "consider" , "con"     , POS_RESTING , do_consider , 0, 0 },
  { "commands" , "com"     , POS_DEAD    , do_commands , 0, SCMD_COMMANDS },
  { "compact"  , "comp"    , POS_DEAD    , do_gen_tog  , 0, SCMD_COMPACT },
  { "copyover" , "copyover", POS_DEAD    , do_copyover , LVL_GRGOD, 0 },
  { "credits"  , "cred"    , POS_DEAD    , do_gen_ps   , 0, SCMD_CREDITS },

  { "date"     , "da"      , POS_DEAD    , do_date     , LVL_IMMORT, SCMD_DATE },
  { "dc"       , "dc"      , POS_DEAD    , do_dc       , LVL_GOD, 0 },
  { "deposit"  , "depo"    , POS_STANDING, do_not_here , 1, 0 },
  { "detach"   , "detach"  , POS_DEAD    , do_detach   , LVL_BUILDER, 0 },
  { "diagnose" , "diag"    , POS_RESTING , do_diagnose , 0, 0 },
  { "dig"      , "dig"     , POS_DEAD    , do_dig      , LVL_BUILDER, 0 },
  { "display"  , "disp"    , POS_DEAD    , do_display  , 0, 0 },
  { "donate"   , "don"     , POS_RESTING , do_drop     , 0, SCMD_DONATE },
  { "drink"    , "dri"     , POS_RESTING , do_drink    , 0, SCMD_DRINK },
  { "drop"     , "dro"     , POS_RESTING , do_drop     , 0, SCMD_DROP },

  { "eat"      , "ea"      , POS_RESTING , do_eat      , 0, SCMD_EAT },
  { "echo"     , "ec"      , POS_SLEEPING, do_echo     , LVL_IMMORT, SCMD_ECHO },
  { "emote"    , "em"      , POS_RESTING , do_echo     , 0, SCMD_EMOTE },
  { ":"        , ":"       , POS_RESTING, do_echo      , 1, SCMD_EMOTE },
  { "enter"    , "ent"     , POS_STANDING, do_enter    , 0, 0 },
  { "equipment", "eq"      , POS_SLEEPING, do_equipment, 0, 0 },
  { "exits"    , "ex"      , POS_RESTING , do_exits    , 0, 0 },
  { "examine"  , "exa"     , POS_SITTING , do_examine  , 0, 0 },
  { "export"   , "export"  , POS_DEAD    , do_export_zone, LVL_IMPL, 0 },

  { "force"    , "force"   , POS_SLEEPING, do_force    , LVL_GOD, 0 },
  { "fill"     , "fil"     , POS_STANDING, do_pour     , 0, SCMD_FILL },
  { "file"     , "file"    , POS_SLEEPING, do_file     , LVL_GOD, 0 },
  { "flee"     , "fl"      , POS_FIGHTING, do_flee     , 1, 0 },
  { "follow"   , "fol"     , POS_RESTING , do_follow   , 0, 0 },
  { "freeze"   , "freeze"  , POS_DEAD    , do_wizutil  , LVL_GRGOD, SCMD_FREEZE },

  { "get"      , "g"       , POS_RESTING , do_get      , 0, 0 },
  { "gecho"    , "gecho"   , POS_DEAD    , do_gecho    , LVL_GOD, 0 },
  { "gemote"   , "gem"     , POS_SLEEPING, do_gen_comm , 0, SCMD_GEMOTE },
  { "give"     , "giv"     , POS_RESTING , do_give     , 0, 0 },
  { "goto"     , "go"      , POS_SLEEPING, do_goto     , LVL_IMMORT, 0 },
  { "gold"     , "gol"     , POS_RESTING , do_gold     , 0, 0 },
  { "gossip"   , "gos"     , POS_SLEEPING, do_gen_comm , 0, SCMD_GOSSIP },
  { "group"    , "gr"      , POS_RESTING , do_group    , 1, 0 },
  { "grab"     , "grab"    , POS_RESTING , do_grab     , 0, 0 },
  { "grats"    , "grat"    , POS_SLEEPING, do_gen_comm , 0, SCMD_GRATZ },
  { "gsay"     , "gsay"    , POS_SLEEPING, do_gsay     , 0, 0 },
  { "gtell"    , "gt"      , POS_SLEEPING, do_gsay     , 0, 0 },

  { "help"     , "h"       , POS_DEAD    , do_help     , 0, 0 },
  { "happyhour", "ha"      , POS_DEAD    , do_happyhour, 0, 0 },
  { "hedit"    , "hedit"   , POS_DEAD    , do_oasis_hedit, LVL_GOD , 0 },
  { "helpcheck", "helpch"  , POS_DEAD    , do_helpcheck, LVL_GOD, 0 },
  { "hide"     , "hi"      , POS_RESTING , do_hide     , 1, 0 },
  { "hindex"   , "hind"    , POS_DEAD    , do_hindex   , 0, 0 },
  { "handbook" , "handb"   , POS_DEAD    , do_gen_ps   , LVL_IMMORT, SCMD_HANDBOOK },
  { "hcontrol" , "hcontrol", POS_DEAD    , do_hcontrol , LVL_GRGOD, 0 },
  { "history"  , "history" , POS_DEAD    , do_history, 0, 0},
  { "hit"      , "hit"     , POS_FIGHTING, do_hit      , 0, SCMD_HIT },
  { "hold"     , "hold"    , POS_RESTING , do_grab     , 1, 0 },
  { "holler"   , "holler"  , POS_RESTING , do_gen_comm , 1, SCMD_HOLLER },
  { "holylight", "holy"    , POS_DEAD    , do_gen_tog  , LVL_IMMORT, SCMD_HOLYLIGHT },
  { "house"    , "house"   , POS_RESTING , do_house    , 0, 0 },

  { "inventory", "i"       , POS_DEAD    , do_inventory, 0, 0 },
  { "iedit"    , "iedit"    , POS_DEAD    , do_oasis_iedit, LVL_BUILDER, 0 },
  { "identify" , "id"      , POS_STANDING, do_not_here , 1, 0 },
  { "idea"     , "ide"      , POS_DEAD    , do_ibt      , 0, SCMD_IDEA },
  { "imotd"    , "imo"     , POS_DEAD    , do_gen_ps   , LVL_IMMORT, SCMD_IMOTD },
  { "immlist"  , "imm"     , POS_DEAD    , do_gen_ps   , 0, SCMD_IMMLIST },
  { "info"     , "info"    , POS_SLEEPING, do_gen_ps   , 0, SCMD_INFO },
  { "invis"    , "invi"    , POS_DEAD    , do_invis    , LVL_IMMORT, 0 },

  { "junk"     , "j"       , POS_RESTING , do_drop     , 0, SCMD_JUNK },

  { "kill"     , "k"       , POS_FIGHTING, do_kill     , 0, 0 },
  { "kick"     , "ki"      , POS_FIGHTING, do_kick     , 1, 0 },

  { "look"     , "l"       , POS_RESTING , do_look     , 0, SCMD_LOOK },
  { "last"     , "last"    , POS_DEAD    , do_last     , LVL_GOD, 0 },
  { "leave"    , "lea"     , POS_STANDING, do_leave    , 0, 0 },
  { "levels"   , "lev"     , POS_DEAD    , do_levels   , 0, 0 },
  { "list"     , "lis"     , POS_STANDING, do_not_here , 0, 0 },
  { "links"    , "lin"     , POS_STANDING, do_links    , LVL_GOD, 0 },
  { "lock"     , "loc"     , POS_SITTING , do_gen_door , 0, SCMD_LOCK },
  { "load"     , "load"     , POS_DEAD    , do_load     , LVL_BUILDER, 0 },

  { "motd"     , "motd"    , POS_DEAD    , do_gen_ps   , 0, SCMD_MOTD },
  { "mail"     , "mail"    , POS_STANDING, do_not_here , 1, 0 },
  { "map"      , "map"     , POS_STANDING, do_map      , 1, 0 },
  { "medit"    , "med"     , POS_DEAD    , do_oasis_medit, LVL_BUILDER, 0 },
  { "mlist"    , "mlist"   , POS_DEAD    , do_oasis_list, LVL_BUILDER, SCMD_OASIS_MLIST },
  { "mcopy"    , "mcopy"   , POS_DEAD    , do_oasis_copy, LVL_GOD, CON_MEDIT },
  { "msgedit"  , "msgedit" , POS_DEAD    , do_msgedit,   LVL_GOD, 0 },
  { "mute"     , "mute"    , POS_DEAD    , do_wizutil  , LVL_GOD, SCMD_MUTE },

  { "news"     , "news"    , POS_SLEEPING, do_gen_ps   , 0, SCMD_NEWS },
  { "noauction", "noauction",POS_DEAD    , do_gen_tog  , 0, SCMD_NOAUCTION },
  { "nogossip" , "nogossip", POS_DEAD    , do_gen_tog  , 0, SCMD_NOGOSSIP },
  { "nograts"  , "nograts" , POS_DEAD    , do_gen_tog  , 0, SCMD_NOGRATZ },
  { "nohassle" , "nohassle", POS_DEAD    , do_gen_tog  , LVL_IMMORT, SCMD_NOHASSLE },
  { "norepeat" , "norepeat", POS_DEAD    , do_gen_tog  , 0, SCMD_NOREPEAT },
  { "noshout"  , "noshout" , POS_SLEEPING, do_gen_tog  , 1, SCMD_NOSHOUT },
  { "nosummon" , "nosummon", POS_DEAD    , do_gen_tog  , 1, SCMD_NOSUMMON },
  { "notell"   , "notell"  , POS_DEAD    , do_gen_tog  , 1, SCMD_NOTELL },
  { "notitle"  , "notitle" , POS_DEAD    , do_wizutil  , LVL_GOD, SCMD_NOTITLE },
  { "nowiz"    , "nowiz"   , POS_DEAD    , do_gen_tog  , LVL_IMMORT, SCMD_NOWIZ },

  { "open"     , "o"       , POS_SITTING , do_gen_door , 0, SCMD_OPEN },
  { "order"    , "ord"     , POS_RESTING , do_order    , 1, 0 },
  { "offer"    , "off"     , POS_STANDING, do_not_here , 1, 0 },
  { "olc"      , "olc"     , POS_DEAD    , do_show_save_list, LVL_BUILDER, 0 },
  { "olist"    , "olist"   , POS_DEAD    , do_oasis_list, LVL_BUILDER, SCMD_OASIS_OLIST },
  { "oedit"    , "oedit"   , POS_DEAD    , do_oasis_oedit, LVL_BUILDER, 0 },
  { "oset"     , "oset"    , POS_DEAD    , do_oset,        LVL_BUILDER, 0 },  
  { "ocopy"    , "ocopy"   , POS_DEAD    , do_oasis_copy, LVL_GOD, CON_OEDIT },

  { "put"      , "p"       , POS_RESTING , do_put      , 0, 0 },
  { "pulse" , "pulse" , POS_RESTING , do_pulse , 0, 0 },

  { "peace"    , "pe"      , POS_DEAD    , do_peace    , LVL_BUILDER, 0 },
  { "pick"     , "pi"      , POS_STANDING, do_gen_door , 1, SCMD_PICK },
  { "ping"     , "ping"    , POS_RESTING,  do_ping, 0, 0}, 
  { "endgame" , "endgame" , POS_RESTING, do_endgame, 1, 0 },
  { "practice" , "pr"      , POS_RESTING , do_practice , 1, 0 },
  { "page"     , "pag"     , POS_DEAD    , do_page     , 1, 0 },
  { "pardon"   , "pardon"  , POS_DEAD    , do_wizutil  , LVL_GOD, SCMD_PARDON },
  { "plist"    , "plist"   , POS_DEAD    , do_plist    , LVL_GOD, 0 },
  { "policy"   , "pol"     , POS_DEAD    , do_gen_ps   , 0, SCMD_POLICIES },
  { "pour"     , "pour"    , POS_STANDING, do_pour     , 0, SCMD_POUR },
  { "prompt"   , "pro"     , POS_DEAD    , do_display  , 0, 0 },
  { "prefedit" , "pre"     , POS_DEAD    , do_oasis_prefedit , 0, 0 },
  { "purge"    , "purge"   , POS_DEAD    , do_purge    , LVL_BUILDER, 0 },

  { "qedit"    , "qedit"   , POS_DEAD    , do_oasis_qedit, LVL_BUILDER, 0 },
  { "qlist"    , "qlist"   , POS_DEAD    , do_oasis_list, LVL_BUILDER, SCMD_OASIS_QLIST },
  { "quaff"    , "qua"     , POS_RESTING , do_use      , 0, SCMD_QUAFF },
  { "qecho"    , "qec"     , POS_DEAD    , do_qcomm    , LVL_GOD, SCMD_QECHO },
  { "quest"    , "que"     , POS_DEAD    , do_quest    , 0, 0 },
  { "qui"      , "qui"     , POS_DEAD    , do_quit     , 0, 0 },
  { "quit"     , "quit"    , POS_DEAD    , do_quit     , 0, SCMD_QUIT },
  { "qsay"     , "qsay"    , POS_RESTING , do_qcomm    , 0, SCMD_QSAY },

  { "reply"    , "r"       , POS_SLEEPING, do_reply    , 0, 0 },
  { "rest"     , "res"     , POS_RESTING , do_rest     , 0, 0 },
  { "read"     , "rea"     , POS_RESTING , do_look     , 0, SCMD_READ },
  { "reload"   , "reload"  , POS_DEAD    , do_reboot   , LVL_IMPL, 0 },
  { "recite"   , "reci"    , POS_RESTING , do_use      , 0, SCMD_RECITE },
  { "receive"  , "rece"    , POS_STANDING, do_not_here , 1, 0 },
  { "recent"   , "recent"  , POS_DEAD    , do_recent   , LVL_IMMORT, 0 },
  { "remove"   , "rem"     , POS_RESTING , do_remove   , 0, 0 },
  { "rent"     , "rent"    , POS_STANDING, do_not_here , 1, 0 },
  { "report"   , "repo"    , POS_RESTING , do_report   , 0, 0 },
  { "reroll"   , "rero"    , POS_DEAD    , do_wizutil  , LVL_GRGOD, SCMD_REROLL },
  { "rescue"   , "resc"    , POS_FIGHTING, do_rescue   , 1, 0 },
  { "restore"  , "resto"   , POS_DEAD    , do_restore  , LVL_GOD, 0 },
  { "return"   , "retu"    , POS_DEAD    , do_return   , 0, 0 },
  { "redit"    , "redit"   , POS_DEAD    , do_oasis_redit, LVL_BUILDER, 0 },
  { "rlist"    , "rlist"   , POS_DEAD    , do_oasis_list, LVL_BUILDER, SCMD_OASIS_RLIST },
  { "rcopy"    , "rcopy"   , POS_DEAD    , do_oasis_copy, LVL_GOD, CON_REDIT },
  { "roomflags", "roomflags", POS_DEAD   , do_gen_tog  , LVL_IMMORT, SCMD_SHOWVNUMS },

  { "sacrifice", "sac"     , POS_RESTING , do_sac      , 0, 0 },
  { "say"      , "s"       , POS_RESTING , do_say      , 0, 0 },
  { "score"    , "sc"      , POS_DEAD    , do_score    , 0, 0 },
  { "scan"     , "sca"     , POS_RESTING , do_scan     , 0, 0 },
  { "scopy"    , "scopy"   , POS_DEAD    , do_oasis_copy, LVL_GOD, CON_SEDIT },
  { "sit"      , "si"      , POS_RESTING , do_sit      , 0, 0 },
  { "'"        , "'"       , POS_RESTING , do_say      , 0, 0 },
  { "save"     , "sav"     , POS_SLEEPING, do_save     , 0, 0 },
  { "saveall"  , "saveall" , POS_DEAD    , do_saveall  , LVL_BUILDER, 0},
  { "sell"     , "sell"    , POS_STANDING, do_not_here , 0, 0 },
  { "sedit"    , "sedit"   , POS_DEAD    , do_oasis_sedit, LVL_BUILDER, 0 },
  { "send"     , "send"    , POS_SLEEPING, do_send     , LVL_GOD, 0 },
  { "set"      , "set"     , POS_DEAD    , do_set      , LVL_IMMORT, 0 },
  { "shout"    , "sho"     , POS_RESTING , do_gen_comm , 0, SCMD_SHOUT },
  { "show"     , "show"    , POS_DEAD    , do_show     , LVL_IMMORT, 0 },
  { "shutdow"  , "shutdow" , POS_DEAD    , do_shutdown , LVL_IMPL, 0 },
  { "shutdown" , "shutdown", POS_DEAD    , do_shutdown , LVL_IMPL, SCMD_SHUTDOWN },
  { "sip"      , "sip"     , POS_RESTING , do_drink    , 0, SCMD_SIP },
  { "skillset" , "skillset", POS_SLEEPING, do_skillset , LVL_GRGOD, 0 },
  { "sleep"    , "sl"      , POS_SLEEPING, do_sleep    , 0, 0 },
  { "slist"    , "slist"   , POS_SLEEPING, do_oasis_list, LVL_BUILDER, SCMD_OASIS_SLIST },
  { "sneak"    , "sneak"   , POS_STANDING, do_sneak    , 1, 0 },
  { "snoop"    , "snoop"   , POS_DEAD    , do_snoop    , LVL_GOD, 0 },
  { "socials"  , "socials" , POS_DEAD    , do_commands , 0, SCMD_SOCIALS },
  { "split"    , "split"   , POS_SITTING , do_split    , 1, 0 },
  { "stand"    , "st"      , POS_RESTING , do_stand    , 0, 0 },
  { "stat"     , "stat"    , POS_DEAD    , do_stat     , LVL_IMMORT, 0 },
  { "steal"    , "ste"     , POS_STANDING, do_steal    , 1, 0 },
  { "switch"   , "switch"  , POS_DEAD    , do_switch   , LVL_GOD, 0 },

  { "tell"     , "t"       , POS_DEAD    , do_tell     , 0, 0 },
  { "take"     , "ta"      , POS_RESTING , do_get      , 0, 0 },
  { "taste"    , "tas"     , POS_RESTING , do_eat      , 0, SCMD_TASTE },
  { "teleport" , "tele"    , POS_DEAD    , do_teleport , LVL_BUILDER, 0 },
  { "tedit"    , "tedit"   , POS_DEAD    , do_tedit    , LVL_GOD, 0 },  /* XXX: Oasisify */
  { "thaw"     , "thaw"    , POS_DEAD    , do_wizutil  , LVL_GRGOD, SCMD_THAW },
  { "title"    , "title"   , POS_DEAD    , do_title    , 0, 0 },
  { "time"     , "time"    , POS_DEAD    , do_time     , 0, 0 },
  { "toggle"   , "toggle"  , POS_DEAD    , do_toggle   , 0, 0 },
  { "track"    , "track"   , POS_STANDING, do_track    , 0, 0 },
  { "transfer" , "transfer", POS_SLEEPING, do_trans    , LVL_GOD, 0 },
  { "trigedit" , "trigedit", POS_DEAD    , do_oasis_trigedit, LVL_BUILDER, 0 },
  { "typo"     , "typo"    , POS_DEAD    , do_ibt      , 0, SCMD_TYPO },
  { "tlist"    , "tlist"   , POS_DEAD    , do_oasis_list, LVL_BUILDER, SCMD_OASIS_TLIST },
  { "tcopy"    , "tcopy"   , POS_DEAD    , do_oasis_copy, LVL_GOD, CON_TRIGEDIT },
  { "tstat"    , "tstat"   , POS_DEAD    , do_tstat    , LVL_BUILDER, 0 },

  { "unlock"   , "unlock"  , POS_SITTING , do_gen_door , 0, SCMD_UNLOCK },
  { "unban"    , "unban"   , POS_DEAD    , do_unban    , LVL_GRGOD, 0 },
  { "unaffect" , "unaffect", POS_DEAD    , do_wizutil  , LVL_GOD, SCMD_UNAFFECT },
  { "unfollow" , "unf"     , POS_RESTING , do_unfollow , 0, 0 },
  { "uptime"   , "uptime"  , POS_DEAD    , do_date     , LVL_GOD, SCMD_UPTIME },
  { "use"      , "use"     , POS_SITTING , do_use      , 1, SCMD_USE },
  { "users"    , "users"   , POS_DEAD    , do_users    , LVL_GOD, 0 },

  { "value"    , "val"     , POS_STANDING, do_not_here , 0, 0 },
  { "version"  , "ver"     , POS_DEAD    , do_gen_ps   , 0, SCMD_VERSION },
  { "visible"  , "vis"     , POS_RESTING , do_visible  , 1, 0 },
  { "vnum"     , "vnum"    , POS_DEAD    , do_vnum     , LVL_IMMORT, 0 },
  { "vstat"    , "vstat"   , POS_DEAD    , do_vstat    , LVL_IMMORT, 0 },
  { "vdelete"  , "vdelete" , POS_DEAD    , do_vdelete  , LVL_BUILDER, 0 },

  { "wake"     , "wake"    , POS_SLEEPING, do_wake     , 0, 0 },
  { "wear"     , "wea"     , POS_RESTING , do_wear     , 0, 0 },
  { "weather"  , "weather" , POS_RESTING , do_weather  , 0, 0 },
  { "who"      , "wh"      , POS_DEAD    , do_who      , 0, 0 },
  { "whois"    , "whoi"    , POS_DEAD    , do_whois    , 0, 0 },
  { "whoami"   , "whoami"  , POS_DEAD    , do_gen_ps   , 0, SCMD_WHOAMI },
  { "where"    , "where"   , POS_RESTING , do_where    , 1, 0 },
  { "whirlwind", "whirl"   , POS_FIGHTING, do_whirlwind, 0, 0 },
  { "whisper"  , "whisper" , POS_RESTING , do_spec_comm, 0, SCMD_WHISPER },
  { "wield"    , "wie"     , POS_RESTING , do_wield    , 0, 0 },
  { "withdraw" , "withdraw", POS_STANDING, do_not_here , 1, 0 },
  { "wiznet"   , "wiz"     , POS_DEAD    , do_wiznet   , LVL_IMMORT, 0 },
  { ";"        , ";"       , POS_DEAD    , do_wiznet   , LVL_IMMORT, 0 },
  { "wizhelp"  , "wizhelp" , POS_DEAD    , do_wizhelp  , LVL_IMMORT, 0 },
  { "wizlist"  , "wizlist" , POS_DEAD    , do_gen_ps   , 0, SCMD_WIZLIST },
  { "wizupdate", "wizupde" , POS_DEAD    , do_wizupdate, LVL_GRGOD, 0 },
  { "wizlock"  , "wizlock" , POS_DEAD    , do_wizlock  , LVL_IMPL, 0 },
  { "write"    , "write"   , POS_STANDING, do_write    , 1, 0 },

  { "zoneresets", "zoner" ,  POS_DEAD    , do_gen_tog , LVL_IMPL, SCMD_ZONERESETS },
  { "zreset"   , "zreset"  , POS_DEAD    , do_zreset   , LVL_BUILDER, 0 },
  { "zedit"    , "zedit"   , POS_DEAD    , do_oasis_zedit, LVL_BUILDER, 0 },
  { "zlist"    , "zlist"   , POS_DEAD    , do_oasis_list, LVL_BUILDER, SCMD_OASIS_ZLIST },
  { "zlock"    , "zlock"   , POS_DEAD    , do_zlock    , LVL_GOD, 0 },
  { "zunlock"  , "zunlock" , POS_DEAD    , do_zunlock  , LVL_GOD, 0 },
  { "zcheck"   , "zcheck"  , POS_DEAD    , do_zcheck   , LVL_BUILDER, 0 },
  { "zpurge"   , "zpurge"  , POS_DEAD    , do_zpurge   , LVL_BUILDER, 0 },

  { "\n", "zzzzzzz", 0, 0, 0, 0 } };    /* this must be last */


  /* Thanks to Melzaren for this change to allow DG Scripts to be attachable
   *to player's while still disallowing them to manually use the DG-Commands. */
static const struct mob_script_command_t mob_script_commands[] = {

  /* DG trigger commands. minimum_level should be set to -1. */
  { "masound"  , do_masound  , 0 },
  { "mkill"    , do_mkill    , 0 },
  { "mjunk"    , do_mjunk    , 0 },
  { "mdamage"  , do_mdamage  , 0 },
  { "mdoor"    , do_mdoor    , 0 },
  { "mecho"    , do_mecho    , 0 },
  { "mrecho"   , do_mrecho   , 0 },
  { "mechoaround", do_mechoaround , 0 },
  { "msend"    , do_msend    , 0 },
  { "mload"    , do_mload    , 0 },
  { "mpurge"   , do_mpurge   , 0 },
  { "mgoto"    , do_mgoto    , 0 },
  { "mat"      , do_mat      , 0 },
  { "mteleport", do_mteleport, 0 },
  { "mforce"   , do_mforce   , 0 },
  { "mhunt"    , do_mhunt    , 0 },
  { "mremember", do_mremember, 0 },
  { "mforget"  , do_mforget  , 0 },
  { "mtransform", do_mtransform , 0 },
  { "mzoneecho", do_mzoneecho, 0 },
  { "mfollow"  , do_mfollow  , 0 },
  { "mlog"     , do_mlog     , 0 },
  { "\n" , do_not_here , 0 } };

int script_command_interpreter(struct char_data *ch, char *arg) {
  /* DG trigger commands */

  int i;
  char first_arg[MAX_INPUT_LENGTH];
  char *line;

  skip_spaces(&arg);
  if (!*arg)
  return 0;

  line = any_one_arg(arg, first_arg);

  for (i = 0; *mob_script_commands[i].command_name != '\n'; i++)
  if (!str_cmp(first_arg, mob_script_commands[i].command_name))
  break; // NB - only allow full matches.

  if (*mob_script_commands[i].command_name == '\n')
  return 0; // no matching commands.

  /* Poiner to the command? */
  ((*mob_script_commands[i].command_pointer) (ch, line, 0,
  mob_script_commands[i].subcmd));
  return 1; // We took care of execution. Let caller know.
}

static const char *fill[] =
{
  "in",
  "from",
  "with",
  "the",
  "on",
  "at",
  "to",
  "\n"
};

static const char *reserved[] =
{
  "a",
  "an",
  "self",
  "me",
  "all",
  "room",
  "someone",
  "something",
  "\n"
};

static int sort_commands_helper(const void *a, const void *b)
{
  return strcmp(complete_cmd_info[*(const int *)a].sort_as,
                complete_cmd_info[*(const int *)b].sort_as);
}

void sort_commands(void)
{
  int a, num_of_cmds = 0;

  while (complete_cmd_info[num_of_cmds].command[0] != '\n')
    num_of_cmds++;
  num_of_cmds++;  /* \n */

  /* Called at boot and again after every aedit save (the command list is
   * rebuilt then), so release the previous order instead of leaking it. */
  free(cmd_sort_info);
  cmd_sort_info = NULL;
  CREATE(cmd_sort_info, int, num_of_cmds);

  for (a = 0; a < num_of_cmds; a++)
    cmd_sort_info[a] = a;

  /* Don't sort the RESERVED or \n entries. */
  qsort(cmd_sort_info + 1, num_of_cmds - 2, sizeof(int), sort_commands_helper);
}


/* This is the actual command interpreter called from game_loop() in comm.c
 * It makes sure you are the proper level and position to execute the command,
 * then calls the appropriate function. */
void command_interpreter(struct char_data *ch, char *argument)
{
  int cmd, length;
  char *line;
  char arg[MAX_INPUT_LENGTH];

  REMOVE_BIT_AR(AFF_FLAGS(ch), AFF_HIDE);

  /* just drop to next line for hitting CR */
  skip_spaces(&argument);
  if (!*argument)
    return;

  /* special case to handle one-character, non-alphanumeric commands; requested
   * by many people so "'hi" or ";godnet test" is possible. Patch sent by Eric
   * Green and Stefan Wasilewski. */
  if (!isalpha(*argument)) {
    arg[0] = argument[0];
    arg[1] = '\0';
    line = argument + 1;
  } else
    line = any_one_arg(argument, arg);

  /* Since all command triggers check for valid_dg_target before acting, the levelcheck
   * here has been removed. Otherwise, find the command. */
  {
    int cont;                                            /* continue the command checks */
    cont = command_wtrigger(ch, arg, line);              /* any world triggers ? */
    if (!cont) cont = command_mtrigger(ch, arg, line);   /* any mobile triggers ? */
    if (!cont) cont = command_otrigger(ch, arg, line);   /* any object triggers ? */
    if (cont) return;                                    /* yes, command trigger took over */
  }

  /* Allow IMPLs to switch into mobs to test the commands. Invariant
   * (internal): an NPC has a descriptor only while an immortal is switched
   * into it, and do_switch sets desc->original to that immortal. An NPC
   * descriptor with no original breaks it; report it (every command,
   * until fixed) instead of dereferencing NULL, and give that NPC no
   * IMPL-only script commands. */
  if (IS_NPC(ch) && ch->desc) {
    if (!ch->desc->original)
      TB_ERROR_MSG("interpreter", "switch-original", "command_interpreter",
                   DIAG_CHAR_FMT " has descriptor %d but no original "
                   "character; IMPL script commands skipped",
                   DIAG_CHAR(ch), ch->desc->desc_num);
    else if (GET_LEVEL(ch->desc->original) >= LVL_IMPL &&
             script_command_interpreter(ch, argument))
      return;
  }

  for (length = strlen(arg), cmd = 0; *complete_cmd_info[cmd].command != '\n'; cmd++)
    if(complete_cmd_info[cmd].command_pointer != do_action &&
       !strncmp(complete_cmd_info[cmd].command, arg, length))
      if (GET_LEVEL(ch) >= complete_cmd_info[cmd].minimum_level)
        break;

  /* it's not a 'real' command, so it's a social */

  if(*complete_cmd_info[cmd].command == '\n')
    for (length = strlen(arg), cmd = 0; *complete_cmd_info[cmd].command != '\n'; cmd++)
      if (complete_cmd_info[cmd].command_pointer == do_action &&
          !strncmp(complete_cmd_info[cmd].command, arg, length))
        if (GET_LEVEL(ch) >= complete_cmd_info[cmd].minimum_level)
          break;

  if (*complete_cmd_info[cmd].command == '\n') {
    int found = 0;
    send_to_char(ch, "%s", CONFIG_HUH);

    for (cmd = 0; *cmd_info[cmd].command != '\n'; cmd++)
    {
      if (*arg != *cmd_info[cmd].command || cmd_info[cmd].minimum_level > GET_LEVEL(ch))
        continue;

      /* Only apply levenshtein counts if the command is not a trigger command. */
      if ( (levenshtein_distance(arg, cmd_info[cmd].command) <= 2) &&
           (cmd_info[cmd].minimum_level >= 0) )
      {
        if (!found)
        {
          send_to_char(ch, "\r\nDid you mean:\r\n");
          found = 1;
        }
        send_to_char(ch, "  %s\r\n", cmd_info[cmd].command);
      }
    }
  }
  else if (!IS_NPC(ch) && PLR_FLAGGED(ch, PLR_FROZEN) && GET_LEVEL(ch) < LVL_IMPL)
    send_to_char(ch, "You try, but the mind-numbing cold prevents you...\r\n");
  else if (complete_cmd_info[cmd].command_pointer == NULL)
    send_to_char(ch, "Sorry, that command hasn't been implemented yet.\r\n");
  else if (IS_NPC(ch) && complete_cmd_info[cmd].minimum_level >= LVL_IMMORT)
    send_to_char(ch, "You can't use immortal commands while switched.\r\n");
  else if (GET_POS(ch) < complete_cmd_info[cmd].minimum_position)
    switch (GET_POS(ch)) {
    case POS_DEAD:
      send_to_char(ch, "Lie still; you are DEAD!!! :-(\r\n");
      break;
    case POS_INCAP:
    case POS_MORTALLYW:
      send_to_char(ch, "You are in a pretty bad shape, unable to do anything!\r\n");
      break;
    case POS_STUNNED:
      send_to_char(ch, "All you can do right now is think about the stars!\r\n");
      break;
    case POS_SLEEPING:
      send_to_char(ch, "In your dreams, or what?\r\n");
      break;
    case POS_RESTING:
      send_to_char(ch, "Nah... You feel too relaxed to do that..\r\n");
      break;
    case POS_SITTING:
      send_to_char(ch, "Maybe you should get on your feet first?\r\n");
      break;
    case POS_FIGHTING:
      send_to_char(ch, "No way!  You're fighting for your life!\r\n");
      break;
  } else if (no_specials || !special(ch, cmd, line))
    ((*complete_cmd_info[cmd].command_pointer) (ch, line, cmd, complete_cmd_info[cmd].subcmd));
}

/* Routines to handle aliasing. */
static struct alias_data *find_alias(struct alias_data *alias_list, char *str)
{
  while (alias_list != NULL) {
    if (*str == *alias_list->alias)	/* hey, every little bit counts :-) */
      if (!strcmp(str, alias_list->alias))
	return (alias_list);

    alias_list = alias_list->next;
  }

  return (NULL);
}

void free_alias(struct alias_data *a)
{
  if (a->alias)
    free(a->alias);
  if (a->replacement)
    free(a->replacement);
  free(a);
}

/* The interface to the outside world: do_alias */
ACMD(do_alias)
{
  char arg[MAX_INPUT_LENGTH];
  char *repl;
  struct alias_data *a, *temp;

  if (IS_NPC(ch))
    return;

  repl = any_one_arg(argument, arg);

  if (!*arg) {			/* no argument specified -- list currently defined aliases */
    send_to_char(ch, "Currently defined aliases:\r\n");
    if ((a = GET_ALIASES(ch)) == NULL)
      send_to_char(ch, " None.\r\n");
    else {
      while (a != NULL) {
	send_to_char(ch, "%-15s %s\r\n", a->alias, a->replacement);
	a = a->next;
      }
    }
  } else {			/* otherwise, add or remove aliases */
    /* is this an alias we've already defined? */
    if ((a = find_alias(GET_ALIASES(ch), arg)) != NULL) {
      REMOVE_FROM_LIST(a, GET_ALIASES(ch), next);
      free_alias(a);
    }
    /* if no replacement string is specified, assume we want to delete */
    if (!*repl) {
      if (a == NULL)
	send_to_char(ch, "No such alias.\r\n");
      else
	send_to_char(ch, "Alias deleted.\r\n");
    } else {			/* otherwise, either add or redefine an alias */
      if (!str_cmp(arg, "alias")) {
	send_to_char(ch, "You can't alias 'alias'.\r\n");
	return;
      }
      CREATE(a, struct alias_data, 1);
      a->alias = strdup(arg);
      delete_doubledollar(repl);
      a->replacement = strdup(repl);
      if (strchr(repl, ALIAS_SEP_CHAR) || strchr(repl, ALIAS_VAR_CHAR))
	a->type = ALIAS_COMPLEX;
      else
	a->type = ALIAS_SIMPLE;
      a->next = GET_ALIASES(ch);
      GET_ALIASES(ch) = a;
      save_char(ch);
      send_to_char(ch, "Alias ready.\r\n");
    }
  }
}

/* Valid numeric replacements are only $1 .. $9 (makes parsing a little easier,
 * and it's not that much of a limitation anyway.)  Also valid is "$*", which
 * stands for the entire original line after the alias. ";" is used to delimit
 * commands. */
#define NUM_TOKENS       9

static int perform_complex_alias(struct txt_q *input_q, char *orig, struct alias_data *a)
{
  struct txt_q temp_queue;
  struct txt_block *qtmp;
  char *tokens[NUM_TOKENS], *temp, *write_point;
  char buf2[MAX_RAW_INPUT_LENGTH], buf[MAX_RAW_INPUT_LENGTH];	/* raw? */
  int num_of_tokens = 0, num;

  /* First, parse the original string */
  strlcpy(buf2, orig, sizeof(buf2));	/* strcpy: OK (orig:MAX_INPUT_LENGTH < buf2:MAX_RAW_INPUT_LENGTH) */
  temp = strtok(buf2, " ");
  while (temp != NULL && num_of_tokens < NUM_TOKENS) {
    tokens[num_of_tokens++] = temp;
    temp = strtok(NULL, " ");
  }

  /* initialize */
  write_point = buf;
  temp_queue.head = temp_queue.tail = NULL;

  /* now parse the alias */
  for (temp = a->replacement; *temp; temp++) {
    if (*temp == ALIAS_SEP_CHAR) {
      *write_point = '\0';
      buf[MAX_INPUT_LENGTH - 1] = '\0';
      write_to_q(buf, &temp_queue, 1);
      write_point = buf;
    } else if (*temp == ALIAS_VAR_CHAR) {
      temp++;
      /* A replacement ending in a bare '$' leaves nothing for temp to point
       * at.  Every branch below treats the '$' as an introducer and writes
       * only what the code after it stands for, so there is nothing to write
       * here -- but the last of them copied the terminator into the output
       * buffer, and the loop's own temp++ then stepped over it and carried on
       * reading whatever followed the replacement in memory. */
      if (!*temp)
        break;
      if ((num = *temp - '1') < num_of_tokens && num >= 0) {
        if ((write_point - buf) + strlen(tokens[num]) >= MAX_RAW_INPUT_LENGTH)
          goto overflow;
	memcpy(write_point, tokens[num], strlen(tokens[num]) + 1);
	write_point += strlen(tokens[num]);
      } else if (*temp == ALIAS_GLOB_CHAR) {
        skip_spaces(&orig);
        if ((write_point - buf) + strlen(orig) >= MAX_RAW_INPUT_LENGTH)
          goto overflow;
        memcpy(write_point, orig, strlen(orig) + 1);
	write_point += strlen(orig);
      } else {
        if (write_point - buf + 2 >= MAX_RAW_INPUT_LENGTH)
          goto overflow;
        if ((*(write_point++) = *temp) == '$')	/* redouble $ for act safety */
	  *(write_point++) = '$';
      }
    } else {
      if (write_point - buf + 1 >= MAX_RAW_INPUT_LENGTH)
        goto overflow;
      *(write_point++) = *temp;
    }
  }

  *write_point = '\0';
  buf[MAX_INPUT_LENGTH - 1] = '\0';
  write_to_q(buf, &temp_queue, 1);

  /* push our temp_queue on to the _front_ of the input queue */
  if (input_q->head == NULL)
    *input_q = temp_queue;
  else {
    temp_queue.tail->next = input_q->head;
    input_q->head = temp_queue.head;
  }
  return (0);

overflow:
  while (temp_queue.head) {
    qtmp = temp_queue.head;
    temp_queue.head = qtmp->next;
    free(qtmp->text);
    free(qtmp);
  }
  return (-1);
}

/* Given a character and a string, perform alias replacement on it.
 * Return values:
 *   0: String was modified in place; call command_interpreter immediately.
 *   1: String was _not_ modified in place; rather, the expanded aliases
 *      have been placed at the front of the character's input queue. */
int perform_alias(struct descriptor_data *d, char *orig, size_t maxlen)
{
  char first_arg[MAX_INPUT_LENGTH], *ptr;
  struct alias_data *a, *tmp;

  /* Mobs don't have alaises. */
  if (IS_NPC(d->character))
    return (0);

  /* bail out immediately if the guy doesn't have any aliases */
  if ((tmp = GET_ALIASES(d->character)) == NULL)
    return (0);

  /* find the alias we're supposed to match */
  ptr = any_one_arg(orig, first_arg);

  /* bail out if it's null */
  if (!*first_arg)
    return (0);

  /* if the first arg is not an alias, return without doing anything */
  if ((a = find_alias(tmp, first_arg)) == NULL)
    return (0);

  if (a->type == ALIAS_SIMPLE) {
    strlcpy(orig, a->replacement, maxlen);
    return (0);
  } else {
    if (perform_complex_alias(&d->input, ptr, a) < 0)
      send_to_char(d->character, "Alias expansion too long.\r\n");
    return (1);
  }
}

/* Various other parsing utilities. */

/* Searches an array of strings for a target string.  "exact" can be 0 or non-0,
 * depending on whether or not the match must be exact for it to be returned.
 * Returns -1 if not found; 0..n otherwise.  Array must be terminated with a
 * '\n' so it knows to stop searching. */
int search_block(char *arg, const char **list, int exact)
{
  int i, l;

  /*  We used to have \r as the first character on certain array items to
   *  prevent the explicit choice of that point.  It seems a bit silly to
   *  dump control characters into arrays to prevent that, so we'll just
   *  check in here to see if the first character of the argument is '!',
   *  and if so, just blindly return a '-1' for not found. - ae. */
  if (*arg == '!')
    return (-1);

  /* Make into lower case, and get length of string */
  for (l = 0; *(arg + l); l++)
    *(arg + l) = LOWER(*(arg + l));

  if (exact) {
    for (i = 0; **(list + i) != '\n'; i++)
      if (!strcmp(arg, *(list + i)))
	return (i);
  } else {
    if (!l)
      l = 1;			/* Avoid "" to match the first available
				 * string */
    for (i = 0; **(list + i) != '\n'; i++)
      if (!strncmp(arg, *(list + i), l))
	return (i);
  }

  return (-1);
}

int is_number(const char *str)
{
  if(*str == '-')
     str++;
  if(!*str)
     return (0);
  while (*str)
    if (!isdigit(*(str++)))
      return (0);

  return (1);
}

/* Function to skip over the leading spaces of a string. */
void skip_spaces(char **string)
{
  for (; **string && **string != '\t' && isspace(**string); (*string)++);
}

/* Given a string, change all instances of double dollar signs ($$) to single
 * dollar signs ($).  When strings come in, all $'s are changed to $$'s to
 * avoid having users be able to crash the system if the inputted string is
 * eventually sent to act().  If you are using user input to produce screen
 * output AND YOU ARE SURE IT WILL NOT BE SENT THROUGH THE act() FUNCTION
 * (i.e., do_gecho, do_title, but NOT do_say), you can call
 * delete_doubledollar() to make the output look correct.
 * Modifies the string in-place. */
char *delete_doubledollar(char *string)
{
  char *ddread, *ddwrite;

  /* If the string has no dollar signs, return immediately */
  if ((ddwrite = strchr(string, '$')) == NULL)
    return (string);

  /* Start from the location of the first dollar sign */
  ddread = ddwrite;


  while (*ddread)   /* Until we reach the end of the string... */
    if ((*(ddwrite++) = *(ddread++)) == '$') /* copy one char */
      if (*ddread == '$')
	ddread++; /* skip if we saw 2 $'s in a row */

  *ddwrite = '\0';

  return (string);
}

int fill_word(char *argument)
{
  return (search_block(argument, fill, TRUE) >= 0);
}

int reserved_word(char *argument)
{
  return (search_block(argument, reserved, TRUE) >= 0);
}

/* Copy one word into an argument buffer. The argument splitters below all
 * follow the CircleMUD contract that the output buffer holds
 * MAX_INPUT_LENGTH bytes. Input can be longer (script-forced commands), so
 * copy at most MAX_INPUT_LENGTH-1 bytes, keep consuming the word, and report
 * truncation. Returns the input position just past the copied word. */
static char *copy_arg_word(char *argument, char *first_arg, char stop_quote,
                           const char *caller)
{
  char *out = first_arg, *const out_end = first_arg + MAX_INPUT_LENGTH - 1;
  size_t length = 0;

  for (; *argument && (stop_quote ? *argument != stop_quote
                                  : !isspace((unsigned char) *argument));
       argument++, length++)
    if (out < out_end)
      *out++ = LOWER(*argument);
  *out = '\0';

  if (length > (size_t) (out_end - first_arg))
    TB_ERROR_MSG("interpreter", "parse-argument", caller,
                 "word of %zu bytes truncated to %d", length,
                 MAX_INPUT_LENGTH - 1);
  return argument;
}

/* Copy the first non-fill-word, space-delimited argument of 'argument'
 * to 'first_arg'; return a pointer to the remainder of the string. */
char *one_argument(char *argument, char *first_arg)
{
  char *begin = first_arg;

  if (!argument) {
    log("SYSERR: one_argument received a NULL pointer!");
    *first_arg = '\0';
    return (NULL);
  }

  do {
    skip_spaces(&argument);
    argument = copy_arg_word(argument, begin, '\0', "one_argument");
  } while (fill_word(begin));

  return (argument);
}

/* one_word is like any_one_arg, except that words in quotes ("") are
 * considered one word. No longer ignores fill words.  -dak */
char *one_word(char *argument, char *first_arg)
{
    skip_spaces(&argument);

    if (*argument == '\"') {
      argument = copy_arg_word(argument + 1, first_arg, '\"', "one_word");
      if (*argument)
        argument++; /* closing quote; an unclosed quote ends at the NUL */
    } else
      argument = copy_arg_word(argument, first_arg, '\0', "one_word");

  return (argument);
}

/* Same as one_argument except that it doesn't ignore fill words. */
char *any_one_arg(char *argument, char *first_arg)
{
  skip_spaces(&argument);
  return (copy_arg_word(argument, first_arg, '\0', "any_one_arg"));
}

/* Same as one_argument except that it takes two args and returns the rest;
 * ignores fill words */
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  return (one_argument(one_argument(argument, first_arg), second_arg)); /* :-) */
}

/* Determine if a given string is an abbreviation of another.
 * Returns 1 if arg1 is an abbreviation of arg2. */
int is_abbrev(const char *arg1, const char *arg2)
{
  if (!*arg1)
    return (0);

  for (; *arg1 && *arg2; arg1++, arg2++)
    if (LOWER(*arg1) != LOWER(*arg2))
      return (0);

  if (!*arg1)
    return (1);
  else
    return (0);
}

/* Return first space-delimited token in arg1; remainder of string in arg2.
 * NOTE: Requires sizeof(arg2) >= sizeof(string)
 * NOTE: arg2 may alias string -- half_chop(buf, arg, buf) is how most of the
 *       tree consumes a line one token at a time -- so the remainder is moved
 *       with memmove().  arg1 must NOT alias string; any_one_arg() writes it
 *       while still reading. */
void half_chop(char *string, char *arg1, char *arg2)
{
  char *temp;

  temp = any_one_arg(string, arg1);
  skip_spaces(&temp);
  memmove(arg2, temp, strlen(temp) + 1);	/* memmove: arg2 may alias string */
}

/* Used in specprocs, mostly.  (Exactly) matches "command" to cmd number */
int find_command(const char *command)
{
  int cmd;

  for (cmd = 0; *complete_cmd_info[cmd].command != '\n'; cmd++)
    if (!strcmp(complete_cmd_info[cmd].command, command))
      return (cmd);

  return (-1);
}

int special(struct char_data *ch, int cmd, char *arg)
{
  struct obj_data *i;
  struct char_data *k;
  int j;

  /* special in room? */
  if (GET_ROOM_SPEC(IN_ROOM(ch)) != NULL)
    if (GET_ROOM_SPEC(IN_ROOM(ch)) (ch, world + IN_ROOM(ch), cmd, arg))
      return (1);

  /* special in equipment list? */
  for (j = 0; j < NUM_WEARS; j++)
    if (GET_EQ(ch, j) && GET_OBJ_SPEC(GET_EQ(ch, j)) != NULL)
      if (GET_OBJ_SPEC(GET_EQ(ch, j)) (ch, GET_EQ(ch, j), cmd, arg))
	return (1);

  /* special in inventory? */
  for (i = ch->carrying; i; i = i->next_content)
    if (GET_OBJ_SPEC(i) != NULL)
      if (GET_OBJ_SPEC(i) (ch, i, cmd, arg))
	return (1);

  /* special in mobile present? */
  for (k = world[IN_ROOM(ch)].people; k; k = k->next_in_room)
    if (!MOB_FLAGGED(k, MOB_NOTDEADYET))
      if (GET_MOB_SPEC(k) && GET_MOB_SPEC(k) (ch, k, cmd, arg))
	return (1);

  /* special in object present? */
  for (i = world[IN_ROOM(ch)].contents; i; i = i->next_content)
    if (GET_OBJ_SPEC(i) != NULL)
      if (GET_OBJ_SPEC(i) (ch, i, cmd, arg))
	return (1);

  return (0);
}

/* Stuff for controlling the non-playing sockets (get name, pwd etc).
 * This function needs to die. */
static int parse_name_internal(char *arg, char *name)
{
  int i;

  skip_spaces(&arg);
  for (i = 0; (*name = *arg); arg++, i++, name++)
    if (!isalpha(*arg))
      return (1);

  if (!i)
    return (1);

  return (0);
}

#define RECON		1
#define USURP		2
#define UNSWITCH	3

/**
 * Release the editor buffers of @p k, a socket a dupe check is booting,
 * before its character @p booted is freed.
 *
 * The dupe checks set k->character to NULL, and close_socket() releases
 * k's buffers only while k->character is set, so this does it as
 * close_socket() would: a mail being written (PLR_MAILING) owns *k->str and
 * k->str; otherwise k->backstr, k's copy of the old text, is freed unless
 * PLR_WRITING. Both pointers are cleared either way: at the menu's
 * description editor (CON_PLR_DESC, or CON_CLOSE after an immortal's dc
 * there) k->str points into @p booted, and k stays in descriptor_list until
 * the close pass, so input k already queued is still processed this pulse
 * by string_add(), which writes through k->str.
 *
 * @param k      The socket being booted; its character is about to be freed.
 * @param booted k's character, a player not in the world.
 */
static void release_booted_editor(struct descriptor_data *k,
                                  struct char_data *booted)
{
  if (PLR_FLAGGED(booted, PLR_MAILING) && k->str) {
    if (*k->str)
      free(*k->str);
    free(k->str);
  } else if (k->backstr && !PLR_FLAGGED(booted, PLR_WRITING))
    free(k->backstr);
  k->str = NULL;
  k->backstr = NULL;
}

/* This function seems a bit over-extended. */
/**
 * After a correct password, make @p d the only controller of its player.
 *
 * Walks descriptor_list and character_list. Other descriptors holding the
 * same idnum are closed (their characters freed or unbound), and an in-game
 * or link-dead character with that idnum is taken over: d->character may be
 * freed and replaced by it.
 *
 * Other descriptors are in any login state, so their characters may have no
 * name yet (still at the name prompt) and no valid idnum.
 *
 * @pre d->character is a loaded player with a valid GET_IDNUM().
 * @return TRUE when d was reconnected to an existing character (the caller
 *         must not enter the game again), FALSE to continue logging in.
 *         Either way the caller must re-read d->character afterwards.
 */
static int perform_dupe_check(struct descriptor_data *d)
{
  struct descriptor_data *k, *next_k;
  struct char_data *target = NULL, *ch, *next_ch;
  int mode = 0;
  int pref_temp = 0; /* for "last" log */
  int id = GET_IDNUM(d->character);

  /* Now that this descriptor has successfully logged in, disconnect all
   * other descriptors controlling a character with the same ID number. */

  for (k = descriptor_list; k; k = next_k) {
    next_k = k->next;

    if (k == d)
      continue;

    if (k->original && (GET_IDNUM(k->original) == id)) {
      /* Original descriptor was switched, booting it and restoring normal body control. */

      write_to_output(d, "\r\nMultiple login detected -- disconnecting.\r\n");
      STATE(k) = CON_CLOSE;
      pref_temp=GET_PREF(k->character);
      if (!target) {
	target = k->original;
	mode = UNSWITCH;
      }
      if (k->character)
	k->character->desc = NULL;
      k->character = NULL;
      k->original = NULL;
    } else if (k->character && GET_IDNUM(k->character) == id && k->original) {
      /* Character taking over their own body, while an immortal was switched to it. */

      do_return(k->character, NULL, 0, 0);
    } else if (k->character && GET_IDNUM(k->character) == id) {
      /* Character taking over their own body. k's character stays, linkless,
       * only if it is in the world (in the game, where it may be taken over
       * below, in an editor, or link-dead). At the menu or another pre-game
       * prompt it belongs to k's login alone: it is in no list, and
       * close_socket() frees nothing once k->character is NULL, so it is
       * freed here; it used to leak (~9 KB per login). */
      struct char_data *other = k->character;
      bool other_in_world = IS_PLAYING(k) || STATE(k) == CON_DISCONNECT;

      pref_temp=GET_PREF(other);

      if (!target && STATE(k) == CON_PLAYING) {
	write_to_output(k, "\r\nThis body has been usurped!\r\n");
	target = other;
	mode = USURP;
      }
      other->desc = NULL;
      k->character = NULL;
      k->original = NULL;
      if (!other_in_world) {
        /* k->str must not outlive other (the menu's description editor
         * points into it); see release_booted_editor(). */
        release_booted_editor(k, other);
        free_char(other);
      }
      write_to_output(k, "\r\nMultiple login detected -- disconnecting.\r\n");
      STATE(k) = CON_CLOSE;
    }
  }

 /* Now, go through the character list, deleting all characters that are not
  * already marked for deletion from the above step (i.e., in the CON_HANGUP
  * state), and have not already been selected as a target for switching into.
  * In addition, if we haven't already found a target, choose one if one is
  * available (while still deleting the other duplicates, though theoretically
  * none should be able to exist). */
  for (ch = character_list; ch; ch = next_ch) {
    next_ch = ch->next;

    if (IS_NPC(ch))
      continue;
    if (GET_IDNUM(ch) != id)
      continue;

    /* ignore chars with descriptors (already handled by above step) */
    if (ch->desc)
      continue;

    /* don't extract the target char we've found one already */
    if (ch == target)
      continue;

    /* we don't already have a target and found a candidate for switching */
    if (!target) {
      target = ch;
      mode = RECON;
      pref_temp = GET_PREF(ch);
      continue;
    }

    /* we've found a duplicate - blow him away, dumping his eq in the mortal
     * start room. That room is looked up by vnum: the hard-coded rnum 1 used
     * here before was whatever room OLC had shifted into that slot, or past
     * top_of_world (char_to_room() refused it and left the duplicate in
     * NOWHERE). With no start room this is reported and the duplicate is
     * extracted where it is (extract_char_final() recovers or reports one
     * in NOWHERE). */
    {
      room_rnum dump = real_room(CONFIG_MORTAL_START), here = IN_ROOM(ch);

      if (dump == NOWHERE)
        TB_ERROR_MSG("interpreter", "dupe-no-start-room", "room",
                     "mortal start room vnum %d does not exist; duplicate "
                     DIAG_CHAR_FMT " extracted in " DIAG_ROOM_FMT,
                     (int) CONFIG_MORTAL_START, DIAG_CHAR(ch), DIAG_ROOM(here));
      else {
        if (here != NOWHERE)
          char_from_room(ch);
        char_to_room(ch, dump);
      }
    }
    extract_char(ch);
  }

  /* no target for switching into was found - allow login to continue */
  if (!target) {
    GET_PREF(d->character) = rand_number(1, 128000);
    if (GET_HOST(d->character))
      free(GET_HOST(d->character));
    GET_HOST(d->character) = strdup(d->host);
    return 0;
  }

  if (GET_HOST(target)) free(GET_HOST(target));
  GET_HOST(target) = strdup(d->host);

  GET_PREF(target) = pref_temp;
  add_llog_entry(target, LAST_RECONNECT);

  /* Okay, we've found a target.  Connect d to target. */
  free_char(d->character); /* get rid of the old char */
  d->character = target;
  d->character->desc = d;
  d->original = NULL;
  d->character->char_specials.timer = 0;
  REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_MAILING);
  REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_WRITING);
  STATE(d) = CON_PLAYING;
  MXPSendTag( d, "<VERSION>" );

  switch (mode) {
  case RECON:
    write_to_output(d, "Reconnecting.\r\n");
    act("$n has reconnected.", TRUE, d->character, 0, 0, TO_ROOM);
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "%s [%s] has reconnected.", GET_NAME(d->character), d->host);
    if (has_mail(GET_IDNUM(d->character)))
      write_to_output(d, "You have mail waiting.\r\n");
    break;
  case USURP:
    write_to_output(d, "You take over your own body, already in use!\r\n");
    act("$n suddenly keels over in pain, surrounded by a white aura...\r\n"
	"$n's body has been taken over by a new spirit!",
	TRUE, d->character, 0, 0, TO_ROOM);
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE,
	"%s has re-logged in ... disconnecting old socket.", GET_NAME(d->character));
    break;
  case UNSWITCH:
    write_to_output(d, "Reconnecting to unswitched char.");
    mudlog(NRM, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "%s [%s] has reconnected.", GET_NAME(d->character), d->host);
    break;
  }

  return (1);
}

/* New Char dupe-check called at the start of character creation */
/**
 * Close other sockets creating, or playing, a character with @p d's new name.
 *
 * Any client can open sockets, so the other descriptors may be at any login
 * state. A character still at CON_GET_NAME, or one that answered 'N' at
 * CON_NAME_CNFRM, has a NULL player.name and is skipped. A NULL name on a
 * socket past name confirmation is reported ("new-char-dupe-check").
 *
 * @pre d->character is a new player whose player.name is set.
 * @post If the name is already in game, both sockets are set to CON_CLOSE
 *       and d->character is freed and NULL; the caller must check STATE(d).
 *       A booted socket's character is freed unless it is in the world,
 *       where it stays without a descriptor; before it is freed, the
 *       socket's editor buffers are released (release_booted_editor()).
 * @return TRUE if any socket was closed.
 */
static bool perform_new_char_dupe_check(struct descriptor_data *d)
{
  struct descriptor_data *k, *next_k;
  bool found = FALSE;

  /* Now that this descriptor has successfully logged in, disconnect all
   * other descriptors controlling a character with the same ID number. */

  for (k = descriptor_list; k; k = next_k) {
    next_k = k->next;

    if (k == d)
      continue;

    /* Any client controls the other sockets: a peer still typing its name
     * (CON_GET_NAME) or one that answered 'N' at CON_NAME_CNFRM legitimately
     * has a character whose player.name is NULL. Only a NULL name past name
     * confirmation (and outside the game) breaks the login invariant. */
    if (k->character == NULL || IS_NPC(k->character))
      continue;
    if (GET_PC_NAME(k->character) == NULL) {
      if (STATE(k) > CON_NAME_CNFRM && STATE(k) != CON_PLAYING)
        TB_WARN_MSG("interpreter", "new-char-dupe-check", "descriptor",
                    "desc %d state %d has a character with a NULL name "
                    "while checking new " DIAG_CHAR_FMT,
                    k->desc_num, STATE(k), DIAG_CHAR(d->character));
      continue;
    }

    /* Do the player names match? str_cmp, as valid_name() does. */
    if (!str_cmp(GET_PC_NAME(k->character), GET_PC_NAME(d->character))) {
      /* Check the other character is still in creation? */
      if ((STATE(k) > CON_PLAYING) && (STATE(k) < CON_QCLASS)) {
        /* Boot the older one. Its character belongs to that login alone (a
         * character in creation is in no list), and close_socket() frees
         * nothing once k->character is NULL, so it is freed here; it used
         * to be dropped and leaked. */
        struct char_data *older = k->character;

        k->character = NULL;
        k->original = NULL;
        older->desc = NULL;
        /* Creation prompts have no editor, but k may be CON_CLOSE after an
         * immortal's dc at the menu's description editor, with k->str still
         * pointing into older. */
        release_booted_editor(k, older);
        free_char(older);
        write_to_output(k, "\r\nMultiple login detected -- disconnecting.\r\n");
        STATE(k) = CON_CLOSE;

        mudlog(NRM, LVL_GOD, TRUE, "Multiple logins detected in char creation for %s.", GET_NAME(d->character));

        found = TRUE;
      } else {
        /* Something went VERY wrong, boot both chars. k's character stays,
         * linkless, only if it is in the world (in the game, in an editor or
         * already linkless); at the menu or another pre-game prompt it
         * belongs to that login alone. d's character is still being created.
         * Both of those used to be dropped and leaked; they are freed. */
        struct char_data *other = k->character;
        bool other_in_world = IS_PLAYING(k) || STATE(k) == CON_DISCONNECT;

        other->desc = NULL;
        k->character = NULL;
        k->original = NULL;
        if (!other_in_world) {
          /* At the menu's description editor k->str points into other, and
           * string_add() may still write through it this pulse. */
          release_booted_editor(k, other);
          free_char(other);
        }
        write_to_output(k, "\r\nMultiple login detected -- disconnecting.\r\n");
        STATE(k) = CON_CLOSE;

        d->character->desc = NULL;
        free_char(d->character);
        d->character = NULL;
        d->original = NULL;
        write_to_output(d, "\r\nSorry, due to multiple connections, all your connections are being closed.\r\n");
        write_to_output(d, "\r\nPlease reconnect.\r\n");
        STATE(d) = CON_CLOSE;

        mudlog(NRM, LVL_GOD, TRUE, "SYSERR: Multiple logins with 1st in-game and the 2nd in char creation.");

        /* d->character is now NULL: stop before the next iteration reads its
         * name. The caller sees STATE(d) == CON_CLOSE. */
        return TRUE;
      }
    }
  }
  return (found);
}

/* load the player, put them in the right room - used by copyover_recover too */
int enter_player_game (struct descriptor_data *d)
{
  int load_result;
  room_vnum load_room;

  reset_char(d->character);

  if (PLR_FLAGGED(d->character, PLR_INVSTART))
    GET_INVIS_LEV(d->character) = GET_LEVEL(d->character);

  /* We have to place the character in a room before equipping them
   * or equip_char() will gripe about the person in NOWHERE. */
  if ((load_room = GET_LOADROOM(d->character)) != NOWHERE)
    load_room = real_room(load_room);

  /* If char was saved with NOWHERE, or real_room above failed... */
  if (load_room == NOWHERE) {
    if (GET_LEVEL(d->character) >= LVL_IMMORT)
      load_room = r_immort_start_room;
    else
      load_room = r_mortal_start_room;
  }

  if (PLR_FLAGGED(d->character, PLR_FROZEN))
    load_room = r_frozen_start_room;

  /* copyover */
  d->character->script_id = GET_IDNUM(d->character);
  /* find_char helper */
  add_to_lookup_table(d->character->script_id, (void *)d->character);

  /* After moving saving of variables to the player file, this should only
   * be called in case nothing was found in the pfile. If something was
   * found, SCRIPT(ch) will be set. */
  if (!SCRIPT(d->character))
    read_saved_vars(d->character);

  d->character->next = character_list;
  character_list = d->character;
  char_to_room(d->character, load_room);
  load_result = Crash_load(d->character);
  
  /* Save the character and their object file */
  save_char(d->character);
  Crash_crashsave(d->character);

  /* Check for a login trigger in the players' start room */
  login_wtrigger(&world[IN_ROOM(d->character)], d->character);

  return load_result;
}

/* Append to the connection's protocol banner. snprintf() returns the length
 * it *wanted*, so "len += snprintf(...)" runs past the buffer on truncation
 * and the next call's size (size - len) wraps to a huge value. Stop at the
 * buffer end and report instead. */
static void banner_append(char *buf, size_t size, size_t *len,
                          const char *format, ...)
  __attribute__ ((format (printf, 4, 5)));
static void banner_append(char *buf, size_t size, size_t *len,
                          const char *format, ...)
{
  va_list args;
  int written;

  if (*len >= size - 1)
    return; /* already truncated and reported */
  va_start(args, format);
  written = vsnprintf(buf + *len, size - *len, format, args);
  va_end(args);
  if (written < 0) {
    TB_ERROR_ERRNO("network", "protocol-banner", "get_protocols",
                   "formatting the protocol banner failed; banner cut at %zu "
                   "bytes", *len);
    buf[*len] = '\0';
    *len = size - 1;
  } else if ((size_t) written >= size - *len) {
    TB_ERROR_MSG("network", "protocol-banner", "get_protocols",
                 "protocol banner truncated at %zu bytes (needed %zu)",
                 size - 1, *len + (size_t) written);
    *len = size - 1;
  } else
    *len += (size_t) written;
}

/**
 * Descriptor event: after protocol negotiation, show what the client
 * reported and the greeting, then move the socket to CON_GET_NAME.
 *
 * @pre event_obj is a mud_event_data whose pStruct is the descriptor.
 *      new_descriptor() calls init_descriptor(), whose ProtocolCreate()
 *      allocates d->pProtocol, every pVariables[] entry and every string
 *      value, before it queues this event; close_socket() cancels the
 *      descriptor's events, so d is live when this runs.
 * The client ID is client-controlled text: it is only ever passed as a %s
 * argument. banner_append() bounds the banner to its buffer and reports a
 * cut ("protocol-banner").
 * @return 0 (one-shot event).
 */
EVENTFUNC(get_protocols)
{
  struct descriptor_data *d;
  struct mud_event_data *pMudEvent;
  char buf[MAX_STRING_LENGTH];
  size_t len = 0;

  if (event_obj == NULL)
    return 0;
  
  pMudEvent = (struct mud_event_data *) event_obj;
  d = (struct descriptor_data *) pMudEvent->pStruct;  
  
  /* Clear extra white space from the "protocol scroll" */
  write_to_output(d, "[H[J");

  buf[0] = '\0';
  /* CLIENT_ID is whatever the client reported (TTYPE or MSDP): untrusted. */
  banner_append(buf, sizeof(buf), &len, "\tO[\toClient\tO] \tw%s\tn | ", d->pProtocol->pVariables[eMSDP_CLIENT_ID]->pValueString);

  if (d->pProtocol->pVariables[eMSDP_XTERM_256_COLORS]->ValueInt)
    banner_append(buf, sizeof(buf), &len, "\tO[\toColors\tO] \tw256\tn | ");
  else if (d->pProtocol->pVariables[eMSDP_ANSI_COLORS]->ValueInt)
    banner_append(buf, sizeof(buf), &len, "\tO[\toColors\tO] \twAnsi\tn | ");
  else
    banner_append(buf, sizeof(buf), &len, "[Colors] No Color | ");

  banner_append(buf, sizeof(buf), &len, "\tO[\toMXP\tO] \tw%s\tn | ", d->pProtocol->bMXP ? "Yes" : "No");
  banner_append(buf, sizeof(buf), &len, "\tO[\toMSDP\tO] \tw%s\tn | ", d->pProtocol->bMSDP ? "Yes" : "No");
  banner_append(buf, sizeof(buf), &len, "\tO[\toATCP\tO] \tw%s\tn\r\n\r\n", d->pProtocol->bATCP ? "Yes" : "No");

  /* The banner holds client-supplied text: always send it as data, never as
   * a format string (a '%' in the client ID would otherwise be interpreted). */
  write_to_output(d, "%s", buf);

  write_to_output(d, "%s", GREETINGS);
  STATE(d) = CON_GET_NAME;
  return 0;
}

/* deal with newcomers and other non-playing sockets */
void nanny(struct descriptor_data *d, char *arg)
{
  int load_result;	/* Overloaded variable */
  int player_i;

  /* OasisOLC states */
  struct {
    int state;
    void (*func)(struct descriptor_data *, char *);
  } olc_functions[] = {
    { CON_OEDIT, oedit_parse },
    { CON_IEDIT, iedit_parse },
    { CON_ZEDIT, zedit_parse },
    { CON_SEDIT, sedit_parse },
    { CON_MEDIT, medit_parse },
    { CON_REDIT, redit_parse },
    { CON_CEDIT, cedit_parse },
    { CON_TRIGEDIT, trigedit_parse },
    { CON_AEDIT, aedit_parse },
    { CON_HEDIT, hedit_parse },
    { CON_QEDIT, qedit_parse },
    { CON_PREFEDIT, prefedit_parse },
    { CON_IBTEDIT, ibtedit_parse },
    { CON_MSGEDIT, msgedit_parse },
    { -1, NULL }
  };

  skip_spaces(&arg);

  /* Quick check for the OLC states. */
  for (player_i = 0; olc_functions[player_i].state >= 0; player_i++)
    if (STATE(d) == olc_functions[player_i].state) {
      (*olc_functions[player_i].func)(d, arg);
      return;
    }

  /* Not in OLC. */
  switch (STATE(d)) {
  case CON_GET_PROTOCOL:
    write_to_output(d, "Collecting Protocol Information... Please Wait.\r\n"); 
    return;
  case CON_GET_NAME:		/* wait for input of name */
    if (d->character == NULL) {
      CREATE(d->character, struct char_data, 1);
      clear_char(d->character);
      CREATE(d->character->player_specials, struct player_special_data, 1);
      
      new_mobile_data(d->character);
      
      GET_HOST(d->character) = strdup(d->host);
      d->character->desc = d;
    }
    if (!*arg)
      STATE(d) = CON_CLOSE;
    else {
      char buf[MAX_INPUT_LENGTH], tmp_name[MAX_INPUT_LENGTH];

      if ((parse_name_internal(arg, tmp_name)) || strlen(tmp_name) < 2 ||
       strlen(tmp_name) > MAX_NAME_LENGTH || !valid_name(tmp_name) ||
       (strlcpy(buf, tmp_name, sizeof(buf)), fill_word(buf)) || reserved_word(buf)) {	/* strcpy: OK (mutual MAX_INPUT_LENGTH) */
          write_to_output(d, "Invalid name, please try another.\r\nName: ");
          return;
      }
      if ((player_i = load_char(tmp_name, d->character)) > -1) {
        GET_PFILEPOS(d->character) = player_i;

        if (PLR_FLAGGED(d->character, PLR_DELETED)) {
          /* Make sure old files are removed so the new player doesn't get the
           * deleted player's equipment. */
          if ((player_i = get_ptable_by_name(tmp_name)) >= 0)
            remove_player(player_i);

          /* We get a false positive from the original deleted character. */
          free_char(d->character);

          /* Check for multiple creations. */
          if (!valid_name(tmp_name)) {
            write_to_output(d, "Invalid name, please try another.\r\nName: ");
            return;
          }
          CREATE(d->character, struct char_data, 1);
          clear_char(d->character);
          CREATE(d->character->player_specials, struct player_special_data, 1);

          new_mobile_data(d->character);

          if (GET_HOST(d->character))
            free(GET_HOST(d->character));
          GET_HOST(d->character) = strdup(d->host);

          d->character->desc = d;
          CREATE(d->character->player.name, char, strlen(tmp_name) + 1);
          strlcpy(d->character->player.name, CAP(tmp_name), strlen(tmp_name) + 1);	/* strcpy: OK (size checked above) */
          /* remove_player() dropped the old index entry (and shifted later
           * ones down), so player_i is stale: -1 makes CON_QCLASS call
           * create_entry() for the new character. */
          GET_PFILEPOS(d->character) = -1;
          write_to_output(d, "Did I get that right, %s (\t(Y\t)/\t(N\t))? ", tmp_name);
          STATE(d) = CON_NAME_CNFRM;
        } else {
          /* undo it just in case they are set */
          REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_WRITING);
          REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_MAILING);
          REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_CRYO);
          d->character->player.time.logon = time(0);
          write_to_output(d, "Password: ");
          echo_off(d);
          d->idle_tics = 0;
          STATE(d) = CON_PASSWORD;
        }
      } else {
        /* player unknown -- make new character */

        /* Check for multiple creations of a character. */
        if (!valid_name(tmp_name)) {
          write_to_output(d, "Invalid name, please try another.\r\nName: ");
          return;
        }
        CREATE(d->character->player.name, char, strlen(tmp_name) + 1);
        strlcpy(d->character->player.name, CAP(tmp_name), strlen(tmp_name) + 1);	/* strcpy: OK (size checked above) */

        write_to_output(d, "Did I get that right, %s (\t(Y\t)/\t(N\t))? ", tmp_name);
        STATE(d) = CON_NAME_CNFRM;
      }
    }
    break;

  case CON_NAME_CNFRM:		/* wait for conf. of new name    */
    if (UPPER(*arg) == 'Y') {
      if (isbanned(d->host) >= BAN_NEW) {
	mudlog(NRM, LVL_GOD, TRUE, "Request for new char %s denied from [%s] (siteban)", GET_PC_NAME(d->character), d->host);
	write_to_output(d, "Sorry, new characters are not allowed from your site!\r\n");
	STATE(d) = CON_CLOSE;
	return;
      }
      if (circle_restrict) {
	write_to_output(d, "Sorry, new players can't be created at the moment.\r\n");
	mudlog(NRM, LVL_GOD, TRUE, "Request for new char %s denied from [%s] (wizlock)", GET_PC_NAME(d->character), d->host);
	STATE(d) = CON_CLOSE;
	return;
      }
      perform_new_char_dupe_check(d);
      /* The dupe check closes this socket (and drops d->character) when the
       * same name is already in game. */
      if (STATE(d) == CON_CLOSE || d->character == NULL)
        return;
      write_to_output(d, "New character.\r\nGive me a password for %s: ", GET_PC_NAME(d->character));
      echo_off(d);
      STATE(d) = CON_NEWPASSWD;
    } else if (*arg == 'n' || *arg == 'N') {
      write_to_output(d, "Okay, what IS it, then? ");
      free(d->character->player.name);
      d->character->player.name = NULL;
      STATE(d) = CON_GET_NAME;
    } else
      write_to_output(d, "Please type Yes or No: ");
    break;

  case CON_PASSWORD:		/* get pwd for known player      */
    /* To really prevent duping correctly, the player's record should be reloaded
     * from disk at this point (after the password has been typed).  However I'm
     * afraid that trying to load a character over an already loaded character is
     * going to cause some problem down the road that I can't see at the moment.
     * So to compensate, I'm going to (1) add a 15 or 20-second time limit for
     * entering a password, and (2) re-add the code to cut off duplicates when a
     * player quits.  JE 6 Feb 96 */

    echo_on(d);    /* turn echo back on */

    /* New echo_on() eats the return on telnet. Extra space better than none. */
    write_to_output(d, "\r\n");

    if (!*arg)
      STATE(d) = CON_CLOSE;
    else {
      struct tb_login_rate_entry *rate =
        tb_login_rate_entry_for(GET_PC_NAME(d->character), time(NULL));
      enum tb_pw_result pw;

      if (rate != NULL && rate->failures >= TB_LOGIN_MAX_FAILURES) {
        write_to_output(d, "Too many login attempts. Try again later.\r\n");
        STATE(d) = CON_CLOSE;
        return;
      }

      pw = tb_password_verify(arg, GET_PASSWD(d->character),
                              GET_IDNUM(d->character),
                              GET_PC_NAME(d->character));
      if (pw == TB_PW_UNVERIFIABLE) {
        /* Reported (rate limited). Not the player's mistake, so not a bad
         * password; but it is an attempt, so a reconnect loop still meets
         * the account's login rate limit. */
        if (rate != NULL && rate->failures < TB_LOGIN_MAX_FAILURES)
          ++rate->failures;
        write_to_output(d, "%s", TB_PW_CANNOT_CHECK);
        STATE(d) = CON_CLOSE;
        return;
      }
      if (pw != TB_PW_MATCH) {
        if (rate != NULL && rate->failures < TB_LOGIN_MAX_FAILURES)
          ++rate->failures;
	mudlog(BRF, LVL_GOD, TRUE, "Bad PW: %s [%s]", GET_NAME(d->character), d->host);
	GET_BAD_PWS(d->character)++;
	save_char(d->character);
	if (++(d->bad_pws) >= CONFIG_MAX_BAD_PWS) {	/* 3 strikes and you're out. */
	  write_to_output(d, "Wrong password... disconnecting.\r\n");
	  STATE(d) = CON_CLOSE;
	} else {
	  write_to_output(d, "Wrong password.\r\nPassword: ");
	  echo_off(d);
	}
	return;
      }

      /* Upgrade only after proving ownership of the legacy credential.
       * A save failure must not prevent the old credential from remaining
       * usable: player save atomicity is a separate prerequisite. */
      if (!tb_password_is_argon2id(GET_PASSWD(d->character))) {
        char upgraded[crypto_pwhash_STRBYTES];
        if (tb_password_hash(upgraded, sizeof(upgraded), arg)) {
          strlcpy(GET_PASSWD(d->character), upgraded,
                  sizeof(d->character->player.passwd));
          sodium_memzero(upgraded, sizeof(upgraded));
          save_char(d->character);
        } else {
          TB_REPORT_ERROR("argon2id", "legacy password upgrade failed");
        }
      }

      /* A successful verification clears only this account's failure count. */
      if (rate != NULL)
        rate->failures = 0;

      /* Password was correct. */
      load_result = GET_BAD_PWS(d->character);
      GET_BAD_PWS(d->character) = 0;
      d->bad_pws = 0;

      if (isbanned(d->host) == BAN_SELECT &&
	  !PLR_FLAGGED(d->character, PLR_SITEOK)) {
	write_to_output(d, "Sorry, this char has not been cleared for login from your site!\r\n");
	STATE(d) = CON_CLOSE;
	mudlog(NRM, LVL_GOD, TRUE, "Connection attempt for %s denied from %s", GET_NAME(d->character), d->host);
	return;
      }
      if (GET_LEVEL(d->character) < circle_restrict) {
	write_to_output(d, "The game is temporarily restricted.. try again later.\r\n");
	STATE(d) = CON_CLOSE;
	mudlog(NRM, LVL_GOD, TRUE, "Request for login denied for %s [%s] (wizlock)", GET_NAME(d->character), d->host);
	return;
      }
      /* check and make sure no other copies of this player are logged in */
      if (perform_dupe_check(d))
	return;

      if (GET_LEVEL(d->character) >= LVL_IMMORT)
	write_to_output(d, "%s", imotd);
      else
	write_to_output(d, "%s", motd);

      if (GET_INVIS_LEV(d->character))
        mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "%s has connected. (invis %d)", GET_NAME(d->character), GET_INVIS_LEV(d->character));
      else
        mudlog(BRF, LVL_IMMORT, TRUE, "%s has connected.", GET_NAME(d->character));

      /* Add to the list of 'recent' players (since last reboot) */
      if (AddRecentPlayer(GET_NAME(d->character), d->host, FALSE, FALSE) == FALSE)
      {
        mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "Failure to AddRecentPlayer (returned FALSE).");
      }

      if (load_result) {
        write_to_output(d, "\r\n\r\n\007\007\007"
		"%s%d LOGIN FAILURE%s SINCE LAST SUCCESSFUL LOGIN.%s\r\n",
		CCRED(d->character, C_SPR), load_result,
		(load_result > 1) ? "S" : "", CCNRM(d->character, C_SPR));
	GET_BAD_PWS(d->character) = 0;
      }
      write_to_output(d, "\r\n*** PRESS RETURN: ");
      STATE(d) = CON_RMOTD;
    }
    break;

  case CON_NEWPASSWD:
  case CON_CHPWD_GETNEW:
    if (!*arg || strlen(arg) > MAX_PWD_LENGTH || strlen(arg) < 3 ||
	!str_cmp(arg, GET_PC_NAME(d->character))) {
      write_to_output(d, "\r\nIllegal password.\r\nPassword: ");
      return;
    }
    if (!tb_password_hash(GET_PASSWD(d->character),
                          sizeof(d->character->player.passwd), arg)) {
      TB_REPORT_ERROR("argon2id", "password hashing failed");
      write_to_output(d, "\r\nPassword setup unavailable; try again later.\r\n");
      STATE(d) = CON_CLOSE;
      return;
    }

    write_to_output(d, "\r\nPlease retype password: ");
    if (STATE(d) == CON_NEWPASSWD)
      STATE(d) = CON_CNFPASSWD;
    else
      STATE(d) = CON_CHPWD_VRFY;
    break;

  case CON_CNFPASSWD:
  case CON_CHPWD_VRFY: {
    enum tb_pw_result pw = tb_password_verify(arg, GET_PASSWD(d->character),
                                              GET_IDNUM(d->character),
                                              GET_PC_NAME(d->character));

    if (pw == TB_PW_UNVERIFIABLE) {
      /* The new hash is already in memory but unconfirmed: never return to
       * a state that could save it.  Close instead (neither state is in
       * the game, so the character is not saved). */
      echo_on(d);
      write_to_output(d, "%s", TB_PW_CANNOT_CHECK);
      STATE(d) = CON_CLOSE;
      return;
    }
    if (pw != TB_PW_MATCH) {
      write_to_output(d, "\r\nPasswords don't match... start over.\r\nPassword: ");
      if (STATE(d) == CON_CNFPASSWD)
	STATE(d) = CON_NEWPASSWD;
      else
	STATE(d) = CON_CHPWD_GETNEW;
      return;
    }
    echo_on(d);

    if (STATE(d) == CON_CNFPASSWD) {
      write_to_output(d, "\r\nWhat is your sex (\t(M\t)/\t(F\t))? ");
      STATE(d) = CON_QSEX;
    } else {
      save_char(d->character);
      write_to_output(d, "\r\nDone.\r\n%s", CONFIG_MENU);
      STATE(d) = CON_MENU;
    }
    break;
  }

  case CON_QSEX:		/* query sex of new user         */
    switch (*arg) {
    case 'm':
    case 'M':
      d->character->player.sex = SEX_MALE;
      break;
    case 'f':
    case 'F':
      d->character->player.sex = SEX_FEMALE;
      break;
    default:
      write_to_output(d, "That is not a sex..\r\n"
		"What IS your sex? ");
      return;
    }

    write_to_output(d, "%s\r\nClass: ", class_menu);
    STATE(d) = CON_QCLASS;
    break;

  case CON_QCLASS:
    load_result = parse_class(*arg);
    if (load_result == CLASS_UNDEFINED) {
      write_to_output(d, "\r\nThat's not a class.\r\nClass: ");
      return;
    } else {
      GET_CLASS(d->character) = load_result;
    }

      if (d->olc) {
        free(d->olc);
        d->olc = NULL;
      }
      if (GET_PFILEPOS(d->character) < 0)
      GET_PFILEPOS(d->character) = create_entry(GET_PC_NAME(d->character));
    /* Now GET_NAME() will work properly. */
    init_char(d->character);
    save_char(d->character);
    save_player_index();
    write_to_output(d, "%s\r\n*** PRESS RETURN: ", motd);
    STATE(d) = CON_RMOTD;
    /* make sure the last log is updated correctly. */
    GET_PREF(d->character)= rand_number(1, 128000);
    /* GET_HOST() was already set when the descriptor got its character;
     * free it or every new character leaks one host string. */
    if (GET_HOST(d->character))
      free(GET_HOST(d->character));
    GET_HOST(d->character)= strdup(d->host);

    mudlog(NRM, LVL_GOD, TRUE, "%s [%s] new player.", GET_NAME(d->character), d->host);

    /* Add to the list of 'recent' players (since last reboot) */
    if (AddRecentPlayer(GET_NAME(d->character), d->host, TRUE, FALSE) == FALSE)
    {
      mudlog(BRF, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), TRUE, "Failure to AddRecentPlayer (returned FALSE).");
    }
    break;

  case CON_RMOTD:		/* read CR after printing motd   */
    write_to_output(d, "%s", CONFIG_MENU);
    if (IS_HAPPYHOUR > 0){
      write_to_output(d, "\r\n");
      write_to_output(d, "\tyThere is currently a Happyhour!\tn\r\n");
      write_to_output(d, "\r\n");
    }
    add_llog_entry(d->character, LAST_CONNECT);
    STATE(d) = CON_MENU;
    break;

  case CON_MENU: {		/* get selection from main menu  */

    switch (*arg) {
    case '0':
      write_to_output(d, "Goodbye.\r\n");
      add_llog_entry(d->character, LAST_QUIT);
      STATE(d) = CON_CLOSE;
      break;

    case '1':
      load_result = enter_player_game(d);
      send_to_char(d->character, "%s", CONFIG_WELC_MESSG);

      /* Clear their load room if it's not persistant. */
      if (!PLR_FLAGGED(d->character, PLR_LOADROOM))
        GET_LOADROOM(d->character) = NOWHERE;
      save_char(d->character);

      greet_mtrigger(d->character, -1);
      greet_memory_mtrigger(d->character);

      act("$n has entered the game.", TRUE, d->character, 0, 0, TO_ROOM);

      STATE(d) = CON_PLAYING;
      MXPSendTag( d, "<VERSION>" );
      if (GET_LEVEL(d->character) == 0) {
	do_start(d->character);
	send_to_char(d->character, "%s", CONFIG_START_MESSG);
      }
      look_at_room(d->character, 0);
      if (has_mail(GET_IDNUM(d->character)))
	send_to_char(d->character, "You have mail waiting.\r\n");
      if (load_result == 2) {	/* rented items lost */
	send_to_char(d->character, "\r\n\007You could not afford your rent!\r\n"
		"Your possesions have been donated to the Salvation Army!\r\n");
      }
      d->has_prompt = 0;
      /* We've updated to 3.1 - some bits might be set wrongly: */
      REMOVE_BIT_AR(PRF_FLAGS(d->character), PRF_BUILDWALK);
      break;

    case '2':
      if (d->character->player.description) {
	write_to_output(d, "Current description:\r\n%s", d->character->player.description);
	/* Don't free this now... so that the old description gets loaded as the
	 * current buffer in the editor.  Do setup the ABORT buffer here, however. */
	d->backstr = strdup(d->character->player.description);
      }
      write_to_output(d, "Enter the new text you'd like others to see when they look at you.\r\n");
      send_editor_help(d);
      d->str = &d->character->player.description;
      d->max_str = PLR_DESC_LENGTH;
      STATE(d) = CON_PLR_DESC;
      break;

    case '3':
      page_string(d, background, 0);
      STATE(d) = CON_RMOTD;
      break;

    case '4':
      write_to_output(d, "\r\nEnter your old password: ");
      echo_off(d);
      STATE(d) = CON_CHPWD_GETOLD;
      break;

    case '5':
      write_to_output(d, "\r\nEnter your password for verification: ");
      echo_off(d);
      STATE(d) = CON_DELCNF1;
      break;

    default:
      write_to_output(d, "\r\nThat's not a menu choice!\r\n%s", CONFIG_MENU);
      break;
    }
    break;
  }

  case CON_CHPWD_GETOLD: {
    enum tb_pw_result pw = tb_password_verify(arg, GET_PASSWD(d->character),
                                              GET_IDNUM(d->character),
                                              GET_PC_NAME(d->character));

    if (pw != TB_PW_MATCH) {
      echo_on(d);
      write_to_output(d, "%s%s", pw == TB_PW_UNVERIFIABLE ? TB_PW_CANNOT_CHECK
                      : "\r\nIncorrect password.\r\n", CONFIG_MENU);
      STATE(d) = CON_MENU;
    } else {
      write_to_output(d, "\r\nEnter a new password: ");
      STATE(d) = CON_CHPWD_GETNEW;
    }
    return;
  }

  case CON_DELCNF1: {
    enum tb_pw_result pw;

    echo_on(d);
    pw = tb_password_verify(arg, GET_PASSWD(d->character),
                            GET_IDNUM(d->character), GET_PC_NAME(d->character));
    if (pw != TB_PW_MATCH) {
      write_to_output(d, "%s%s", pw == TB_PW_UNVERIFIABLE ? TB_PW_CANNOT_CHECK
                      : "\r\nIncorrect password.\r\n", CONFIG_MENU);
      STATE(d) = CON_MENU;
    } else {
      write_to_output(d, "\r\nYOU ARE ABOUT TO DELETE THIS CHARACTER PERMANENTLY.\r\n"
		"ARE YOU ABSOLUTELY SURE?\r\n\r\n"
		"Please type \"yes\" to confirm: ");
      STATE(d) = CON_DELCNF2;
    }
    break;
  }

  case CON_DELCNF2:
    if (!strcmp(arg, "yes") || !strcmp(arg, "YES")) {
      if (PLR_FLAGGED(d->character, PLR_FROZEN)) {
	write_to_output(d, "You try to kill yourself, but the ice stops you.\r\n"
		"Character not deleted.\r\n\r\n");
	STATE(d) = CON_CLOSE;
	return;
      }
      if (GET_LEVEL(d->character) < LVL_GRGOD)
	SET_BIT_AR(PLR_FLAGS(d->character), PLR_DELETED);
      save_char(d->character);
      Crash_delete_file(GET_NAME(d->character));
      /* If the selfdelete_fastwipe flag is set (in config.c), remove all the
       * player's immediately. */
      if (selfdelete_fastwipe)
        if ((player_i = get_ptable_by_name(GET_NAME(d->character))) >= 0) {
          SET_BIT(player_table[player_i].flags, PINDEX_SELFDELETE);
          remove_player(player_i);
        }

      delete_variables(GET_NAME(d->character));
      write_to_output(d, "Character '%s' deleted! Goodbye.\r\n", GET_NAME(d->character));
      mudlog(NRM, MAX(LVL_GOD, GET_INVIS_LEV(d->character)), TRUE, "%s (lev %d) has self-deleted.",
       GET_NAME(d->character), GET_LEVEL(d->character));
      STATE(d) = CON_CLOSE;
      return;
    } else {
      write_to_output(d, "\r\nCharacter not deleted.\r\n%s", CONFIG_MENU);
      STATE(d) = CON_MENU;
    }
    break;

  /* It is possible, if enough pulses are missed, to kick someone off while they
   * are at the password prompt. We'll let the game_loop()axe them. */
  case CON_CLOSE:
    break;

  default:
    log("SYSERR: Nanny: illegal state of con'ness (%d) for '%s'; closing connection.",
	STATE(d), d->character ? GET_NAME(d->character) : "<unknown>");
    STATE(d) = CON_DISCONNECT;	/* Safest to do. */
    break;
  }
}
