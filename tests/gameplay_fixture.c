/* Shared fixture for the gameplay regression tests. See gameplay_fixture.h. */
#include "gameplay_fixture.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "mud_event.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fx_trigger_fn fx_hook;
int fx_trigger_calls[FX_TRIGGER_COUNT];
int fx_rand_value = -1;
int fx_core_dumps;
int fx_log_lines;
char fx_last_send[512];

/* db.c globals the linked sources reference. */
struct obj_data *object_list;
struct obj_data *obj_proto;
struct char_data *mob_proto;

static int fx_fire(enum fx_trigger kind, struct char_data *actor,
                   struct obj_data *obj, int arg)
{
  fx_trigger_calls[kind]++;
  return fx_hook ? fx_hook(kind, actor, obj, arg) : 1;
}

/* ---- DG entry points: routed to the test's hook ------------------------ */
int get_otrigger(obj_data *obj, char_data *actor)
{ return fx_fire(FX_GET_OTRIG, actor, obj, 0); }
int drop_otrigger(obj_data *obj, char_data *actor)
{ return fx_fire(FX_DROP_OTRIG, actor, obj, 0); }
int drop_wtrigger(obj_data *obj, char_data *actor)
{ return fx_fire(FX_DROP_WTRIG, actor, obj, 0); }
int give_otrigger(obj_data *obj, char_data *actor, char_data *victim)
{ (void)victim; return fx_fire(FX_GIVE_OTRIG, actor, obj, 0); }
int receive_mtrigger(char_data *ch, char_data *actor, obj_data *obj)
{ (void)ch; return fx_fire(FX_RECEIVE_MTRIG, actor, obj, 0); }
int wear_otrigger(obj_data *obj, char_data *actor, int where)
{ return fx_fire(FX_WEAR_OTRIG, actor, obj, where); }
int remove_otrigger(obj_data *obj, char_data *actor)
{ return fx_fire(FX_REMOVE_OTRIG, actor, obj, 0); }
int consume_otrigger(obj_data *obj, char_data *actor, int cmd)
{ return fx_fire(FX_CONSUME_OTRIG, actor, obj, cmd); }
int door_mtrigger(char_data *actor, int subcmd, int dir)
{ (void)subcmd; return fx_fire(FX_DOOR_MTRIG, actor, NULL, dir); }
int door_wtrigger(char_data *actor, int subcmd, int dir)
{ (void)subcmd; return fx_fire(FX_DOOR_WTRIG, actor, NULL, dir); }
void fight_mtrigger(char_data *ch)
{ (void)fx_fire(FX_FIGHT_MTRIG, ch, NULL, 0); }
void load_mtrigger(char_data *ch)
{ (void)fx_fire(FX_LOAD_MTRIG, ch, NULL, 0); }
int death_mtrigger(char_data *ch, char_data *actor)
{ (void)actor; return fx_fire(FX_DEATH_MTRIG, ch, NULL, 0); }
void hitprcnt_mtrigger(char_data *ch)
{ (void)fx_fire(FX_HITPRCNT_MTRIG, ch, NULL, 0); }
int damage_mtrigger(char_data *ch, char_data *victim, int dam, int attacktype)
{ (void)ch; (void)victim; (void)attacktype; return dam; }
void bribe_mtrigger(char_data *ch, char_data *actor, int amount)
{ (void)ch; (void)actor; (void)amount; }
void load_otrigger(obj_data *obj) { (void)obj; }
int leave_mtrigger(char_data *actor, int dir) { (void)actor; (void)dir; return 1; }
int leave_wtrigger(struct room_data *room, char_data *actor, int dir)
{ (void)room; (void)actor; (void)dir; return 1; }
int leave_otrigger(struct room_data *room, char_data *actor, int dir)
{ (void)room; (void)actor; (void)dir; return 1; }
int entry_mtrigger(char_data *ch) { (void)ch; return 1; }
int enter_wtrigger(struct room_data *room, char_data *actor, int dir)
{ (void)room; (void)actor; (void)dir; return 1; }
void entry_memory_mtrigger(char_data *ch) { (void)ch; }
int greet_mtrigger(char_data *actor, int dir) { (void)actor; (void)dir; return 1; }
void greet_memory_mtrigger(char_data *actor) { (void)actor; }
void extract_script(void *thing, int type) { (void)thing; (void)type; }
void free_proto_script(void *thing, int type) { (void)thing; (void)type; }

/* ---- utils.c functions observed by the tests (linked with --wrap) ------ */
int __wrap_rand_number(int from, int to)
{
  if (fx_rand_value < 0)
    return from;
  if (fx_rand_value < from)
    return from;
  return fx_rand_value > to ? to : fx_rand_value;
}

