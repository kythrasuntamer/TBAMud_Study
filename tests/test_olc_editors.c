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
/* handler.c moves, reduced to the accounting they own: carry weight   */
/* for inventory, the sum of affected[] modifiers for equipment.       */
/* ------------------------------------------------------------------ */
static int worn_modifier_total;
static int obj_modifier_sum(struct obj_data *obj)
{
  int i, sum = 0;
  for (i = 0; i < MAX_OBJ_AFFECT; i++)
    sum += obj->affected[i].modifier;
  return sum;
}
void obj_to_char(struct obj_data *object, struct char_data *ch)
{
  object->carried_by = ch;
  IS_CARRYING_W(ch) += GET_OBJ_WEIGHT(object);
}
void obj_from_char(struct obj_data *object)
{
  IS_CARRYING_W(object->carried_by) -= GET_OBJ_WEIGHT(object);
  object->carried_by = NULL;
}
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to)
{
  obj->in_obj = obj_to;
  GET_OBJ_WEIGHT(obj_to) += GET_OBJ_WEIGHT(obj);
}
void obj_from_obj(struct obj_data *obj)
{
  GET_OBJ_WEIGHT(obj->in_obj) -= GET_OBJ_WEIGHT(obj);
  obj->in_obj = NULL;
}
void equip_char_at(const char *src_file, int src_line, const char *src_func, struct char_data *ch, struct obj_data *obj, int pos)
{ (void)src_file; (void)src_line; (void)src_func; 
  obj->worn_by = ch;
  obj->worn_on = pos;
  worn_modifier_total += obj_modifier_sum(obj);
}
struct obj_data *unequip_char(struct char_data *ch, int pos)
{
  struct obj_data *obj = OLC_IEDIT_OBJ(ch->desc);
  (void)pos;
  worn_modifier_total -= obj_modifier_sum(obj);
  obj->worn_by = NULL;
  return obj;
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

/* genobj.c's free_object_strings(), counted so tests can see a release. */
static int free_object_strings_calls;
void free_object_strings(struct obj_data *obj)
{
  free_object_strings_calls++;
  free(obj->name);
  free(obj->description);
  free(obj->short_description);
  free(obj->action_description);
  obj->name = obj->description = obj->short_description = obj->action_description = NULL;
}

void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  (void)cleanup_type;
  cleanups++;
  if (!d->olc)
    return;
  olc_free_script_copy(d);   /* as oasis.c does */
  if (OLC_ZONE(d)) {
    int i;
    for (i = 0; OLC_ZONE(d)->cmd && OLC_ZONE(d)->cmd[i].command != 'S'; i++) {
      free(OLC_ZONE(d)->cmd[i].sarg1);
      free(OLC_ZONE(d)->cmd[i].sarg2);
    }
    free(OLC_ZONE(d)->cmd);
    free(OLC_ZONE(d));
  }
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
int count_non_protocol_chars(char *str)   /* "\tX" is a colour code */
{
  int n = 0;
  while (*str) {
    if (*str == '\t' && str[1]) { str += 2; continue; }
    n++; str++;
  }
  return n;
}
int is_number(const char *str)   /* interpreter.c */
{
  if (*str == '-')
    str++;
  if (!*str)
    return 0;
  while (*str)
    if (!isdigit((unsigned char)*(str++)))
      return 0;
  return 1;
}
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

/* Script stubs that keep dg_handler.c's ownership contract: extract_script()
 * detaches SCRIPT(thing), free_proto_script() frees thing's proto list. The
 * calls are recorded so tests can check which owner was released. */
static int extract_script_calls, free_proto_calls;
static void *last_extracted_thing;
static struct trig_proto_list **proto_slot(void *thing, int type)
{
  switch (type) {
  case MOB_TRIGGER: return &((struct char_data *)thing)->proto_script;
  case OBJ_TRIGGER: return &((struct obj_data *)thing)->proto_script;
  default:          return &((struct room_data *)thing)->proto_script;
  }
}
void extract_script(void *thing, int type)
{
  extract_script_calls++;
  last_extracted_thing = thing;
  if (type == OBJ_TRIGGER)
    SCRIPT((struct obj_data *)thing) = NULL;
  else if (type == MOB_TRIGGER)
    SCRIPT((struct char_data *)thing) = NULL;
  else
    SCRIPT((struct room_data *)thing) = NULL;
}
static void free_trig_list(struct trig_proto_list *tp)
{
  while (tp) {
    struct trig_proto_list *next = tp->next;
    free(tp);
    tp = next;
  }
}
void free_proto_script(void *thing, int type)
{
  struct trig_proto_list **slot = proto_slot(thing, type);
  free_proto_calls++;
  free_trig_list(*slot);
  *slot = NULL;
}
static struct trig_proto_list *make_trig_list(int n)
{
  struct trig_proto_list *head = NULL, *tp;
  while (n-- > 0) {
    CREATE(tp, struct trig_proto_list, 1);
    tp->vnum = 3100 + n;
    tp->next = head;
    head = tp;
  }
  return head;
}
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
mob_rnum real_mobile(mob_vnum vnum)
{
  mob_rnum i;
  for (i = 0; mob_index && i <= top_of_mobt; i++)
    if (mob_index[i].vnum == vnum)
      return i;
  return NOBODY;
}
/* genmob's update branch: copy_mobile() is a struct copy that keeps nr. */
mob_rnum add_mobile(struct char_data *mob, mob_vnum vnum)
{
  mob_rnum r = real_mobile(vnum);
  if (r != NOBODY)
    mob_proto[r] = *mob;
  return r;
}
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

int delete_zone_command(struct zone_data *zone, int pos)
{
  int count = count_commands(zone->cmd), i, l;
  struct reset_com *newlist;
  if (pos < 0 || pos >= count)
    return 0;
  newlist = calloc((size_t)count, sizeof(*newlist));
  TEST_ASSERT_NOT_NULL(newlist);
  for (i = 0, l = 0; i < count; i++)
    if (i != pos)
      newlist[l++] = zone->cmd[i];
  newlist[count - 1].command = 'S';
  free(zone->cmd);
  zone->cmd = newlist;
  return 1;
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
  obj_rnum r;
  struct obj_data *obj;
  (void)newobj;
  if (add_object_result >= 0)
    return (obj_rnum)add_object_result;
  /* An update in place: genobj's update_all_objects() struct-copies the
   * prototype over each live instance, so script and proto_script become
   * the prototype's (NULL). */
  if ((r = real_object(ovnum)) == NOTHING)
    return r;
  /* copy_object(&obj_proto[r], newobj) is a struct copy as well. */
  obj_proto[r].proto_script = newobj->proto_script;
  for (obj = object_list; obj; obj = obj->next)
      if (obj->item_number == r) {
        obj->script = NULL;
        obj->proto_script = NULL;
      }
  return r;
}

static int deleted_rnum = -1, delete_calls;
int delete_object(obj_rnum rnum) { delete_calls++; deleted_rnum = rnum; return rnum; }

/* Shop tables and genshp.c, reduced to what sedit_parse links against. */
struct shop_data *shop_index = NULL;
int top_shop = -1;
const char *trade_letters[] = { "\n" };
const char *shop_bits[] = { "\n" };
SPECIAL(shop_keeper) { (void)ch; (void)me; (void)cmd; (void)argument; return FALSE; }
shop_rnum real_shop(shop_vnum vnum)
{
  int i;
  for (i = 0; i <= top_shop; i++)
    if (shop_index[i].vnum == vnum)
      return i;
  return NOWHERE;
}
int add_shop(struct shop_data *shop) { (void)shop; return TRUE; }
int save_shops(zone_rnum zone_num) { (void)zone_num; return TRUE; }
void copy_shop(struct shop_data *tshop, struct shop_data *fshop, int free_old_strings)
{ (void)free_old_strings; *tshop = *fshop; }
void free_shop(struct shop_data *shop) { free(shop); }
void modify_shop_string(char **str, char *newstr) { free(*str); *str = strdup(newstr); }
static long last_int_list_add = -2;
void add_shop_to_int_list(IDXTYPE **tlist, IDXTYPE newi) { (void)tlist; last_int_list_add = (long)newi; }
void remove_shop_from_int_list(IDXTYPE **list, IDXTYPE num) { (void)list; (void)num; }
void add_shop_to_type_list(struct shop_buy_data **list, struct shop_buy_data *newl) { (void)list; (void)newl; }
void remove_shop_from_type_list(struct shop_buy_data **list, int num) { (void)list; (void)num; }

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
  extract_script_calls = free_proto_calls = 0;
  free_object_strings_calls = 0;
  last_extracted_thing = NULL;
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
    int i;
    for (i = 0; OLC_ZONE(d)->cmd[i].command != 'S'; i++) {
      free(OLC_ZONE(d)->cmd[i].sarg1);   /* the scratch list owns them */
      free(OLC_ZONE(d)->cmd[i].sarg2);
    }
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
  oedit_parse(d, "w");
  oedit_parse(d, (char *)vnum);
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

/* ------------------------------------------------------------------ */
/* F1-13: a new room renumbers other sessions without wrapping NOWHERE */
/* exits or treating a door direction as a room.                       */
/* ------------------------------------------------------------------ */
static void test_redit_insert_keeps_nowhere_exits_and_door_directions(void)
{
  struct descriptor_data *d = &builder_desc, zd, rd;
  struct char_data zb, rb;
  struct player_special_data sp;
  struct reset_com door;
  struct room_direction_data *pending;

  make_world(3);
  memset(&sp, 0, sizeof(sp));
  memset(&door, 0, sizeof(door));
  door.command = 'D'; door.arg1 = 1; door.arg2 = 5; door.arg3 = 1;  /* down */

  memset(&zd, 0, sizeof(zd)); memset(&zb, 0, sizeof(zb));
  zb.player_specials = &sp; zd.character = &zb;
  CREATE(zd.olc, struct oasis_olc_data, 1);
  make_zedit_session(&zd, &door, 1);

  memset(&rd, 0, sizeof(rd)); memset(&rb, 0, sizeof(rb));
  rb.player_specials = &sp; rd.character = &rb;
  CREATE(rd.olc, struct oasis_olc_data, 1);
  CREATE(OLC_ROOM(&rd), struct room_data, 1);
  CREATE(pending, struct room_direction_data, 1);
  pending->to_room = NOWHERE;
  OLC_ROOM(&rd)->dir_option[EAST] = pending;
  STATE(&rd) = CON_REDIT;

  descriptor_list = &zd;
  zd.next = &rd;

  /* New room 2999 lands at rnum 0. */
  CREATE(OLC_ROOM(d), struct room_data, 1);
  OLC_ROOM(d)->name = strdup("A new room");
  OLC_ROOM(d)->description = strdup("New.\r\n");
  OLC_ROOM(d)->number = NOWHERE;
  OLC_NUM(d) = 2999;
  add_room_result = 0;
  redit_save_internally(d);

  TEST_ASSERT_EQUAL_INT(1, add_room_calls);
  TEST_ASSERT_EQUAL_INT(NOWHERE, pending->to_room);
  TEST_ASSERT_EQUAL_INT(2, OLC_ZONE(&zd)->cmd[0].arg1);
  TEST_ASSERT_EQUAL_INT(5, OLC_ZONE(&zd)->cmd[0].arg2);

  descriptor_list = NULL;
  free_zedit_session(&zd);
  free(zd.olc);
  free(pending);
  free(OLC_ROOM(&rd));
  free(rd.olc);
}


/* ------------------------------------------------------------------ */
/* F1-09: the session's trigger list copy has exactly one owner.        */
/* ------------------------------------------------------------------ */
static void test_oedit_save_hands_script_copy_to_prototype(void)
{
  struct descriptor_data *d = &builder_desc;
  struct trig_proto_list *list = make_trig_list(2);

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;
  OLC_SCRIPT(d) = list;

  oedit_save_internally(d);

  /* The prototype owns the list; cleanup_olc() must not free it again. */
  TEST_ASSERT_EQUAL_PTR(list, obj_proto[1].proto_script);
  TEST_ASSERT_NULL(OLC_SCRIPT(d));
  free_trig_list(obj_proto[1].proto_script);
  obj_proto[1].proto_script = NULL;
}

static void test_oedit_quit_without_changes_frees_script_copy(void)
{
  struct descriptor_data *d = &builder_desc;

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_MAIN_MENU;
  OLC_SCRIPT(d) = make_trig_list(2);
  OLC_VAL(d) = 0;

  /* LeakSanitizer and the valgrind run fail on the old leak. */
  oedit_parse(d, "q");

  TEST_ASSERT_NULL(d->olc);
  TEST_ASSERT_NULL(obj_proto[1].proto_script);
}

static void test_olc_free_script_copy_clears_session_handle(void)
{
  struct descriptor_data *d = &builder_desc;

  OLC_SCRIPT(d) = make_trig_list(3);
  olc_free_script_copy(d);
  TEST_ASSERT_NULL(OLC_SCRIPT(d));
  olc_free_script_copy(d);   /* idempotent */
  TEST_ASSERT_NULL(OLC_SCRIPT(d));
}


/* ------------------------------------------------------------------ */
/* F1-10: 'W' releases the scratch copy it replaces.                   */
/* ------------------------------------------------------------------ */
static void test_oedit_copy_twice_releases_each_replaced_scratch(void)
{
  struct descriptor_data *d = &builder_desc;

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_MAIN_MENU;
  OLC_SCRIPT(d) = make_trig_list(1);

  oedit_copy_from(d, "3000");
  oedit_copy_from(d, "3002");

  /* Each replaced scratch object was released (LeakSanitizer checks the
   * trigger lists), and the session still edits 3001. */
  TEST_ASSERT_EQUAL_INT(2, free_object_strings_calls);
  TEST_ASSERT_EQUAL_INT(1, GET_OBJ_RNUM(OLC_OBJ(d)));
}

static void test_oedit_copy_from_missing_source_keeps_scratch(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *before;

  make_objects(3);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  before = OLC_OBJ(d);
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_MAIN_MENU;

  oedit_copy_from(d, "4999");

  TEST_ASSERT_EQUAL_PTR(before, OLC_OBJ(d));
  TEST_ASSERT_EQUAL_INT(0, free_object_strings_calls);
}

static void test_redit_copy_twice_releases_each_replaced_room(void)
{
  struct descriptor_data *d = &builder_desc;

  make_world(3);
  OLC_NUM(d) = 3001;
  redit_setup_existing(d, 1);
  STATE(d) = CON_REDIT;
  OLC_MODE(d) = REDIT_MAIN_MENU;

  redit_parse(d, "w");
  redit_parse(d, "3000");
  redit_parse(d, "w");
  redit_parse(d, "3002");

  /* LeakSanitizer fails the run if a replaced room leaked. */
  TEST_ASSERT_EQUAL_INT(3001, OLC_ROOM(d)->number);
}


/* ------------------------------------------------------------------ */
/* F1-07: saving a prototype releases each live instance's script      */
/* before add_object() overwrites it.                                  */
/* ------------------------------------------------------------------ */
static void test_oedit_save_releases_live_instance_scripts_first(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data instance, other;
  struct script_data fake_script;

  make_objects(3);
  memset(&instance, 0, sizeof(instance));
  memset(&other, 0, sizeof(other));
  memset(&fake_script, 0, sizeof(fake_script));
  instance.item_number = 1;
  instance.script = &fake_script;
  instance.proto_script = make_trig_list(2);
  other.item_number = 2;
  instance.next = &other;
  object_list = &instance;

  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;

  oedit_save_internally(d);

  TEST_ASSERT_EQUAL_INT(1, extract_script_calls);
  TEST_ASSERT_EQUAL_PTR(&instance, last_extracted_thing);
  TEST_ASSERT_NULL(instance.proto_script);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  object_list = NULL;
}


/* ------------------------------------------------------------------ */
/* F1-08: saving frees the prototype's previous trigger list.          */
/* ------------------------------------------------------------------ */
static void test_oedit_save_frees_previous_prototype_trigger_list(void)
{
  struct descriptor_data *d = &builder_desc;
  struct trig_proto_list *edited = make_trig_list(1);

  make_objects(3);
  obj_proto[1].proto_script = make_trig_list(2);
  OLC_NUM(d) = 3001;
  oedit_setup_existing(d, 1);
  STATE(d) = CON_OEDIT;
  OLC_SCRIPT(d) = edited;

  oedit_save_internally(d);

  /* The old two-node list is freed (LeakSanitizer checks), and the
   * prototype now holds the session's edited list. */
  TEST_ASSERT_EQUAL_INT(1, free_proto_calls);
  TEST_ASSERT_EQUAL_PTR(edited, obj_proto[1].proto_script);
  free_trig_list(obj_proto[1].proto_script);
  obj_proto[1].proto_script = NULL;
}


/* ------------------------------------------------------------------ */
/* F1-11: a saved mobile prototype always names its own rnum.          */
/* ------------------------------------------------------------------ */
static void test_medit_save_repairs_and_reports_wrong_scratch_nr(void)
{
  struct descriptor_data *d = &builder_desc;
  struct char_data *scratch;
  int i;

  mob_index = calloc(3, sizeof(*mob_index));
  mob_proto = calloc(3, sizeof(*mob_proto));
  TEST_ASSERT_NOT_NULL(mob_index);
  TEST_ASSERT_NOT_NULL(mob_proto);
  for (i = 0; i < 3; i++) {
    mob_index[i].vnum = 3000 + i;
    mob_proto[i].nr = i;
  }
  top_of_mobt = 2;

  CREATE(scratch, struct char_data, 1);
  scratch->nr = 2;            /* the 'W' source, mob 3002 */
  OLC_MOB(d) = scratch;
  OLC_NUM(d) = 3001;
  STATE(d) = CON_MEDIT;

  medit_save_internally(d);

  TEST_ASSERT_EQUAL_INT(1, mob_proto[1].nr);
  TEST_ASSERT_EQUAL_STRING("mob-proto-nr", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3001 scratch nr 2 != rnum 1"));

  free(scratch);
  OLC_MOB(d) = NULL;
  free(mob_index); mob_index = NULL;
  free(mob_proto); mob_proto = NULL;
  top_of_mobt = 0;
}


/* ------------------------------------------------------------------ */
/* F1-14: a room saved to a new vnum renumbers other sessions even     */
/* when its scratch copy still carries the 'W' source's number.        */
/* ------------------------------------------------------------------ */
static void test_redit_copy_saved_to_new_vnum_renumbers_other_sessions(void)
{
  struct descriptor_data *d = &builder_desc, zd;
  struct char_data zb;
  struct player_special_data sp;
  struct reset_com load;

  make_world(3);
  memset(&sp, 0, sizeof(sp));
  memset(&load, 0, sizeof(load));
  load.command = 'M'; load.arg1 = 0; load.arg3 = 2;   /* mob into rnum 2 */

  memset(&zd, 0, sizeof(zd)); memset(&zb, 0, sizeof(zb));
  zb.player_specials = &sp; zd.character = &zb;
  CREATE(zd.olc, struct oasis_olc_data, 1);
  make_zedit_session(&zd, &load, 1);
  descriptor_list = &zd;

  /* New room 3070 copied from 3001: the scratch still says 3001. */
  CREATE(OLC_ROOM(d), struct room_data, 1);
  OLC_ROOM(d)->name = strdup("A copied room");
  OLC_ROOM(d)->description = strdup("Copied.\r\n");
  OLC_ROOM(d)->number = 3001;
  OLC_NUM(d) = 2999;
  add_room_result = 0;
  redit_save_internally(d);

  TEST_ASSERT_EQUAL_INT(3, OLC_ZONE(&zd)->cmd[0].arg3);
  TEST_ASSERT_EQUAL_INT(2999, OLC_ROOM(d)->number);

  descriptor_list = NULL;
  free_zedit_session(&zd);
  free(zd.olc);
}


/* ------------------------------------------------------------------ */
/* F1-15: aborting a command edit after deletes stays inside the list. */
/* ------------------------------------------------------------------ */
static void test_zedit_abort_after_deletes_stays_in_bounds(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmds[5];
  int i;

  make_zone();
  memset(cmds, 0, sizeof(cmds));
  for (i = 0; i < 5; i++)
    cmds[i].command = '*';
  make_zedit_session(d, cmds, 5);
  OLC_VAL(d) = 4;                  /* the last command edited */

  for (i = 0; i < 4; i++) {
    OLC_MODE(d) = ZEDIT_DELETE_ENTRY;
    zedit_parse(d, "0");
  }
  OLC_MODE(d) = ZEDIT_CHANGE_ENTRY;
  zedit_parse(d, "a");             /* ASan: cmd[4] of a 2-slot list before */

  TEST_ASSERT_EQUAL_INT(0, OLC_VAL(d));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free_zedit_session(d);
}

static void test_zedit_abort_with_stale_index_is_reported(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = '*';
  make_zedit_session(d, &cmd, 1);
  OLC_VAL(d) = 7;
  OLC_MODE(d) = ZEDIT_CHANGE_ENTRY;

  zedit_parse(d, "A");

  TEST_ASSERT_EQUAL_STRING("zedit-stale-index", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3001 command index 7 outside list of 1"));
  TEST_ASSERT_EQUAL_INT(0, OLC_VAL(d));
  free_zedit_session(d);
}


/* ------------------------------------------------------------------ */
/* F1-19: a door direction must name one of the game's exits.          */
/* ------------------------------------------------------------------ */
static void test_zedit_door_direction_rejects_dir_count_and_disabled_diagonals(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = 'D';
  cmd.arg2 = 1;
  make_zedit_session(d, &cmd, 1);
  OLC_VAL(d) = 0;

  CONFIG_DIAGONAL_DIRS = TRUE;
  OLC_MODE(d) = ZEDIT_ARG2;
  zedit_parse(d, "10");            /* dirs[10] is the "\n" sentinel */
  TEST_ASSERT_EQUAL_INT(1, OLC_ZONE(d)->cmd[0].arg2);
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "Direction must be 0 to 9"));

  CONFIG_DIAGONAL_DIRS = FALSE;
  out_buf[0] = '\0';
  zedit_parse(d, "6");             /* northwest, but diagonals are off */
  TEST_ASSERT_EQUAL_INT(1, OLC_ZONE(d)->cmd[0].arg2);
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "Direction must be 0 to 5"));

  zedit_parse(d, "5");
  TEST_ASSERT_EQUAL_INT(5, OLC_ZONE(d)->cmd[0].arg2);
  free_zedit_session(d);
}


