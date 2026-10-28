/* Executable tests of the trigger editor (dg_olc.c) with every server source
 * linked in, as for test_dg_engine (comm.c's main() is renamed).
 *
 * The fixture is one zone (#1, vnums 100-199), one room, an implementor with a
 * descriptor, and a heap-allocated trig_index, because trigedit_save()
 * reallocates the index when a trigger is created.  Each test runs in its own
 * temporary directory holding world/trg/index, so trigedit_save() writes real
 * files that the test can inspect.
 *
 * Tests assert on the diagnostic operation (tests/tb_error_capture.h) as well
 * as on the resulting state, and builder input is passed in heap copies so
 * ASan sees over-reads.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include "oasis.h"
#include "dg_olc.h"
#include "protocol.h"
#include "tb_error_capture.h"

#include <sys/stat.h>
#include <unistd.h>

static room_data test_room;
static struct zone_data test_zone;
static char_data *builder;
static struct descriptor_data *bd;
static char test_dir[64];
static char old_cwd[4096];

/* ------------------------------------------------------------------ */
/* Fixture helpers                                                     */
/* ------------------------------------------------------------------ */

static struct cmdlist_element *make_cmdlist(const char *text)
{
  struct cmdlist_element *c;

  CREATE(c, struct cmdlist_element, 1);
  c->cmd = strdup(text);
  TEST_ASSERT_NOT_NULL(c->cmd);
  return c;
}

static void free_proto(trig_data *t)
{
  struct cmdlist_element *c, *next;

  for (c = t->cmdlist; c; c = next) {
    next = c->next;
    free(c->cmd);
    free(c);
  }
  free(t->name);
  free(t->arglist);
  free(t);
}

/* Build trig_index from vnums (ascending), each with a one-line script. */
static void make_index(const int *vnums, int count)
{
  int i;
  char cmd[32];

  CREATE(trig_index, struct index_data *, count > 0 ? count : 1);
  for (i = 0; i < count; i++) {
    trig_data *p;

    CREATE(trig_index[i], struct index_data, 1);
    trig_index[i]->vnum = vnums[i];
    CREATE(p, trig_data, 1);
    p->nr = i;
    p->name = strdup("proto");
    snprintf(cmd, sizeof(cmd), "say %d", vnums[i]);
    p->cmdlist = make_cmdlist(cmd);
    trig_index[i]->proto = p;
  }
  top_of_trigt = count;
}

/* A live instance of the prototype at rnum, linked into trigger_list. */
static trig_data *make_live(int rnum)
{
  trig_data *t = read_trigger(rnum);

  TEST_ASSERT_NOT_NULL(t);
  t->next_in_world = trigger_list;
  trigger_list = t;
  return t;
}

static void write_file(const char *path, const char *text)
{
  FILE *fp = fopen(path, "w");

  TEST_ASSERT_NOT_NULL(fp);
  fputs(text, fp);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* Read a whole small file into a static buffer ("" if missing). */
static const char *read_file(const char *path)
{
  static char buf[65536];
  size_t n = 0;
  FILE *fp = fopen(path, "r");

  buf[0] = '\0';
  if (!fp)
    return buf;
  n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  fclose(fp);
  return buf;
}

/* Start editing vnum through the real command. */
static void start_edit(int vnum)
{
  char arg[32];

  snprintf(arg, sizeof(arg), "%d", vnum);
  do_oasis_trigedit(builder, arg, 0, 0);
  TEST_ASSERT_NOT_NULL(bd->olc);
  TEST_ASSERT_EQUAL_INT(CON_TRIGEDIT, STATE(bd));
  TEST_ASSERT_EQUAL_INT(vnum, OLC_NUM(bd));
}

/* Replace the editor's script, as the text editor would. */
static void set_script(const char *text)
{
  free(OLC_STORAGE(bd));
  CREATE(OLC_STORAGE(bd), char, MAX_CMD_LENGTH);
  strlcpy(OLC_STORAGE(bd), text, MAX_CMD_LENGTH);
  OLC_VAL(bd) = 1;
}

/* Feed one line of builder input to the editor (heap copy for ASan). */
static void feed(int mode, const char *input)
{
  char *arg = strdup(input);

  TEST_ASSERT_NOT_NULL(arg);
  OLC_MODE(bd) = mode;
  trigedit_parse(bd, arg);
  free(arg);
}

/* Answer "y" to "save your changes?", which saves and leaves the editor. */
static void save_and_quit(void)
{
  feed(TRIGEDIT_CONFIRM_SAVESTRING, "y");
  TEST_ASSERT_NULL(bd->olc);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(bd));
}