void __wrap_core_dump_real(const char *who, int line)
{
  (void)who; (void)line;
  fx_core_dumps++;
}

void __wrap_basic_mud_log(const char *format, ...)
{
  (void)format;
  fx_log_lines++;
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(fx_last_send, sizeof(fx_last_send), messg, args) < 0)
    fx_last_send[0] = '\0';
  va_end(args);
  return strlen(fx_last_send);
}

void send_to_room(room_rnum room, const char *messg, ...)
{ (void)room; (void)messg; }

/* ---- limits.c / quest.c / db.c pieces ---------------------------------- */
void autoquest_trigger_check(struct char_data *ch, struct char_data *vict,
                             struct obj_data *object, int type)
{ (void)ch; (void)vict; (void)object; (void)type; }

void gain_condition(struct char_data *ch, int condition, int value)
{ (void)ch; (void)condition; (void)value; }

int increase_gold(struct char_data *ch, int amount)
{
  int curr_gold = GET_GOLD(ch);
  if (amount < 0)
    return curr_gold;
  curr_gold = (amount > MAX_GOLD - curr_gold) ? MAX_GOLD : curr_gold + amount;
  GET_GOLD(ch) = curr_gold;
  return curr_gold;
}

int decrease_gold(struct char_data *ch, int amount)
{
  GET_GOLD(ch) = (amount > GET_GOLD(ch)) ? 0 : GET_GOLD(ch) - amount;
  return GET_GOLD(ch);
}

struct obj_data *create_obj(void)
{
  struct obj_data *obj = calloc(1, sizeof(*obj));
  if (!obj)
    abort();
  obj->item_number = NOTHING;
  IN_ROOM(obj) = NOWHERE;
  obj->worn_on = NOWHERE;
  obj->next = object_list;
  object_list = obj;
  return obj;
}

void free_obj(struct obj_data *obj)
{
  struct extra_descr_data *ed, *next_ed;

  for (ed = obj->ex_description; ed; ed = next_ed) {   /* create_money() */
    next_ed = ed->next;
    free(ed->keyword);
    free(ed->description);
    free(ed);
  }
  free(obj->name);
  free(obj->description);
  free(obj->short_description);
  free(obj->action_description);
  if (obj->script_id != 0)
    remove_from_lookup_table(obj->script_id, obj);
  free(obj);
}

/* ---- link-only stubs: not reached by the tests, or reached harmlessly -- */
struct zone_data *zone_table;
struct index_data **trig_index;
struct trig_data *trigger_list;
int top_of_trigt;
long max_obj_id = OBJ_ID_BASE;
struct message_list fight_messages[MAX_MESSAGES];

int House_can_enter(struct char_data *ch, room_vnum house)
{ (void)ch; (void)house; return 1; }
int backstab_mult(int level) { (void)level; return 2; }
int buildwalk(struct char_data *ch, int dir) { (void)ch; (void)dir; return 0; }
int can_edit_zone(struct char_data *ch, zone_rnum rnum)
{ (void)ch; (void)rnum; return 0; }
char *find_exdesc(char *word, struct extra_descr_data *list)
{ (void)word; (void)list; return NULL; }
room_rnum find_target_room(struct char_data *ch, char *rawroomstr)
{ (void)ch; (void)rawroomstr; return NOWHERE; }
void forget(struct char_data *ch, struct char_data *victim) { (void)ch; (void)victim; }
void remember(struct char_data *ch, struct char_data *victim) { (void)ch; (void)victim; }
void gain_exp(struct char_data *ch, int gain) { (void)ch; (void)gain; }
int invalid_class(struct char_data *ch, struct obj_data *obj)
{ (void)ch; (void)obj; return 0; }
int ok_damage_shopkeeper(struct char_data *ch, struct char_data *victim)
{ (void)ch; (void)victim; return 1; }
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; return NOBODY; }
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return NOWHERE; }
void send_to_group(struct char_data *ch, struct group_data *group, const char *msg, ...)
{ (void)ch; (void)group; (void)msg; }
int thaco(int class_num, int level) { (void)class_num; (void)level; return 20; }
void *simple_list(struct list_data *pList) { (void)pList; return NULL; }
void free_list(struct list_data *pList) { (void)pList; }
/* event_cancel() is a macro over event_cancel_at() (call-site capture). */
void event_cancel_at(struct event *event, const char *file, int line,
                     const char *func)
{ (void)event; (void)file; (void)line; (void)func; }
trig_data *read_trigger(int nr) { (void)nr; return NULL; }
void extract_trigger(struct trig_data *trig) { (void)trig; }
int fx_last_event_id = -1;
void *fx_last_event_struct;
char fx_last_event_vars[64];
struct mud_event_data *new_mud_event(event_id iId, void *pStruct, char *sVariables)
{
  fx_last_event_id = (int) iId;
  fx_last_event_struct = pStruct;
  if (snprintf(fx_last_event_vars, sizeof(fx_last_event_vars), "%s",
               sVariables ? sVariables : "") < 0)
    fx_last_event_vars[0] = '\0';
  return NULL;
}
void attach_mud_event(struct mud_event_data *pMudEvent, long time)
{ (void)pMudEvent; (void)time; }
struct mud_event_data *char_has_mud_event(struct char_data *ch, event_id iId)
{ (void)ch; (void)iId; return NULL; }
void clear_char_event_list(struct char_data *ch) { (void)ch; }

