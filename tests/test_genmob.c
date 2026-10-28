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
void script_save_to_disk(FILE *fp, void *item, int type) { (void)fp; (void)item; (void)type; }

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

  for (i = 0; i <= top_of_mobt; i++) {
    free(mob_proto[i].player.name);
    free(mob_proto[i].player.short_descr);
  }
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
  TEST_ASSERT_EQUAL_STRING("zcmd-delete", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  free_objs();
}

int main(void)
{
  logfile = stderr;

  UNITY_BEGIN();
  RUN_TEST(test_delete_last_zone_command_stays_in_bounds);
  RUN_TEST(test_delete_consecutive_zone_commands);
  RUN_TEST(test_delete_mobile_renumbers_only_real_rnums);
  RUN_TEST(test_delete_mobile_releases_prototype_and_detaches_instances);
  RUN_TEST(test_check_mobile_strings_with_unprototyped_mob);
  RUN_TEST(test_delete_object_zone_commands);
  return UNITY_END();
}