static void release_large_outbuf(void)
{
  if (bd->large_outbuf) {
    free(bd->large_outbuf->text);
    free(bd->large_outbuf);
    bd->large_outbuf = NULL;
  }
}

static void clear_output(void)
{
  release_large_outbuf();
  bd->output = bd->small_outbuf;
  bd->bufspace = SMALL_BUFSIZE - 1;
  bd->bufptr = 0;
  *bd->output = '\0';
}

void setUp(void)
{
  logfile = fopen("/dev/null", "w");

  TEST_ASSERT_NOT_NULL(getcwd(old_cwd, sizeof(old_cwd)));
  snprintf(test_dir, sizeof(test_dir), "/tmp/tb_dg_olc_XXXXXX");
  TEST_ASSERT_NOT_NULL(mkdtemp(test_dir));
  TEST_ASSERT_EQUAL_INT(0, chdir(test_dir));
  TEST_ASSERT_EQUAL_INT(0, mkdir("world", 0700));
  TEST_ASSERT_EQUAL_INT(0, mkdir("world/trg", 0700));
  write_file("world/trg/index", "1.trg\n$\n");

  memset(&test_room, 0, sizeof(test_room));
  test_room.number = 100;
  test_room.zone = 0;
  test_room.name = "test room";
  world = &test_room;
  top_of_world = 0;

  memset(&test_zone, 0, sizeof(test_zone));
  test_zone.number = 1;
  test_zone.bot = 100;
  test_zone.top = 199;
  test_zone.name = "test zone";
  zone_table = &test_zone;
  top_of_zone_table = 0;

  trigger_list = NULL;
  character_list = NULL;
  init_lookup_table();
  event_init();

  CREATE(builder, char_data, 1);
  clear_char(builder);
  CREATE(builder->player_specials, struct player_special_data, 1);
  builder->player.name = strdup("Builder");
  GET_LEVEL(builder) = LVL_IMPL;
  GET_IDNUM(builder) = 4242;
  GET_OLC_ZONE(builder) = ALL_PERMISSION;
  CREATE(bd, struct descriptor_data, 1);
  bd->output = bd->small_outbuf;
  bd->bufspace = SMALL_BUFSIZE - 1;
  bd->connected = CON_PLAYING;
  bd->character = builder;
  bd->pProtocol = ProtocolCreate();
  builder->desc = bd;
  descriptor_list = bd;
  builder->next = character_list;
  character_list = builder;
  char_to_room(builder, 0);

  tb_error_capture_reset();
}

