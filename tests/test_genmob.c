/* Regression tests for the production GenOLC mobile deleter
 * (genmob.c delete_mobile) and the zone-command removal it relies on
 * (genzon.c). Both are linked with --gc-sections; the world tables, shops
 * and the few handler/db functions delete_mobile reaches are faked here.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "shop.h"
#include "genolc.h"
#include "genmob.h"
#include "genzon.h"
#include "genobj.h"
#include "boards.h"
#include "dg_scripts.h"
#include "comm.h"
#include <sys/stat.h>
#include <unistd.h>

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* ---- world fakes ------------------------------------------------------ */

struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
struct char_data *mob_proto = NULL;
struct shop_data *shop_index = NULL;
int top_shop = -1;

static int extract_calls;
void extract_char(struct char_data *ch)
{
  extract_calls++;
  if (IS_NPC(ch))
    SET_BIT_AR(MOB_FLAGS(ch), MOB_NOTDEADYET);
}

/* genobj.c's delete_object() and genmob.c's delete_mobile() queue a zone
 * whose reset list changed for saving, and add_to_save_list() (genolc.c)
 * resolves the zone through db.c's real_zone(), which this test does not
 * link. */
zone_rnum real_zone(zone_vnum vnum)
{
  zone_rnum i;
  for (i = 0; i <= top_of_zone_table; i++)
    if (zone_table && zone_table[i].number == vnum)
      return i;
  return NOWHERE;
}

/* delete_object() strips a deleted prototype from shop product lists; this
 * fixture has no shops (top_shop == -1), so reaching it is a test bug. */
void remove_shop_from_int_list(IDXTYPE **list, IDXTYPE num)
{
  (void)list; (void)num;
  TEST_FAIL_MESSAGE("remove_shop_from_int_list reached without shops");
}

mob_rnum real_mobile(mob_vnum vnum)
{
  mob_rnum i;
  for (i = 0; i <= top_of_mobt; i++)
    if (mob_index[i].vnum == vnum)
      return i;
  return NOBODY;
}
void free_proto_script(void *thing, int type) { (void)thing; (void)type; }

/* Referenced from genolc.c's save_types[] and genzon.c's zone writer, which
 * the sanitizer's global metadata keeps in the link; never called here. */
struct obj_data *obj_proto = NULL;
struct index_data **trig_index = NULL;
int top_of_trigt = 0;	/* save_zone() bounds trigger rnums by it */
trig_rnum real_trigger(trig_vnum vnum) { (void)vnum; return NOTHING; }
int save_shops(zone_rnum z) { (void)z; return FALSE; }

/* delete_object() walks live objects (none here) and renumbers boards. */
struct obj_data *object_list = NULL;
struct board_info_type board_info[NUM_OF_BOARDS];
void extract_obj(struct obj_data *obj) { (void)obj; }
void obj_from_char(struct obj_data *object) { (void)object; }
void obj_from_obj(struct obj_data *obj) { (void)obj; }
void obj_to_char(struct obj_data *object, struct char_data *ch) { (void)object; (void)ch; }
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to) { (void)obj; (void)obj_to; }
void obj_to_room(struct obj_data *object, room_rnum room) { (void)object; (void)room; }
obj_rnum real_object(obj_vnum vnum) { (void)vnum; return NOTHING; }
int save_rooms(zone_rnum z) { (void)z; return FALSE; }
int save_config(IDXTYPE z) { (void)z; return FALSE; }
int save_quests(zone_rnum z) { (void)z; return FALSE; }
/* dg_olc.h calls it through the script_save_to_disk() macro. */
int script_save_to_disk_at(FILE *fp, void *item, int type, const char *file,
                           int line, const char *func)
{ (void)fp; (void)item; (void)type; (void)file; (void)line; (void)func; return 0; }

/* ---- fixture ---------------------------------------------------------- */

#define NMOBS 3