/* ------------------------------------------------------------------ */
/* F1-20: new commands are fully initialised; 'V' strings are owned    */
/* by the scratch list and validated for the zone file format.         */
/* ------------------------------------------------------------------ */
static void test_zedit_new_command_has_defined_fields(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = '*';
  make_zedit_session(d, &cmd, 1);
  OLC_MODE(d) = ZEDIT_NEW_ENTRY;

  zedit_parse(d, "1");             /* new_command() fills it with 0x5a */

  TEST_ASSERT_EQUAL_INT('N', OLC_ZONE(d)->cmd[1].command);
  TEST_ASSERT_EQUAL_INT(0, OLC_ZONE(d)->cmd[1].if_flag);
  TEST_ASSERT_EQUAL_INT(-1, OLC_ZONE(d)->cmd[1].arg1);
  TEST_ASSERT_EQUAL_INT(-1, OLC_ZONE(d)->cmd[1].arg3);
  TEST_ASSERT_EQUAL_INT(0, OLC_ZONE(d)->cmd[1].line);
  TEST_ASSERT_NULL(OLC_ZONE(d)->cmd[1].sarg1);
  free_zedit_session(d);
}

static struct descriptor_data *zedit_v_session(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = 'V';
  cmd.sarg1 = strdup("oldname");
  cmd.sarg2 = strdup("old value");
  make_zedit_session(d, &cmd, 1);
  OLC_VAL(d) = 0;
  return d;
}

