/* libFuzzer target for the world-file loaders (db.c, dg_db_scripts.c).
 *
 * The world files under lib/world/{wld,mob,obj,zon,trg} are written by OLC
 * and edited by hand by builders and implementors; db.c reads them once at
 * boot. A malformed file must be rejected with a structured diagnostic (a
 * TB_* or SYSERR line, then exit(1) at boot) and must never crash or corrupt
 * memory. Every input here is one such file.
 *
 * Input: the first byte picks the loader, the rest is the file.
 *   W  .wld  index_boot()'s counting pass, discrete_load() -> parse_room(),
 *            setup_dir(), dg_read_trigger(); then renum_world()
 *   w  the same with diagonal exits enabled (CONFIG_DIAGONAL_DIRS)
 *   M  .mob  discrete_load() -> parse_mobile(), parse_simple_mob(),
 *            parse_enhanced_mob(), parse_espec(), dg_read_trigger()
 *   O  .obj  discrete_load() -> parse_object(), check_object(),
 *            dg_obj_trigger()
 *   Z  .zon  load_zones(), then renum_zone_table() against a tiny world
 *   T  .trg  discrete_load() -> parse_trigger()
 *   S  '~'-terminated strings through fread_string() until EOF
 *   C  the same through fread_clean_string()
 *   L  lines through get_line(), then help lines through get_one_line()
 *   F  file_to_string() into a buffer whose size the next byte picks, then
 *      file_to_string_alloc() while a pager reads the old text
 * Any other first byte picks a loader by value, so mutating it still reaches
 * every loader. Seeds are real world files with the letter prepended.
 *
 * How boot-only code runs millions of times in one process:
 *  - db.c is #included, as tests/test_db_parse.c does, so its file-scope
 *    parsers and statics are reachable, and exit() is redirected to a
 *    longjmp() back here: a fatal-at-boot rejection is a clean return, not a
 *    finding. dg_db_scripts.c is #included the same way.
 *  - parse_room(), parse_mobile(), parse_object() and load_zones() keep their
 *    record cursor in a function-local static. fuzz_world_db.cmake builds a
 *    copy of db.c (line-for-line identical, #line-mapped to src/db.c) in
 *    which FUZZ_WORLD_CURSOR() lets the harness zero them per input.
 *  - Each input gets prototype tables of exactly the size index_boot() would
 *    allocate for it (count_hash_records() over the same bytes), so a record
 *    stored past the counted capacity is an ASan heap-buffer-overflow.
 *  - After a fatal exit() the parser's frames are gone, and with them the only
 *    pointers to memory it had allocated but not yet stored (an extra
 *    description, a half-read trigger). The real server exits there, so that
 *    memory is not a leak. Allocations made by db.c / dg_db_scripts.c code
 *    are therefore tagged, and after an exit() whatever the fixtures do not
 *    own is freed. After a normal return nothing is swept: anything still
 *    allocated is a real leak, and LeakSanitizer reports it.
 *
 * Run: bin/fuzz_world -dict=tests/fuzz/world.dict -max_len=16384 \
 *        -timeout=10 -rss_limit_mb=2048 <work-corpus> tests/fuzz/corpus/world
 * FUZZ_WORLD_LOG=1 sends the loaders' diagnostics to stderr (default: a sink;
 * they are still formatted, so a bad format or argument is still caught).
 * FUZZ_WORLD_MASK steps past known open crashes during a campaign, and
 * FUZZ_WORLD_NO_SWEEP self-tests the exit() sweep; both are described below.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE	/* fopencookie(), memfd_create() */
#endif
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "genzon.h"	/* real_zone_by_thing(), wrapped below */
#include "act.h"		/* find_exdesc(), wrapped below */
#include "genolc.h"	/* save_list */
#include <setjmp.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sanitizer/allocator_interface.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Inputs larger than this are skipped: they add nothing a smaller input
 * cannot reach, and they bound the allocation-tag table below. */
#define FUZZ_MAX_INPUT   (64 * 1024)
/* index_boot() sizes the tables by the '#' lines it counts; skip inputs that
 * would make a fixture table larger than this many records. */
#define FUZZ_MAX_RECORDS 512

/* ---------------------------------------------------------------------
 * exit() redirect: a fatal boot-time rejection returns to fuzz_run().
 * ------------------------------------------------------------------- */

static jmp_buf fuzz_exit_jmp;
static volatile int fuzz_exit_armed;

static void fuzz_exit(int code) __attribute__((noreturn));
static void fuzz_exit(int code)
{
  (void)code;
  if (!fuzz_exit_armed)		/* exit() outside a parse: a harness bug */
    abort();
  fuzz_exit_armed = 0;
  longjmp(fuzz_exit_jmp, 1);
}

