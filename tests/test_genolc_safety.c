/* Executable regression tests for the generic OLC library writers and
 * renumberers (genobj.c, genzon.c, genwld.c, genqst.c, genshp.c) and the DG
 * trigger loader (dg_db_scripts.c).
 *
 * The production sources are linked as they are; unrelated sections are
 * discarded with --gc-sections and the few engine services they reach
 * (lookups, object moves, save list, string reader) are supplied here as
 * small faithful models.  Each test asserts that the guard reported its
 * failure (tb_error_capture.h), not only that nothing crashed.
 *
 * Tests that save files run inside a private temporary directory created in
 * main() with the world/<type>/ layout the writers expect.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "shop.h"
#include "genolc.h"
#include "genobj.h"
#include "genzon.h"
#include "genwld.h"
#include "genshp.h"
#include "quest.h"
#include "dg_scripts.h"
#include "boards.h"
#include "lists.h"
#include "dg_event.h"
#include "mud_event.h"
#include "tb_error_capture.h"

#include <dirent.h>


/* ------------------------------------------------------------------------ */
/* Globals the production files reference (test_stubs.c owns world,         */
/* obj_index, mob_index, top_of_* and the start rooms).                     */
/* ------------------------------------------------------------------------ */
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
struct obj_data *object_list = NULL;
struct obj_data *obj_proto = NULL;
struct char_data *mob_proto = NULL;
struct shop_data *shop_index = NULL;
int top_shop = -1;
struct aq_data *aquest_table = NULL;
qst_rnum total_quests = 0;
struct index_data **trig_index = NULL;
int top_of_trigt = 0;
struct board_info_type board_info[NUM_OF_BOARDS];

const char *apply_types[] = {
  "NONE", "STR", "DEX", "INT", "WIS", "CON", "CHA", "CLASS", "LEVEL", "AGE",
  "CHAR_WEIGHT", "CHAR_HEIGHT", "MAXMANA", "MAXHIT", "MAXMOVE", "GOLD", "EXP",
  "ARMOR", "HITROLL", "DAMROLL", "SAVING_PARA", "SAVING_ROD", "SAVING_PETRI",
  "SAVING_BREATH", "SAVING_SPELL", "\n"
};

/* ------------------------------------------------------------------------ */
/* Engine services.                                                         */
/* ------------------------------------------------------------------------ */
static int mudlogs;
void mudlog(int type, int level, int file, const char *str, ...)
{
  (void)type; (void)level; (void)file; (void)str;
  mudlogs++;
}
void basic_mud_log(const char *format, ...) { (void)format; }

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *allocation;
  (void)file; (void)line; (void)func;
  allocation = calloc(count ? count : 1, element_size ? element_size : 1);
  if (!allocation) abort();
  return allocation;
}
void *tb_realloc_array_at(void *ptr, size_t count, size_t element_size,
                          const char *file, int line, const char *func)
{
  void *allocation;
  (void)file; (void)line; (void)func;
  allocation = realloc(ptr, (count ? count : 1) * (element_size ? element_size : 1));
  if (!allocation) abort();
  return allocation;
}

obj_rnum real_object(obj_vnum vnum)
{
  obj_rnum i;
  if (!obj_index) return NOTHING;
  for (i = 0; i <= top_of_objt; i++)
    if (obj_index[i].vnum == vnum) return i;
  return NOTHING;
}
room_rnum real_room(room_vnum vnum)
{
  room_rnum i;
  if (!world) return NOWHERE;
  for (i = 0; i <= top_of_world; i++)
    if (world[i].number == vnum) return i;
  return NOWHERE;
}
mob_rnum real_mobile(mob_vnum vnum)
{
  mob_rnum i;
  if (!mob_index) return NOBODY;
  for (i = 0; i <= top_of_mobt; i++)
    if (mob_index[i].vnum == vnum) return i;
  return NOBODY;
}
qst_rnum real_quest(qst_vnum vnum)
{
  qst_rnum i;
  for (i = 0; i < total_quests; i++)
    if (QST_NUM(i) == vnum) return i;
  return NOTHING;
}
trig_rnum real_trigger(trig_vnum vnum)
{
  int i;
  for (i = 0; i < top_of_trigt; i++)
    if (trig_index[i] && trig_index[i]->vnum == vnum) return i;
  return NOTHING;
}
room_vnum genolc_zone_bottom(zone_rnum rznum) { return zone_table[rznum].bot; }

static int save_list_adds;
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; save_list_adds++; return TRUE; }
int in_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return FALSE; }
int remove_from_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return TRUE; }

/* utils.c get_line(): next non-blank, non-comment line without its newline. */
int get_line(FILE *fl, char *buf)
{
  char temp[READ_SIZE];
  int lines = 0;
  do {
    if (!fgets(temp, sizeof(temp), fl)) return 0;
    lines++;
  } while (*temp == '*' || *temp == '\n' || *temp == '\r');
  temp[strcspn(temp, "\r\n")] = '\0';
  strcpy(buf, temp);
  return lines;
}
/* db.c fread_string(): text up to '~'; NULL for an empty body. */
char *fread_string(FILE *fl, const char *error)
{
  char buf[MAX_STRING_LENGTH];
  size_t len = 0;
  int c;
  (void)error;
  while ((c = fgetc(fl)) != EOF && c != '~' && len < sizeof(buf) - 1)
    buf[len++] = (char)c;
  while (c != EOF && c != '\n') c = fgetc(fl);   /* rest of the '~' line */
  buf[len] = '\0';
  return len ? strdup(buf) : NULL;
}
bitvector_t asciiflag_conv(char *flag) { (void)flag; return 0; }
void strip_cr(char *buffer) { (void)buffer; }
char *convert_from_tabs(char *string) { return string; }
int sprintascii(char *out, bitvector_t bits) { (void)bits; strcpy(out, "0"); return 1; }
void script_save_to_disk(FILE *fp, void *item, int type) { (void)fp; (void)item; (void)type; }
void skip_spaces(char **string) { while (**string && isspace((unsigned char)**string)) (*string)++; }
char *str_udup(const char *txt) { return strdup(txt && *txt ? txt : "undefined"); }
int is_abbrev(const char *arg1, const char *arg2)
{
  if (!*arg1) return 0;
  for (; *arg1 && *arg2; arg1++, arg2++)
    if (LOWER(*arg1) != LOWER(*arg2)) return 0;
  return !*arg1;
}
char *one_argument(char *argument, char *first_arg)
{
  skip_spaces(&argument);
  while (*argument && !isspace((unsigned char)*argument)) *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}