static void test_zedit_global_name_with_space_is_refused_and_reported(void)
{
  struct descriptor_data *d = zedit_v_session();

  OLC_MODE(d) = ZEDIT_SARG1;
  zedit_parse(d, "my var");

  TEST_ASSERT_EQUAL_STRING("oldname", OLC_ZONE(d)->cmd[0].sarg1);
  TEST_ASSERT_EQUAL_INT(ZEDIT_SARG1, OLC_MODE(d));
  TEST_ASSERT_EQUAL_STRING("zedit-sarg", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3001 global name len 6 refused: contains whitespace"));
  free_zedit_session(d);
}

static void test_zedit_reediting_global_frees_previous_strings(void)
{
  struct descriptor_data *d = zedit_v_session();

  /* LeakSanitizer fails the run if the old strings are dropped. */
  OLC_MODE(d) = ZEDIT_SARG1;
  zedit_parse(d, "newname");
  zedit_parse(d, "new value");

  TEST_ASSERT_EQUAL_STRING("newname", OLC_ZONE(d)->cmd[0].sarg1);
  TEST_ASSERT_EQUAL_STRING("new value", OLC_ZONE(d)->cmd[0].sarg2);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free_zedit_session(d);
}

static void test_zedit_deleting_global_frees_its_strings(void)
{
  struct descriptor_data *d = zedit_v_session();

  OLC_MODE(d) = ZEDIT_DELETE_ENTRY;
  zedit_parse(d, "0");             /* LeakSanitizer checks the strings */

  TEST_ASSERT_EQUAL_INT('S', OLC_ZONE(d)->cmd[0].command);
  free_zedit_session(d);
}


/* ------------------------------------------------------------------ */
/* F1-16: menus never index a name table with a world-file value       */
/* outside the table.                                                  */
/* ------------------------------------------------------------------ */
static void test_zedit_menu_reports_out_of_range_wear_and_door_values(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmds[2];

  make_objects(3);
  make_zone();
  memset(cmds, 0, sizeof(cmds));
  cmds[0].command = 'E'; cmds[0].arg1 = 0; cmds[0].arg3 = 40;   /* 'E 1 3000 1 40' */
  cmds[1].command = 'D'; cmds[1].arg1 = 0; cmds[1].arg2 = 42;   /* 'D 0 3001 42 1' */
  make_zedit_session(d, cmds, 2);
  OLC_MODE(d) = ZEDIT_MAIN_MENU;

  zedit_parse(d, "?");             /* redisplay the menu */

  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("table-index", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "editing vnum 3001 holds dirs index 42 outside [0, 10)"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "Set door <invalid>"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "<invalid>, Max"));
  free_zedit_session(d);
}


