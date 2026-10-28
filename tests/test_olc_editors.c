/* Executable tests of the production OasisOLC prototype editors (oedit.c,
 * medit.c, redit.c, zedit.c, sedit.c).
 *
 * Each test builds a builder descriptor with an OLC session, drives the real
 * *_parse() entry point or setup/save function, and asserts both the data
 * outcome and that every refused or repaired operation was reported through
 * TB_ERROR_MSG / TB_WARN_MSG (tb_error_capture.h). The generic OLC library
 * (genobj/genmob/genwld/genzon/genshp) and the UID table are stubbed so each
 * test controls exactly what the editors see.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "oasis.h"
#include "dg_scripts.h"
#include "dg_olc.h"
#include "spells.h"
#include "fight.h"
#include "constants.h"
#include "genolc.h"
#include "genzon.h"
#include "genobj.h"
#include "genwld.h"
#include "modify.h"
#include <stdarg.h>
#include "tb_error_capture.h"

/* ------------------------------------------------------------------ */
/* Captured output                                                     */
/* ------------------------------------------------------------------ */
static char out_buf[16384];
static int mudlogs;
static char last_mudlog[512];

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  size_t used = strlen(out_buf);
  (void)d;
  if (used >= sizeof(out_buf) - 1)
    return 0;
  va_start(args, txt);
  if (vsnprintf(out_buf + used, sizeof(out_buf) - used, txt, args) < 0)
    out_buf[used] = '\0';
  va_end(args);
  return strlen(out_buf) - used;
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(out_buf);
  (void)ch;
  if (used >= sizeof(out_buf) - 1)
    return 0;
  va_start(args, messg);
  if (vsnprintf(out_buf + used, sizeof(out_buf) - used, messg, args) < 0)
    out_buf[used] = '\0';
  va_end(args);
  return strlen(out_buf) - used;
}

void mudlog(int type, int level, int file, const char *str, ...)
{
  va_list args;
  (void)type; (void)level; (void)file;
  mudlogs++;
  va_start(args, str);
  if (vsnprintf(last_mudlog, sizeof(last_mudlog), str, args) < 0)
    last_mudlog[0] = '\0';
  va_end(args);
}

void basic_mud_log(const char *format, ...) { (void)format; }

/* ------------------------------------------------------------------ */
/* UID table: the test decides which (uid, address) pairs are live.    */
/* ------------------------------------------------------------------ */
static long live_uid;
static const void *live_uid_obj;

long obj_script_id(obj_data *obj)
{
  if (!obj->script_id)
    obj->script_id = 100001;
  return obj->script_id;
}

int obj_uid_matches(long uid, const obj_data *obj)
{
  return obj && uid == live_uid && (const void *)obj == live_uid_obj;
}

/* ------------------------------------------------------------------ */
/* OLC session lifecycle                                               */
/* ------------------------------------------------------------------ */
static int cleanups;

static void free_scratch_obj(struct obj_data *obj)
{
  if (!obj)
    return;
  free(obj->name);
  free(obj->description);
  free(obj->short_description);
  free(obj->action_description);
  free(obj);
}

void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  (void)cleanup_type;
  cleanups++;
  if (!d->olc)
    return;
  free_scratch_obj(OLC_OBJ(d));
  if (OLC_ROOM(d)) {
    free_room_strings(OLC_ROOM(d));
    free(OLC_ROOM(d));
  }
  free(d->olc);
  d->olc = NULL;
}

/* ------------------------------------------------------------------ */
/* World tables and link stubs                                         */
/* ------------------------------------------------------------------ */
struct obj_data *object_list = NULL;
struct obj_data *obj_proto = NULL;
struct char_data *mob_proto = NULL;
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
const char *nrm = "", *grn = "", *cyn = "", *yel = "";

const char *dirs[] = { "north", "east", "south", "west", "up", "down",
  "northwest", "northeast", "southeast", "southwest", "\n" };
const char *equipment_types[] = { "light", "rfinger", "\n" };
const char *genders[] = { "neutral", "male", "female", "\n" };
const char *position_types[] = { "dead", "\n" };
const char *affected_bits[] = { "\n" };
const char *apply_types[] = { "\n" };
const char *container_bits[] = { "\n" };
const char *drinks[] = { "\n" };
const char *extra_bits[] = { "\n" };
const char *item_types[] = { "UNDEFINED", "\n" };
const char *wear_bits[] = { "\n" };
const char *zone_bits[] = { "\n" };
const char *room_bits[] = { "\n" };
const char *sector_types[] = { "inside", "\n" };
const char *exit_bits[] = { "\n" };
const char *action_bits[] = { "\n" };
struct attack_hit_type attack_hit_text[] = { { "hit", "hits" } };
struct spell_info_type spell_info[TOP_SPELL_DEFINE + 1];

int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *p;
  (void)file; (void)line; (void)func;
  p = calloc(count ? count : 1, element_size ? element_size : 1);
  TEST_ASSERT_NOT_NULL(p);
  return p;
}