void tearDown(void)
{
  int i;
  trig_data *t, *next;

  if (bd->olc)
    cleanup_olc(bd, CLEANUP_ALL);
  for (t = trigger_list; t; t = next) {
    next = t->next_in_world;
    free_trigger(t);
  }
  trigger_list = NULL;
  for (i = 0; i < top_of_trigt; i++) {
    free_proto(trig_index[i]->proto);
    free(trig_index[i]);
  }
  free(trig_index);
  trig_index = NULL;
  top_of_trigt = 0;

  char_from_room(builder);
  /* act() gives everyone in the room a script UID (char_script_id()), which
   * registers the builder in the UID lookup table; extract_char() would
   * remove it in the game, so release it here before freeing the builder. */
  if (builder->script_id)
    remove_from_lookup_table(builder->script_id, builder);
  character_list = NULL;
  descriptor_list = NULL;
  release_large_outbuf();
  free(bd->showstr_head);
  free(bd->showstr_vector);
  free(bd->backstr);
  ProtocolDestroy(bd->pProtocol);
  free(bd);
  free(builder->player_specials);
  free(builder->player.name);
  free(builder);
  builder = NULL;
  bd = NULL;
  event_free_all();
  world = NULL;
  zone_table = NULL;

  remove("world/trg/index");
  remove("world/trg/newindex");
  remove("world/trg/1.trg");
  remove("world/trg/1.new");
  rmdir("world/trg");
  rmdir("world");
  TEST_ASSERT_EQUAL_INT(0, chdir(old_cwd));
  rmdir(test_dir);
  if (logfile)
    fclose(logfile);
  logfile = NULL;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* E2-02: creating trigger 101 between 100 and 102 moves 102 from rnum 1 to
 * rnum 2.  Live instances of 102 must follow; before the fix they kept rnum 1
 * (now 101), and editing 102 afterwards freed the cmdlist they still used. */
static void inserting_a_trigger_renumbers_displaced_live_instances(void)
{
  const int vnums[] = { 100, 102 };
  trig_data *live;

  make_index(vnums, 2);
  live = make_live(1);

  start_edit(101);
  set_script("say 101\r\n");
  save_and_quit();

  TEST_ASSERT_EQUAL_INT(3, top_of_trigt);
  TEST_ASSERT_EQUAL_INT(2, GET_TRIG_RNUM(live));
  TEST_ASSERT_EQUAL_INT(102, trig_index[GET_TRIG_RNUM(live)]->vnum);
  TEST_ASSERT_TRUE(live->cmdlist == trig_index[2]->proto->cmdlist);
  TEST_ASSERT_FALSE(tb_captured_has_op("trigedit-rnum-shift"));

  /* Editing 102 must re-point its live instance at the new script. */
  start_edit(102);
  set_script("say new102\r\n");
  save_and_quit();
  TEST_ASSERT_TRUE(live->cmdlist == trig_index[2]->proto->cmdlist);
  TEST_ASSERT_EQUAL_STRING("say new102", live->cmdlist->cmd);
  TEST_ASSERT_NOT_NULL(strstr(read_file("world/trg/1.trg"), "say new102"));
  TEST_ASSERT_FALSE(tb_captured_has_op("trigedit-rnum-shift"));
}

/* E2-02: the post-insert check reports an instance whose rnum names a
 * prototype it does not share a cmdlist with. */
static void stale_live_rnum_after_insert_is_reported(void)
{
  const int vnums[] = { 100, 102 };
  trig_data *live;
  struct cmdlist_element *foreign = make_cmdlist("say foreign");

  make_index(vnums, 2);
  live = make_live(0);
  live->cmdlist = foreign;     /* corrupt: rnum 0 but not proto 0's script */

  start_edit(101);
  set_script("say 101\r\n");
  save_and_quit();

  TEST_ASSERT_TRUE(tb_captured_has_op("trigedit-rnum-shift"));
  live->cmdlist = trig_index[0]->proto->cmdlist;
  free(foreign->cmd);
  free(foreign);
}

/* Point the text editor at the script, as option 6 of the menu does. */
static void edit_script(const char *text)
{
  set_script(text);
  bd->str = &OLC_STORAGE(bd);
  bd->max_str = MAX_CMD_LENGTH;
}

/* E2-01: each "if 1"/"break" pair pushes a block that "break" never pops, so
 * 300 pairs (about 4 KB) overran the 256-entry block stack. */
static void format_script_refuses_nesting_past_its_stack(void)
{
  const int vnums[] = { 100 };
  char *script, *before;
  size_t n = 0;
  int i;

  make_index(vnums, 1);
  start_edit(150);
  CREATE(script, char, MAX_CMD_LENGTH);
  for (i = 0; i < 300; i++)
    n += (size_t)snprintf(script + n, MAX_CMD_LENGTH - n, "if 1\r\nbreak\r\n");
  edit_script(script);
  before = OLC_STORAGE(bd);
  clear_output();

  TEST_ASSERT_FALSE(format_script(bd));
  TEST_ASSERT_TRUE(tb_captured_has_op("trigedit-format-nesting"));
  TEST_ASSERT_TRUE(OLC_STORAGE(bd) == before);       /* left untouched */
  TEST_ASSERT_EQUAL_STRING(script, OLC_STORAGE(bd));
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "nests deeper"));
  free(script);
}

/* A well-formed nested script still formats, and with the right indents. */
static void format_script_indents_nested_blocks(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  start_edit(150);
  edit_script("if 1\r\nwhile 1\r\nsay x\r\ndone\r\nend\r\n");

  TEST_ASSERT_TRUE(format_script(bd));
  TEST_ASSERT_EQUAL_STRING("if 1\r\n  while 1\r\n    say x\r\n  done\r\nend\r\n",
                           OLC_STORAGE(bd));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* E2-03: a scratch trigger filled from another prototype carries that
 * prototype's rnum.  Saving it must not hand the rnum to this prototype, or
 * every instance read_trigger() makes belongs to the other vnum. */
static void saving_keeps_the_prototype_rnum(void)
{
  const int vnums[] = { 100, 102 };
  trig_data *inst;

  make_index(vnums, 2);
  start_edit(100);
  GET_TRIG_RNUM(OLC_TRIG(bd)) = 1;   /* as left by copying trigger 102 */
  set_script("say copied\r\n");
  save_and_quit();

  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_RNUM(trig_index[0]->proto));
  inst = make_live(0);
  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_RNUM(inst));
  TEST_ASSERT_EQUAL_INT(1, trig_index[0]->number);
  TEST_ASSERT_EQUAL_INT(0, trig_index[1]->number);
  TEST_ASSERT_FALSE(tb_captured_has_op("trigedit-proto-nr"));
}