/* ------------------------------------------------------------------ */
/* F1-17: iedit re-inserts a carried, contained or worn object so the  */
/* handler re-accounts its weight and affects.                         */
/* ------------------------------------------------------------------ */
static struct obj_data *iedit_live_session(const char *sdesc)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *target = scratch_obj(sdesc);

  OLC_OBJ(d) = scratch_obj(sdesc);
  OLC_IEDIT_OBJ(d) = target;
  OLC_IEDIT_UID(d) = obj_script_id(target);
  OLC_NUM(d) = 3005;
  OLC_MODE(d) = OEDIT_CONFIRM_SAVESTRING;
  live_uid = target->script_id;
  live_uid_obj = target;
  return target;
}

static void test_iedit_weight_change_on_carried_object_keeps_carry_weight(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *target = iedit_live_session("a rock");

  GET_OBJ_WEIGHT(target) = 1;
  obj_to_char(target, &builder);
  GET_OBJ_WEIGHT(OLC_OBJ(d)) = 500;

  iedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(500, IS_CARRYING_W(&builder));
  TEST_ASSERT_EQUAL_PTR(&builder, target->carried_by);
  free_scratch_obj(target);
}

static void test_iedit_weight_change_inside_container_updates_container(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *target = iedit_live_session("a coin");
  struct obj_data bag;

  memset(&bag, 0, sizeof(bag));
  GET_OBJ_WEIGHT(&bag) = 3;
  GET_OBJ_WEIGHT(target) = 2;
  obj_to_obj(target, &bag);          /* bag now weighs 5 */
  GET_OBJ_WEIGHT(OLC_OBJ(d)) = 10;

  iedit_parse(d, "y");

  TEST_ASSERT_EQUAL_INT(13, GET_OBJ_WEIGHT(&bag));
  TEST_ASSERT_EQUAL_PTR(&bag, target->in_obj);
  free_scratch_obj(target);
}

