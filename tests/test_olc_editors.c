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
#include "shop.h"
#include "genshp.h"
#include "olc_checks.h"
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

/* Zone command lists: the same exact-size reallocation contract as genzon.c,
 * so a stale index past the shrunken list is visible to ASan. new_command()
 * fills the new entry with garbage, as genzon's uninitialised stack copy
 * may, so zedit must initialise every field it later reads. */
int top_of_trigt = 0;
struct index_data **trig_index = NULL;
trig_rnum real_trigger(trig_vnum vnum) { (void)vnum; return NOTHING; }
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; return NOBODY; }
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return TRUE; }
room_vnum genolc_zone_bottom(zone_rnum rznum) { (void)rznum; return 0; }
room_vnum genolc_zonep_bottom(struct zone_data *zone) { (void)zone; return 0; }
void remove_room_zone_commands(zone_rnum zone, room_rnum room_num) { (void)zone; (void)room_num; }
int save_zone(zone_rnum zone_num) { (void)zone_num; return TRUE; }

int count_commands(struct reset_com *list)
{
  int count = 0;
  while (list[count].command != 'S')
    count++;
  return count;
}

void add_cmd_to_list(struct reset_com **list, struct reset_com *newcmd, int pos)
{
  int count = count_commands(*list), i, l;
  struct reset_com *newlist = calloc((size_t)count + 2, sizeof(*newlist));
  TEST_ASSERT_NOT_NULL(newlist);
  for (i = 0, l = 0; i <= count; i++)
    newlist[i] = (i == pos) ? *newcmd : (*list)[l++];
  newlist[count + 1].command = 'S';
  free(*list);
  *list = newlist;
}

int new_command(struct zone_data *zone, int pos)
{
  struct reset_com new_com;
  int count = count_commands(zone->cmd);
  if (pos < 0 || pos > count)
    return 0;
  memset(&new_com, 0x5a, sizeof(new_com));
  new_com.sarg1 = new_com.sarg2 = NULL;
  new_com.command = 'N';
  add_cmd_to_list(&zone->cmd, &new_com, pos);
  return 1;
}

void delete_zone_command(struct zone_data *zone, int pos)
{
  int count = count_commands(zone->cmd), i, l;
  struct reset_com *newlist;
  if (pos < 0 || pos >= count)
    return;
  newlist = calloc((size_t)count, sizeof(*newlist));
  TEST_ASSERT_NOT_NULL(newlist);
  for (i = 0, l = 0; i < count; i++)
    if (i != pos)
      newlist[l++] = zone->cmd[i];
  newlist[count - 1].command = 'S';
  free(zone->cmd);
  zone->cmd = newlist;
}

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }
int copy_object(struct obj_data *to, struct obj_data *from)
{
  *to = *from;
  to->name = dup_or_null(from->name);
  to->description = dup_or_null(from->description);
  to->short_description = dup_or_null(from->short_description);
  to->action_description = dup_or_null(from->action_description);
  to->ex_description = NULL;
  return TRUE;
}
static int add_object_result = -1;   /* < 0: update in place */
obj_rnum add_object(struct obj_data *newobj, obj_vnum ovnum)
{
  (void)newobj;
  return add_object_result >= 0 ? (obj_rnum)add_object_result : real_object(ovnum);
}

static int deleted_rnum = -1, delete_calls;
int delete_object(obj_rnum rnum) { delete_calls++; deleted_rnum = rnum; return rnum; }

static int zone_editable = TRUE;
int can_edit_zone(struct char_data *ch, zone_rnum rnum) { (void)ch; (void)rnum; return zone_editable; }

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
  deleted_rnum = -1;
  add_object_result = -1;
  delete_calls = 0;
  zone_editable = TRUE;
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

/* ------------------------------------------------------------------ */
/* Prototype table fixtures: heap arrays sized exactly to the table so */
/* ASan flags any index past the end.                                  */
/* ------------------------------------------------------------------ */
static struct zone_data zone_fixture;