SPECIAL(questmaster) { (void)ch; (void)me; (void)cmd; (void)argument; return 0; }

void free_ex_descriptions(struct extra_descr_data *ex)
{
  struct extra_descr_data *next;
  for (; ex; ex = next) {
    next = ex->next;
    free(ex->keyword);
    free(ex->description);
    free(ex);
  }
}
void copy_ex_descriptions(struct extra_descr_data **to, struct extra_descr_data *from)
{
  struct extra_descr_data **tail = to;
  *to = NULL;
  for (; from; from = from->next) {
    CREATE(*tail, struct extra_descr_data, 1);
    (*tail)->keyword = from->keyword ? strdup(from->keyword) : NULL;
    (*tail)->description = from->description ? strdup(from->description) : NULL;
    tail = &(*tail)->next;
  }
}

/* DG and event services. */
static int scripts_extracted;
void extract_script(void *thing, int type)
{
  struct script_data *sc = NULL;
  scripts_extracted++;
  if (type == OBJ_TRIGGER) { sc = ((struct obj_data *)thing)->script; ((struct obj_data *)thing)->script = NULL; }
  else if (type == WLD_TRIGGER) { sc = ((struct room_data *)thing)->script; ((struct room_data *)thing)->script = NULL; }
  free(sc);
}
void free_proto_script(void *thing, int type)
{
  struct trig_proto_list **head = NULL, *p, *next;
  if (type == OBJ_TRIGGER) head = &((struct obj_data *)thing)->proto_script;
  else if (type == WLD_TRIGGER) head = &((struct room_data *)thing)->proto_script;
  else if (type == MOB_TRIGGER) head = &((struct char_data *)thing)->proto_script;
  if (!head) return;
  for (p = *head; p; p = next) { next = p->next; free(p); }
  *head = NULL;
}
void update_wait_events(struct room_data *to, struct room_data *from) { (void)to; (void)from; }
void event_cancel(struct event *event) { (void)event; }
void *simple_list(struct list_data *list) { (void)list; return NULL; }
void free_list(struct list_data *list) { (void)list; }

/* Object movement: just enough of handler.c to observe where things go. */
static void unlink_content(struct obj_data **head, struct obj_data *obj)
{
  for (; *head; head = &(*head)->next_content)
    if (*head == obj) { *head = obj->next_content; obj->next_content = NULL; return; }
}
void obj_from_obj(struct obj_data *obj)
{
  if (obj->in_obj) unlink_content(&obj->in_obj->contains, obj);
  obj->in_obj = NULL;
}
void obj_from_char(struct obj_data *obj) { obj->carried_by = NULL; }
void obj_from_room(struct obj_data *obj)
{
  if (obj->in_room != NOWHERE && world) unlink_content(&world[obj->in_room].contents, obj);
  obj->in_room = NOWHERE;
}
void obj_to_room(struct obj_data *obj, room_rnum room)
{
  if (room == NOWHERE || room > top_of_world) return;   /* like handler.c */
  obj->in_room = room;
  obj->next_content = world[room].contents;
  world[room].contents = obj;
}
void obj_to_char(struct obj_data *obj, struct char_data *ch) { obj->carried_by = ch; }
void obj_to_obj(struct obj_data *obj, struct obj_data *cont)
{
  obj->in_obj = cont;
  obj->next_content = cont->contains;
  cont->contains = obj;
}
static int objs_extracted;
void extract_obj(struct obj_data *obj)
{
  struct obj_data **pp;
  if (obj->in_obj) obj_from_obj(obj);
  while (obj->contains) extract_obj(obj->contains);
  for (pp = &object_list; *pp; pp = &(*pp)->next)
    if (*pp == obj) { *pp = obj->next; break; }
  if (GET_OBJ_RNUM(obj) != NOTHING) obj_index[GET_OBJ_RNUM(obj)].number--;
  objs_extracted++;
  free(obj);
}
void char_from_room(struct char_data *ch) { (void)ch; }
void char_to_room(struct char_data *ch, room_rnum room) { (void)ch; (void)room; }
size_t send_to_char(struct char_data *ch, const char *messg, ...) { (void)ch; (void)messg; return 0; }
void send_to_room(room_rnum room, const char *messg, ...) { (void)room; (void)messg; }

/* ------------------------------------------------------------------------ */
/* Fixtures.                                                                */
/* ------------------------------------------------------------------------ */
static void set_zone_cmds(int zone, const char *cmds, const int (*args)[3])
{
  int n = (int)strlen(cmds), i;
  free(zone_table[zone].cmd);
  CREATE(zone_table[zone].cmd, struct reset_com, n + 1);
  for (i = 0; i < n; i++) {
    zone_table[zone].cmd[i].command = cmds[i];
    zone_table[zone].cmd[i].arg1 = args[i][0];
    zone_table[zone].cmd[i].arg2 = args[i][1];
    zone_table[zone].cmd[i].arg3 = args[i][2];
    zone_table[zone].cmd[i].line = 100 + i;
  }
  zone_table[zone].cmd[n].command = 'S';
}