char *str_udup(const char *txt) { return strdup((txt && *txt) ? txt : "undefined"); }
int genolc_checkstring(struct descriptor_data *d, char *arg) { (void)d; (void)arg; return TRUE; }
void get_char_colors(struct char_data *ch) { (void)ch; }
void clear_screen(struct descriptor_data *d) { (void)d; }
void column_list(struct char_data *ch, int num_cols, const char **list, int list_length, bool show_nums)
{ (void)ch; (void)num_cols; (void)list; (void)list_length; (void)show_nums; }
size_t sprintbit(bitvector_t v, const char *names[], char *result, size_t reslen)
{ (void)v; (void)names; if (reslen) *result = '\0'; return 0; }
size_t sprinttype(int type, const char *names[], char *result, size_t reslen)
{ (void)type; (void)names; if (reslen) *result = '\0'; return 0; }
void sprintbitarray(int bitvector[], const char *names[], int maxar, char *result, size_t reslen)
{ (void)bitvector; (void)names; (void)maxar; if (reslen) *result = '\0'; }
void string_write(struct descriptor_data *d, char **txt, size_t len, long mailto, void *data)
{ (void)d; (void)txt; (void)len; (void)mailto; free(data); }
void copy_ex_descriptions(struct extra_descr_data **to, struct extra_descr_data *from) { (void)from; *to = NULL; }
void free_ex_descriptions(struct extra_descr_data *head) { (void)head; }
void dg_olc_script_copy(struct descriptor_data *d) { OLC_SCRIPT(d) = NULL; }
void dg_script_menu(struct descriptor_data *d) { (void)d; }
int dg_script_edit_parse(struct descriptor_data *d, char *arg) { (void)d; (void)arg; return 1; }
void assign_triggers(void *i, int type) { (void)i; (void)type; }
void extract_script(void *thing, int type) { (void)thing; (void)type; }
void free_proto_script(void *thing, int type) { (void)thing; (void)type; }
void copy_proto_script(void *source, void *dest, int type) { (void)source; (void)dest; (void)type; }

zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return 0; }
int save_objects(zone_rnum zone_num) { (void)zone_num; return TRUE; }

obj_rnum real_object(obj_vnum vnum)
{
  obj_rnum i;
  for (i = 0; obj_index && i <= top_of_objt; i++)
    if (obj_index[i].vnum == vnum)
      return i;
  return NOTHING;
}

static int add_room_calls;
static room_rnum add_room_result = NOWHERE;
room_rnum add_room(struct room_data *room) { (void)room; add_room_calls++; return add_room_result; }
int delete_room(room_rnum rnum) { (void)rnum; return FALSE; }
int save_rooms(zone_rnum zone_num) { (void)zone_num; return TRUE; }
int free_room_strings(struct room_data *room)
{
  int i;
  free(room->name);
  free(room->description);
  for (i = 0; i < DIR_COUNT; i++)
    if (room->dir_option[i]) {
      free(room->dir_option[i]->general_description);
      free(room->dir_option[i]->keyword);
      free(room->dir_option[i]);
    }
  return TRUE;
}

room_rnum real_room(room_vnum vnum)
{
  room_rnum i;
  for (i = 0; world && i <= top_of_world; i++)
    if (world[i].number == vnum)
      return i;
  return NOWHERE;
}

int copy_object(struct obj_data *to, struct obj_data *from) { *to = *from; return TRUE; }
obj_rnum add_object(struct obj_data *newobj, obj_vnum ovnum) { (void)newobj; return real_object(ovnum); }
int delete_object(obj_rnum rnum) { (void)rnum; return NOTHING; }

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */
static struct descriptor_data builder_desc;
static struct char_data builder;
static struct player_special_data builder_specials;

static void make_builder(void)
{
  memset(&builder_desc, 0, sizeof(builder_desc));
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  builder.player_specials = &builder_specials;
  builder.player.name = "Builder";
  builder.player.level = LVL_BUILDER;
  GET_IDNUM(&builder) = 4242;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
  CREATE(builder_desc.olc, struct oasis_olc_data, 1);
}

static struct obj_data *scratch_obj(const char *sdesc)
{
  struct obj_data *obj;
  CREATE(obj, struct obj_data, 1);
  obj->item_number = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  obj->name = strdup("thing");
  obj->short_description = strdup(sdesc);
  obj->description = strdup("A thing lies here.");
  return obj;
}

void setUp(void)
{
  tb_error_capture_reset();
  out_buf[0] = '\0';
  last_mudlog[0] = '\0';
  mudlogs = cleanups = 0;
  live_uid = 0;
  live_uid_obj = NULL;
  add_room_calls = 0;
  add_room_result = NOWHERE;
  world = NULL;
  top_of_world = 0;
  make_builder();
}

void tearDown(void)
{
  if (builder_desc.olc)
    cleanup_olc(&builder_desc, CLEANUP_ALL);
}