/* E2-03/E2-04: "W) Copy Trigger" replaces the scratch trigger and buffer
 * (which leaked before) and must still ask to save on quit.  LeakSanitizer
 * and Valgrind see the leak; the state checks cover the rest. */
static void copy_trigger_replaces_scratch_and_keeps_save_prompt(void)
{
  const int vnums[] = { 100, 102 };

  make_index(vnums, 2);
  start_edit(100);
  feed(TRIGEDIT_NAME, "my name");
  feed(TRIGEDIT_ARGUMENT, "my arg");
  feed(TRIGEDIT_COPY, "102");

  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_RNUM(OLC_TRIG(bd)));
  TEST_ASSERT_NOT_NULL(strstr(OLC_STORAGE(bd), "say 102"));
  TEST_ASSERT_TRUE(OLC_VAL(bd) != 0);

  feed(TRIGEDIT_MAIN_MENU, "q");
  TEST_ASSERT_EQUAL_INT(TRIGEDIT_CONFIRM_SAVESTRING, OLC_MODE(bd));
  save_and_quit();

  TEST_ASSERT_EQUAL_INT(0, GET_TRIG_RNUM(trig_index[0]->proto));
  TEST_ASSERT_EQUAL_STRING("say 102", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_FALSE(tb_captured_has_op("trigedit-proto-nr"));
}

/* E2-06: a line longer than the old 256-byte line buffer lost its CRLF and
 * the next command was glued onto it, while "/f" reported success. */
static void format_script_keeps_long_lines_whole(void)
{
  const int vnums[] = { 100 };
  char script[512], expect[512];

  make_index(vnums, 1);
  start_edit(150);
  memset(script, 'x', 400);
  strlcpy(script + 400, "\r\nsay two\r\n", sizeof(script) - 400);
  memcpy(expect, script, sizeof(expect));
  edit_script(script);

  TEST_ASSERT_TRUE(format_script(bd));
  TEST_ASSERT_EQUAL_STRING(expect, OLC_STORAGE(bd));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* A formatted script that would exceed the editor's limit is refused loudly
 * and the original is kept. */
static void format_script_refuses_result_over_max_str(void)
{
  const int vnums[] = { 100 };
  char *before;

  make_index(vnums, 1);
  start_edit(150);
  edit_script("if 1\r\nsay a fairly long line\r\nend\r\n");
  bd->max_str = 20;
  before = OLC_STORAGE(bd);
  clear_output();

  TEST_ASSERT_FALSE(format_script(bd));
  TEST_ASSERT_TRUE(tb_captured_has_op("trigedit-format-too-long"));
  TEST_ASSERT_TRUE(OLC_STORAGE(bd) == before);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "String too long at line 2"));
}

/* E2-07: a script the loader would reject is refused before any prototype
 * or live trigger changes, the builder is told, and the edit stays open.
 * (Edit buffers loaded from disk may exceed MAX_CMD_LENGTH.) */
static void oversized_script_is_refused_before_memory_changes(void)
{
  const int vnums[] = { 100 };
  size_t size = MAX_STRING_LENGTH + 4096, n = 0;
  char *big;

  make_index(vnums, 1);
  start_edit(100);
  CREATE(big, char, size);
  while (n + 100 < size - 1) {
    memset(big + n, 'x', 98);
    big[n + 98] = '\r';
    big[n + 99] = '\n';
    n += 100;
  }
  free(OLC_STORAGE(bd));
  OLC_STORAGE(bd) = big;
  OLC_VAL(bd) = 1;
  clear_output();

  feed(TRIGEDIT_CONFIRM_SAVESTRING, "y");

  TEST_ASSERT_TRUE(tb_captured_has_op("trigedit-save-size"));
  TEST_ASSERT_NOT_NULL(bd->olc);                        /* still editing */
  TEST_ASSERT_EQUAL_INT(TRIGEDIT_MAIN_MENU, OLC_MODE(bd));
  TEST_ASSERT_EQUAL_STRING("say 100", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_EQUAL_INT('x', OLC_STORAGE(bd)[100]);     /* not strtok'd */
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "NOT saved"));
  TEST_ASSERT_EQUAL_STRING("", read_file("world/trg/1.trg"));
}

