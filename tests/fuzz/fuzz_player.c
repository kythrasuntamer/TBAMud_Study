/* libFuzzer target for the player and object-save loaders.
 *
 * Player files, the player index, rent (crash-save) files, player variable
 * files and house files are persistent data. A full disk truncates them, a
 * crash leaves half a record, an admin edits them by hand. Every loader must
 * reject a malformed file with a structured diagnostic (tb_error_log_at())
 * and must never crash, leak or corrupt memory. This target writes the fuzz
 * input into a private scratch "lib" directory and runs the production
 * readers on it end to end:
 *
 *   players.c    load_char() and its tag parsers (Affs, Skil, Qest, Alis,
 *                Desc, Act/Aff/Pref ...), build_player_index(),
 *                clean_pfiles(), save_char() of what was loaded
 *   dg_scripts.c read_saved_vars_ascii() ("Vars:" in a pfile) and
 *                read_saved_vars() (the legacy plrvars file)
 *   objsave.c    Crash_load() (objsave_parse_objects_from(), handle_obj(),
 *                auto_equip()), Crash_listrent(), Crash_clean_file(),
 *                Crash_crashsave() of what was loaded
 *   house.c      House_boot() on a binary hcontrol, House_load(),
 *                House_listrent() (hcontrol show), do_house(), the legacy
 *                binary house converter (hcontrol asciiconvert)
 *
 * The object and character lifecycle behind them is production code too:
 * db.c (read_object, create_obj, free_obj, free_char, asciiflag_conv),
 * handler.c (obj_to_char, equip_char, extract_obj, affect_to_char ...),
 * utils.c (get_line, get_filename, mudlog), genobj.c, dg_handler.c,
 * dg_variables.c. Only output (send_to_char, act, page_string) and a few
 * game-wide hooks are replaced; see fuzz_player_stubs.c.
 *
 * Input: byte 0 selects the loader (enum fuzz_mode: a mode letter, or any
 * other byte modulo the mode count); the rest is the file content. The
 * login mode takes "pfile \0 rent file \0 variables file".
 *
 * Beyond ASan/UBSan/LeakSanitizer the harness checks, after every input:
 *   - no object is left on object_list once the character, the rooms and
 *     the listings are cleaned up (a loader that creates an object and
 *     drops it leaks it into the world, where LeakSanitizer cannot see it);
 *   - every obj_index[].number is back to 0 (read_object() accounting);
 *   - no file name built from file content leaves the lib tree
 *     (get_filename() with a "../" name: a damaged pfile or index entry
 *     would make the server write or unlink outside lib/);
 *   - core_dump() (a production assertion) is never reached.
 * A failed check aborts with a "fuzz_player:" message, like a sanitizer.
 * FUZZ_PLAYER_SOFT_CHECKS=1 counts them instead (to keep fuzzing past a
 * known finding); the counts are printed at exit.
 *
 * A loader that calls exit(), or reports TB_FATAL_MSG and is about to
 * abort(), is turned into a clean return: deliberate termination on bad
 * input is not memory corruption. Both are counted and printed at exit.
 *
 * Build: cmake -DENABLE_FUZZING=ON -DCMAKE_C_COMPILER=clang ...
 * Run:   bin/fuzz_player -dict=tests/fuzz/fuzz_player.dict \
 *          -max_len=65536 -timeout=10 tests/fuzz/corpus/player
 * The scratch lib lives under $TMPDIR (default /tmp) and is removed at exit.
 * The process changes into it while a loader runs, so the harness makes a
 * relative -artifact_prefix / -exact_artifact_path absolute at startup.
 */
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "house.h"
#include "constants.h"
#include "pfdefaults.h"

#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdint.h>
#include <sys/stat.h>

#include "fuzz_player.h"

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */
enum fuzz_mode {
  MODE_PFILE,     /* 'P' pfile -> load_char(), save_char() */
  MODE_INDEX,     /* 'I' player index -> build_player_index(), clean_pfiles() */
  MODE_RENT,      /* 'R' rent file -> Crash_listrent(), Crash_load() ... */
  MODE_LOGIN,     /* 'L' pfile \0 rent \0 vars -> the enter_player_game() path */
  MODE_VARS,      /* 'V' legacy plrvars file -> read_saved_vars() */
  MODE_HOUSE,     /* 'H' house object file -> House_boot(), House_listrent() */
  MODE_HCONTROL,  /* 'C' binary hcontrol -> House_boot(), hcontrol show, do_house */
  MODE_BINHOUSE,  /* 'B' legacy binary house file -> hcontrol asciiconvert */
  MODE_COUNT
};

static const char mode_letters[MODE_COUNT] = { 'P', 'I', 'R', 'L', 'V', 'H', 'C', 'B' };
static const char *const mode_names[MODE_COUNT] = {
  "pfile", "index", "rent", "login", "vars", "house", "hcontrol", "binhouse"
};

/* The character every mode loads. get_filename() puts it under F-J. */
#define FUZZ_NAME        "fuzzer"
#define FUZZ_NAME_CAP    "Fuzzer"
#define FUZZ_IDNUM       4242L
#define FUZZ_PFILE       LIB_PLRFILES "F-J" SLASH FUZZ_NAME "." SUF_PLR
#define FUZZ_RENTFILE    LIB_PLROBJS "F-J" SLASH FUZZ_NAME "." SUF_OBJS
#define FUZZ_VARSFILE    LIB_PLRVARS "F-J" SLASH FUZZ_NAME "." SUF_MEM
#define FUZZ_INDEXFILE   LIB_PLRFILES INDEX_FILE

/* The house: room 18500 with its exit south to the atrium 18501. */
#define HOUSE_VNUM       18500
#define ATRIUM_VNUM      18501
#define HOUSE_FILE       LIB_HOUSE "18500.house"
#define HOUSE_ASCII_FILE LIB_HOUSE "18500.house.ascii"

/* ------------------------------------------------------------------ */
/* Check and interception state                                        */
/* ------------------------------------------------------------------ */
static sigjmp_buf loader_env;
static volatile sig_atomic_t in_loader;     /* a loader is running */
static int fatal_is_rejection;              /* TB_FATAL in this mode = clean refusal */
static int soft_checks;                     /* FUZZ_PLAYER_SOFT_CHECKS=1 */
static int verbose;                         /* FUZZ_PLAYER_VERBOSE=1: print diagnostics */
static enum fuzz_mode current_mode;