static void make_zones(int count)
{
  int z;
  top_of_zone_table = count - 1;
  CREATE(zone_table, struct zone_data, count);
  for (z = 0; z < count; z++) {
    zone_table[z].number = z;
    zone_table[z].bot = z * 100 + 1;
    zone_table[z].top = z * 100 + 99;
    zone_table[z].min_level = zone_table[z].max_level = -1;
    CREATE(zone_table[z].cmd, struct reset_com, 1);
    zone_table[z].cmd[0].command = 'S';
  }
}

/* Prototypes start without strings; tests that need them call
 * give_proto_strings() so ownership stays explicit. */
static void make_objs(int count)
{
  int i;
  top_of_objt = count - 1;
  CREATE(obj_index, struct index_data, count);
  CREATE(obj_proto, struct obj_data, count);
  for (i = 0; i < count; i++) {
    obj_index[i].vnum = 10 + i;
    obj_proto[i].item_number = i;
    obj_proto[i].in_room = NOWHERE;
  }
}

static void give_proto_strings(obj_rnum rnum)
{
  obj_proto[rnum].name = strdup("proto");
  obj_proto[rnum].short_description = strdup("a proto");
}

static struct obj_data *make_instance(obj_rnum rnum)
{
  struct obj_data *obj;
  CREATE(obj, struct obj_data, 1);
  obj->item_number = rnum;
  obj->in_room = NOWHERE;
  if (rnum != NOTHING) {
    obj->name = obj_proto[rnum].name;
    obj->short_description = obj_proto[rnum].short_description;
    obj_index[rnum].number++;
  }
  obj->next = object_list;
  object_list = obj;
  return obj;
}

static void make_rooms(int count)
{
  int i;
  top_of_world = count - 1;
  CREATE(world, struct room_data, count);
  for (i = 0; i < count; i++) {
    world[i].number = (i + 1) * 10;
    world[i].zone = 0;
    world[i].name = strdup("room");
    world[i].description = strdup("desc\r\n");
  }
}

static void free_everything(void)
{
  int i;
  while (object_list) {
    struct obj_data *o = object_list;
    object_list = o->next;
    if (GET_OBJ_RNUM(o) == NOTHING || GET_OBJ_RNUM(o) > top_of_objt || o->name != obj_proto[GET_OBJ_RNUM(o)].name)
      free(o->name);
    free(o);
  }
  if (obj_proto)
    for (i = 0; i <= top_of_objt; i++) {
      free_object_strings(&obj_proto[i]);
      free_proto_script(&obj_proto[i], OBJ_TRIGGER);
    }
  free(obj_proto); obj_proto = NULL;
  free(obj_index); obj_index = NULL;
  top_of_objt = 0;
  if (zone_table)
    for (i = 0; i <= top_of_zone_table; i++) free(zone_table[i].cmd);
  free(zone_table); zone_table = NULL;
  top_of_zone_table = 0;
  if (world)
    for (i = 0; i <= top_of_world; i++) free_room_strings(&world[i]);
  free(world); world = NULL;
  top_of_world = 0;
  free(mob_index); mob_index = NULL;
  top_of_mobt = 0;
  if (shop_index)
    for (i = 0; i <= top_shop; i++) {
      free(shop_index[i].producing);
      free(shop_index[i].in_room);
      free(shop_index[i].type);
    }
  free(shop_index); shop_index = NULL;
  top_shop = -1;
  free(aquest_table); aquest_table = NULL;
  total_quests = 0;
}

void setUp(void)
{
  int i;
  tb_error_capture_reset();
  mudlogs = save_list_adds = scripts_extracted = objs_extracted = 0;
  for (i = 0; i < NUM_OF_BOARDS; i++) {
    board_info[i].vnum = NOTHING;
    board_info[i].rnum = NOTHING;
  }
}
void tearDown(void) { free_everything(); }

/* ------------------------------------------------------------------------ */
/* C2-01: delete_object() zone-command renumbering.                         */
/* ------------------------------------------------------------------------ */

/* [O X, S]: deleting X removes the only command; the scan must stop at the
 * new terminator instead of reading past the shrunken list. */