/* E2-07: a zone file that cannot be written is reported to the builder and
 * the log, with no partial file left; the in-memory save still happened. */
static void unwritable_zone_file_is_reported(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  /* fopen("w") fails on a directory even for root. */
  TEST_ASSERT_EQUAL_INT(0, mkdir("world/trg/1.new", 0700));
  start_edit(100);
  set_script("say edited\r\n");
  clear_output();

  save_and_quit();

  TEST_ASSERT_TRUE(tb_captured_has_op("trigedit-save"));
  TEST_ASSERT_EQUAL_STRING("say edited", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "NOT to disk"));
  TEST_ASSERT_NULL(strstr(bd->output, "Trigger saved to disk"));
  TEST_ASSERT_EQUAL_STRING("", read_file("world/trg/1.trg"));
}

/* The normal save writes the zone file and the index entry. */
static void save_writes_zone_file(void)
{
  const int vnums[] = { 100 };
  const char *file;

  make_index(vnums, 1);
  start_edit(100);
  set_script("say one\r\n\r\nsay two\r\n");
  clear_output();
  save_and_quit();

  file = read_file("world/trg/1.trg");
  TEST_ASSERT_NOT_NULL(strstr(file, "#100\n"));
  TEST_ASSERT_NOT_NULL(strstr(file, "say one\nsay two\n~\n$~\n"));
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "Trigger saved to disk"));
  /* Re-saving a listed zone must not add a blank line before '$'. */
  TEST_ASSERT_EQUAL_STRING("1.trg\n$\n", read_file("world/trg/index"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* E2-05: setting the argument again leaked the previous one (LeakSanitizer
 * and Valgrind report it). */
static void replacing_the_argument_frees_the_old_one(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  trig_index[0]->proto->arglist = strdup("old");
  start_edit(100);
  TEST_ASSERT_EQUAL_STRING("old", OLC_TRIG(bd)->arglist);
  feed(TRIGEDIT_ARGUMENT, "one");
  feed(TRIGEDIT_ARGUMENT, "two");
  TEST_ASSERT_EQUAL_STRING("two", OLC_TRIG(bd)->arglist);
  feed(TRIGEDIT_ARGUMENT, "");
  TEST_ASSERT_NULL(OLC_TRIG(bd)->arglist);
}

/* A zone missing from the index is inserted in numeric order. */
static void save_inserts_zone_into_index_in_order(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  write_file("world/trg/index", "0.trg\n2.trg\n$\n");
  start_edit(100);
  set_script("say one\r\n");
  save_and_quit();

  TEST_ASSERT_EQUAL_STRING("0.trg\n1.trg\n2.trg\n$\n", read_file("world/trg/index"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* E2-08: an index that ends without '$' used to be replaced by one with
 * neither the new entry nor a terminator, silently. */
static void index_without_terminator_is_kept_and_reported(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  write_file("world/trg/index", "0.trg\n");
  start_edit(100);
  set_script("say one\r\n");
  clear_output();
  save_and_quit();

  TEST_ASSERT_TRUE(tb_captured_has_op("trigger-index-terminator"));
  TEST_ASSERT_EQUAL_STRING("0.trg\n", read_file("world/trg/index"));
  TEST_ASSERT_EQUAL_STRING("", read_file("world/trg/newindex"));
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "index was NOT updated"));
}

/* E2-08: a malformed entry keeps the old index and names the zone. */
static void malformed_index_entry_is_reported(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  write_file("world/trg/index", "zero.trg\n$\n");
  start_edit(100);
  set_script("say one\r\n");
  save_and_quit();

  TEST_ASSERT_TRUE(tb_captured_has_op("trigger-index-parse"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 1"));
  TEST_ASSERT_EQUAL_STRING("zero.trg\n$\n", read_file("world/trg/index"));
}

/* E2-10: non-numeric menu answers used to parse as 0.  "abc" at the copy
 * prompt copied trigger #0 over the work in progress. */
static void copy_prompt_rejects_non_numbers(void)
{
  const int vnums[] = { 0, 100 };

  make_index(vnums, 2);
  start_edit(100);
  clear_output();
  feed(TRIGEDIT_COPY, "abc");
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "not a trigger vnum"));
  TEST_ASSERT_NOT_NULL(strstr(OLC_STORAGE(bd), "say 100"));
  TEST_ASSERT_EQUAL_INT(0, OLC_VAL(bd));

  feed(TRIGEDIT_COPY, "65636");       /* 100 after truncation to IDXTYPE */
  TEST_ASSERT_EQUAL_INT(0, OLC_VAL(bd));
}

static void numeric_argument_rejects_text_and_reports_clamp(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  start_edit(100);
  clear_output();
  feed(TRIGEDIT_NARG, "lots");
  TEST_ASSERT_EQUAL_INT(TRIGEDIT_NARG, OLC_MODE(bd));
  TEST_ASSERT_EQUAL_INT(0, OLC_TRIG(bd)->narg);

  feed(TRIGEDIT_NARG, "500");
  TEST_ASSERT_EQUAL_INT(100, OLC_TRIG(bd)->narg);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "limited to 100"));

  feed(TRIGEDIT_INTENDED, "x");
  TEST_ASSERT_EQUAL_INT(TRIGEDIT_INTENDED, OLC_MODE(bd));
  feed(TRIGEDIT_INTENDED, "2");
  TEST_ASSERT_EQUAL_INT(WLD_TRIGGER, OLC_TRIG(bd)->attach_type);
}

static void trigedit_rejects_overflowing_vnum(void)
{
  char arg[] = "4294967296";

  clear_output();
  do_oasis_trigedit(builder, arg, 0, 0);
  TEST_ASSERT_NULL(bd->olc);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "can't exist"));
}