/* Run body(arg); return 1 if it ended in exit(). */
static int fuzz_run(void (*body)(void *), void *arg)
{
  if (setjmp(fuzz_exit_jmp) != 0)
    return 1;
  fuzz_exit_armed = 1;
  body(arg);
  fuzz_exit_armed = 0;
  return 0;
}

/* ---------------------------------------------------------------------
 * Allocation tags: what db.c / dg_db_scripts.c allocated during this input
 * and has not freed. An open-addressing set keyed by pointer, emptied per
 * input by bumping a generation; an ASan free hook untags on every free().
 *
 * The tables hold each pointer bitwise-inverted (FUZZ_TAG_HIDE). LeakSanitizer
 * scans globals conservatively: a plain copy of every allocation here would
 * make all of them reachable, and no leak in the loaders could ever be seen.
 * ------------------------------------------------------------------- */

#define FUZZ_TAG_SLOTS (1u << 16)
#define FUZZ_TAG_MAX   (FUZZ_TAG_SLOTS / 2)	/* keep the load at most 1/2 */
#define FUZZ_TAG_HIDE(p) (~(uintptr_t)(p))
#define FUZZ_TAG_TOMB  ((uintptr_t)0)	/* FUZZ_TAG_HIDE of no real pointer */

static uintptr_t fuzz_tag_slot[FUZZ_TAG_SLOTS];	/* hidden pointer or TOMB */
static unsigned fuzz_tag_gen[FUZZ_TAG_SLOTS];
static unsigned fuzz_tag_cur = 1;
static uintptr_t fuzz_tag_list[FUZZ_TAG_MAX];	/* hidden, insertion order */
static size_t fuzz_tag_count;
static int fuzz_tag_overflow;
static __thread int fuzz_tagging;	/* set only while an input runs */

static size_t fuzz_tag_hash(uintptr_t hidden)
{
  uint64_t v = (uint64_t)FUZZ_TAG_HIDE(hidden) >> 4;
  return (size_t)((v * 0x9E3779B97F4A7C15ULL) >> 48) & (FUZZ_TAG_SLOTS - 1);
}

/* Slot holding the hidden pointer in this generation, or FUZZ_TAG_SLOTS. */
static size_t fuzz_tag_find(uintptr_t hidden)
{
  size_t h = fuzz_tag_hash(hidden), n;

  for (n = 0; n < FUZZ_TAG_SLOTS; n++, h = (h + 1) & (FUZZ_TAG_SLOTS - 1)) {
    if (fuzz_tag_gen[h] != fuzz_tag_cur)
      return FUZZ_TAG_SLOTS;	/* empty: end of the probe chain */
    if (fuzz_tag_slot[h] == hidden)
      return h;
  }
  return FUZZ_TAG_SLOTS;
}

static void fuzz_tag(const void *p)
{
  uintptr_t hidden = FUZZ_TAG_HIDE(p);
  size_t h, n;

  if (!fuzz_tagging || p == NULL || fuzz_tag_find(hidden) != FUZZ_TAG_SLOTS)
    return;
  if (fuzz_tag_count >= FUZZ_TAG_MAX) {
    fuzz_tag_overflow = 1;
    return;
  }
  h = fuzz_tag_hash(hidden);
  for (n = 0; n < FUZZ_TAG_SLOTS; n++, h = (h + 1) & (FUZZ_TAG_SLOTS - 1))
    if (fuzz_tag_gen[h] != fuzz_tag_cur || fuzz_tag_slot[h] == FUZZ_TAG_TOMB) {
      fuzz_tag_gen[h] = fuzz_tag_cur;
      fuzz_tag_slot[h] = hidden;
      fuzz_tag_list[fuzz_tag_count++] = hidden;
      return;
    }
  fuzz_tag_overflow = 1;
}

static void fuzz_malloc_hook(const volatile void *p, size_t size)
{
  (void)p;
  (void)size;
}

static void fuzz_free_hook(const volatile void *p)
{
  size_t h;

  if (!fuzz_tagging || p == NULL)
    return;
  h = fuzz_tag_find(FUZZ_TAG_HIDE(p));
  if (h != FUZZ_TAG_SLOTS)
    fuzz_tag_slot[h] = FUZZ_TAG_TOMB;
}

static void fuzz_tags_begin(void)
{
  if (++fuzz_tag_cur == 0) {	/* generation wrapped: really clear */
    memset(fuzz_tag_gen, 0, sizeof(fuzz_tag_gen));
    fuzz_tag_cur = 1;
  }
  fuzz_tag_count = 0;
  fuzz_tag_overflow = 0;
  fuzz_tagging = 1;
}