static void make_mobs(void)
{
  int i;

  top_of_mobt = NMOBS - 1;
  CREATE(mob_proto, struct char_data, NMOBS);
  CREATE(mob_index, struct index_data, NMOBS);
  for (i = 0; i < NMOBS; i++) {
    mob_index[i].vnum = i + 1;          /* vnums 1..3 at rnums 0..2 */
    mob_proto[i].nr = i;
    mob_proto[i].player.name = strdup("mob");
    mob_proto[i].player.short_descr = strdup("a mob");
    mob_proto[i].player_specials = &dummy_mob;
    SET_BIT_AR(MOB_FLAGS(&mob_proto[i]), MOB_ISNPC);
  }
}

/* Zone 0's reset list: the given commands then 'S', allocated to exactly
 * fit like remove_cmd_from_list() does, so ASan sees any read past it. */
static void make_zone(const struct reset_com *cmds, int n)
{
  int i;

  CREATE(zone_table, struct zone_data, 1);
  top_of_zone_table = 0;
  zone_table[0].number = 0;
  zone_table[0].bot = 0;
  zone_table[0].top = 99;
  CREATE(zone_table[0].cmd, struct reset_com, n + 1);
  for (i = 0; i < n; i++)
    zone_table[0].cmd[i] = cmds[i];
  zone_table[0].cmd[n].command = 'S';
}

void setUp(void)
{
  make_mobs();
  extract_calls = 0;
  character_list = NULL;
  tb_error_capture_reset();
}

void tearDown(void)
{
  int i;

  /* All five strings: save_mobiles() fills missing ones with placeholders. */
  for (i = 0; i <= top_of_mobt; i++)
    free_mobile_strings(&mob_proto[i]);
  free(mob_proto);
  free(mob_index);
  mob_proto = NULL;
  mob_index = NULL;
  if (zone_table) {
    free(zone_table[0].cmd);
    free(zone_table);
    zone_table = NULL;
  }
  free(shop_index);
  shop_index = NULL;
  top_shop = -1;
  free_save_list();      /* runs at shutdown only: it leaves the head set */
  save_list = NULL;
}

/* Zone-table tests do not exercise the prototype's strings; free them up
 * front so they do not depend on how delete_mobile releases them. */
static void drop_strings(mob_rnum rnum)
{
  free(mob_proto[rnum].player.name);
  free(mob_proto[rnum].player.short_descr);
  mob_proto[rnum].player.name = NULL;
  mob_proto[rnum].player.short_descr = NULL;
}

static struct reset_com mcmd(mob_rnum rnum)
{
  struct reset_com c;
  memset(&c, 0, sizeof(c));
  c.command = 'M';
  c.arg1 = rnum;
  c.arg3 = 3001;
  return c;
}

/* C1-21: deleting the zone's last command before 'S' moved the terminator
 * into cmd_no, the loop stepped past it and read beyond the exactly-sized
 * list (ASan heap-buffer-overflow before the fix). */
static void test_delete_last_zone_command_stays_in_bounds(void)
{
  struct reset_com cmds[] = { mcmd(1), mcmd(0) };

  make_zone(cmds, 2);
  drop_strings(0);
  delete_mobile(0);
  TEST_ASSERT_EQUAL_CHAR('M', ZCMD(0, 0).command);
  TEST_ASSERT_EQUAL_INT(0, ZCMD(0, 0).arg1);      /* old rnum 1 -> 0 */
  TEST_ASSERT_EQUAL_CHAR('S', ZCMD(0, 1).command);
  TEST_ASSERT_EQUAL_STRING("zcmd-delete", tb_captured_operation);
}

/* C1-21: two consecutive loads of the deleted mob: the second was skipped
 * and kept an rnum that, after the shift, named a different mob. */
static void test_delete_consecutive_zone_commands(void)
{
  struct reset_com cmds[] = { mcmd(1), mcmd(1), mcmd(2) };

  make_zone(cmds, 3);
  drop_strings(1);
  delete_mobile(1);
  TEST_ASSERT_EQUAL_CHAR('M', ZCMD(0, 0).command);
  TEST_ASSERT_EQUAL_INT(1, ZCMD(0, 0).arg1);      /* old rnum 2 -> 1 */
  TEST_ASSERT_EQUAL_CHAR('S', ZCMD(0, 1).command);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
}