/* E2-11: object and room type tables have one name fewer than the mob
 * table; option 21 printed the "\n" sentinel and toggled an undefined bit. */
static void type_menu_is_bounded_per_attach_type(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  start_edit(100);
  feed(TRIGEDIT_INTENDED, "1");                 /* objects */
  OLC_TRIG(bd)->trigger_type = 0;
  clear_output();
  feed(TRIGEDIT_TYPES, "21");
  TEST_ASSERT_EQUAL_INT(0, OLC_TRIG(bd)->trigger_type);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "Please enter 1-20"));

  feed(TRIGEDIT_TYPES, "20");
  TEST_ASSERT_EQUAL_INT(1L << 19, OLC_TRIG(bd)->trigger_type);

  feed(TRIGEDIT_INTENDED, "0");                 /* mobiles: 21 types */
  OLC_TRIG(bd)->trigger_type = 0;
  feed(TRIGEDIT_TYPES, "21");
  TEST_ASSERT_EQUAL_INT(1L << 20, OLC_TRIG(bd)->trigger_type);
}

/* E2-13: colour codes make a long script's listing exceed the display
 * buffer; the excess used to vanish without a word. */
static void long_highlighted_listing_says_it_was_cut(void)
{
  const int vnums[] = { 100 };
  const char *row = "%a% %a% %a% %a% %a%\r\n";
  size_t size = 40000, n = 0, rl = strlen(row);
  char *big;

  make_index(vnums, 1);
  start_edit(100);
  CREATE(big, char, size);
  while (n + rl < size)
    n += (size_t)snprintf(big + n, size - n, "%s", row);
  free(OLC_STORAGE(bd));
  OLC_STORAGE(bd) = big;

  feed(TRIGEDIT_MAIN_MENU, "6");

  TEST_ASSERT_TRUE(tb_captured_has_op("trigedit-highlight-truncate"));
  TEST_ASSERT_NOT_NULL(bd->showstr_head);
  TEST_ASSERT_NOT_NULL(strstr(bd->showstr_head, "listing truncated"));
  TEST_ASSERT_TRUE(OLC_STORAGE(bd) == big);           /* script untouched */
  bd->str = NULL;
}

/* A short script is listed in full with no diagnostic. */
static void short_highlighted_listing_is_complete(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  start_edit(100);
  set_script("say one\r\nsay two\r\n");
  feed(TRIGEDIT_MAIN_MENU, "6");
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "two"));
  TEST_ASSERT_NULL(strstr(bd->output, "listing truncated"));
  bd->str = NULL;
}