static void make_objects(int n)
{
  int i;
  free(obj_index);
  free(obj_proto);
  obj_index = calloc((size_t)n, sizeof(*obj_index));
  obj_proto = calloc((size_t)n, sizeof(*obj_proto));
  for (i = 0; i < n; i++) {
    obj_index[i].vnum = 3000 + i;
    obj_proto[i].item_number = i;
    obj_proto[i].short_description = (char *)"a prototype";
  }
  top_of_objt = n - 1;
}

static void make_zone(void)
{
  memset(&zone_fixture, 0, sizeof(zone_fixture));
  zone_fixture.number = 30;
  zone_fixture.name = (char *)"Test zone";
  zone_table = &zone_fixture;
  top_of_zone_table = 0;
}

/* A zedit session for room 3001 with the given commands plus 'S'. */
static void make_zedit_session(struct descriptor_data *d, const struct reset_com *cmds, int n)
{
  int i;
  CREATE(OLC_ZONE(d), struct zone_data, 1);
  CREATE(OLC_ZONE(d)->cmd, struct reset_com, n + 1);
  for (i = 0; i < n; i++)
    OLC_ZONE(d)->cmd[i] = cmds[i];
  OLC_ZONE(d)->cmd[n].command = 'S';
  OLC_NUM(d) = 3001;
  OLC_ZNUM(d) = 0;
  STATE(d) = CON_ZEDIT;
}

static void free_zedit_session(struct descriptor_data *d)
{
  if (d->olc && OLC_ZONE(d)) {
    free(OLC_ZONE(d)->cmd);
    free(OLC_ZONE(d));
    OLC_ZONE(d) = NULL;
  }
}

/* ------------------------------------------------------------------ */
/* F1-03: deletes renumber other sessions; stale rnums are reported.   */
/* ------------------------------------------------------------------ */
static void test_zedit_menu_reports_stale_object_rnum_instead_of_reading_past_table(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_objects(3);
  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = 'O';
  cmd.arg1 = 3;          /* was top_of_objt before another builder deleted */
  cmd.arg3 = 0;
  make_zedit_session(d, &cmd, 1);
  OLC_MODE(d) = ZEDIT_MAIN_MENU;

  zedit_parse(d, "?");   /* any unknown choice redisplays the menu */

  TEST_ASSERT_EQUAL_STRING("stale-rnum", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "object rnum 3 outside [0, 2]"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "<stale object rnum 3>"));
  free_zedit_session(d);
}