/* C1-V01 / C1-22: the renumber loops had no NOBODY guard, so a player's nr
 * and a keeper-less shop's keeper (NOBODY = 65535 with unsigned rnums)
 * became 65534, a valid-looking rnum; and a shop whose keeper was the
 * deleted mob was handed to the mob at refpt - 1. */
static void test_delete_mobile_renumbers_only_real_rnums(void)
{
  struct reset_com none[1];
  struct char_data player;
  struct player_special_data specials;

  memset(&player, 0, sizeof(player));
  memset(&specials, 0, sizeof(specials));
  player.nr = NOBODY;
  player.player_specials = &specials;
  character_list = &player;

  CREATE(shop_index, struct shop_data, 3);
  top_shop = 2;
  shop_index[0].vnum = 10; shop_index[0].keeper = NOBODY;
  shop_index[1].vnum = 11; shop_index[1].keeper = 1;     /* the deleted mob */
  shop_index[2].vnum = 12; shop_index[2].keeper = 2;

  make_zone(none, 0);
  drop_strings(1);
  delete_mobile(1);

  TEST_ASSERT_EQUAL_INT(NOBODY, player.nr);
  TEST_ASSERT_EQUAL_INT(NOBODY, shop_index[0].keeper);
  TEST_ASSERT_EQUAL_INT(NOBODY, shop_index[1].keeper);
  TEST_ASSERT_EQUAL_INT(1, shop_index[2].keeper);
  TEST_ASSERT_EQUAL_STRING("shop-keeper-deleted", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop 11 keeper mob vnum 2"));
  character_list = NULL;
}

static void make_npc(struct char_data *npc, mob_rnum rnum)
{
  memset(npc, 0, sizeof(*npc));
  npc->nr = rnum;
  npc->player = mob_proto[rnum].player;     /* shares the proto's strings */
  npc->player_specials = &dummy_mob;
  SET_BIT_AR(MOB_FLAGS(npc), MOB_ISNPC);
}

/* C1-23: delete_mobile() ran extract_char() on the prototype (queuing an
 * extraction for something not in character_list) and the shift then
 * overwrote, and leaked, its strings. Queued instances of the deleted mob
 * were renumbered onto the previous prototype, whose live count
 * extract_char_final() would then decrement. */
static void test_delete_mobile_releases_prototype_and_detaches_instances(void)
{
  struct reset_com none[1];
  struct char_data doomed, other;

  make_npc(&doomed, 1);
  make_npc(&other, 2);
  doomed.next = &other;
  character_list = &doomed;
  make_zone(none, 0);

  delete_mobile(1);                       /* LeakSanitizer: proto strings */

  TEST_ASSERT_EQUAL_INT(1, extract_calls); /* the instance, not the proto */
  TEST_ASSERT_EQUAL_INT(NOBODY, doomed.nr);
  TEST_ASSERT_NULL(doomed.player.name);
  TEST_ASSERT_EQUAL_INT(1, other.nr);
  TEST_ASSERT_EQUAL_INT(3, mob_index[1].vnum);
  TEST_ASSERT_EQUAL_PTR(other.player.name, mob_proto[1].player.name);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  character_list = NULL;
}

/* check_mobile_strings() named the mob through mob_index[mob->nr]; an OLC
 * working copy of a new mob has nr NOBODY (65535), far past the table. */
static void test_check_mobile_strings_with_unprototyped_mob(void)
{
  struct char_data scratch;

  memset(&scratch, 0, sizeof(scratch));
  scratch.nr = NOBODY;
  scratch.player.name = strdup("x");
  scratch.player.short_descr = strdup("an x");
  scratch.player.long_descr = strdup("An x is here.\r\n");
  scratch.player.description = strdup("It is an x.\r\n");
  check_mobile_strings(&scratch);
  free_mobile_strings(&scratch);
}

/* ---- delete_object (genobj.c) ----------------------------------------- */

static struct reset_com zcmd(char command, int arg1, int arg2, int arg3)
{
  struct reset_com c;
  memset(&c, 0, sizeof(c));
  c.command = command;
  c.arg1 = arg1;
  c.arg2 = arg2;
  c.arg3 = arg3;
  return c;
}

static void make_objs(int n)
{
  int i;

  top_of_objt = n - 1;
  /* No board owns these objects: delete_object() refuses a board's object,
   * and a zero-filled board_info[] would claim rnum 0 for every board. */
  for (i = 0; i < NUM_OF_BOARDS; i++)
    board_info[i].rnum = NOTHING;
  CREATE(obj_proto, struct obj_data, n);
  CREATE(obj_index, struct index_data, n);
  for (i = 0; i < n; i++) {
    obj_index[i].vnum = 100 + i;
    obj_proto[i].item_number = i;
  }
}

static void free_objs(void)
{
  free(obj_proto);
  free(obj_index);
  obj_proto = NULL;
  obj_index = NULL;
}

/* C1-V03: the same delete-then-increment bug in delete_object(). A 'P'
 * that was deleted also fell through and tested the command shifted into
 * its slot, and deleting the last command read past the list (ASan
 * heap-buffer-overflow before the fix). */
static void test_delete_object_zone_commands(void)
{
  struct reset_com cmds[] = {
    zcmd('O', 2, 1, 3001),     /* obj rnum 2: kept, renumbered to 1 */
    zcmd('P', 0, 1, 2),        /* puts deleted obj 0 into obj 2: removed */
    zcmd('E', 2, 1, 5),        /* kept, renumbered */
    zcmd('O', 0, 1, 3001),     /* deleted obj, last before 'S': removed */
  };

  make_objs(3);
  make_zone(cmds, 4);
  delete_object(0);
  TEST_ASSERT_EQUAL_CHAR('O', ZCMD(0, 0).command);
  TEST_ASSERT_EQUAL_INT(1, ZCMD(0, 0).arg1);
  TEST_ASSERT_EQUAL_CHAR('E', ZCMD(0, 1).command);
  TEST_ASSERT_EQUAL_INT(1, ZCMD(0, 1).arg1);
  TEST_ASSERT_EQUAL_CHAR('S', ZCMD(0, 2).command);
  TEST_ASSERT_EQUAL_STRING("zone-cmd-drop", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  free_objs();
}

/* C2-01: delete_mobile() removed the deleted mob's zone resets in memory
 * but never queued the zone for saving, so they came back from the zone
 * file at the next boot; and a removal that failed would have been
 * revisited forever (delete_zone_command() now reports and returns FALSE). */
static void test_delete_mobile_queues_changed_zone_for_save(void)
{
  struct reset_com cmds[] = { mcmd(1), mcmd(2) };

  make_zone(cmds, 2);
  drop_strings(1);
  TEST_ASSERT_FALSE(in_save_list(zone_table[0].number, SL_ZON));
  delete_mobile(1);
  TEST_ASSERT_EQUAL_CHAR('M', ZCMD(0, 0).command);
  TEST_ASSERT_EQUAL_INT(1, ZCMD(0, 0).arg1);      /* old rnum 2 -> 1 */
  TEST_ASSERT_EQUAL_CHAR('S', ZCMD(0, 1).command);
  TEST_ASSERT_TRUE(in_save_list(zone_table[0].number, SL_ZON));
  TEST_ASSERT_EQUAL_STRING("zcmd-delete", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
}

/* C1-28: add_mobile() indexed zone_table with real_zone_by_thing(vnum)
 * unchecked.  A vnum in no zone (zedit shrank the range while medit was
 * open) read zone_table[NOWHERE] (ASan heap-buffer-overflow before the fix,
 * after mob_proto had already grown). */
static void test_add_mobile_refuses_vnum_outside_every_zone(void)
{
  struct reset_com none[1];
  struct char_data scratch;

  memset(none, 0, sizeof(none));
  make_zone(none, 0);                     /* zone 0 covers vnums 0..99 */
  memset(&scratch, 0, sizeof(scratch));
  scratch.nr = NOBODY;
  scratch.player_specials = &dummy_mob;

  TEST_ASSERT_EQUAL_INT(NOBODY, add_mobile(&scratch, 500));
  TEST_ASSERT_EQUAL_INT(NMOBS - 1, top_of_mobt);
  TEST_ASSERT_EQUAL_STRING("add-mobile", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 500 belongs to no zone"));

  /* An existing prototype whose zone no longer covers it is left alone. */
  zone_table[0].bot = 50;
  tb_error_capture_reset();
  TEST_ASSERT_EQUAL_INT(NOBODY, add_mobile(&scratch, 2));
  TEST_ASSERT_EQUAL_STRING("mob", mob_proto[1].player.name);
  TEST_ASSERT_EQUAL_STRING("add-mobile", tb_captured_operation);
}

/* ---- save_mobiles (genmob.c) ------------------------------------------ */

/* fopen() as the code under test sees it (linked with --wrap=fopen).  When
 * armed, the next write-only stream starts with its error indicator set,
 * as if a write had failed earlier while the final flush still works:
 * reading a write-only stream sets the indicator (EBADF). */
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
FILE *__real_fopen(const char *path, const char *mode);
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
FILE *__wrap_fopen(const char *path, const char *mode);
static int poison_next_write_stream;

FILE *__wrap_fopen(const char *path, const char *mode)
{
  FILE *fp = __real_fopen(path, mode);

  if (fp && poison_next_write_stream && mode[0] == 'w' && !strchr(mode, '+')) {
    poison_next_write_stream = 0;
    (void)fgetc(fp);
    TEST_ASSERT_TRUE(ferror(fp));
  }
  return fp;
}

static void read_file(const char *path, char *buf, size_t size)
{
  FILE *fp = fopen(path, "r");
  size_t n;
  TEST_ASSERT_NOT_NULL(fp);
  n = fread(buf, 1, size - 1, fp);
  buf[n] = '\0';
  (void)fclose(fp);
}

/* C1-29: save_mobiles() replaced the zone's .mob file after a write had
 * failed, as long as the final flush worked, so the zone lost mobs at the
 * next boot. */
static void test_save_mobiles_keeps_previous_file_after_stream_error(void)
{
  struct reset_com none[1];
  char text[64];
  FILE *fp;

  memset(none, 0, sizeof(none));
  make_zone(none, 0);
  fp = fopen(MOB_PREFIX "0.mob", "w");
  TEST_ASSERT_NOT_NULL(fp);
  (void)fputs("previous\n", fp);
  (void)fclose(fp);

  poison_next_write_stream = 1;
  TEST_ASSERT_FALSE(save_mobiles(0));
  TEST_ASSERT_EQUAL_STRING("save-stream-error", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 0 mobiles"));
  read_file(MOB_PREFIX "0.mob", text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("previous\n", text);
  fp = fopen(MOB_PREFIX "0.new", "r");
  TEST_ASSERT_NULL(fp);

  TEST_ASSERT_TRUE(save_mobiles(0));          /* a clean stream commits */
  read_file(MOB_PREFIX "0.mob", text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING_LEN("#1\nmob~\n", text, 8);
}

int main(void)
{
  char dir[] = "/tmp/tb_test_genmob.XXXXXX";
  int failures;

  /* save_mobiles() writes LIB_WORLD "mob/" relative to the working
   * directory. */
  if (!mkdtemp(dir) || chdir(dir) != 0 || mkdir("world", 0700) != 0 ||
      mkdir("world/mob", 0700) != 0) {
    perror("test_genmob fixture directory");
    return 1;
  }
  logfile = stderr;

  UNITY_BEGIN();
  RUN_TEST(test_delete_last_zone_command_stays_in_bounds);
  RUN_TEST(test_delete_consecutive_zone_commands);
  RUN_TEST(test_delete_mobile_renumbers_only_real_rnums);
  RUN_TEST(test_delete_mobile_releases_prototype_and_detaches_instances);
  RUN_TEST(test_check_mobile_strings_with_unprototyped_mob);
  RUN_TEST(test_delete_object_zone_commands);
  RUN_TEST(test_delete_mobile_queues_changed_zone_for_save);
  RUN_TEST(test_add_mobile_refuses_vnum_outside_every_zone);
  RUN_TEST(test_save_mobiles_keeps_previous_file_after_stream_error);
  failures = UNITY_END();

  (void)remove(MOB_PREFIX "0.mob");
  (void)rmdir("world/mob");
  (void)rmdir("world");
  if (chdir("/") == 0)
    (void)rmdir(dir);
  return failures;
}