static unsigned long n_inputs, n_diagnostics, n_exit, n_fatal;
static unsigned long n_world_leak, n_index_count, n_traversal, n_core_dump;
static unsigned long n_mode[MODE_COUNT];

static int orig_cwd_fd = -1, scratch_fd = -1;
static char scratch_dir[PATH_MAX];
static char **fixed_argv;   /* argv as rewritten at startup (kept reachable) */

/* Known findings, recorded in tests/fuzz/known-crashes/player/. A campaign
 * sets FUZZ_PLAYER_MASK=name[,name...] to step around the ones it has
 * already reported and keep fuzzing; without it every one still reproduces
 * (the DISABLED known-crash ctests rely on that). name-path is fixed:
 * get_filename() rejects such names (2f7889b), so masking it only hides a
 * regression; name-path-login now stops at vars-noname instead. */
enum known_bug {
  KNOWN_VARS_NONAME,    /* read_saved_vars() fopen()s an unwritten name buffer */
  KNOWN_HCONTROL_TIME,  /* hcontrol show prints an unwritten date buffer */
  KNOWN_NAME_PATH,       /* a file-supplied name flows into get_filename() unchecked */
  KNOWN_FREAD_STRING_NUL, /* fread_string_checked() underflows on a NUL-led line */
  KNOWN_CONTROW_LEAK,    /* a rent object whose container never arrives is leaked */
  KNOWN_RESTRUNG_LEAK,   /* a repeated Name/Shrt/Desc record tag leaks the first */
  KNOWN_COUNT
};
static const char *const known_bug_names[KNOWN_COUNT] = {
  "vars-noname", "hcontrol-time", "name-path", "fread-string-nul",
  "controw-leak", "restrung-leak"
};

/* KNOWN_RESTRUNG_LEAK: objsave.c sets temp->name/short_description/description
 * with a raw strdup(line) (objsave.c:1468/1490/1404) without freeing the
 * previous value, so a record that repeats Name:/Shrt:/Desc: leaks the first
 * copy. TRUE if any '#'-delimited record in the rent/house body repeats one
 * of those tags. */
static int record_has_repeated_string_tag(const uint8_t *data, size_t size)
{
  const char *tags[] = { "Name", "Shrt", "Desc" };
  int seen[3];
  size_t i = 0;

  memset(seen, 0, sizeof(seen));
  while (i < size) {
    size_t start = i, len;
    int t;

    while (i < size && data[i] != '\n')
      i++;
    len = i - start;
    if (i < size)
      i++;   /* skip the newline */
    if (len == 0)
      continue;
    if (data[start] == '#') {
      memset(seen, 0, sizeof(seen));
      continue;
    }
    for (t = 0; t < 3; t++)
      if (len >= 4 && memcmp(data + start, tags[t], 4) == 0) {
        if (seen[t])
          return TRUE;
        seen[t] = 1;
      }
  }
  return FALSE;
}

/* KNOWN_FREAD_STRING_NUL: fread_string_checked() does point-- then reads
 * *point before the point > tmp guard, so a '~'-string line that fgets()
 * returns empty (its first byte is NUL) underflows tmp[] by one. It is
 * reached for Desc, ADes and EDes. Conservatively skip any string-reading
 * input whose body holds a NUL (a real save file never does). */
static int body_has_nul(const uint8_t *data, size_t size)
{
  return memchr(data, '\0', size) != NULL;
}
static int masked[KNOWN_COUNT];

static void parse_masks(const char *list)
{
  int k;

  for (k = 0; k < KNOWN_COUNT && list != NULL; k++) {
    const char *p = list;
    size_t n = strlen(known_bug_names[k]);

    while ((p = strstr(p, known_bug_names[k])) != NULL) {
      if ((p == list || p[-1] == ',') && (p[n] == '\0' || p[n] == ','))
        masked[k] = TRUE;
      p += n;
    }
  }
}

/* A harness check failed: abort like a sanitizer, or count it. */
static void check_failed(unsigned long *counter, const char *what, const char *detail)
{
  ++*counter;
  if (soft_checks) {
    if (*counter == 1)
      fprintf(stderr, "fuzz_player: [soft] %s (%s mode): %s\n", what,
              mode_names[current_mode], detail);
    return;
  }
  fprintf(stderr, "==fuzz_player== ERROR: %s (%s mode): %s\n", what,
          mode_names[current_mode], detail);
  abort();
}

/* ------------------------------------------------------------------ */
/* Link-time wrappers (-Wl,--wrap=...)                                 */
/* ------------------------------------------------------------------ */
void __real_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
  __attribute__ ((format (printf, 9, 10)));
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
  __attribute__ ((format (printf, 9, 10)));

/* Every structured diagnostic. The detail is formatted here first, so a
 * format/argument mismatch in the caller is caught, then handed to the real
 * logger (into the in-memory log). TB_FATAL_MSG is documented as "logged
 * immediately before abort()": in a mode where that is a deliberate refusal
 * (boot-time loaders), unwind to the harness instead of aborting. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  char detail[MAX_STRING_LENGTH / 2];
  va_list args;

  if (detail_fmt == NULL)
    strlcpy(detail, "(no detail)", sizeof(detail));
  else {
    va_start(args, detail_fmt);
    if (vsnprintf(detail, sizeof(detail), detail_fmt, args) < 0)
      strlcpy(detail, "(detail formatting failed)", sizeof(detail));
    va_end(args);
  }
  n_diagnostics++;
  if (verbose)
    fprintf(stderr, "fuzz_player: diag %s %s/%s %s at %s:%d: %s\n",
            severity ? severity : "?", subsystem ? subsystem : "?",
            operation ? operation : "?", resource ? resource : "?",
            source_file ? source_file : "?", source_line, detail);
  __real_tb_error_log_at(severity, subsystem, operation, resource, errnum,
                         source_file, source_line, source_func, "%s", detail);

  if (severity != NULL && !strcmp(severity, "FATAL") && in_loader &&
      fatal_is_rejection) {
    n_fatal++;
    siglongjmp(loader_env, 2);
  }
}

void __real_exit(int status) __attribute__ ((noreturn));
void __wrap_exit(int status) __attribute__ ((noreturn));
/* exit() on bad input from inside a loader becomes a clean return; any
 * other exit() (libFuzzer's own) is passed through. */