/* Stop tagging; after an exit(), free what the parser had orphaned. Set
 * FUZZ_WORLD_NO_SWEEP=1 to skip that (a self-test: the orphans of an
 * exit()-path input must then be reported as leaks). */
static int fuzz_no_sweep;

static void fuzz_tags_end(int exited)
{
  size_t k, h;

  fuzz_tagging = 0;
  if (!exited || fuzz_no_sweep)
    return;
  if (fuzz_tag_overflow) {	/* cannot happen below FUZZ_MAX_INPUT */
    fprintf(stderr, "fuzz_world: allocation tag table overflowed\n");
    abort();
  }
  for (k = 0; k < fuzz_tag_count; k++) {
    h = fuzz_tag_find(fuzz_tag_list[k]);
    if (h == FUZZ_TAG_SLOTS)
      continue;			/* freed already */
    fuzz_tag_slot[h] = FUZZ_TAG_TOMB;
    free((void *)FUZZ_TAG_HIDE(fuzz_tag_list[k]));
  }
}

/* The allocators db.c and dg_db_scripts.c reach (CREATE() expands to
 * tb_calloc_array_at()), tagging what they return. Defined before the macros
 * below so these bodies call the real functions. */
static void *fuzz_calloc_array_at(size_t count, size_t size, const char *file,
                                  int line, const char *func)
{
  void *p = tb_calloc_array_at(count, size, file, line, func);
  fuzz_tag(p);
  return p;
}

static char *fuzz_strdup(const char *s)
{
  char *p = strdup(s);
  fuzz_tag(p);
  return p;
}

/* ---------------------------------------------------------------------
 * The code under test.
 * ------------------------------------------------------------------- */

/* Set by the harness before each input; consumed by the copy of db.c. */
static int fuzz_reset_room, fuzz_reset_mob, fuzz_reset_obj, fuzz_reset_zone;
#define FUZZ_WORLD_CURSOR(name, ...) \
  if (fuzz_reset_##name) { fuzz_reset_##name = 0; __VA_ARGS__ }

#define tb_calloc_array_at(n, s, f, l, fn) fuzz_calloc_array_at((n), (s), (f), (l), (fn))
#ifdef strdup
#undef strdup
#endif
#define strdup(s) fuzz_strdup(s)
#define exit(code) fuzz_exit(code)

/* Known-crash masks, FUZZ_WORLD_MASK=<name>[,<name>...]. While a bug that
 * already has an input in known-crashes/world/ is open, a fuzzing campaign
 * can turn inputs that would reach it into a clean rejection and so keep
 * going past it. Each mask matches only the bug's exact precondition. The
 * corpus and known-crash tests run with no mask.
 *   trg-empty-script: parse_trigger() passes strtok()'s NULL result for a
 *     script that is empty or only CR/LF to strdup() (dg_db_scripts.c:86).
 *   convert-zone-nowhere-{room,mob,obj}: the 128-bit conversion of a legacy
 *     record indexes zone_table[real_zone_by_thing(vnum)] when the vnum is
 *     in no zone, i.e. zone_table[NOWHERE].
 *   note-exdesc-null-keyword: check_object() hands a note's extra
 *     descriptions to find_exdesc(), which reads *keyword; an empty "~"
 *     keyword was stored as NULL. */
static int fuzz_mask_trg_empty_script;
static int fuzz_mask_convert_room, fuzz_mask_convert_mob, fuzz_mask_convert_obj;
static int fuzz_mask_note_null_keyword;
static int fuzz_trg_string_no;	/* fread_string() calls in this trigger */

static zone_rnum fuzz_real_zone_by_thing(room_vnum vnum, const char *caller)
{
  zone_rnum z = real_zone_by_thing(vnum);

  if (z == NOWHERE &&
      ((fuzz_mask_convert_room && !strcmp(caller, "parse_room")) ||
       (fuzz_mask_convert_mob && !strcmp(caller, "parse_mobile")) ||
       (fuzz_mask_convert_obj && !strcmp(caller, "parse_object"))))
    fuzz_exit(1);
  return z;
}
#define real_zone_by_thing(v) fuzz_real_zone_by_thing((v), __func__)

static char *fuzz_find_exdesc(char *word, struct extra_descr_data *list)
{
  struct extra_descr_data *i;

  if (fuzz_mask_note_null_keyword)
    for (i = list; i; i = i->next)
      if (i->keyword == NULL)
        fuzz_exit(1);
  return find_exdesc(word, list);
}
#define find_exdesc(word, list) fuzz_find_exdesc((word), (list))

/* NOLINTBEGIN(bugprone-suspicious-include): reach the loaders' statics. */
#include "fuzz_world_db.c"
/* NOLINTEND(bugprone-suspicious-include) */

static char *fuzz_trg_fread_string(FILE *fl, const char *error)
{
  char *s = fread_string(fl, error);

  if (++fuzz_trg_string_no == 3 && fuzz_mask_trg_empty_script &&
      (s == NULL || s[strspn(s, "\r\n")] == '\0'))
    fuzz_exit(1);
  return s;
}

/* parse_trigger() is renamed so a wrapper can count its strings. */
#define parse_trigger fuzz_real_parse_trigger
#define fread_string(fl, error) fuzz_trg_fread_string((fl), (error))
void fuzz_real_parse_trigger(FILE *trig_f, int nr);
/* NOLINTBEGIN(bugprone-suspicious-include) */
#include "dg_db_scripts.c"
/* NOLINTEND(bugprone-suspicious-include) */
#undef fread_string
#undef parse_trigger
#undef real_zone_by_thing
#undef find_exdesc
#undef exit
#undef strdup
#undef tb_calloc_array_at

void parse_trigger(FILE *trig_f, int nr)
{
  fuzz_trg_string_no = 0;
  fuzz_real_parse_trigger(trig_f, nr);
}

/* ---------------------------------------------------------------------
 * Symbols the loaders reach in files this target does not link.
 * ------------------------------------------------------------------- */

FILE *logfile = NULL;			/* comm.c; NULL logs to stderr */
struct descriptor_data *descriptor_list = NULL;	/* comm.c */
int scheck = 0;				/* comm.c: not syntax-check mode */
/* comm.c (it defines main()): mudlog() sends to playing descriptors, and the
 * only descriptor here is the F mode's pager, which is never CON_PLAYING. */
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  (void)ch;
  (void)messg;
  abort();
}
const char *unused_spellname = "!UNUSED!";	/* spell_parser.c */