static void test_iedit_affect_change_on_worn_object_is_reapplied(void)
{
  struct descriptor_data *d = &builder_desc;
  struct obj_data *target = iedit_live_session("a ring");

  worn_modifier_total = 0;
  target->affected[0].location = APPLY_HIT;
  target->affected[0].modifier = 10;
  equip_char(&builder, target, WEAR_FINGER_R);   /* +10 accounted */
  OLC_OBJ(d)->affected[0].location = APPLY_HIT;
  OLC_OBJ(d)->affected[0].modifier = 50;

  iedit_parse(d, "y");

  /* -10 on the way out, +50 on the way back in: removing it later
   * subtracts exactly what is applied. */
  TEST_ASSERT_EQUAL_INT(50, worn_modifier_total);
  TEST_ASSERT_EQUAL_PTR(&builder, target->worn_by);
  TEST_ASSERT_EQUAL_INT(WEAR_FINGER_R, target->worn_on);
  free_scratch_obj(target);
}


/* ------------------------------------------------------------------ */
/* F1-18: choosing a keeper changes no live spec proc until save, and  */
/* needs the keeper's zone to be editable.                             */
/* ------------------------------------------------------------------ */
static int guild_proc_calls;
static SPECIAL(test_guild_proc) { (void)ch; (void)me; (void)cmd; (void)argument; guild_proc_calls++; return FALSE; }

static void make_keeper_mobs(void)
{
  int i;
  mob_index = calloc(3, sizeof(*mob_index));
  mob_proto = calloc(3, sizeof(*mob_proto));
  TEST_ASSERT_NOT_NULL(mob_index);
  TEST_ASSERT_NOT_NULL(mob_proto);
  for (i = 0; i < 3; i++) {
    mob_index[i].vnum = 1200 + i;
    mob_proto[i].nr = i;
  }
  top_of_mobt = 2;
  mob_index[1].func = test_guild_proc;   /* a guildmaster, mob 1201 */
}