/* E2-09: a descriptor still holding an OLC structure while playing was
 * handled with free(d->olc), leaking the scratch trigger and script buffer
 * (LeakSanitizer reports them), and was logged only by a mudlog that named
 * no builder. */
static void stale_olc_structure_is_released_and_reported(void)
{
  const int vnums[] = { 100 };

  make_index(vnums, 1);
  start_edit(100);
  STATE(bd) = CON_PLAYING;       /* an editor that left without cleanup_olc() */
  tb_error_capture_reset();
  start_edit(100);
  TEST_ASSERT_EQUAL_STRING("olc-stale-state", tb_captured_ops[0]);
}

/* E2-12: dg_script_menu() indexed trig_index[real_trigger(vnum)] without
 * checking for NOTHING. A prototype's list can keep the vnum of a trigger
 * that does not exist, and the menu then read past trig_index. */
static void script_menu_lists_a_missing_trigger(void)
{
  const int vnums[] = { 100 };
  struct trig_proto_list missing = { 150, NULL };
  struct trig_proto_list present = { 100, &missing };

  make_index(vnums, 1);
  CREATE(bd->olc, struct oasis_olc_data, 1);
  OLC_ITEM_TYPE(bd) = MOB_TRIGGER;
  OLC_NUM(bd) = 120;
  OLC_SCRIPT(bd) = &present;
  clear_output();
  dg_script_menu(bd);
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "[100]"));
  TEST_ASSERT_NOT_NULL(strstr(bd->output, "missing trigger"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("script-menu-missing-trigger", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "trigger vnum 150 on mob vnum 120"));
  OLC_SCRIPT(bd) = NULL;
  free(bd->olc);
  bd->olc = NULL;
}

/* E2-16: the scratch trigger kept its prototype's cmdlist.  Another builder
 * saving that trigger frees the list, which left this scratch pointing at
 * freed memory.  The script lives in OLC_STORAGE; saving still works. */
static void scratch_trigger_never_borrows_a_cmdlist(void)
{
  const int vnums[] = { 100, 102 };

  make_index(vnums, 2);
  start_edit(100);
  TEST_ASSERT_NULL(OLC_TRIG(bd)->cmdlist);
  TEST_ASSERT_NOT_NULL(strstr(OLC_STORAGE(bd), "say 100"));

  feed(TRIGEDIT_COPY, "102");
  TEST_ASSERT_NULL(OLC_TRIG(bd)->cmdlist);
  TEST_ASSERT_NOT_NULL(strstr(OLC_STORAGE(bd), "say 102"));

  set_script("say edited\r\n");
  save_and_quit();
  TEST_ASSERT_EQUAL_STRING("say edited", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_EQUAL_STRING("say 102", trig_index[1]->proto->cmdlist->cmd);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(long_highlighted_listing_says_it_was_cut);
  RUN_TEST(short_highlighted_listing_is_complete);
  RUN_TEST(type_menu_is_bounded_per_attach_type);
  RUN_TEST(copy_prompt_rejects_non_numbers);
  RUN_TEST(numeric_argument_rejects_text_and_reports_clamp);
  RUN_TEST(trigedit_rejects_overflowing_vnum);
  RUN_TEST(save_inserts_zone_into_index_in_order);
  RUN_TEST(index_without_terminator_is_kept_and_reported);
  RUN_TEST(malformed_index_entry_is_reported);
  RUN_TEST(replacing_the_argument_frees_the_old_one);
  RUN_TEST(oversized_script_is_refused_before_memory_changes);
  RUN_TEST(unwritable_zone_file_is_reported);
  RUN_TEST(save_writes_zone_file);
  RUN_TEST(format_script_keeps_long_lines_whole);
  RUN_TEST(format_script_refuses_result_over_max_str);
  RUN_TEST(copy_trigger_replaces_scratch_and_keeps_save_prompt);
  RUN_TEST(saving_keeps_the_prototype_rnum);
  RUN_TEST(format_script_refuses_nesting_past_its_stack);
  RUN_TEST(format_script_indents_nested_blocks);
  RUN_TEST(inserting_a_trigger_renumbers_displaced_live_instances);
  RUN_TEST(stale_live_rnum_after_insert_is_reported);
  RUN_TEST(stale_olc_structure_is_released_and_reported);
  RUN_TEST(script_menu_lists_a_missing_trigger);
  RUN_TEST(scratch_trigger_never_borrows_a_cmdlist);
  return UNITY_END();
}