/* ---- fixture construction --------------------------------------------- */
void fx_setup(int rooms)
{
  int i;

  memset(fx_trigger_calls, 0, sizeof(fx_trigger_calls));
  fx_hook = NULL;
  fx_rand_value = -1;
  fx_core_dumps = fx_log_lines = 0;
  fx_last_send[0] = '\0';
  fx_last_event_id = -1;
  fx_last_event_struct = NULL;
  fx_last_event_vars[0] = '\0';
  object_list = NULL;
  world = calloc((size_t) rooms, sizeof(struct room_data));
  if (!world)
    abort();
  top_of_world = rooms - 1;
  for (i = 0; i < rooms; i++) {
    world[i].number = 3000 + i;
    world[i].light = 1;
  }
}

void fx_teardown(void)
{
  int i, d;

  while (object_list)
    extract_obj(object_list);
  for (i = 0; i <= top_of_world; i++)
    for (d = 0; d < NUM_OF_DIRS; d++)
      free(world[i].dir_option[d]);
  free(world);
  world = NULL;
  top_of_world = 0;
  fx_hook = NULL;
}

struct char_data *fx_new_char(const char *name, int npc, room_rnum room)
{
  struct char_data *ch = calloc(1, sizeof(*ch));
  if (!ch)
    abort();
  ch->nr = NOBODY;
  IN_ROOM(ch) = NOWHERE;
  ch->player.name = strdup(name);
  ch->player.short_descr = strdup(name);
  GET_LEVEL(ch) = 10;
  GET_POS(ch) = POS_STANDING;
  GET_MAX_HIT(ch) = GET_HIT(ch) = 100;
  ch->real_abils.str = ch->aff_abils.str = 13;
  ch->real_abils.dex = ch->aff_abils.dex = 13;
  ch->real_abils.intel = ch->aff_abils.intel = 13;
  ch->real_abils.wis = ch->aff_abils.wis = 13;
  ch->real_abils.con = ch->aff_abils.con = 13;
  ch->real_abils.cha = ch->aff_abils.cha = 13;
  if (npc) {
    SET_BIT_AR(MOB_FLAGS(ch), MOB_ISNPC);
    ch->player_specials = &dummy_mob;
  } else {
    ch->player_specials = calloc(1, sizeof(*ch->player_specials));
    if (!ch->player_specials)
      abort();
    GET_IDNUM(ch) = 4242;
    SET_BIT_AR(PRF_FLAGS(ch), PRF_HOLYLIGHT);
  }
  if (room != NOWHERE)
    char_to_room(ch, room);
  return ch;
}

void fx_free_char(struct char_data *ch)
{
  int i;

  for (i = 0; i < NUM_WEARS; i++)
    if (GET_EQ(ch, i))
      extract_obj(GET_EQ(ch, i));
  while (ch->carrying)
    extract_obj(ch->carrying);
  if (IN_ROOM(ch) != NOWHERE)
    char_from_room(ch);
  char_from_furniture(ch);
  if (!IS_NPC(ch))
    free(ch->player_specials);
  free(ch->player.name);
  free(ch->player.short_descr);
  free(ch);
}

struct obj_data *fx_new_obj(const char *name, int type)
{
  struct obj_data *obj = create_obj();
  char buf[128];

  obj->name = strdup(name);
  snprintf(buf, sizeof(buf), "a %s", name);
  obj->short_description = strdup(buf);
  snprintf(buf, sizeof(buf), "A %s is here.", name);
  obj->description = strdup(buf);
  GET_OBJ_TYPE(obj) = type;
  SET_BIT_AR(GET_OBJ_WEAR(obj), ITEM_WEAR_TAKE);
  GET_OBJ_WEIGHT(obj) = 1;
  return obj;
}