static void test_object_delete_renumbers_other_builders_sessions(void)
{
  struct descriptor_data zedit_d, sedit_d, oedit_d;
  struct char_data zb, sb, ob;
  struct player_special_data sp;
  struct reset_com cmds[3];

  make_objects(4);
  memset(&sp, 0, sizeof(sp));
  memset(cmds, 0, sizeof(cmds));
  cmds[0].command = 'O'; cmds[0].arg1 = 1;   /* the deleted prototype */
  cmds[1].command = 'O'; cmds[1].arg1 = 3;   /* above it: moves down */
  cmds[2].command = 'P'; cmds[2].arg1 = 0; cmds[2].arg3 = 2;

  memset(&zedit_d, 0, sizeof(zedit_d)); memset(&zb, 0, sizeof(zb));
  zb.player_specials = &sp; GET_IDNUM(&zb) = 7; zedit_d.character = &zb;
  CREATE(zedit_d.olc, struct oasis_olc_data, 1);
  make_zedit_session(&zedit_d, cmds, 3);

  memset(&sedit_d, 0, sizeof(sedit_d)); memset(&sb, 0, sizeof(sb));
  sb.player_specials = &sp; GET_IDNUM(&sb) = 8; sedit_d.character = &sb;
  CREATE(sedit_d.olc, struct oasis_olc_data, 1);
  CREATE(OLC_SHOP(&sedit_d), struct shop_data, 1);
  CREATE(S_PRODUCTS(OLC_SHOP(&sedit_d)), obj_vnum, 4);
  S_PRODUCT(OLC_SHOP(&sedit_d), 0) = 1;
  S_PRODUCT(OLC_SHOP(&sedit_d), 1) = 2;
  S_PRODUCT(OLC_SHOP(&sedit_d), 2) = 0;
  S_PRODUCT(OLC_SHOP(&sedit_d), 3) = NOTHING;
  S_KEEPER(OLC_SHOP(&sedit_d)) = NOBODY;
  STATE(&sedit_d) = CON_SEDIT;

  memset(&oedit_d, 0, sizeof(oedit_d)); memset(&ob, 0, sizeof(ob));
  ob.player_specials = &sp; GET_IDNUM(&ob) = 9; oedit_d.character = &ob;
  CREATE(oedit_d.olc, struct oasis_olc_data, 1);
  OLC_OBJ(&oedit_d) = scratch_obj("edited elsewhere");
  OLC_OBJ(&oedit_d)->item_number = 3;
  STATE(&oedit_d) = CON_OEDIT;

  descriptor_list = &zedit_d;
  zedit_d.next = &sedit_d;
  sedit_d.next = &oedit_d;

  olc_renumber_sessions(OLC_RNUM_OBJ, 1, -1, NULL);

  TEST_ASSERT_EQUAL_CHAR('*', OLC_ZONE(&zedit_d)->cmd[0].command);
  TEST_ASSERT_EQUAL_INT(2, OLC_ZONE(&zedit_d)->cmd[1].arg1);
  TEST_ASSERT_EQUAL_INT(0, OLC_ZONE(&zedit_d)->cmd[2].arg1);
  TEST_ASSERT_EQUAL_INT(1, OLC_ZONE(&zedit_d)->cmd[2].arg3);
  TEST_ASSERT_EQUAL_INT(1, S_PRODUCT(OLC_SHOP(&sedit_d), 0));
  TEST_ASSERT_EQUAL_INT(0, S_PRODUCT(OLC_SHOP(&sedit_d), 1));
  TEST_ASSERT_EQUAL_INT(NOTHING, S_PRODUCT(OLC_SHOP(&sedit_d), 2));
  TEST_ASSERT_EQUAL_INT(2, GET_OBJ_RNUM(OLC_OBJ(&oedit_d)));
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);   /* zone cmd 0, product 0 */
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("olc-session-renumber", tb_captured_operation);

  descriptor_list = NULL;
  free_zedit_session(&zedit_d);
  free(zedit_d.olc);
  free(S_PRODUCTS(OLC_SHOP(&sedit_d)));
  free(OLC_SHOP(&sedit_d));
  free(sedit_d.olc);
  free_scratch_obj(OLC_OBJ(&oedit_d));
  free(oedit_d.olc);
}

static void test_room_insert_leaves_nowhere_exits_alone(void)
{
  struct descriptor_data redit_d;
  struct char_data rb;
  struct player_special_data sp;
  struct room_direction_data *north, *south;

  memset(&sp, 0, sizeof(sp));
  memset(&redit_d, 0, sizeof(redit_d)); memset(&rb, 0, sizeof(rb));
  rb.player_specials = &sp; redit_d.character = &rb;
  CREATE(redit_d.olc, struct oasis_olc_data, 1);
  CREATE(OLC_ROOM(&redit_d), struct room_data, 1);
  CREATE(north, struct room_direction_data, 1);
  CREATE(south, struct room_direction_data, 1);
  north->to_room = NOWHERE;   /* exit created in the menu, not yet set */
  south->to_room = 4;
  OLC_ROOM(&redit_d)->dir_option[NORTH] = north;
  OLC_ROOM(&redit_d)->dir_option[SOUTH] = south;
  STATE(&redit_d) = CON_REDIT;
  descriptor_list = &redit_d;

  olc_renumber_sessions(OLC_RNUM_ROOM, 2, +1, NULL);

  TEST_ASSERT_EQUAL_INT(NOWHERE, north->to_room);
  TEST_ASSERT_EQUAL_INT(5, south->to_room);

  descriptor_list = NULL;
  free(north); free(south);
  free(OLC_ROOM(&redit_d));
  free(redit_d.olc);
}