void __wrap_exit(int status)
{
  if (in_loader) {
    if (n_exit++ == 0)
      fprintf(stderr, "fuzz_player: note: a loader called exit(%d) (%s mode); "
              "treated as a clean refusal\n", status, mode_names[current_mode]);
    siglongjmp(loader_env, 3);
  }
  __real_exit(status);
}

void __wrap_core_dump_real(const char *who, int line);
/* core_dump() is the server's soft assertion (log, fork a core, carry on).
 * Reaching it from file content means a loader broke a handler.c contract. */
void __wrap_core_dump_real(const char *who, int line)
{
  char detail[256];

  snprintf(detail, sizeof(detail), "core_dump() at %s:%d", who ? who : "?", line);
  check_failed(&n_core_dump, "production assertion reached", detail);
}

int __real_get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name);
int __wrap_get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name);
/* Player files are named after the character. The name comes from the
 * player index or from a pfile's Name tag, both file content. A name with a
 * ".." component makes the server read, write or unlink outside lib/. The
 * real result is checked and never used when it escapes, so the harness
 * cannot touch files outside its scratch directory. */
int __wrap_get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name)
{
  int ok = __real_get_filename(filename, fbufsize, mode, orig_name);

  if (ok && filename != NULL &&
      (!strncmp(filename, "../", 3) || strstr(filename, "/../") != NULL ||
       (strlen(filename) >= 3 && !strcmp(filename + strlen(filename) - 3, "/..")))) {
    char detail[PATH_MAX + 64];

    snprintf(detail, sizeof(detail), "get_filename() built \"%s\" from a "
             "file-supplied name", filename);
    /* KNOWN_NAME_PATH: get_filename() builds a path from a file-supplied
     * player name (an index record, or a pfile Name tag read before login)
     * without rejecting '..'; every caller trusts the result. */
    if (masked[KNOWN_NAME_PATH])
      n_traversal++;
    else
      check_failed(&n_traversal, "file name escapes the lib directory", detail);
    *filename = '\0';
    return 0;
  }
  return ok;
}

void __wrap_autoquest_trigger_check(struct char_data *ch, struct char_data *vict,
                                    struct obj_data *object, int type);
/* Autoquest completion is game logic, not loading. The fixture defines no
 * quests, for which the real function returns at once. */
void __wrap_autoquest_trigger_check(struct char_data *ch, struct char_data *vict,
                                    struct obj_data *object, int type)
{
  (void)ch; (void)vict; (void)object; (void)type;
}

void __wrap_page_string(struct descriptor_data *d, char *str, int keep_internal);
/* The pager: read the whole page (catches an unterminated listing buffer). */
void __wrap_page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  if (str != NULL)
    fuzz_sink_string(str);
}

/* ------------------------------------------------------------------ */
/* Fixture world: rooms, object prototypes, the player index           */
/* ------------------------------------------------------------------ */
#define N_ROOMS 4
static struct room_data rooms[N_ROOMS];
static struct room_direction_data house_exit, atrium_exit;
static const room_vnum room_vnums[N_ROOMS] = { 1204, 3001, HOUSE_VNUM, ATRIUM_VNUM };
#define RNUM_START  1
#define RNUM_HOUSE  2
#define RNUM_ATRIUM 3

struct proto_spec {
  obj_vnum vnum;
  int type;
  const char *name, *sdesc, *ldesc, *adesc, *edesc_key, *edesc;
  int wear[3];          /* ITEM_WEAR_* bits, -1 terminated */
  int extra[2];         /* ITEM_* extra-flag bits, -1 terminated */
  int val[NUM_OBJ_VAL_POSITIONS];
  int weight, cost, rent;
  int aff_loc[2], aff_mod[2];
  int perm_aff;         /* AFF_* bit in the permanent bitvector, 0 for none */
};

/* Sorted by vnum (real_object() is a binary search). The low vnums cover
 * every placement path in handle_obj()/auto_equip(): containers with and
 * without a weight limit, a worn container, a light, armour, a weapon,
 * alignment- and class-restricted items. The rest are the vnums in the
 * sample rent files the seed corpus is built from. */