static void free_keeper_mobs(void)
{
  free(mob_index); mob_index = NULL;
  free(mob_proto); mob_proto = NULL;
  top_of_mobt = 0;
}

static void test_sedit_keeper_choice_leaves_spec_proc_until_save(void)
{
  struct descriptor_data *d = &builder_desc;
  struct shop_data scratch;

  make_keeper_mobs();
  memset(&scratch, 0, sizeof(scratch));
  S_KEEPER(&scratch) = NOBODY;
  OLC_SHOP(d) = &scratch;
  OLC_NUM(d) = 3000;
  OLC_MODE(d) = SEDIT_KEEPER;

  sedit_parse(d, "1201");

  TEST_ASSERT_EQUAL_INT(1, S_KEEPER(&scratch));
  TEST_ASSERT_EQUAL_PTR(test_guild_proc, mob_index[1].func);

  sedit_save_internally(d);

  TEST_ASSERT_EQUAL_PTR(shop_keeper, mob_index[1].func);
  TEST_ASSERT_EQUAL_PTR(test_guild_proc, S_FUNC(&scratch));
  TEST_ASSERT_EQUAL_STRING("shop-keeper-func", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop vnum 3000 keeper vnum 1201"));

  OLC_SHOP(d) = NULL;
  free_keeper_mobs();
}

static void test_sedit_keeper_from_foreign_zone_is_refused(void)
{
  struct descriptor_data *d = &builder_desc;
  struct shop_data scratch;

  make_keeper_mobs();
  memset(&scratch, 0, sizeof(scratch));
  S_KEEPER(&scratch) = NOBODY;
  OLC_SHOP(d) = &scratch;
  OLC_NUM(d) = 3000;
  OLC_MODE(d) = SEDIT_KEEPER;
  zone_editable = FALSE;

  sedit_parse(d, "1201");

  TEST_ASSERT_EQUAL_INT(NOBODY, S_KEEPER(&scratch));
  TEST_ASSERT_EQUAL_INT(SEDIT_KEEPER, OLC_MODE(d));
  TEST_ASSERT_EQUAL_STRING("shop-keeper-permission", tb_captured_operation);

  OLC_SHOP(d) = NULL;
  free_keeper_mobs();
}


/* ------------------------------------------------------------------ */
/* F1-22: a failed save is reported and never announced as saved.      */
/* ------------------------------------------------------------------ */
static void test_oedit_failed_save_is_reported_not_announced(void)
{
  struct descriptor_data *d = &builder_desc;

  make_objects(3);
  OLC_NUM(d) = 4999;              /* add_object() stub returns NOTHING */
  OLC_OBJ(d) = scratch_obj("a new thing");
  STATE(d) = CON_OEDIT;
  OLC_MODE(d) = OEDIT_CONFIRM_SAVESTRING;

  oedit_parse(d, "y");

  TEST_ASSERT_EQUAL_STRING("oedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 4999"));
  TEST_ASSERT_EQUAL_INT(0, mudlogs);
  TEST_ASSERT_NULL(strstr(out_buf, "saved to"));
  TEST_ASSERT_NULL(d->olc);
}

static void test_zedit_save_for_vanished_room_is_reported_not_announced(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_world(3);
  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = '*';
  make_zedit_session(d, &cmd, 1);
  OLC_NUM(d) = 4000;              /* deleted while this builder edited */
  OLC_MODE(d) = ZEDIT_CONFIRM_SAVESTRING;

  zedit_parse(d, "y");

  TEST_ASSERT_EQUAL_STRING("zedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 4000"));
  TEST_ASSERT_EQUAL_INT(0, mudlogs);
  TEST_ASSERT_NULL(strstr(out_buf, "Saving zone info"));
  free_zedit_session(d);
}


/* ------------------------------------------------------------------ */
/* F1-23: out-of-range builder input is clamped loudly, and non-numeric */
/* input is refused instead of read as 0.                               */
/* ------------------------------------------------------------------ */
static void test_oedit_weight_over_max_is_clamped_and_reported(void)
{
  struct descriptor_data *d = &builder_desc;
  char input[32];

  OLC_OBJ(d) = scratch_obj("a heavy thing");
  OLC_NUM(d) = 3001;
  OLC_MODE(d) = OEDIT_WEIGHT;
  TEST_ASSERT_TRUE(snprintf(input, sizeof(input), "%d", MAX_OBJ_WEIGHT + 1) > 0);

  oedit_parse(d, input);

  TEST_ASSERT_EQUAL_INT(MAX_OBJ_WEIGHT, GET_OBJ_WEIGHT(OLC_OBJ(d)));
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("olc-clamp", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "vnum 3001 Weight input"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "is out of range"));
}

static void test_oedit_weight_in_range_is_silent(void)
{
  struct descriptor_data *d = &builder_desc;

  OLC_OBJ(d) = scratch_obj("a light thing");
  OLC_NUM(d) = 3001;
  OLC_MODE(d) = OEDIT_WEIGHT;

  oedit_parse(d, "12");

  TEST_ASSERT_EQUAL_INT(12, GET_OBJ_WEIGHT(OLC_OBJ(d)));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void test_sedit_non_numeric_input_is_refused(void)
{
  struct descriptor_data *d = &builder_desc;
  struct shop_data scratch;

  memset(&scratch, 0, sizeof(scratch));
  S_OPEN1(&scratch) = 6;
  OLC_SHOP(d) = &scratch;
  OLC_NUM(d) = 3000;
  OLC_MODE(d) = SEDIT_OPEN1;

  sedit_parse(d, "abc");

  TEST_ASSERT_EQUAL_INT(6, S_OPEN1(&scratch));
  TEST_ASSERT_EQUAL_INT(SEDIT_OPEN1, OLC_MODE(d));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "Field must be numerical"));
  OLC_SHOP(d) = NULL;
}