/* spell_parser.c: spell_info[] is filled by mag_assign_spells() at boot;
 * here spells 1..54 are named and the rest are unused, as in the stock set. */
const char *skill_name(int num)
{
  if (num > 0 && num <= TOP_SPELL_DEFINE)
    return num <= 54 ? "fixture spell" : unused_spellname;
  else if (num == -1)
    return "UNUSED";
  else
    return "UNDEFINED";
}

/* dg_scripts.c, verbatim. */
trig_rnum real_trigger(trig_vnum vnum)
{
  trig_rnum bot, top, mid;

  bot = 0;
  top = top_of_trigt - 1;

  if (!top_of_trigt || trig_index[bot]->vnum > vnum || trig_index[top]->vnum < vnum)
    return (NOTHING);

  while (bot <= top) {
    mid = (bot + top) / 2;

    if (trig_index[mid]->vnum == vnum)
      return (mid);
    if (trig_index[mid]->vnum > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return (NOTHING);
}

/* dg_scripts.c, verbatim. */
void add_trigger(struct script_data *sc, trig_data *t, int loc)
{
  trig_data *i;
  int n;

  for (n = loc, i = TRIGGERS(sc); i && i->next && (n != 0); n--, i = i->next);

  if (!loc) {
    t->next = TRIGGERS(sc);
    TRIGGERS(sc) = t;
  } else if (!i)
    TRIGGERS(sc) = t;
  else {
    t->next = i->next;
    i->next = t;
  }

  SCRIPT_TYPES(sc) |= GET_TRIG_TYPE(t);

  t->next_in_world = trigger_list;
  trigger_list = t;
}

/* quest.c: discrete_load()'s DB_BOOT_QST case; no mode here selects it. */
void parse_quest(FILE *quest_f, int nr)
{
  (void)quest_f;
  (void)nr;
  abort();
}

/* ---------------------------------------------------------------------
 * Fixtures: the zone table rooms/mobs/objects are checked against, the
 * trigger prototypes 'T' lines attach, and a tiny world for zone resets.
 * ------------------------------------------------------------------- */

/* One zone per 100 vnums, as in the stock world, with vnums 40000-54999 and
 * 65500+ in no zone (the stock world has gaps too). */
#define FUZZ_ZONE_BLOCKS 655
static struct zone_data fuzz_zones[FUZZ_ZONE_BLOCKS];
static zone_rnum fuzz_zone_top;

static void fuzz_zones_init(void)
{
  int b, n = 0;

  for (b = 0; b < FUZZ_ZONE_BLOCKS; b++) {
    if (b >= 400 && b < 550)
      continue;
    fuzz_zones[n].number = (zone_vnum)b;
    fuzz_zones[n].bot = (room_vnum)(b * 100);
    fuzz_zones[n].top = (room_vnum)(b * 100 + 99);
    n++;
  }
  fuzz_zone_top = (zone_rnum)(n - 1);
}

static void fuzz_use_zones(void)
{
  zone_table = fuzz_zones;
  top_of_zone_table = fuzz_zone_top;
}

#define FUZZ_TRIGS 4
static const trig_vnum fuzz_trig_vnums[FUZZ_TRIGS] = { 1, 3000, 3001, 65534 };
static struct cmdlist_element fuzz_trig_cmd;
static struct trig_data fuzz_trig_proto[FUZZ_TRIGS];
static struct index_data fuzz_trig_slot[FUZZ_TRIGS];
static struct index_data *fuzz_trig_table[FUZZ_TRIGS];

static void fuzz_triggers_init(void)
{
  int k;

  fuzz_trig_cmd.cmd = (char *)"say fixture";
  for (k = 0; k < FUZZ_TRIGS; k++) {
    fuzz_trig_proto[k].nr = (IDXTYPE)k;
    fuzz_trig_proto[k].attach_type = WLD_TRIGGER;
    fuzz_trig_proto[k].name = (char *)"fixture trigger";
    fuzz_trig_proto[k].trigger_type = 0;
    fuzz_trig_proto[k].cmdlist = &fuzz_trig_cmd;
    fuzz_trig_proto[k].arglist = (char *)"100";
    fuzz_trig_slot[k].vnum = fuzz_trig_vnums[k];
    fuzz_trig_slot[k].proto = &fuzz_trig_proto[k];
    fuzz_trig_table[k] = &fuzz_trig_slot[k];
  }
}

static void fuzz_use_triggers(void)
{
  trig_index = fuzz_trig_table;
  top_of_trigt = FUZZ_TRIGS;
}

/* renum_zone_table() resolves zone commands against these (sorted) vnums. */
#define FUZZ_ROOMS 5
#define FUZZ_PROTOS 3
static const room_vnum fuzz_room_vnums[FUZZ_ROOMS] = { 0, 3001, 3002, 3003, 65534 };
static const IDXTYPE fuzz_mob_vnums[FUZZ_PROTOS] = { 3000, 3001, 3005 };
static const IDXTYPE fuzz_obj_vnums[FUZZ_PROTOS] = { 3000, 3001, 3022 };
static struct room_data fuzz_rooms[FUZZ_ROOMS];
static struct index_data fuzz_mob_slot[FUZZ_PROTOS], fuzz_obj_slot[FUZZ_PROTOS];

static void fuzz_world_init(void)
{
  int k;

  for (k = 0; k < FUZZ_ROOMS; k++)
    fuzz_rooms[k].number = fuzz_room_vnums[k];
  for (k = 0; k < FUZZ_PROTOS; k++) {
    fuzz_mob_slot[k].vnum = fuzz_mob_vnums[k];
    fuzz_obj_slot[k].vnum = fuzz_obj_vnums[k];
  }
}

/* ---------------------------------------------------------------------
 * Freeing what the loaders stored, so a normal return leaves nothing.
 * ------------------------------------------------------------------- */

static void fuzz_free_proto_list(struct trig_proto_list *p)
{
  struct trig_proto_list *next;

  for (; p; p = next) {
    next = p->next;
    free(p);
  }
}

/* A room's script holds read_trigger() copies; their cmdlist belongs to the
 * fixture prototype. */
static void fuzz_free_script(struct script_data *sc)
{
  trig_data *t, *next;

  if (sc == NULL)
    return;
  for (t = sc->trig_list; t; t = next) {
    next = t->next;
    free(t->name);
    free(t->arglist);
    free(t);
  }
  free(sc);
}

static void fuzz_free_room(struct room_data *r)
{
  int d;

  free(r->name);
  free(r->description);
  for (d = 0; d < NUM_OF_DIRS; d++)
    if (r->dir_option[d]) {
      free(r->dir_option[d]->general_description);
      free(r->dir_option[d]->keyword);
      free(r->dir_option[d]);
    }
  free_extra_descriptions(r->ex_description);
  fuzz_free_proto_list(r->proto_script);
  fuzz_free_script(r->script);
}

static void fuzz_free_mob(struct char_data *m)
{
  free(m->player.name);
  free(m->player.short_descr);
  free(m->player.long_descr);
  free(m->player.description);
  free(m->player.title);
  fuzz_free_proto_list(m->proto_script);
}

static void fuzz_free_obj(struct obj_data *o)
{
  free(o->name);
  free(o->short_description);
  free(o->description);
  free(o->action_description);
  free_extra_descriptions(o->ex_description);
  fuzz_free_proto_list(o->proto_script);
}

static void fuzz_free_trigger(struct index_data *t_index)
{
  trig_data *trig = t_index->proto;
  struct cmdlist_element *c, *next;

  free(trig->name);
  free(trig->arglist);
  for (c = trig->cmdlist; c; c = next) {
    next = c->next;
    free(c->cmd);
    free(c);
  }
  free(trig);
  free(t_index);
}

/* load_zones() sizes cmd[] by its own count; free every slot's strings. */
static void fuzz_free_zone(struct zone_data *z)
{
  size_t n, c;

  free(z->name);
  free(z->builders);
  if (z->cmd) {
    n = __sanitizer_get_allocated_size(z->cmd) / sizeof(struct reset_com);
    for (c = 0; c < n; c++) {
      free(z->cmd[c].sarg1);
      free(z->cmd[c].sarg2);
    }
    free(z->cmd);
  }
}

/* ---------------------------------------------------------------------
 * The loaders.
 * ------------------------------------------------------------------- */

struct fuzz_load {
  FILE *fp;
  int boot_mode;		/* DB_BOOT_WLD, ... */
  char name[32];		/* the file name diagnostics see */
  char *buf;			/* line buffer for the L mode */
  int clean;			/* S mode: fread_clean_string() */
};

static void fuzz_body_discrete(void *arg)
{
  struct fuzz_load *ld = arg;

  discrete_load(ld->fp, ld->boot_mode, ld->name);
  if (ld->boot_mode == DB_BOOT_WLD)
    renum_world();		/* boot_world(): exits' vnums -> rnums */
}

/* index_boot() for one file: count the records, allocate exactly that many
 * slots, then discrete_load(). */
static int fuzz_discrete(FILE *fp, int boot_mode, const char *name)
{
  struct fuzz_load ld;
  int rec, k, exited;

  rec = count_hash_records(fp);
  if (rec == 0 || rec > FUZZ_MAX_RECORDS)
    return 0;	/* index_boot(): 0 records is fatal; huge ones are skipped */
  if (fseek(fp, 0L, SEEK_SET) != 0)
    abort();

  memset(&ld, 0, sizeof(ld));
  ld.fp = fp;
  ld.boot_mode = boot_mode;
  strlcpy(ld.name, name, sizeof(ld.name));

  fuzz_use_zones();
  fuzz_use_triggers();
  switch (boot_mode) {
  case DB_BOOT_WLD:
    world = calloc((size_t)rec, sizeof(*world));
    top_of_world = 0;
    break;
  case DB_BOOT_MOB:
    mob_proto = calloc((size_t)rec, sizeof(*mob_proto));
    mob_index = calloc((size_t)rec, sizeof(*mob_index));
    top_of_mobt = 0;
    break;
  case DB_BOOT_OBJ:
    obj_proto = calloc((size_t)rec, sizeof(*obj_proto));
    obj_index = calloc((size_t)rec, sizeof(*obj_index));
    top_of_objt = 0;
    break;
  case DB_BOOT_TRG:
    trig_index = calloc((size_t)rec, sizeof(*trig_index));
    top_of_trigt = 0;
    break;
  default:
    abort();
  }
  if ((boot_mode == DB_BOOT_WLD && !world) ||
      (boot_mode == DB_BOOT_MOB && (!mob_proto || !mob_index)) ||
      (boot_mode == DB_BOOT_OBJ && (!obj_proto || !obj_index)) ||
      (boot_mode == DB_BOOT_TRG && !trig_index))
    abort();

  boot_capacity = rec;
  boot_records = 0;
  boot_last_vnum = -1;
  fuzz_reset_room = fuzz_reset_mob = fuzz_reset_obj = 1;

  exited = fuzz_run(fuzz_body_discrete, &ld);

  switch (boot_mode) {
  case DB_BOOT_WLD:
    for (k = 0; k < rec; k++)
      fuzz_free_room(&world[k]);
    free(world);
    world = NULL;
    top_of_world = 0;
    break;
  case DB_BOOT_MOB:
    for (k = 0; k < rec; k++)
      fuzz_free_mob(&mob_proto[k]);
    free(mob_proto);
    free(mob_index);
    mob_proto = NULL;
    mob_index = NULL;
    top_of_mobt = 0;
    break;
  case DB_BOOT_OBJ:
    for (k = 0; k < rec; k++)
      fuzz_free_obj(&obj_proto[k]);
    free(obj_proto);
    free(obj_index);
    obj_proto = NULL;
    obj_index = NULL;
    top_of_objt = 0;
    break;
  case DB_BOOT_TRG:
    for (k = 0; k < top_of_trigt; k++)
      fuzz_free_trigger(trig_index[k]);
    free(trig_index);
    break;
  }
  trigger_list = NULL;		/* the read_trigger() copies were freed above */
  fuzz_use_triggers();
  /* Converting a legacy record queues its zone with add_to_save_list();
   * the server saves and empties that list later. Empty it here, or every
   * new zone would look like a leak to libFuzzer, which stops checking for
   * leaks after 1000 such inputs. */
  while (save_list) {
    struct save_list_data *next = save_list->next;
    free(save_list);
    save_list = next;
  }
  return exited;
}

static void fuzz_body_zone(void *arg)
{
  struct fuzz_load *ld = arg;

  load_zones(ld->fp, ld->name);
  /* boot_db() then resolves the commands' vnums against the loaded world. */
  world = fuzz_rooms;
  top_of_world = FUZZ_ROOMS - 1;
  mob_index = fuzz_mob_slot;
  top_of_mobt = FUZZ_PROTOS - 1;
  obj_index = fuzz_obj_slot;
  top_of_objt = FUZZ_PROTOS - 1;
  fuzz_use_triggers();
  renum_zone_table();
}

/* index_boot(DB_BOOT_ZON): one zone_data per .zon file. */
static int fuzz_zone(FILE *fp)
{
  struct fuzz_load ld;
  int exited;

  memset(&ld, 0, sizeof(ld));
  ld.fp = fp;
  strlcpy(ld.name, "fuzz.zon", sizeof(ld.name));
  zone_table = calloc(1, sizeof(*zone_table));
  if (!zone_table)
    abort();
  top_of_zone_table = 0;
  fuzz_reset_zone = 1;

  exited = fuzz_run(fuzz_body_zone, &ld);

  fuzz_free_zone(&zone_table[0]);
  free(zone_table);
  zone_table = NULL;
  top_of_zone_table = 0;
  world = NULL;
  top_of_world = 0;
  mob_index = NULL;
  obj_index = NULL;
  top_of_mobt = top_of_objt = 0;
  return exited;
}

static void fuzz_body_strings(void *arg)
{
  struct fuzz_load *ld = arg;
  int k;

  for (k = 0; k < 4096 && !feof(ld->fp); k++)
    free(ld->clean ? fread_clean_string(ld->fp, "fuzz string")
                   : fread_string(ld->fp, "fuzz string"));
}

static void fuzz_body_lines(void *arg)
{
  struct fuzz_load *ld = arg;
  int k;

  while (get_line(ld->fp, ld->buf))
    ;
  if (fseek(ld->fp, 0L, SEEK_SET) != 0)
    abort();
  for (k = 0; k < 65536; k++)	/* ends in exit() at EOF */
    get_one_line(ld->fp, ld->buf);
}

/* file_to_string() reads by name; the bytes go into a memfd. */
static int fuzz_memfd = -1;
static char fuzz_memfd_path[64];

static void fuzz_body_text(void *arg)
{
  const uint8_t *bytes = arg;
  size_t bufsize = 16 + (size_t)bytes[0] * 4;	/* 16..1036 bytes */
  char *buf = malloc(bufsize), *global = strdup("Old text.\r\n");
  char *vector[1];
  struct descriptor_data d;

  if (!buf || !global)
    abort();
  if (file_to_string(fuzz_memfd_path, buf, bufsize) == 0 && strlen(buf) >= bufsize)
    abort();
  free(buf);

  /* "reload" while someone pages the old text in place. */
  memset(&d, 0, sizeof(d));
  STATE(&d) = CON_CLOSE;	/* not CON_PLAYING: mudlog() skips it */
  vector[0] = global;
  d.showstr_vector = vector;
  d.showstr_count = 1;
  descriptor_list = &d;
  (void)file_to_string_alloc(fuzz_memfd_path, &global);
  descriptor_list = NULL;
  free(d.showstr_head);
  free(global);
}

static int fuzz_text(const uint8_t *data, size_t size)
{
  if (size < 1)
    return 0;
  if (ftruncate(fuzz_memfd, 0) != 0 ||
      pwrite(fuzz_memfd, data + 1, size - 1, 0) != (ssize_t)(size - 1))
    abort();
  return fuzz_run(fuzz_body_text, (void *)data);
}

/* ---------------------------------------------------------------------
 * Entry points.
 * ------------------------------------------------------------------- */

/* TRUE if name is one of the comma-separated FUZZ_WORLD_MASK entries. */
static int fuzz_mask_on(const char *name)
{
  const char *list = getenv("FUZZ_WORLD_MASK"), *p;
  size_t n = strlen(name);

  for (p = list; p && (p = strstr(p, name)) != NULL; p += n)
    if ((p == list || p[-1] == ',') && (p[n] == '\0' || p[n] == ','))
      return 1;
  return 0;
}

static ssize_t fuzz_log_sink(void *cookie, const char *buf, size_t size)
{
  (void)cookie;
  (void)buf;
  return (ssize_t)size;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
  static const cookie_io_functions_t sink = { NULL, fuzz_log_sink, NULL, NULL };
  FILE *fp;
  char line[READ_SIZE];

  (void)argc;
  (void)argv;
  if (!getenv("FUZZ_WORLD_LOG") && !(logfile = fopencookie(NULL, "w", sink)))
    abort();
  fuzz_no_sweep = getenv("FUZZ_WORLD_NO_SWEEP") != NULL;
  fuzz_mask_trg_empty_script = fuzz_mask_on("trg-empty-script");
  fuzz_mask_convert_room = fuzz_mask_on("convert-zone-nowhere-room");
  fuzz_mask_convert_mob = fuzz_mask_on("convert-zone-nowhere-mob");
  fuzz_mask_convert_obj = fuzz_mask_on("convert-zone-nowhere-obj");
  fuzz_mask_note_null_keyword = fuzz_mask_on("note-exdesc-null-keyword");

  load_default_config();	/* boot_db() -> load_config(): CONFIG_* defaults */
  fuzz_zones_init();
  fuzz_triggers_init();
  fuzz_world_init();
  fuzz_use_triggers();

  fuzz_memfd = memfd_create("fuzz_world", 0);
  if (fuzz_memfd < 0)
    abort();
  snprintf(fuzz_memfd_path, sizeof(fuzz_memfd_path), "/proc/self/fd/%d", fuzz_memfd);

  /* Let libc make its one-time allocations (time zone, stdio buffers) now,
   * not inside an input, where the exit() sweep could free them. */
  basic_mud_log("fuzz_world: logging warm-up");
  TB_ERROR_MSG("fuzz", "warm-up", "fuzz_world", "diagnostic warm-up");
  (void)strerror(ENOENT);
  if (!(fp = fmemopen((void *)"a\nb", 3, "r")))
    abort();
  while (get_line(fp, line))
    ;
  fclose(fp);

  if (!__sanitizer_install_malloc_and_free_hooks(fuzz_malloc_hook, fuzz_free_hook))
    abort();
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  static const char modes[] = "WwMOZTSCLF";
  const char *pick;
  char mode;
  struct fuzz_load ld;
  FILE *fp = NULL;
  int exited = 0;
  char *line = NULL;

  if (size < 1 || size > FUZZ_MAX_INPUT)
    return 0;
  pick = data[0] ? strchr(modes, data[0]) : NULL;
  mode = pick ? *pick : modes[data[0] % (sizeof(modes) - 1)];

  CONFIG_DIAGONAL_DIRS = (mode == 'w');
  fuzz_tags_begin();

  if (mode == 'F') {
    exited = fuzz_text(data + 1, size - 1);
  } else {
    if (!(fp = fmemopen((void *)(data + 1), size - 1, "r")))
      abort();
    switch (mode) {
    case 'W': case 'w':
      exited = fuzz_discrete(fp, DB_BOOT_WLD, "fuzz.wld");
      break;
    case 'M':
      exited = fuzz_discrete(fp, DB_BOOT_MOB, "fuzz.mob");
      break;
    case 'O':
      exited = fuzz_discrete(fp, DB_BOOT_OBJ, "fuzz.obj");
      break;
    case 'T':
      exited = fuzz_discrete(fp, DB_BOOT_TRG, "fuzz.trg");
      break;
    case 'Z':
      exited = fuzz_zone(fp);
      break;
    case 'S': case 'C':
      memset(&ld, 0, sizeof(ld));
      ld.fp = fp;
      ld.clean = (mode == 'C');
      exited = fuzz_run(fuzz_body_strings, &ld);
      break;
    case 'L':
      /* Exactly READ_SIZE bytes, the documented minimum, so ASan sees a
       * write past it. */
      if (!(line = malloc(READ_SIZE)))
        abort();
      memset(&ld, 0, sizeof(ld));
      ld.fp = fp;
      ld.buf = line;
      exited = fuzz_run(fuzz_body_lines, &ld);
      free(line);
      break;
    default:
      abort();
    }
    fclose(fp);
  }

  fuzz_tags_end(exited);
  return 0;
}
