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

static void clear_output(void)
{
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
  character_list = NULL;
  descriptor_list = NULL;
  if (bd->large_outbuf)
    free(bd->large_outbuf);
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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(inserting_a_trigger_renumbers_displaced_live_instances);
  RUN_TEST(stale_live_rnum_after_insert_is_reported);
  return UNITY_END();
}