/* ------------------------------------------------------------------ */
/* F1-01: iedit must not follow a live-object pointer that was freed.  */
/* ------------------------------------------------------------------ */
static void test_iedit_save_after_target_extracted_reports_and_does_not_touch_it(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *target = scratch_obj("a doomed sword");

  OLC_OBJ(d) = scratch_obj("an edited sword");
  OLC_IEDIT_OBJ(d) = target;
  OLC_IEDIT_UID(d) = obj_script_id(target);
  OLC_NUM(d) = 3005;
  OLC_MODE(d) = OEDIT_CONFIRM_SAVESTRING;

  /* extract_obj(): the UID is unregistered and the memory freed. ASan keeps
   * the block poisoned, so any read through OLC_IEDIT_OBJ fails the test. */
  free_scratch_obj(target);

  iedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("iedit-target-gone", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "uid 100001 vnum 3005"));
  TEST_ASSERT_EQUAL_INT(0, mudlogs);
  TEST_ASSERT_NULL(strstr(out_buf, "Object instance saved"));
  TEST_ASSERT_NULL(d->olc);
}

static void test_iedit_save_rejects_new_object_at_reused_address(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *impostor = scratch_obj("an unrelated object");

  OLC_OBJ(d) = scratch_obj("an edited sword");
  OLC_IEDIT_OBJ(d) = impostor;  /* same address the extracted target had */
  OLC_IEDIT_UID(d) = 100001;
  OLC_NUM(d) = 3005;
  OLC_MODE(d) = OEDIT_CONFIRM_SAVESTRING;
  live_uid = 100002;            /* the impostor registered under a new UID */
  live_uid_obj = impostor;

  iedit_parse(d, "y");

  TEST_ASSERT_EQUAL_STRING("iedit-target-gone", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("an unrelated object", impostor->short_description);
  free_scratch_obj(impostor);
}

static void test_iedit_save_live_target_copies_and_logs_from_scratch(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *target = scratch_obj("a plain sword");

  IN_ROOM(target) = 0;
  OLC_OBJ(d) = scratch_obj("a shiny sword");
  OLC_IEDIT_OBJ(d) = target;
  OLC_IEDIT_UID(d) = obj_script_id(target);
  OLC_NUM(d) = 3005;
  OLC_MODE(d) = OEDIT_CONFIRM_SAVESTRING;
  live_uid = target->script_id;
  live_uid_obj = target;

  iedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("a shiny sword", target->short_description);
  TEST_ASSERT_EQUAL_INT(1, mudlogs);
  TEST_ASSERT_NOT_NULL(strstr(last_mudlog, "a shiny sword"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "Object instance saved"));
  free_scratch_obj(target);
}

/* ------------------------------------------------------------------ */
/* World fixture: rooms 3000..3000+n-1 at rnums 0..n-1.                */
/* ------------------------------------------------------------------ */
static struct room_data world_rooms[8];

static void make_world(int n)
{
  int i;
  memset(world_rooms, 0, sizeof(world_rooms));
  for (i = 0; i < n; i++) {
    world_rooms[i].number = 3000 + i;
    world_rooms[i].name = (char *)"A room";
    world_rooms[i].description = (char *)"Plain.\r\n";
  }
  world = world_rooms;
  top_of_world = n - 1;
}

/* ------------------------------------------------------------------ */
/* F1-02: a room copy must not alias the source room's live lists.     */
/* ------------------------------------------------------------------ */
static void test_redit_copy_does_not_alias_source_contents(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data on_floor;

  memset(&on_floor, 0, sizeof(on_floor));
  make_world(2);
  world[1].contents = &on_floor;
  world[1].light = 2;

  redit_setup_existing(d, 1);

  TEST_ASSERT_NULL(OLC_ROOM(d)->contents);
  TEST_ASSERT_NULL(OLC_ROOM(d)->people);
  TEST_ASSERT_EQUAL_INT(0, OLC_ROOM(d)->light);
  TEST_ASSERT_NULL(SCRIPT(OLC_ROOM(d)));
  TEST_ASSERT_EQUAL_PTR(&on_floor, world[1].contents);
}

static void test_redit_save_refuses_scratch_holding_live_contents(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data on_floor;

  memset(&on_floor, 0, sizeof(on_floor));
  make_world(2);
  redit_setup_existing(d, 1);
  OLC_NUM(d) = 3070;
  OLC_ROOM(d)->contents = &on_floor;   /* a broken setup path */

  redit_save_internally(d);

  TEST_ASSERT_EQUAL_INT(0, add_room_calls);
  TEST_ASSERT_EQUAL_STRING("redit-scratch-alias", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3070"));
  OLC_ROOM(d)->contents = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_iedit_save_after_target_extracted_reports_and_does_not_touch_it);
  RUN_TEST(test_iedit_save_rejects_new_object_at_reused_address);
  RUN_TEST(test_iedit_save_live_target_copies_and_logs_from_scratch);
  RUN_TEST(test_redit_copy_does_not_alias_source_contents);
  RUN_TEST(test_redit_save_refuses_scratch_holding_live_contents);
  return UNITY_END();
}