static const struct proto_spec proto_specs[] = {
  { 100, ITEM_CONTAINER, "bag sack", "a canvas sack", "A canvas sack lies here.", NULL,
    "sack bag", "It is a plain canvas sack.\r\n",
    { ITEM_WEAR_TAKE, -1 }, { -1 }, { 50, 0, 0, 0 }, 5, 10, 1, { 0 }, { 0 }, 0 },
  { 101, ITEM_TRASH, "trinket", "a small trinket", "A small trinket lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { -1 }, { 0 }, 1, 1, 0, { 0 }, { 0 }, 0 },
  { 102, ITEM_CONTAINER, "cloak pockets", "a cloak with many pockets",
    "A pocketed cloak is folded here.", NULL, NULL, NULL,
    { ITEM_WEAR_TAKE, ITEM_WEAR_ABOUT, -1 }, { -1 }, { 20, 0, 0, 0 }, 4, 50, 5,
    { APPLY_AC, 0 }, { -1, 0 }, 0 },
  { 103, ITEM_LIGHT, "lantern", "a brass lantern", "A brass lantern is here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { ITEM_GLOW, -1 }, { 0, 0, 24, 0 },
    3, 20, 2, { 0 }, { 0 }, 0 },
  { 104, ITEM_ARMOR, "breastplate plate", "a steel breastplate",
    "A steel breastplate stands here.", NULL, NULL, NULL,
    { ITEM_WEAR_TAKE, ITEM_WEAR_BODY, -1 }, { -1 }, { 5, 0, 0, 0 }, 20, 300, 30,
    { APPLY_HIT, APPLY_CON }, { 10, 1 }, 0 },
  { 105, ITEM_WEAPON, "longsword sword", "a gleaming longsword",
    "A gleaming longsword lies here.", "You swing $p.\r\n", "sword", "A fine blade.\r\n",
    { ITEM_WEAR_TAKE, ITEM_WEAR_WIELD, -1 }, { ITEM_ANTI_EVIL, -1 }, { 0, 2, 6, 3 },
    8, 500, 40, { APPLY_STR, APPLY_DAMROLL }, { 2, 1 }, 0 },
  { 106, ITEM_WORN, "ring silver", "a silver ring", "A silver ring glints here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_FINGER, -1 }, { ITEM_ANTI_GOOD, -1 }, { 0 },
    1, 100, 10, { APPLY_SAVING_SPELL, 0 }, { -2, 0 }, AFF_INFRAVISION },
  { 107, ITEM_WORN, "amulet", "an onyx amulet", "An onyx amulet lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_NECK, -1 },
    { ITEM_ANTI_WARRIOR, ITEM_ANTI_MAGIC_USER }, { 0 }, 1, 100, 10,
    { APPLY_MANA, 0 }, { 20, 0 }, AFF_DETECT_INVIS },
  { 108, ITEM_CONTAINER, "chest", "a bottomless chest", "A bottomless chest sits here.",
    "The lid creaks.\r\n", NULL, NULL, { ITEM_WEAR_TAKE, -1 }, { -1 }, { 0, 0, 0, 0 },
    30, 1000, 0, { 0 }, { 0 }, 0 },
  { 3021, ITEM_WEAPON, "dagger", "a dagger", "A dagger lies here.", NULL, NULL, NULL,
    { ITEM_WEAR_TAKE, ITEM_WEAR_WIELD, -1 }, { -1 }, { 0, 1, 4, 11 }, 1, 10, 1, { 0 }, { 0 }, 0 },
  { 3022, ITEM_WEAPON, "mace", "a heavy mace", "A heavy mace lies here.", NULL, NULL, NULL,
    { ITEM_WEAR_TAKE, ITEM_WEAR_WIELD, -1 }, { ITEM_ANTI_CLERIC, -1 }, { 0, 2, 5, 7 }, 9, 60, 6,
    { 0 }, { 0 }, 0 },
  { 3042, ITEM_ARMOR, "shield", "a wooden shield", "A wooden shield lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_SHIELD, -1 }, { -1 }, { 2, 0, 0, 0 }, 6, 40, 4,
    { 0 }, { 0 }, 0 },
  { 3050, ITEM_SCROLL, "scroll", "a scroll of recall", "A scroll lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { ITEM_MAGIC, -1 }, { 12, 42, -1, -1 },
    1, 50, 5, { 0 }, { 0 }, 0 },
  { 3051, ITEM_POTION, "potion yellow", "a yellow potion", "A yellow potion is here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { ITEM_MAGIC, -1 }, { 12, 33, -1, -1 },
    1, 50, 5, { 0 }, { 0 }, 0 },
  { 3052, ITEM_POTION, "potion red", "a red potion", "A red potion is here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { ITEM_MAGIC, -1 }, { 12, 16, -1, -1 },
    1, 50, 5, { 0 }, { 0 }, 0 },
  { 3053, ITEM_WAND, "wand", "a thin wand", "A thin wand lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { ITEM_MAGIC, -1 }, { 10, 5, 5, 32 },
    1, 200, 20, { 0 }, { 0 }, 0 },
  { 3054, ITEM_STAFF, "staff", "a gnarled staff", "A gnarled staff lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_HOLD, -1 }, { ITEM_MAGIC, -1 }, { 10, 3, 3, 26 },
    4, 300, 30, { 0 }, { 0 }, 0 },
  { 12008, ITEM_ARMOR, "boots iron", "a pair of iron boots", "Iron boots stand here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_FEET, -1 }, { -1 }, { 3, 0, 0, 0 }, 4, 60, 6,
    { 0 }, { 0 }, 0 },
  { 12030, ITEM_DRINKCON, "flask", "a leather flask", "A leather flask lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, -1 }, { -1 }, { 10, 5, 0, 0 }, 2, 10, 0, { 0 }, { 0 }, 0 },
  { 24295, ITEM_DRINKCON, "bottle", "a glass bottle", "A glass bottle lies here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, -1 }, { -1 }, { 4, 0, 0, 0 }, 5, 3, 0, { 0 }, { 0 }, 0 },
  { 55561, ITEM_ARMOR, "jerkin", "a leather jerkin", "A leather jerkin is here.", NULL,
    NULL, NULL, { ITEM_WEAR_TAKE, ITEM_WEAR_BODY, -1 }, { -1 }, { 1, 0, 0, 0 }, 2, 30, 3,
    { 0 }, { 0 }, 0 },
  { 55563, ITEM_WEAPON, "glass sword", "a glass sword", "A glass sword lies here.", NULL,
    "glass sword", "Invisible to the naked eye.\r\n", { ITEM_WEAR_TAKE, ITEM_WEAR_WIELD, -1 },
    { ITEM_INVISIBLE, -1 }, { 0, 4, 8, 3 }, 1, 9999, 0,
    { APPLY_HITROLL, APPLY_DAMROLL }, { 5, 5 }, 0 },
};
#define N_PROTOS ((int) (sizeof(proto_specs) / sizeof(proto_specs[0])))

static struct obj_data protos[N_PROTOS];
static struct index_data proto_index[N_PROTOS];

static char *xstrdup(const char *s)
{
  char *copy = s ? strdup(s) : NULL;

  if (s && !copy)
    abort();
  return copy;
}

/* Prototypes are rebuilt for every input, so a loader that frees or
 * overwrites prototype data is caught (double free / use after free) in
 * the input that did it, not in a later one. */
static void build_protos(void)
{
  int i, j;

  memset(protos, 0, sizeof(protos));
  memset(proto_index, 0, sizeof(proto_index));
  for (i = 0; i < N_PROTOS; i++) {
    const struct proto_spec *s = &proto_specs[i];
    struct obj_data *o = &protos[i];

    clear_object(o);
    o->item_number = i;
    o->name = xstrdup(s->name);
    o->short_description = xstrdup(s->sdesc);
    o->description = xstrdup(s->ldesc);
    o->action_description = xstrdup(s->adesc);
    if (s->edesc_key) {
      CREATE(o->ex_description, struct extra_descr_data, 1);
      o->ex_description->keyword = xstrdup(s->edesc_key);
      o->ex_description->description = xstrdup(s->edesc);
    }
    GET_OBJ_TYPE(o) = s->type;
    for (j = 0; j < 3 && s->wear[j] >= 0; j++)
      SET_BIT_AR(GET_OBJ_WEAR(o), s->wear[j]);
    for (j = 0; j < 2 && s->extra[j] > 0; j++)
      SET_BIT_AR(GET_OBJ_EXTRA(o), s->extra[j]);
    for (j = 0; j < NUM_OBJ_VAL_POSITIONS; j++)
      GET_OBJ_VAL(o, j) = s->val[j];
    GET_OBJ_WEIGHT(o) = s->weight;
    GET_OBJ_COST(o) = s->cost;
    GET_OBJ_RENT(o) = s->rent;
    for (j = 0; j < 2; j++) {
      o->affected[j].location = s->aff_loc[j];
      o->affected[j].modifier = s->aff_mod[j];
    }
    if (s->perm_aff)
      SET_BIT_AR(GET_OBJ_AFFECT(o), s->perm_aff);
    proto_index[i].vnum = s->vnum;
    proto_index[i].number = 0;
  }
  obj_proto = protos;
  obj_index = proto_index;
  top_of_objt = N_PROTOS - 1;
}

static void free_protos(void)
{
  int i;

  for (i = 0; i < N_PROTOS; i++) {
    struct obj_data *o = &protos[i];

    free(o->name);
    free(o->short_description);
    free(o->description);
    free(o->action_description);
    if (o->ex_description) {
      free(o->ex_description->keyword);
      free(o->ex_description->description);
      free(o->ex_description);
    }
  }
  memset(protos, 0, sizeof(protos));
}

static void build_world(void)
{
  int i;

  memset(rooms, 0, sizeof(rooms));
  memset(&house_exit, 0, sizeof(house_exit));
  memset(&atrium_exit, 0, sizeof(atrium_exit));
  for (i = 0; i < N_ROOMS; i++) {
    rooms[i].number = room_vnums[i];
    rooms[i].zone = 0;
    rooms[i].name = (char *) "A room";
    rooms[i].description = (char *) "A room.\r\n";
  }
  house_exit.to_room = RNUM_ATRIUM;
  atrium_exit.to_room = RNUM_HOUSE;
  rooms[RNUM_HOUSE].dir_option[SOUTH] = &house_exit;
  rooms[RNUM_ATRIUM].dir_option[NORTH] = &atrium_exit;
  world = rooms;
  top_of_world = N_ROOMS - 1;
}

/* The player index a running game would have: the fuzzed character plus
 * the players the sample files mention (house owner and guest ids). */
static void build_player_table(void)
{
  static const struct { long id; const char *name; int level; } pt[] = {
    { 1, "daeros", LVL_IMPL }, { 2, "kythra", 50 }, { 5, "alyssa", 1 },
    { FUZZ_IDNUM, FUZZ_NAME, 10 },
  };
  int i, n = (int) (sizeof(pt) / sizeof(pt[0]));

  CREATE(player_table, struct player_index_element, n);
  for (i = 0; i < n; i++) {
    player_table[i].id = pt[i].id;
    player_table[i].name = xstrdup(pt[i].name);
    player_table[i].level = pt[i].level;
    player_table[i].flags = 0;
    player_table[i].last = time(0);
  }
  top_of_p_table = top_of_p_file = n - 1;
  top_idnum = FUZZ_IDNUM;
}

/* ------------------------------------------------------------------ */
/* Characters                                                          */
/* ------------------------------------------------------------------ */
static struct descriptor_data player_desc;

/* A character as nanny() allocates one before load_char(). */
static struct char_data *new_player_shell(void)
{
  struct char_data *ch;

  CREATE(ch, struct char_data, 1);
  clear_char(ch);
  CREATE(ch->player_specials, struct player_special_data, 1);
  new_mobile_data(ch);
  memset(&player_desc, 0, sizeof(player_desc));
  strlcpy(player_desc.host, "127.0.0.1", sizeof(player_desc.host));
  STATE(&player_desc) = CON_MENU;
  player_desc.character = ch;
  GET_HOST(ch) = xstrdup(player_desc.host);
  ch->desc = &player_desc;
  return ch;
}

/* The fuzzed player without a pfile (rent-only modes): an evil warrior, so
 * the anti-evil and anti-warrior items in a rent file are refused by
 * auto_equip() and the warrior WEAR_HOLD weapon rule applies. */
static struct char_data *new_fixture_player(void)
{
  struct char_data *ch = new_player_shell();

  ch->player.name = xstrdup(FUZZ_NAME_CAP);
  GET_IDNUM(ch) = FUZZ_IDNUM;
  GET_LEVEL(ch) = 10;
  GET_CLASS(ch) = CLASS_WARRIOR;
  GET_ALIGNMENT(ch) = -500;
  GET_GOLD(ch) = 1000;
  GET_BANK_GOLD(ch) = 5000;
  GET_PFILEPOS(ch) = (int) get_ptable_by_name(FUZZ_NAME);
  ch->player.time.logon = time(0);
  return ch;
}

/* An immortal with no descriptor, for listings and hcontrol. */
static struct char_data *new_immortal(void)
{
  struct char_data *imm;

  CREATE(imm, struct char_data, 1);
  clear_char(imm);
  CREATE(imm->player_specials, struct player_special_data, 1);
  new_mobile_data(imm);
  imm->player.name = xstrdup("Daeros");
  GET_IDNUM(imm) = 1;
  GET_LEVEL(imm) = LVL_IMPL;
  return imm;
}

/* extract_char() without the game: the character's objects go through the
 * real extract_obj() (which unequips, unlinks and frees), then free_char(). */
static void release_char(struct char_data *ch)
{
  int i;

  if (ch == NULL)
    return;
  for (i = 0; i < NUM_WEARS; i++)
    if (GET_EQ(ch, i))
      extract_obj(GET_EQ(ch, i));
  while (ch->carrying)
    extract_obj(ch->carrying);
  if (IN_ROOM(ch) != NOWHERE)
    char_from_room(ch);
  free_char(ch);
}

/* ------------------------------------------------------------------ */
/* Scratch lib directory                                               */
/* ------------------------------------------------------------------ */
static const char *const lib_dirs[] = {
  "plrfiles", "plrobjs", "plrvars", "plrtext", "house", "etc"
};
static const char *const letter_dirs[] = { "A-E", "F-J", "K-O", "P-T", "U-Z", "ZZZ" };

static void make_dir(const char *path)
{
  if (mkdir(path, 0700) != 0 && errno != EEXIST) {
    fprintf(stderr, "fuzz_player: mkdir %s: %s\n", path, strerror(errno));
    abort();
  }
}

/* Remove every file under dir (one level of subdirectories). */
static void clear_dir(const char *dir, int depth)
{
  DIR *d = opendir(dir);
  struct dirent *e;
  char path[PATH_MAX];

  if (d == NULL)
    return;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if (depth > 0 && e->d_type == DT_DIR)
      clear_dir(path, depth - 1);
    else
      (void) unlink(path);
  }
  closedir(d);
}

/* Called inside the scratch directory before every input, so a file left by
 * one input (a staged save, a converted house file) never changes the next. */
static void clear_scratch(void)
{
  size_t i;

  for (i = 0; i < sizeof(lib_dirs) / sizeof(lib_dirs[0]); i++)
    clear_dir(lib_dirs[i], 1);
}

static void remove_scratch(void)
{
  size_t i, j;
  char path[PATH_MAX];

  if (!*scratch_dir || chdir(scratch_dir) != 0)
    return;
  clear_scratch();
  for (i = 0; i < sizeof(lib_dirs) / sizeof(lib_dirs[0]); i++) {
    for (j = 0; j < sizeof(letter_dirs) / sizeof(letter_dirs[0]); j++) {
      snprintf(path, sizeof(path), "%s/%s", lib_dirs[i], letter_dirs[j]);
      (void) rmdir(path);
    }
    (void) rmdir(lib_dirs[i]);
  }
  if (chdir("/") == 0)
    (void) rmdir(scratch_dir);
}

static void write_file(const char *path, const uint8_t *data, size_t size)
{
  FILE *fl = fopen(path, "wb");

  if (fl == NULL || (size && fwrite(data, 1, size, fl) != size) || fclose(fl) != 0) {
    fprintf(stderr, "fuzz_player: cannot write %s: %s\n", path, strerror(errno));
    abort();
  }
}

static int file_exists(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

/* A one-record hcontrol for the house modes: house 18500, atrium 18501,
 * exit south, owned by the fuzzed player, one guest (kythra, idnum 2). */
static void write_house_control(void)
{
  struct house_control_rec rec;

  memset(&rec, 0, sizeof(rec));
  rec.vnum = HOUSE_VNUM;
  rec.atrium = ATRIUM_VNUM;
  rec.exit_num = SOUTH;
  rec.built_on = 1792958959;
  rec.mode = HOUSE_PRIVATE;
  rec.owner = FUZZ_IDNUM;
  rec.num_of_guests = 1;
  rec.guests[0] = 2;
  rec.last_payment = 1792998534;
  write_file(HCONTROL_FILE, (const uint8_t *) &rec, sizeof(rec));
}

/* ------------------------------------------------------------------ */
/* Per-input world reset and leak checks                               */
/* ------------------------------------------------------------------ */
static void begin_input(void)
{
  clear_scratch();
  build_world();
  build_protos();
  build_player_table();
  object_list = NULL;
  character_list = NULL;
  CONFIG_SCRIPT_PLAYERS = TRUE;
  CONFIG_MAX_OBJ_SAVE = 30;
  CONFIG_MIN_RENT_COST = 100;
  CONFIG_FREE_RENT = FALSE;
  /* Long enough that the sample files' timestamps never expire, short
   * enough that days * SECS_PER_REAL_DAY still fits an int (24855 days). */
  CONFIG_CRASH_TIMEOUT = 20000;
  CONFIG_RENT_TIMEOUT = 20000;
  CONFIG_DIAGONAL_DIRS = FALSE;
  fuzz_log_rewind();
}

/* Everything the loaders created must be gone once its owners are. */
static void end_input(void)
{
  int i;

  for (i = 0; i < N_ROOMS; i++)
    while (world[i].contents)
      extract_obj(world[i].contents);

  if (object_list != NULL) {
    char detail[256];
    struct obj_data *o;
    int n = 0;

    for (o = object_list; o; o = o->next)
      n++;
    snprintf(detail, sizeof(detail), "%d object(s) still on object_list after "
             "cleanup; first is vnum %d (carried_by %p, in_obj %p, in_room %d)",
             n, GET_OBJ_VNUM(object_list), (void *) object_list->carried_by,
             (void *) object_list->in_obj, (int) IN_ROOM(object_list));
    /* KNOWN_CONTROW_LEAK: Crash_load_objs() parks an object nested in a
     * container that never arrives in cont_row[] and neither places nor
     * frees it, so it stays on object_list. */
    if (masked[KNOWN_CONTROW_LEAK])
      n_world_leak++;
    else
      check_failed(&n_world_leak, "objects leaked into the world", detail);
    while (object_list) {
      o = object_list;
      if (o->in_obj)
        obj_from_obj(o);
      o->carried_by = NULL;
      o->worn_by = NULL;
      IN_ROOM(o) = NOWHERE;
      while (o->contains)
        obj_from_obj(o->contains);
      extract_obj(o);
    }
  }
  for (i = 0; i < N_PROTOS; i++)
    if (obj_index[i].number != 0) {
      char detail[160];

      snprintf(detail, sizeof(detail), "obj_index[vnum %d].number is %d after "
               "every object was extracted", obj_index[i].vnum, obj_index[i].number);
      check_failed(&n_index_count, "object count out of balance", detail);
      break;
    }
  free_protos();
  free_player_index();
  top_of_p_table = top_of_p_file = -1;
  top_idnum = 0;
}

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */

/* Split data at NUL bytes into up to max parts (the rest stays in the last). */
static int split_parts(const uint8_t *data, size_t size, const uint8_t **part,
                       size_t *len, int max)
{
  int n = 0;

  while (n < max - 1) {
    const uint8_t *nul = memchr(data, '\0', size);

    if (nul == NULL)
      break;
    part[n] = data;
    len[n] = (size_t) (nul - data);
    n++;
    size -= len[n - 1] + 1;
    data = nul + 1;
  }
  part[n] = data;
  len[n] = size;
  return n + 1;
}

static struct char_data *loaded_ch, *viewer;

static void run_pfile(const uint8_t *data, size_t size)
{
  int id;

  if (masked[KNOWN_FREAD_STRING_NUL] && body_has_nul(data, size))
    return;
  write_file(FUZZ_PFILE, data, size);
  loaded_ch = new_player_shell();
  if ((id = load_char(FUZZ_NAME, loaded_ch)) < 0)
    return;
  /* What nanny() does next; then a save of the loaded character, which
   * walks every loaded affect, alias, quest and variable again. */
  GET_PFILEPOS(loaded_ch) = id;
  loaded_ch->player.time.logon = time(0);
  save_char(loaded_ch);
}

static void run_index(const uint8_t *data, size_t size)
{
  free_player_index();
  player_table = NULL;
  top_of_p_table = -1;
  write_file(FUZZ_INDEXFILE, data, size);

  /* boot_db(): an index with records of which none parse is refused with
   * TB_FATAL_MSG + abort(); that is the documented boot-time behaviour. */
  fatal_is_rejection = TRUE;
  build_player_index();
  fatal_is_rejection = FALSE;

  (void) get_ptable_by_name(FUZZ_NAME);
  (void) get_id_by_name("kythra");
  (void) get_name_by_id(FUZZ_IDNUM);
  save_player_index();      /* writes back the records boot could not parse */
  clean_pfiles();           /* boot_db() with auto_pwipe */
  update_obj_file();        /* boot_db(): Crash_clean_file() for every entry */
}

/* The rent half of enter_player_game() for ch, standing in the start room. */
static void rent_login(struct char_data *ch)
{
  room_rnum load_room = NOWHERE;

  if (GET_LOADROOM(ch) != NOWHERE)
    load_room = real_room(GET_LOADROOM(ch));
  if (load_room == NOWHERE)
    load_room = RNUM_START;
  /* KNOWN_VARS_NONAME: read_saved_vars() ignores get_filename()'s result,
   * which fails for a character with no name (no or empty Name tag, or an
   * Act word that sets the NPC bit), and fopen()s the unwritten buffer. */
  if (!SCRIPT(ch) &&
      !(masked[KNOWN_VARS_NONAME] && (GET_NAME(ch) == NULL || !*GET_NAME(ch))))
    read_saved_vars(ch);
  ch->next = character_list;
  character_list = ch;
  char_to_room(ch, load_room);
  (void) Crash_load(ch);
  save_char(ch);
  Crash_crashsave(ch);
}

static void run_rent(const uint8_t *data, size_t size)
{
  if ((masked[KNOWN_FREAD_STRING_NUL] && body_has_nul(data, size)) ||
      (masked[KNOWN_RESTRUNG_LEAK] && record_has_repeated_string_tag(data, size)))
    return;
  write_file(FUZZ_RENTFILE, data, size);
  viewer = new_immortal();
  /* An immortal lists the file, boot cleans it, then the player logs in. */
  Crash_listrent(viewer, (char *) FUZZ_NAME);
  (void) Crash_clean_file((char *) FUZZ_NAME);
  loaded_ch = new_fixture_player();
  rent_login(loaded_ch);
  (void) Crash_delete_crashfile(loaded_ch);
}

static void run_login(const uint8_t *data, size_t size)
{
  const uint8_t *part[3];
  size_t len[3];
  int n = split_parts(data, size, part, len, 3), id;

  if ((masked[KNOWN_FREAD_STRING_NUL] &&
       (body_has_nul(part[0], len[0]) || (n > 1 && body_has_nul(part[1], len[1])))) ||
      (masked[KNOWN_RESTRUNG_LEAK] && n > 1 &&
       record_has_repeated_string_tag(part[1], len[1])))
    return;
  write_file(FUZZ_PFILE, part[0], len[0]);
  if (n > 1)
    write_file(FUZZ_RENTFILE, part[1], len[1]);
  if (n > 2)
    write_file(FUZZ_VARSFILE, part[2], len[2]);
  loaded_ch = new_player_shell();
  if ((id = load_char(FUZZ_NAME, loaded_ch)) < 0)
    return;
  GET_PFILEPOS(loaded_ch) = id;
  loaded_ch->player.time.logon = time(0);
  reset_char(loaded_ch);
  rent_login(loaded_ch);
}

static void run_vars(const uint8_t *data, size_t size)
{
  write_file(FUZZ_VARSFILE, data, size);
  loaded_ch = new_fixture_player();
  read_saved_vars(loaded_ch);
}

static struct char_data *house_owner_in_house(void)
{
  struct char_data *ch = new_fixture_player();

  char_to_room(ch, RNUM_HOUSE);
  return ch;
}

static void run_house(const uint8_t *data, size_t size)
{
  char show[] = "18500";

  if ((masked[KNOWN_FREAD_STRING_NUL] && body_has_nul(data, size)) ||
      (masked[KNOWN_RESTRUNG_LEAK] && record_has_repeated_string_tag(data, size)))
    return;
  write_house_control();
  write_file(HOUSE_FILE, data, size);
  viewer = new_immortal();
  House_boot();                         /* House_load() */
  hcontrol_list_houses(viewer, show);   /* House_listrent() */
  House_crashsave(HOUSE_VNUM);          /* House_save() of what was loaded */
}

/* TRUE if any whole hcontrol record in data has a built_on or last_payment
 * that localtime() cannot convert. */
static int hcontrol_has_bad_time(const uint8_t *data, size_t size)
{
  struct house_control_rec rec;
  size_t off;

  for (off = 0; off + sizeof(rec) <= size; off += sizeof(rec)) {
    memcpy(&rec, data + off, sizeof(rec));
    if (localtime(&rec.built_on) == NULL || localtime(&rec.last_payment) == NULL)
      return TRUE;
  }
  return FALSE;
}

static void run_hcontrol(const uint8_t *data, size_t size)
{
  static const char house_objs[] = "#100\nLoc : 0\n\n#101\nLoc : -1\n\n$~\n";
  char none[] = "", guest[] = "kythra", stranger[] = "alyssa";
  int i;

  write_file(HCONTROL_FILE, data, size);
  write_file(HOUSE_FILE, (const uint8_t *) house_objs, sizeof(house_objs) - 1);
  viewer = new_immortal();
  House_boot();
  /* KNOWN_HCONTROL_TIME: hcontrol show prints built_on/last_pay after a
   * localtime() failure left them unwritten. */
  if (!(masked[KNOWN_HCONTROL_TIME] && hcontrol_has_bad_time(data, size)))
    hcontrol_list_houses(viewer, none);   /* every loaded record, with guests */
  loaded_ch = house_owner_in_house();
  for (i = 0; i < N_ROOMS; i++)
    (void) House_can_enter(loaded_ch, world[i].number);
  do_house(loaded_ch, none, 0, 0);      /* list guests */
  do_house(loaded_ch, guest, 0, 0);     /* remove (or add) a guest */
  do_house(loaded_ch, stranger, 0, 0);  /* add a guest */
}

static void run_binhouse(const uint8_t *data, size_t size)
{
  char convert[] = "asciiconvert", show[] = "18500";

  write_house_control();
  viewer = new_immortal();
  House_boot();                         /* no object file yet */
  write_file(HOUSE_FILE, data, size);
  do_hcontrol(viewer, convert, 0, 0);
  /* The admin installs the converted file; it must load. */
  if (file_exists(HOUSE_ASCII_FILE) && rename(HOUSE_ASCII_FILE, HOUSE_FILE) == 0)
    hcontrol_list_houses(viewer, show);
}

/* ------------------------------------------------------------------ */
/* libFuzzer entry points                                              */
/* ------------------------------------------------------------------ */
static void report_counts(void)
{
  int i;

  if (n_inputs == 0)
    return;
  fprintf(stderr, "fuzz_player: %lu inputs (", n_inputs);
  for (i = 0; i < MODE_COUNT; i++)
    fprintf(stderr, "%s%s %lu", i ? ", " : "", mode_names[i], n_mode[i]);
  fprintf(stderr, "); %lu diagnostics; %lu exit() and %lu TB_FATAL refusals "
          "intercepted; soft-check failures: world-leak %lu, index-count %lu, "
          "path-escape %lu, core_dump %lu\n", n_diagnostics, n_exit, n_fatal,
          n_world_leak, n_index_count, n_traversal, n_core_dump);
}

static void at_exit_cleanup(void)
{
  report_counts();
  remove_scratch();
}

/* A relative artifact path would be resolved inside the scratch directory
 * when a crash happens there; make it absolute now. */
static char *absolute_flag(const char *arg, const char *flag, const char *cwd)
{
  size_t flen = strlen(flag);
  const char *value;
  char *out;
  size_t need;

  if (strncmp(arg, flag, flen) != 0)
    return NULL;
  value = arg + flen;
  if (*value == '/')
    return NULL;
  need = flen + strlen(cwd) + 1 + strlen(value) + 1;
  if ((out = malloc(need)) == NULL)
    abort();
  snprintf(out, need, "%s%s/%s", flag, cwd, value);
  return out;
}

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
  const char *tmp = getenv("TMPDIR"), *soft = getenv("FUZZ_PLAYER_SOFT_CHECKS");
  const char *verb = getenv("FUZZ_PLAYER_VERBOSE");
  char cwd[PATH_MAX], path[PATH_MAX];
  char **new_argv;
  int i, has_prefix = FALSE;
  size_t a, b;

  soft_checks = soft && *soft && *soft != '0';
  verbose = verb && *verb && *verb != '0';
  /* localtime() in the logger re-reads /etc/localtime without TZ. */
  setenv("TZ", "UTC", 1);
  tzset();

  if (getcwd(cwd, sizeof(cwd)) == NULL)
    abort();
  if ((new_argv = calloc((size_t) *argc + 2, sizeof(char *))) == NULL)
    abort();
  for (i = 0; i < *argc; i++) {
    char *fixed = NULL;

    if (i > 0 && !(fixed = absolute_flag((*argv)[i], "-artifact_prefix=", cwd)))
      fixed = absolute_flag((*argv)[i], "-exact_artifact_path=", cwd);
    if (i > 0 && !strncmp((*argv)[i], "-artifact_prefix=", 17))
      has_prefix = TRUE;
    new_argv[i] = fixed ? fixed : (*argv)[i];
  }
  if (!has_prefix) {
    size_t need = strlen("-artifact_prefix=") + strlen(cwd) + 2;

    if ((new_argv[*argc] = malloc(need)) == NULL)
      abort();
    snprintf(new_argv[*argc], need, "-artifact_prefix=%s/", cwd);
    ++*argc;
  }
  *argv = fixed_argv = new_argv;
  parse_masks(getenv("FUZZ_PLAYER_MASK"));

  snprintf(scratch_dir, sizeof(scratch_dir), "%s/tb_fuzz_player_XXXXXX",
           tmp && *tmp ? tmp : "/tmp");
  if (mkdtemp(scratch_dir) == NULL) {
    fprintf(stderr, "fuzz_player: mkdtemp %s: %s\n", scratch_dir, strerror(errno));
    abort();
  }
  for (a = 0; a < sizeof(lib_dirs) / sizeof(lib_dirs[0]); a++) {
    snprintf(path, sizeof(path), "%s/%s", scratch_dir, lib_dirs[a]);
    make_dir(path);
    if (a < 4)    /* the per-letter player directories */
      for (b = 0; b < sizeof(letter_dirs) / sizeof(letter_dirs[0]); b++) {
        snprintf(path, sizeof(path), "%s/%s/%s", scratch_dir, lib_dirs[a], letter_dirs[b]);
        make_dir(path);
      }
  }
  if ((orig_cwd_fd = open(".", O_RDONLY | O_DIRECTORY)) < 0 ||
      (scratch_fd = open(scratch_dir, O_RDONLY | O_DIRECTORY)) < 0)
    abort();
  fuzz_log_open();
  atexit(at_exit_cleanup);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  int m;

  if (size == 0)
    return 0;
  for (m = 0; m < MODE_COUNT && mode_letters[m] != (char) data[0]; m++)
    ;
  if (m == MODE_COUNT)
    m = data[0] % MODE_COUNT;
  current_mode = (enum fuzz_mode) m;
  n_inputs++;
  n_mode[m]++;
  data++;
  size--;

  if (fchdir(scratch_fd) != 0)
    abort();
  begin_input();
  loaded_ch = viewer = NULL;
  fatal_is_rejection = FALSE;

  if (sigsetjmp(loader_env, 0) == 0) {
    in_loader = TRUE;
    switch (current_mode) {
    case MODE_PFILE:    run_pfile(data, size); break;
    case MODE_INDEX:    run_index(data, size); break;
    case MODE_RENT:     run_rent(data, size); break;
    case MODE_LOGIN:    run_login(data, size); break;
    case MODE_VARS:     run_vars(data, size); break;
    case MODE_HOUSE:    run_house(data, size); break;
    case MODE_HCONTROL: run_hcontrol(data, size); break;
    case MODE_BINHOUSE: run_binhouse(data, size); break;
    case MODE_COUNT:    break;
    }
  }
  in_loader = FALSE;
  fatal_is_rejection = FALSE;

  if (loaded_ch != NULL) {
    if (character_list == loaded_ch)
      character_list = loaded_ch->next;
    release_char(loaded_ch);
  }
  release_char(viewer);
  loaded_ch = viewer = NULL;
  end_input();
  if (fchdir(orig_cwd_fd) != 0)
    abort();
  return 0;
}