/* ------------------------------------------------------------------ */
/* F1-24: furniture seats re-prompt; the duplicate-apply check matches */
/* the stored location.                                                */
/* ------------------------------------------------------------------ */
static void test_oedit_furniture_bad_seat_count_reprompts_value_1(void)
{
  struct descriptor_data *d = &builder_desc;

  OLC_OBJ(d) = scratch_obj("a bench");
  GET_OBJ_TYPE(OLC_OBJ(d)) = ITEM_FURNITURE;
  GET_OBJ_VAL(OLC_OBJ(d), 0) = 2;
  OLC_MODE(d) = OEDIT_VALUE_1;

  oedit_parse(d, "99");
  TEST_ASSERT_EQUAL_INT(OEDIT_VALUE_1, OLC_MODE(d));
  TEST_ASSERT_EQUAL_INT(2, GET_OBJ_VAL(OLC_OBJ(d), 0));

  /* Furniture has no value 2, so menu 2 returns straight to the main
   * menu; before the fix it was shown twice. */
  out_buf[0] = '\0';
  oedit_parse(d, "3");
  TEST_ASSERT_EQUAL_INT(OEDIT_MAIN_MENU, OLC_MODE(d));
  TEST_ASSERT_EQUAL_INT(3, GET_OBJ_VAL(OLC_OBJ(d), 0));
  TEST_ASSERT_NULL(strstr(out_buf, "Seats must be"));
}

static void test_oedit_duplicate_apply_is_refused_and_neighbour_allowed(void)
{
  struct descriptor_data *d = &builder_desc;

  OLC_OBJ(d) = scratch_obj("a ring");
  OLC_OBJ(d)->affected[0].location = 3;      /* menu choice 4 */
  OLC_OBJ(d)->affected[0].modifier = 2;
  OLC_VAL(d) = 1;                            /* editing slot 2 */

  OLC_MODE(d) = OEDIT_APPLY;
  oedit_parse(d, "4");                       /* the same apply again */
  TEST_ASSERT_EQUAL_INT(0, OLC_OBJ(d)->affected[1].location);
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "already has that apply in slot 1"));

  OLC_MODE(d) = OEDIT_APPLY;
  oedit_parse(d, "3");                       /* location 2: a different apply */
  TEST_ASSERT_EQUAL_INT(2, OLC_OBJ(d)->affected[1].location);
  TEST_ASSERT_EQUAL_INT(OEDIT_APPLYMOD, OLC_MODE(d));
}


/* ------------------------------------------------------------------ */
/* F1-26: listing shop rooms never rewrites a deleted room; object     */
/* rnum 0 can be a product.                                            */
/* ------------------------------------------------------------------ */
static void test_sedit_room_list_reports_deleted_room_without_rewriting(void)
{
  struct descriptor_data *d = &builder_desc;
  struct shop_data scratch;
  IDXTYPE rooms[3];

  make_world(3);
  memset(&scratch, 0, sizeof(scratch));
  rooms[0] = 3001; rooms[1] = 4999; rooms[2] = NOWHERE;
  S_ROOMS(&scratch) = rooms;
  OLC_SHOP(d) = &scratch;
  OLC_NUM(d) = 3000;
  OLC_MODE(d) = SEDIT_DELETE_ROOM;

  sedit_parse(d, "9");             /* redisplays the room list */

  TEST_ASSERT_EQUAL_INT(4999, rooms[1]);
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "<deleted room>"));
  TEST_ASSERT_EQUAL_STRING("shop-room-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop vnum 3000 room vnum 4999"));
  OLC_SHOP(d) = NULL;
}

static void test_sedit_object_rnum_zero_can_be_a_product(void)
{
  struct descriptor_data *d = &builder_desc;
  struct shop_data scratch;
  IDXTYPE products[1] = { NOTHING };

  make_objects(3);
  memset(&scratch, 0, sizeof(scratch));
  S_PRODUCTS(&scratch) = products;
  OLC_SHOP(d) = &scratch;
  OLC_MODE(d) = SEDIT_NEW_PRODUCT;
  last_int_list_add = -2;

  sedit_parse(d, "3000");          /* obj_proto[0] */

  TEST_ASSERT_EQUAL_INT(0, last_int_list_add);
  OLC_SHOP(d) = NULL;
}