/* ------------------------------------------------------------------ */
/* F1-04: delete acts on the edited vnum, never the 'W' copy source.   */
/* ------------------------------------------------------------------ */
static void oedit_copy_from(struct descriptor_data *d, const char *vnum)
{
  struct obj_data *before = OLC_OBJ(d);
  oedit_parse(d, "w");
  oedit_parse(d, (char *)vnum);
  if (OLC_OBJ(d) != before)
    free_scratch_obj(before);   /* the 'W' re-setup leak is F1-10 */
}

static void test_oedit_delete_after_copy_deletes_edited_object_not_source(void)
{
  struct descriptor_data *d = &builder_desc;

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_MAIN_MENU;

  oedit_copy_from(d, "3000");
  TEST_ASSERT_EQUAL_INT(1, GET_OBJ_RNUM(OLC_OBJ(d)));

  oedit_parse(d, "x");
  oedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(1, delete_calls);
  TEST_ASSERT_EQUAL_INT(1, deleted_rnum);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void test_oedit_delete_refused_without_zone_permission(void)
{
  struct descriptor_data *d = &builder_desc;

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_DELETE;
  zone_editable = FALSE;

  oedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(0, delete_calls);
  TEST_ASSERT_EQUAL_STRING("delete-permission", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 3001"));
}

static void test_oedit_delete_refused_when_scratch_rnum_disagrees(void)
{
  struct descriptor_data *d = &builder_desc;

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  GET_OBJ_RNUM(OLC_OBJ(d)) = 2;   /* a renumber this session missed */
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_DELETE;

  oedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(0, delete_calls);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("delete-target-mismatch", tb_captured_operation);
}

/* ------------------------------------------------------------------ */
/* F1-05: an object insert renumbers other oedit sessions too.         */
/* ------------------------------------------------------------------ */
static void test_oedit_insert_renumbers_other_oedit_session(void)
{
  struct descriptor_data *d = &builder_desc, other;
  struct char_data ob;
  struct player_special_data sp;

  make_objects(3);
  memset(&other, 0, sizeof(other)); memset(&ob, 0, sizeof(ob)); memset(&sp, 0, sizeof(sp));
  ob.player_specials = &sp; other.character = &ob;
  CREATE(other.olc, struct oasis_olc_data, 1);
  OLC_OBJ(&other) = scratch_obj("object 3001");
  OLC_OBJ(&other)->item_number = 1;
  OLC_NUM(&other) = 3001;
  STATE(&other) = CON_OEDIT;
  descriptor_list = &other;

  /* This builder saves new object 2999, which insert_object places at 0. */
  OLC_NUM(d) = 2999;
  OLC_OBJ(d) = scratch_obj("new object");
  STATE(d) = CON_OEDIT;
  add_object_result = 0;

  oedit_save_internally(d);

  TEST_ASSERT_EQUAL_INT(2, GET_OBJ_RNUM(OLC_OBJ(&other)));

  descriptor_list = NULL;
  free_scratch_obj(OLC_OBJ(&other));
  free(other.olc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_iedit_save_after_target_extracted_reports_and_does_not_touch_it);
  RUN_TEST(test_iedit_save_rejects_new_object_at_reused_address);
  RUN_TEST(test_iedit_save_live_target_copies_and_logs_from_scratch);
  RUN_TEST(test_redit_copy_does_not_alias_source_contents);
  RUN_TEST(test_redit_save_refuses_scratch_holding_live_contents);
  RUN_TEST(test_zedit_menu_reports_stale_object_rnum_instead_of_reading_past_table);
  RUN_TEST(test_object_delete_renumbers_other_builders_sessions);
  RUN_TEST(test_room_insert_leaves_nowhere_exits_alone);
  RUN_TEST(test_oedit_delete_after_copy_deletes_edited_object_not_source);
  RUN_TEST(test_oedit_delete_refused_without_zone_permission);
  RUN_TEST(test_oedit_delete_refused_when_scratch_rnum_disagrees);
  RUN_TEST(test_oedit_insert_renumbers_other_oedit_session);
  return UNITY_END();
}
