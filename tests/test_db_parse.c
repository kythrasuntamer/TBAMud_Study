/* Executable tests of the production world/text loaders in db.c.
 *
 * db.c is #included (not linked) so its file-scope static parsers are
 * reachable; unreferenced sections are discarded at link time, so only the
 * symbols the exercised parsers reach are supplied below.  utils.c is linked
 * from an object library compiled with tb_error_log_at renamed, so the
 * capturing reporter in tb_error_capture.h receives every TB_ERROR_* record.
 *
 * Boot-time parsers end the process with exit(1) on fatal input.  exit() is
 * redirected to tb_test_exit(), which longjmp()s back into the test so the
 * fatal diagnostic can be asserted.  Each test frees what the parser
 * allocated so ASan/LSan and Valgrind see a clean run.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include <setjmp.h>

#include "tb_error_capture.h"

static jmp_buf tb_test_exit_jmp;
static int tb_test_exit_armed;
static int tb_test_exit_code;

static void tb_test_exit(int code) __attribute__((noreturn));
static void tb_test_exit(int code)
{
  tb_test_exit_code = code;
  if (!tb_test_exit_armed)
    abort();
  tb_test_exit_armed = 0;
  longjmp(tb_test_exit_jmp, 1);
}

/* Run STMT, which must end in exit(); fail the test if it returns. */
#define EXPECT_EXIT(stmt) do { \
  tb_test_exit_armed = 1; \
  if (setjmp(tb_test_exit_jmp) == 0) { \
    stmt; \
    tb_test_exit_armed = 0; \
    TEST_FAIL_MESSAGE("expected exit(): " #stmt); \
  } \
} while (0)

#define exit(code) tb_test_exit(code)
#include "db.c"
#undef exit

/* ---------------------------------------------------------------------
 * Fixtures for symbols the exercised parsers reach outside db.c/utils.c.
 * ------------------------------------------------------------------- */

FILE *logfile = NULL;                          /* comm.c: NULL logs to stderr */
struct descriptor_data *descriptor_list = NULL; /* comm.c */
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{ (void)ch; (void)messg; return 0; }
void parse_at(char *str) { (void)str; }

/* DG fixtures for reset_zone(). The object UID table is a single slot: the
 * tests load one object at a time. A load trigger "purges" its object the
 * way extract_obj() does, by unregistering and freeing it immediately. */
static long fixture_uid;
static const void *fixture_uid_owner;
static int fixture_purge_on_load, fixture_load_triggers, fixture_triggers_added;
static int fixture_vars_added;

long obj_script_id(obj_data *obj)
{
  if (obj->script_id == 0) {
    obj->script_id = OBJ_ID_BASE + 1;
    fixture_uid = obj->script_id;
    fixture_uid_owner = obj;
  }
  return obj->script_id;
}
int obj_uid_matches(long uid, const obj_data *obj)
{ return obj && uid == fixture_uid && fixture_uid_owner == (const void *)obj; }

static void fixture_free_obj(struct obj_data *obj)
{
  struct obj_data **pp;
  for (pp = &object_list; *pp; pp = &(*pp)->next)
    if (*pp == obj) { *pp = obj->next; break; }
  if (obj->script_id == fixture_uid) { fixture_uid = 0; fixture_uid_owner = NULL; }
  free(SCRIPT(obj));
  free(obj);
}

void copy_proto_script(void *source, void *dest, int type)
{ (void)source; (void)dest; (void)type; }
void assign_triggers(void *i, int type)
{
  obj_data *obj = i;
  (void)type;
  CREATE(SCRIPT(obj), struct script_data, 1);
  SCRIPT(obj)->types = OTRIG_LOAD;
}
void load_otrigger(obj_data *obj)
{
  fixture_load_triggers++;
  if (fixture_purge_on_load)
    fixture_free_obj(obj);
}
int wear_otrigger(obj_data *obj, char_data *actor, int where)
{ (void)obj; (void)actor; (void)where; return 1; }
void obj_to_room(struct obj_data *object, room_rnum room)
{ IN_ROOM(object) = room; }
trig_data *read_trigger(int nr) { (void)nr; return NULL; }
void add_trigger(struct script_data *sc, trig_data *t, int loc)
{ (void)sc; (void)t; (void)loc; fixture_triggers_added++; }
void add_var(struct trig_var_data **var_list, const char *name,
             const char *value, long id)
{ (void)var_list; (void)name; (void)value; (void)id; fixture_vars_added++; }
void reset_wtrigger(room_data *room) { (void)room; }
void load_mtrigger(char_data *ch) { (void)ch; }
void char_to_room(struct char_data *ch, room_rnum room)
{ (void)ch; (void)room; }
void obj_to_char(struct obj_data *object, struct char_data *ch)
{ (void)object; (void)ch; }
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to)
{ (void)obj; (void)obj_to; }
void equip_char(struct char_data *ch, struct obj_data *obj, int pos)
{ (void)ch; (void)obj; (void)pos; }
void extract_obj(struct obj_data *obj) { (void)obj; }
struct obj_data *get_obj_num(obj_rnum nr) { (void)nr; return NULL; }
struct obj_data *get_obj_in_list_num(int num, struct obj_data *list)
{ (void)num; (void)list; return NULL; }
unsigned long circle_random(void) { return 0; }