/* ------------------------------------------------------------------ */
/* F1-27: long builder text is stored whole or refused, never cut.     */
/* ------------------------------------------------------------------ */
static void test_redit_name_over_byte_limit_is_refused_not_cut(void)
{
  struct descriptor_data *d = &builder_desc;
  char name[MAX_INPUT_LENGTH];
  int i;

  for (i = 0; i < 60; i++)
    memcpy(name + i * 3, "\tRa", 3);          /* 60 visible, 180 bytes */
  name[180] = '\0';
  CREATE(OLC_ROOM(d), struct room_data, 1);
  OLC_ROOM(d)->name = strdup("Old name");
  OLC_NUM(d) = 3001;
  OLC_MODE(d) = REDIT_NAME;

  redit_parse(d, name);

  TEST_ASSERT_EQUAL_STRING("Old name", OLC_ROOM(d)->name);
  TEST_ASSERT_EQUAL_INT(180, (int)strlen(name));
  TEST_ASSERT_EQUAL_STRING("redit-name-length", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 3001 name of 180 bytes"));
}


/* ------------------------------------------------------------------ */
/* F1-V01: iedit needs edit permission for the object's zone.          */
/* ------------------------------------------------------------------ */
static struct obj_data *found_obj;
char *one_argument(char *argument, char *first_arg)   /* interpreter.c, reduced */
{
  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ')
    *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}
void clear_object(struct obj_data *obj) { memset(obj, 0, sizeof(*obj)); obj->item_number = NOTHING; }
int generic_find(char *arg, bitvector_t bitvector, struct char_data *ch,
                 struct char_data **tar_ch, struct obj_data **tar_obj)
{
  (void)arg; (void)bitvector; (void)ch;
  *tar_ch = NULL;
  *tar_obj = found_obj;
  return found_obj ? 1 : 0;
}

static void test_iedit_refuses_object_from_zone_builder_cannot_edit(void)
{
  struct obj_data *sword;

  make_objects(3);
  sword = scratch_obj("a zone-12 sword");
  sword->item_number = 1;
  found_obj = sword;
  free(builder_desc.olc);          /* a builder in CON_PLAYING */
  builder_desc.olc = NULL;
  STATE(&builder_desc) = CON_PLAYING;
  zone_editable = FALSE;

  do_oasis_iedit(&builder, (char *)" sword", 0, 0);

  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_STRING("iedit-permission", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242 obj uid 100001 vnum 3001"));
  TEST_ASSERT_NOT_NULL(strstr(out_buf, "do not have permission"));
  found_obj = NULL;
  free_scratch_obj(sword);
}


/* ------------------------------------------------------------------ */
/* F1-21: a stale OLC structure found at session start is reported and */
/* released fully.                                                     */
/* ------------------------------------------------------------------ */
static void test_iedit_start_with_stale_session_reports_and_releases_it(void)
{
  struct obj_data *sword;

  make_objects(3);
  sword = scratch_obj("a sword");
  sword->item_number = 1;
  found_obj = sword;
  STATE(&builder_desc) = CON_PLAYING;
  OLC_MODE(&builder_desc) = OEDIT_MAIN_MENU;
  OLC_NUM(&builder_desc) = 3002;
  OLC_OBJ(&builder_desc) = scratch_obj("a stale scratch object");

  do_oasis_iedit(&builder, (char *)" sword", 0, 0);

  TEST_ASSERT_EQUAL_INT(1, cleanups);   /* LeakSanitizer checks the rest */
  TEST_ASSERT_EQUAL_STRING("olc-stale-state", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242 stale olc mode 1 vnum 3002"));
  TEST_ASSERT_NOT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(CON_IEDIT, STATE(&builder_desc));
  found_obj = NULL;
  cleanup_olc(&builder_desc, CLEANUP_ALL);
  free_scratch_obj(sword);
}


/* ------------------------------------------------------------------ */
/* F1-12: an unhandled mode closes the session and returns, instead of */
/* redrawing the menu through the d->olc that cleanup_olc() freed.     */
/* ------------------------------------------------------------------ */
static void test_sedit_unknown_mode_aborts_without_touching_freed_session(void)
{
  struct descriptor_data *d = &builder_desc;
  static struct shop_data scratch;   /* static: the session outlives a failed assert */

  memset(&scratch, 0, sizeof(scratch));
  OLC_SHOP(d) = &scratch;
  OLC_NUM(d) = 3000;
  OLC_MODE(d) = 999;

  sedit_parse(d, "1");             /* old code: NULL d->olc write, SIGSEGV */

  TEST_ASSERT_NULL(d->olc);
  TEST_ASSERT_EQUAL_STRING("sedit-bad-mode", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 4242 olc mode 999 vnum 3000; session closed"));
}

static void test_zedit_unknown_mode_aborts_and_reports(void)
{
  struct descriptor_data *d = &builder_desc;
  struct reset_com cmd;

  make_zone();
  memset(&cmd, 0, sizeof(cmd));
  cmd.command = '*';
  make_zedit_session(d, &cmd, 1);
  OLC_MODE(d) = 999;

  zedit_parse(d, "x");

  TEST_ASSERT_NULL(d->olc);
  TEST_ASSERT_EQUAL_STRING("zedit-bad-mode", tb_captured_operation);
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
  RUN_TEST(test_redit_insert_keeps_nowhere_exits_and_door_directions);
  RUN_TEST(test_oedit_save_hands_script_copy_to_prototype);
  RUN_TEST(test_oedit_quit_without_changes_frees_script_copy);
  RUN_TEST(test_olc_free_script_copy_clears_session_handle);
  RUN_TEST(test_oedit_copy_twice_releases_each_replaced_scratch);
  RUN_TEST(test_oedit_copy_from_missing_source_keeps_scratch);
  RUN_TEST(test_redit_copy_twice_releases_each_replaced_room);
  RUN_TEST(test_oedit_save_releases_live_instance_scripts_first);
  RUN_TEST(test_oedit_save_frees_previous_prototype_trigger_list);
  RUN_TEST(test_medit_save_repairs_and_reports_wrong_scratch_nr);
  RUN_TEST(test_redit_copy_saved_to_new_vnum_renumbers_other_sessions);
  RUN_TEST(test_zedit_abort_after_deletes_stays_in_bounds);
  RUN_TEST(test_zedit_abort_with_stale_index_is_reported);
  RUN_TEST(test_zedit_door_direction_rejects_dir_count_and_disabled_diagonals);
  RUN_TEST(test_zedit_new_command_has_defined_fields);
  RUN_TEST(test_zedit_global_name_with_space_is_refused_and_reported);
  RUN_TEST(test_zedit_reediting_global_frees_previous_strings);
  RUN_TEST(test_zedit_deleting_global_frees_its_strings);
  RUN_TEST(test_zedit_menu_reports_out_of_range_wear_and_door_values);
  RUN_TEST(test_iedit_weight_change_on_carried_object_keeps_carry_weight);
  RUN_TEST(test_iedit_weight_change_inside_container_updates_container);
  RUN_TEST(test_iedit_affect_change_on_worn_object_is_reapplied);
  RUN_TEST(test_sedit_keeper_choice_leaves_spec_proc_until_save);
  RUN_TEST(test_sedit_keeper_from_foreign_zone_is_refused);
  RUN_TEST(test_oedit_failed_save_is_reported_not_announced);
  RUN_TEST(test_zedit_save_for_vanished_room_is_reported_not_announced);
  RUN_TEST(test_oedit_weight_over_max_is_clamped_and_reported);
  RUN_TEST(test_oedit_weight_in_range_is_silent);
  RUN_TEST(test_sedit_non_numeric_input_is_refused);
  RUN_TEST(test_oedit_furniture_bad_seat_count_reprompts_value_1);
  RUN_TEST(test_oedit_duplicate_apply_is_refused_and_neighbour_allowed);
  RUN_TEST(test_sedit_room_list_reports_deleted_room_without_rewriting);
  RUN_TEST(test_sedit_object_rnum_zero_can_be_a_product);
  RUN_TEST(test_redit_name_over_byte_limit_is_refused_not_cut);
  RUN_TEST(test_iedit_refuses_object_from_zone_builder_cannot_edit);
  RUN_TEST(test_iedit_start_with_stale_session_reports_and_releases_it);
  RUN_TEST(test_sedit_unknown_mode_aborts_without_touching_freed_session);
  RUN_TEST(test_zedit_unknown_mode_aborts_and_reports);
  return UNITY_END();
}