static void delete_object_drops_last_zone_command(void)
{
  static const int args[][3] = { { 1, 0, 0 } };
  make_zones(1); make_objs(2);
  set_zone_cmds(0, "O", args);
  TEST_ASSERT_EQUAL_INT(1, delete_object(1));
  TEST_ASSERT_EQUAL_CHAR('S', zone_table[0].cmd[0].command);
  TEST_ASSERT_EQUAL_STRING("zone-cmd-drop", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
}

/* [O 1, O 2, S]: the command that slides into the deleted slot is still
 * renumbered (2 -> 1), not skipped. */
static void delete_object_renumbers_command_after_drop(void)
{
  static const int args[][3] = { { 1, 0, 0 }, { 2, 0, 0 } };
  make_zones(1); make_objs(3);
  set_zone_cmds(0, "OO", args);
  delete_object(1);
  TEST_ASSERT_EQUAL_CHAR('O', zone_table[0].cmd[0].command);
  TEST_ASSERT_EQUAL_INT(1, zone_table[0].cmd[0].arg1);
  TEST_ASSERT_EQUAL_CHAR('S', zone_table[0].cmd[1].command);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
}

/* [P 0 1, M 5, S]: a dropped 'P' must not fall through into the 'M' that
 * moved into its slot and decrement the mob rnum. */
static void delete_object_dropped_put_does_not_touch_next_command(void)
{
  static const int args[][3] = { { 0, 0, 1 }, { 5, 0, 0 } };
  make_zones(1); make_objs(3);
  set_zone_cmds(0, "PM", args);
  delete_object(1);
  TEST_ASSERT_EQUAL_CHAR('M', zone_table[0].cmd[0].command);
  TEST_ASSERT_EQUAL_INT(5, zone_table[0].cmd[0].arg1);
  TEST_ASSERT_EQUAL_STRING("zone-cmd-drop", tb_captured_operation);
}

/* An out-of-range position is reported, not ignored. */
static void delete_zone_command_reports_bad_position(void)
{
  make_zones(1);
  TEST_ASSERT_FALSE(delete_zone_command(&zone_table[0], 3));
  TEST_ASSERT_EQUAL_STRING("zone-cmd-index", tb_captured_operation);
}

/* ------------------------------------------------------------------------ */
/* C2-02: prototype-less objects keep NOTHING across delete_object().       */
/* ------------------------------------------------------------------------ */
static void delete_object_keeps_nothing_rnum(void)
{
  struct obj_data *corpse, *other;
  make_zones(1); make_objs(3);
  corpse = make_instance(NOTHING);
  other = make_instance(2);
  delete_object(0);
  TEST_ASSERT_EQUAL_UINT(NOTHING, GET_OBJ_RNUM(corpse));
  TEST_ASSERT_EQUAL_UINT(1, GET_OBJ_RNUM(other));
}

/* A stale rnum is reported rather than used to index obj_proto[]. */
static void free_object_strings_proto_reports_bad_rnum(void)
{
  struct obj_data stale;
  make_objs(1);
  memset(&stale, 0, sizeof(stale));
  stale.item_number = 65534;
  stale.script_id = 77;
  free_object_strings_proto(&stale);
  TEST_ASSERT_EQUAL_STRING("obj-rnum-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "uid 77"));
}

/* ------------------------------------------------------------------------ */
/* C2-05: delete_quest() range check and questmaster rnum.                  */
/* ------------------------------------------------------------------------ */
static SPECIAL(original_spec) { (void)ch; (void)me; (void)cmd; (void)argument; return 0; }

static void make_mobs(int count, int first_vnum)
{
  int i;
  top_of_mobt = count - 1;
  CREATE(mob_index, struct index_data, count);
  for (i = 0; i < count; i++)
    mob_index[i].vnum = first_vnum + i;
}

static void make_quest(qst_vnum vnum, mob_vnum qm)
{
  total_quests = 1;
  CREATE(aquest_table, struct aq_data, 1);
  aquest_table[0].vnum = vnum;
  aquest_table[0].qm = qm;
  aquest_table[0].func = original_spec;
}

static void delete_quest_rejects_nothing(void)
{
  make_zones(1);
  TEST_ASSERT_FALSE(delete_quest(NOTHING));
  TEST_ASSERT_EQUAL_STRING("quest-delete-range", tb_captured_operation);
}

/* qm 3005 is a vnum far above top_of_mobt: it must be resolved, not used
 * as an index (the old code wrote mob_index[3005].func). */
static void delete_quest_reports_missing_questmaster(void)
{
  make_zones(1); make_mobs(10, 0);
  make_quest(50, 3005);
  TEST_ASSERT_TRUE(delete_quest(0));
  TEST_ASSERT_EQUAL_STRING("quest-delete-qm", tb_captured_operation);
}

static void delete_quest_restores_spec_on_questmaster_rnum(void)
{
  make_zones(1); make_mobs(3, 3004);
  mob_index[1].func = questmaster;
  make_quest(50, 3005);
  TEST_ASSERT_TRUE(delete_quest(0));
  TEST_ASSERT_TRUE(mob_index[1].func == original_spec);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* ------------------------------------------------------------------------ */
/* C2-03: room mud events follow their room when world[] moves.             */
/* ------------------------------------------------------------------------ */
struct room_event_fixture {
  struct list_data list;
  struct item_data item;
  struct event ev;
  struct mud_event_data mud;
};

static void attach_room_event(struct room_event_fixture *f, room_rnum room)
{
  memset(f, 0, sizeof(*f));
  f->mud.iId = eSPL_DARKNESS;
  f->mud.pStruct = &world[room];
  f->ev.isMudEvent = TRUE;
  f->ev.event_obj = &f->mud;
  f->item.pContent = &f->ev;
  f->list.pFirstItem = f->list.pLastItem = &f->item;
  f->list.iSize = 1;
  world[room].events = &f->list;
}

static void add_room_rebases_room_events(void)
{
  struct room_event_fixture f;
  struct room_data nr;
  room_rnum moved;
  make_zones(1); make_rooms(2);
  attach_room_event(&f, 0);
  memset(&nr, 0, sizeof(nr));
  nr.number = 5;          /* sorts first: every existing room shifts up */
  nr.name = "new";
  nr.description = "new";
  add_room(&nr);
  moved = real_room(10);
  TEST_ASSERT_EQUAL_UINT(1, moved);
  TEST_ASSERT_TRUE(f.mud.pStruct == &world[moved]);
  world[moved].events = NULL;
}

static void delete_room_rebases_room_events(void)
{
  struct room_event_fixture f;
  make_zones(1); make_rooms(3);
  attach_room_event(&f, 2);
  TEST_ASSERT_TRUE(delete_room(1));
  TEST_ASSERT_TRUE(f.mud.pStruct == &world[1]);
  TEST_ASSERT_EQUAL_INT(30, world[1].number);
  world[1].events = NULL;
}

/* ------------------------------------------------------------------------ */
/* C2-04: a copied room does not share the source room's contents.          */
/* ------------------------------------------------------------------------ */
static void add_room_does_not_share_source_contents(void)
{
  struct obj_data *sword;
  struct room_data nr;
  make_zones(1); make_rooms(2); make_objs(1);
  sword = make_instance(0);
  obj_to_room(sword, 0);
  nr = world[0];          /* as redit_setup_existing() + rcopy */
  nr.number = 5;
  add_room(&nr);
  TEST_ASSERT_NULL(world[0].contents);
  TEST_ASSERT_TRUE(world[1].contents == sword);
  TEST_ASSERT_EQUAL_STRING("room-add-shared-list", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "contents"));
  world[1].contents = NULL;
}

/* ------------------------------------------------------------------------ */
/* C2-07: contents of a deleted container go where the container was.      */
/* ------------------------------------------------------------------------ */
static struct char_data carrier;

/* Prototype 0 is the bag being deleted, prototype 1 the sword inside it. */
static struct obj_data *bag_with_sword(struct obj_data **bag_out)
{
  struct obj_data *bag, *sword;
  make_zones(1); make_objs(2);
  bag = make_instance(0);
  sword = make_instance(1);
  obj_to_obj(sword, bag);
  *bag_out = bag;
  return sword;
}

static void delete_object_moves_contents_to_carrier(void)
{
  struct obj_data *bag, *sword = bag_with_sword(&bag);
  memset(&carrier, 0, sizeof(carrier));
  bag->carried_by = &carrier;          /* carried: IN_ROOM is NOWHERE */
  delete_object(0);
  TEST_ASSERT_TRUE(object_list == sword);
  TEST_ASSERT_TRUE(sword->carried_by == &carrier);
  TEST_ASSERT_NULL(sword->in_obj);
  TEST_ASSERT_EQUAL_UINT(0, GET_OBJ_RNUM(sword));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void delete_object_moves_contents_to_room_zero(void)
{
  struct obj_data *bag, *sword;
  make_rooms(1);
  sword = bag_with_sword(&bag);
  obj_to_room(bag, 0);                 /* rnum 0 is a real room */
  delete_object(0);
  TEST_ASSERT_TRUE(object_list == sword);
  TEST_ASSERT_EQUAL_UINT(0, sword->in_room);
  TEST_ASSERT_TRUE(world[0].contents == sword);
  world[0].contents = NULL;
}

static void delete_object_reports_orphaned_contents(void)
{
  struct obj_data *bag;
  bag_with_sword(&bag);                /* bag has no location at all */
  delete_object(0);
  TEST_ASSERT_NULL(object_list);
  TEST_ASSERT_EQUAL_INT(2, objs_extracted);
  TEST_ASSERT_EQUAL_STRING("obj-delete-orphan", tb_captured_operation);
}

/* ------------------------------------------------------------------------ */
/* C2-08: shop products and boards that name the deleted prototype.         */
/* ------------------------------------------------------------------------ */
static void make_shop(shop_vnum vnum, const obj_rnum *products, int nproducts)
{
  int i;
  top_shop = 0;
  CREATE(shop_index, struct shop_data, 1);
  shop_index[0].vnum = vnum;
  CREATE(shop_index[0].producing, obj_vnum, nproducts + 1);
  for (i = 0; i < nproducts; i++)
    shop_index[0].producing[i] = products[i];
  shop_index[0].producing[nproducts] = NOTHING;
  CREATE(shop_index[0].type, struct shop_buy_data, 1);
  shop_index[0].type[0].type = NOTHING;
  CREATE(shop_index[0].in_room, room_vnum, 1);
  shop_index[0].in_room[0] = NOWHERE;
  shop_index[0].keeper = NOBODY;
}

static int file_exists(const char *path)
{
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  fclose(fp);
  return 1;
}

static void delete_object_drops_matching_shop_product(void)
{
  static const obj_rnum products[] = { 0, 2 };
  make_zones(1); make_objs(3);
  make_shop(5, products, 2);
  delete_object(2);                    /* the last prototype */
  TEST_ASSERT_EQUAL_UINT(0, shop_index[0].producing[0]);
  TEST_ASSERT_EQUAL_UINT(NOTHING, shop_index[0].producing[1]);
  TEST_ASSERT_EQUAL_STRING("shop-product-drop", tb_captured_operation);
}

static void delete_object_refuses_board_object(void)
{
  make_zones(1); make_objs(2);
  board_info[3].rnum = 1;
  TEST_ASSERT_EQUAL_UINT(NOTHING, delete_object(1));
  TEST_ASSERT_EQUAL_INT(1, (int)top_of_objt);
  TEST_ASSERT_EQUAL_STRING("obj-delete-board", tb_captured_operation);
}

static void delete_object_reports_unsaved_rnum(void)
{
  make_objs(1);
  TEST_ASSERT_EQUAL_UINT(NOTHING, delete_object(NOTHING));
  TEST_ASSERT_EQUAL_STRING("obj-delete-range", tb_captured_operation);
}

static void save_shops_rejects_stale_product(void)
{
  static const obj_rnum products[] = { 2 };
  make_zones(1); make_objs(2);
  make_shop(5, products, 1);
  TEST_ASSERT_FALSE(save_shops(0));
  TEST_ASSERT_EQUAL_STRING("shop-save-product", tb_captured_operation);
  TEST_ASSERT_FALSE(file_exists(SHP_PREFIX "0.new"));
}

/* C2-09: a keeper rnum past mob_index[] (65534: NOBODY decremented by
 * delete_mobile()) aborts the save instead of being dereferenced. */
static void save_shops_rejects_stale_keeper(void)
{
  make_zones(1); make_objs(1); make_mobs(2, 3000);
  make_shop(5, NULL, 0);
  shop_index[0].keeper = 65534;
  TEST_ASSERT_FALSE(save_shops(0));
  TEST_ASSERT_EQUAL_STRING("shop-save-keeper", tb_captured_operation);
  TEST_ASSERT_FALSE(file_exists(SHP_PREFIX "0.new"));
}

static void save_shops_writes_valid_shop(void)
{
  make_zones(1); make_objs(1); make_mobs(2, 3000);
  make_shop(5, NULL, 0);
  shop_index[0].keeper = 1;
  TEST_ASSERT_TRUE(save_shops(0));
  TEST_ASSERT_TRUE(file_exists(SHP_PREFIX "0.shp"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* ------------------------------------------------------------------------ */
/* C2-12: zedit new never duplicates a zone or truncates its files.         */
/* ------------------------------------------------------------------------ */
static void write_text_file(const char *path, const char *text)
{
  FILE *fp = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fp);
  fputs(text, fp);
  fclose(fp);
}

static void read_text_file(const char *path, char *buf, size_t size)
{
  FILE *fp = fopen(path, "r");
  size_t n;
  TEST_ASSERT_NOT_NULL(fp);
  n = fread(buf, 1, size - 1, fp);
  buf[n] = '\0';
  fclose(fp);
}

/* The last zone in the table was never compared. */
static void create_new_zone_rejects_last_zone_number(void)
{
  const char *err = NULL;
  make_zones(1);
  TEST_ASSERT_EQUAL_UINT(NOWHERE, create_new_zone(0, 200, 299, &err));
  TEST_ASSERT_EQUAL_INT(0, (int)top_of_zone_table);
  TEST_ASSERT_EQUAL_STRING("zone-create-duplicate", tb_captured_operation);
  TEST_ASSERT_FALSE(file_exists(WLD_PREFIX "/0.wld"));
}

static void create_new_zone_rejects_overlapping_range(void)
{
  const char *err = NULL;
  make_zones(2);                       /* 1-99 and 101-199 */
  TEST_ASSERT_EQUAL_UINT(NOWHERE, create_new_zone(5, 150, 250, &err));
  TEST_ASSERT_EQUAL_STRING("zone-create-overlap", tb_captured_operation);
}

static void create_new_zone_keeps_existing_file(void)
{
  const char *err = NULL;
  char text[64];
  make_zones(1);
  write_text_file(ZON_PREFIX "/7.zon", "ORIGINAL ZONE 7\n");
  TEST_ASSERT_EQUAL_UINT(NOWHERE, create_new_zone(7, 700, 799, &err));
  TEST_ASSERT_EQUAL_STRING("zone-create-exists", tb_captured_operation);
  read_text_file(ZON_PREFIX "/7.zon", text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("ORIGINAL ZONE 7\n", text);
}

/* ------------------------------------------------------------------------ */
/* C2-13: a room extra description with a NULL body is saved, not deref'd.  */
/* ------------------------------------------------------------------------ */
static void save_rooms_handles_null_exdesc_description(void)
{
  struct extra_descr_data *x;
  char text[512];
  make_zones(1); make_rooms(1);
  CREATE(x, struct extra_descr_data, 1);
  x->keyword = strdup("sign");
  x->description = NULL;               /* fread_string("~") */
  world[0].ex_description = x;
  TEST_ASSERT_TRUE(save_rooms(0));
  TEST_ASSERT_EQUAL_STRING("room-save-exdesc", tb_captured_operation);
  read_text_file(WLD_PREFIX "/0.wld", text, sizeof(text));
  TEST_ASSERT_NOT_NULL(strstr(text, "E\nsign~\n~\n"));
}

/* ------------------------------------------------------------------------ */
/* C2-V01: start rooms above a deleted room move down with world[].         */
/* ------------------------------------------------------------------------ */
static void delete_room_renumbers_start_rooms(void)
{
  make_zones(1); make_rooms(4);
  r_mortal_start_room = 3;             /* vnum 40, the last room */
  r_immort_start_room = 2;             /* vnum 30 */
  r_frozen_start_room = 1;             /* vnum 20: deleted below */
  TEST_ASSERT_TRUE(delete_room(1));
  TEST_ASSERT_EQUAL_INT(40, world[r_mortal_start_room].number);
  TEST_ASSERT_EQUAL_INT(30, world[r_immort_start_room].number);
  TEST_ASSERT_EQUAL_UINT(0, r_frozen_start_room);
  TEST_ASSERT_EQUAL_STRING("room-delete-start", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "frozen"));
  r_mortal_start_room = r_immort_start_room = r_frozen_start_room = 0;
}

/* ------------------------------------------------------------------------ */
/* C2-10: re-saving a prototype frees what live instances owned.            */
/* ------------------------------------------------------------------------ */
static void add_object_releases_live_instance_state(void)
{
  struct obj_data *inst, newobj;
  struct list_data *events = (struct list_data *)&newobj; /* opaque marker */
  make_zones(1); make_objs(1);
  give_proto_strings(0);
  inst = make_instance(0);
  inst->name = strdup("restrung live name");   /* instance-owned */
  CREATE(inst->script, struct script_data, 1);
  CREATE(inst->proto_script, struct trig_proto_list, 1);
  inst->events = events;
  memset(&newobj, 0, sizeof(newobj));
  newobj.name = "new";
  newobj.short_description = "a new thing";
  TEST_ASSERT_EQUAL_UINT(0, add_object(&newobj, 10));
  TEST_ASSERT_EQUAL_INT(1, scripts_extracted);
  TEST_ASSERT_TRUE(inst->name == obj_proto[0].name);
  TEST_ASSERT_EQUAL_STRING("new", inst->name);
  TEST_ASSERT_NULL(inst->script);
  TEST_ASSERT_NULL(inst->proto_script);
  TEST_ASSERT_TRUE(inst->events == events);
  TEST_ASSERT_EQUAL_STRING("obj-proto-update", tb_captured_operation);
  inst->events = NULL;
}

/* ------------------------------------------------------------------------ */
/* C2-11: copy_object()/copy_room() ownership of triggers and events.       */
/* ------------------------------------------------------------------------ */

/* The prototype's old trigger list is freed (LSan catches the old leak). */
static void add_object_frees_replaced_proto_script(void)
{
  struct obj_data newobj;
  make_zones(1); make_objs(1);
  CREATE(obj_proto[0].proto_script, struct trig_proto_list, 1);
  memset(&newobj, 0, sizeof(newobj));
  newobj.name = "new";
  add_object(&newobj, 10);
  TEST_ASSERT_NULL(obj_proto[0].proto_script);
}

static void copy_object_keeps_destination_runtime_state(void)
{
  struct obj_data to, from;
  struct script_data *own_script;
  struct list_data *own_events = (struct list_data *)&to;   /* opaque */
  memset(&to, 0, sizeof(to));
  memset(&from, 0, sizeof(from));
  CREATE(own_script, struct script_data, 1);
  to.script = own_script;
  to.events = own_events;
  CREATE(from.script, struct script_data, 1);
  from.name = "from";
  copy_object(&to, &from);
  TEST_ASSERT_TRUE(to.script == own_script);
  TEST_ASSERT_TRUE(to.events == own_events);
  TEST_ASSERT_EQUAL_STRING("from", to.name);
  free(to.name);
  free(own_script);
  free(from.script);
}

/* Updating an existing room keeps its pending events and frees the trigger
 * list the OLC copy replaces. */
static void add_room_update_keeps_live_events(void)
{
  struct room_event_fixture f;
  struct room_data nr;
  make_zones(1); make_rooms(1);
  attach_room_event(&f, 0);
  CREATE(world[0].proto_script, struct trig_proto_list, 1);
  memset(&nr, 0, sizeof(nr));
  nr.number = 10;                      /* existing room */
  nr.name = "renamed";
  nr.description = "d";
  TEST_ASSERT_EQUAL_UINT(0, add_room(&nr));
  TEST_ASSERT_TRUE(world[0].events == &f.list);
  TEST_ASSERT_NULL(world[0].proto_script);
  TEST_ASSERT_EQUAL_STRING("renamed", world[0].name);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  world[0].events = NULL;
}

/* ------------------------------------------------------------------------ */
/* C2-14: a trigger with an empty command body loads with a placeholder.    */
/* ------------------------------------------------------------------------ */
static void free_trigger_index(void)
{
  int i;
  for (i = 0; i < top_of_trigt; i++) {
    struct trig_data *t = trig_index[i]->proto;
    struct cmdlist_element *c, *next;
    for (c = t->cmdlist; c; c = next) { next = c->next; free(c->cmd); free(c); }
    free(t->name);
    free(t->arglist);
    free(t);
    free(trig_index[i]);
  }
  free(trig_index);
  trig_index = NULL;
  top_of_trigt = 0;
}

static void parse_body(const char *text)
{
  FILE *fp;
  write_text_file(TRG_PREFIX "/t.trg", text);
  fp = fopen(TRG_PREFIX "/t.trg", "r");
  TEST_ASSERT_NOT_NULL(fp);
  CREATE(trig_index, struct index_data *, 1);
  top_of_trigt = 0;
  parse_trigger(fp, 100);
  fclose(fp);
}

static void parse_trigger_accepts_empty_body(void)
{
  parse_body("empty~\n0 g 100\n~\n~\n");
  TEST_ASSERT_EQUAL_INT(1, top_of_trigt);
  TEST_ASSERT_EQUAL_STRING("* No script", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_EQUAL_STRING("trigger-parse-empty", tb_captured_operation);
  free_trigger_index();
}

static void parse_trigger_accepts_blank_line_body(void)
{
  parse_body("blank~\n0 g 100\n~\n\n\n~\n");
  TEST_ASSERT_EQUAL_STRING("* No script", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_EQUAL_STRING("trigger-parse-empty", tb_captured_operation);
  free_trigger_index();
}

static void parse_trigger_reads_commands(void)
{
  parse_body("ok~\n0 g 100\n~\nsay hi\nwait 1\n~\n");
  TEST_ASSERT_EQUAL_STRING("say hi", trig_index[0]->proto->cmdlist->cmd);
  TEST_ASSERT_EQUAL_STRING("wait 1", trig_index[0]->proto->cmdlist->next->cmd);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free_trigger_index();
}

/* ------------------------------------------------------------------------ */
/* C2-15: an oversized quest aborts the save instead of vanishing.          */
/* ------------------------------------------------------------------------ */
static void save_quests_rejects_oversized_quest(void)
{
  static char big[MAX_STRING_LENGTH];
  char text[64];
  make_zones(1);
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  write_text_file(QST_PREFIX "/0.qst", "#5 previous file\n");
  total_quests = 2;
  CREATE(aquest_table, struct aq_data, 2);
  aquest_table[0].vnum = 5;
  aquest_table[1].vnum = 6;
  aquest_table[1].desc = big;
  aquest_table[1].info = big;
  aquest_table[0].qm = aquest_table[1].qm = NOBODY;
  TEST_ASSERT_FALSE(save_quests(0));
  TEST_ASSERT_EQUAL_STRING("quest-save-size", tb_captured_operation);
  TEST_ASSERT_FALSE(file_exists(QST_PREFIX "/0.new"));
  read_text_file(QST_PREFIX "/0.qst", text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("#5 previous file\n", text);
}

/* ------------------------------------------------------------------------ */
/* C2-17: save_zone() checks stored rnums before converting them.           */
/* ------------------------------------------------------------------------ */
static void save_zone_rejects_stale_object_rnum(void)
{
  static const int args[][3] = { { 2, 1, 0 } };   /* obj rnum 2, top is 1 */
  make_zones(1); make_objs(2); make_rooms(1);
  set_zone_cmds(0, "O", args);
  write_text_file(ZON_PREFIX "/0.zon", "previous\n");
  TEST_ASSERT_FALSE(save_zone(0));
  TEST_ASSERT_EQUAL_STRING("zone-save-rnum", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "line 100"));
  TEST_ASSERT_FALSE(file_exists(ZON_PREFIX "/0.new"));
}

static void save_zone_rejects_var_without_strings(void)
{
  static const int args[][3] = { { 0, 0, 0 } };
  make_zones(1); make_rooms(1);
  set_zone_cmds(0, "V", args);
  TEST_ASSERT_FALSE(save_zone(0));
  TEST_ASSERT_EQUAL_STRING("zone-save-sarg", tb_captured_operation);
}

static void save_zone_writes_valid_commands(void)
{
  static const int args[][3] = { { 1, 5, 0 }, { 0, 2, 1 } };
  char text[512];
  make_zones(1); make_objs(2); make_rooms(1);
  set_zone_cmds(0, "OD", args);
  TEST_ASSERT_TRUE(save_zone(0));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  read_text_file(ZON_PREFIX "/0.zon", text, sizeof(text));
  TEST_ASSERT_NOT_NULL(strstr(text, "O 0 11 5 10"));
  TEST_ASSERT_NOT_NULL(strstr(text, "D 0 10 2 1"));
}

/* C2-18: disabled ('*') reset lines are reported when a save drops them. */
static void save_zone_reports_disabled_command(void)
{
  static const int args[][3] = { { 0, 0, 0 } };
  make_zones(1); make_rooms(1);
  set_zone_cmds(0, "*", args);
  TEST_ASSERT_TRUE(save_zone(0));
  TEST_ASSERT_EQUAL_STRING("zone-save-drop-cmd", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "line 100"));
}

/* ------------------------------------------------------------------------ */
/* Temporary world directory for the writers.                               */
/* ------------------------------------------------------------------------ */
static char test_root[] = "/tmp/tb-genolc-XXXXXX";
static const char *test_dirs[] = { "world", "world/obj", "world/wld",
  "world/zon", "world/shp", "world/qst", "world/trg", "world/mob" };
#define NUM_TEST_DIRS (sizeof(test_dirs) / sizeof(*test_dirs))

static int enter_test_root(void)
{
  size_t i;
  if (!mkdtemp(test_root) || chdir(test_root) != 0) return 0;
  for (i = 0; i < NUM_TEST_DIRS; i++)
    if (mkdir(test_dirs[i], 0700) != 0) return 0;
  return 1;
}

/* Remove every file the writers left, deepest directories first. */
static void leave_test_root(void)
{
  size_t i;
  for (i = NUM_TEST_DIRS; i-- > 0; ) {
    DIR *dir = opendir(test_dirs[i]);
    struct dirent *entry;
    char path[PATH_MAX];
    if (dir) {
      while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s", test_dirs[i], entry->d_name) < (int)sizeof(path))
          remove(path);
      }
      closedir(dir);
    }
    rmdir(test_dirs[i]);
  }
  if (chdir("/") == 0)
    rmdir(test_root);
}

int main(void)
{
  int failures;
  if (!enter_test_root()) {
    perror("test root");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(delete_object_drops_last_zone_command);
  RUN_TEST(delete_object_renumbers_command_after_drop);
  RUN_TEST(delete_object_dropped_put_does_not_touch_next_command);
  RUN_TEST(delete_zone_command_reports_bad_position);
  RUN_TEST(delete_object_keeps_nothing_rnum);
  RUN_TEST(free_object_strings_proto_reports_bad_rnum);
  RUN_TEST(delete_quest_rejects_nothing);
  RUN_TEST(delete_quest_reports_missing_questmaster);
  RUN_TEST(delete_quest_restores_spec_on_questmaster_rnum);
  RUN_TEST(add_room_rebases_room_events);
  RUN_TEST(delete_room_rebases_room_events);
  RUN_TEST(add_room_does_not_share_source_contents);
  RUN_TEST(delete_object_moves_contents_to_carrier);
  RUN_TEST(delete_object_moves_contents_to_room_zero);
  RUN_TEST(delete_object_reports_orphaned_contents);
  RUN_TEST(delete_object_drops_matching_shop_product);
  RUN_TEST(delete_object_refuses_board_object);
  RUN_TEST(delete_object_reports_unsaved_rnum);
  RUN_TEST(save_shops_rejects_stale_product);
  RUN_TEST(save_shops_rejects_stale_keeper);
  RUN_TEST(save_shops_writes_valid_shop);
  RUN_TEST(create_new_zone_rejects_last_zone_number);
  RUN_TEST(create_new_zone_rejects_overlapping_range);
  RUN_TEST(create_new_zone_keeps_existing_file);
  RUN_TEST(save_rooms_handles_null_exdesc_description);
  RUN_TEST(delete_room_renumbers_start_rooms);
  RUN_TEST(add_object_releases_live_instance_state);
  RUN_TEST(add_object_frees_replaced_proto_script);
  RUN_TEST(copy_object_keeps_destination_runtime_state);
  RUN_TEST(add_room_update_keeps_live_events);
  RUN_TEST(parse_trigger_accepts_empty_body);
  RUN_TEST(parse_trigger_accepts_blank_line_body);
  RUN_TEST(parse_trigger_reads_commands);
  RUN_TEST(save_quests_rejects_oversized_quest);
  RUN_TEST(save_zone_rejects_stale_object_rnum);
  RUN_TEST(save_zone_rejects_var_without_strings);
  RUN_TEST(save_zone_writes_valid_commands);
  RUN_TEST(save_zone_reports_disabled_command);
  failures = UNITY_END();
  leave_test_root();
  return failures;
}