void setUp(void)
{
  tb_error_capture_reset();
  tb_test_exit_armed = 0;
  tb_test_exit_code = 0;
}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * reset_zone(): object load triggers that purge their object (B1-02)
 * ------------------------------------------------------------------- */

static struct obj_data zone_obj_proto[1];
static struct index_data zone_obj_index[1];
static struct room_data zone_rooms[1];
static struct zone_data zone_fixture[1];

static void zone_fixture_setup(struct reset_com *cmds)
{
  memset(zone_obj_proto, 0, sizeof(zone_obj_proto));
  memset(zone_obj_index, 0, sizeof(zone_obj_index));
  memset(zone_rooms, 0, sizeof(zone_rooms));
  memset(zone_fixture, 0, sizeof(zone_fixture));
  zone_obj_index[0].vnum = 3022;
  zone_obj_proto[0].item_number = 0;
  zone_rooms[0].number = 3001;
  obj_proto = zone_obj_proto;
  obj_index = zone_obj_index;
  top_of_objt = 0;
  world = zone_rooms;
  top_of_world = 0;
  zone_fixture[0].number = 30;
  zone_fixture[0].bot = 3000;
  zone_fixture[0].top = 3001;
  zone_fixture[0].cmd = cmds;
  zone_table = zone_fixture;
  top_of_zone_table = 0;
  object_list = NULL;
  fixture_uid = 0;
  fixture_uid_owner = NULL;
  fixture_load_triggers = fixture_triggers_added = fixture_vars_added = 0;
}

static void zone_fixture_teardown(void)
{
  while (object_list)
    fixture_free_obj(object_list);
  world = NULL;
  zone_table = NULL;
  obj_proto = NULL;
  obj_index = NULL;
}

/* 'O' loads an object whose load trigger purges it, then 'T' attaches a
 * trigger to "the object just loaded". Before the fix reset_zone kept the
 * freed pointer as tobj and dereferenced SCRIPT(tobj) (heap-use-after-free). */
static void reset_zone_skips_trigger_after_load_purge(void)
{
  struct reset_com cmds[] = {
    { 'O', FALSE, 0, 99, 0, 1, NULL, NULL },
    { 'T', TRUE, OBJ_TRIGGER, 0, 0, 2, NULL, NULL },
    { 'V', TRUE, OBJ_TRIGGER, 0, 0, 3, (char *)"foo", (char *)"bar" },
    { 'S', FALSE, 0, 0, 0, 4, NULL, NULL },
  };

  zone_fixture_setup(cmds);
  fixture_purge_on_load = 1;
  reset_zone(0);

  TEST_ASSERT_EQUAL_INT(1, fixture_load_triggers);
  TEST_ASSERT_EQUAL_INT(0, fixture_triggers_added);
  TEST_ASSERT_EQUAL_INT(0, fixture_vars_added);
  TEST_ASSERT_NULL(object_list);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("zone-reset", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 3022"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 30"));
  zone_fixture_teardown();
}

/* A load trigger that leaves its object alone changes nothing. */
static void reset_zone_keeps_object_when_trigger_does_not_purge(void)
{
  struct reset_com cmds[] = {
    { 'O', FALSE, 0, 99, 0, 1, NULL, NULL },
    { 'T', TRUE, OBJ_TRIGGER, 0, 0, 2, NULL, NULL },
    { 'S', FALSE, 0, 0, 0, 3, NULL, NULL },
  };

  zone_fixture_setup(cmds);
  fixture_purge_on_load = 0;
  reset_zone(0);

  TEST_ASSERT_EQUAL_INT(1, fixture_load_triggers);
  TEST_ASSERT_EQUAL_INT(1, fixture_triggers_added);
  TEST_ASSERT_NOT_NULL(object_list);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  zone_fixture_teardown();
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(reset_zone_skips_trigger_after_load_purge);
  RUN_TEST(reset_zone_keeps_object_when_trigger_does_not_purge);
  return UNITY_END();
}
