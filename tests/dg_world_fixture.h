/* A tiny in-memory world for the whole-server DG command tests
 * (tests/dg_world/CMakeLists.txt). Include from exactly one translation unit
 * per test executable, after unity.h and the mud headers.
 *
 * World: rooms vnum 100..103 (rnum 0..3); object prototypes 200..206;
 * mobile prototypes 300..301. Every object and character the production code
 * creates is extracted again by dgw_teardown(), so ASan/LSan and Valgrind
 * report anything a command leaks or frees twice.
 *
 * Diagnostics: tb_error_log_at() is wrapped at link time. Every structured
 * record is kept (operation and severity), so a test can assert that a guard
 * failed loudly with a specific stable operation name.
 */
#ifndef DG_WORLD_FIXTURE_H
#define DG_WORLD_FIXTURE_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DGW_ROOM0      100
#define DGW_NUM_ROOMS  4

#define DGW_OBJ_BAG     200   /* container, capacity 50, weight 5 */
#define DGW_OBJ_STONE   201   /* other, weight 2 */
#define DGW_OBJ_BENCH   202   /* furniture, 2 seats */
#define DGW_OBJ_SKIN    203   /* drink container */
#define DGW_OBJ_SWORD   204   /* weapon */
#define DGW_OBJ_CHEST   205   /* container, capacity 100, weight 10 */
#define DGW_OBJ_ROCK    206   /* other, weight 20 */
#define DGW_NUM_OBJS    7

#define DGW_MOB_GUARD   300
#define DGW_MOB_OGRE    301
#define DGW_NUM_MOBS    2

/* ---- diagnostic capture ------------------------------------------------ */

#define DGW_MAX_DIAG 256
static int dgw_diag_count;
static char dgw_diag_op[DGW_MAX_DIAG][64];
static char dgw_diag_sev[DGW_MAX_DIAG][16];
static char dgw_diag_detail[DGW_MAX_DIAG][512];

void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));

void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  va_list args;
  int slot;
  (void)subsystem; (void)resource; (void)errnum; (void)source_line;

  if (dgw_diag_count >= DGW_MAX_DIAG)
    return;
  slot = dgw_diag_count++;
  snprintf(dgw_diag_op[slot], sizeof(dgw_diag_op[slot]), "%s",
           operation ? operation : "");
  snprintf(dgw_diag_sev[slot], sizeof(dgw_diag_sev[slot]), "%s",
           severity ? severity : "");
  va_start(args, detail_fmt);
  if (vsnprintf(dgw_diag_detail[slot], sizeof(dgw_diag_detail[slot]),
                detail_fmt, args) < 0)
    dgw_diag_detail[slot][0] = '\0';
  va_end(args);
  /* Keep the record visible in the test output too. */
  printf("[diag %s %s @%s] %s\n", dgw_diag_sev[slot], dgw_diag_op[slot],
         source_func ? source_func : "?", dgw_diag_detail[slot]);
  (void)source_file;
}

/* Number of captured records whose operation is op. */
static inline int dgw_saw(const char *op)
{
  int i, n = 0;
  for (i = 0; i < dgw_diag_count; i++)
    if (!strcmp(dgw_diag_op[i], op))
      n++;
  return n;
}

/* Detail text of the most recent record with operation op, or "". */
static inline const char *dgw_detail(const char *op)
{
  int i;
  for (i = dgw_diag_count - 1; i >= 0; i--)
    if (!strcmp(dgw_diag_op[i], op))
      return dgw_diag_detail[i];
  return "";
}

/* ---- world ------------------------------------------------------------- */

static struct room_data dgw_world[DGW_NUM_ROOMS];
static struct index_data dgw_obj_index[DGW_NUM_OBJS];
static struct obj_data dgw_obj_proto[DGW_NUM_OBJS];
static struct index_data dgw_mob_index[DGW_NUM_MOBS];
static struct char_data dgw_mob_proto[DGW_NUM_MOBS];
#define DGW_TRIG_VNUM 400
#define DGW_MAX_CMDLISTS 16
static struct index_data dgw_trig_index0;
static struct index_data *dgw_trig_index[1];
static struct cmdlist_element *dgw_cmdlists[DGW_MAX_CMDLISTS];
static int dgw_num_cmdlists;

static inline void dgw_obj_proto_set(int i, obj_vnum vnum, int type, char *name,
                              char *shortdesc, int weight)
{
  struct obj_data *p = &dgw_obj_proto[i];
  memset(p, 0, sizeof(*p));
  dgw_obj_index[i].vnum = vnum;
  p->item_number = i;
  p->in_room = NOWHERE;
  p->name = name;
  p->short_description = shortdesc;
  p->description = shortdesc;
  GET_OBJ_TYPE(p) = type;
  GET_OBJ_WEIGHT(p) = weight;
  SET_BIT_AR(GET_OBJ_WEAR(p), ITEM_WEAR_TAKE);
}

static inline void dgw_mob_proto_set(int i, mob_vnum vnum, char *name, char *shortdesc)
{
  struct char_data *p = &dgw_mob_proto[i];
  memset(p, 0, sizeof(*p));
  dgw_mob_index[i].vnum = vnum;
  p->nr = i;
  p->in_room = NOWHERE;
  p->player.name = name;
  p->player.short_descr = shortdesc;
  p->player.long_descr = shortdesc;
  p->player_specials = &dummy_mob;
  p->player.level = 10;
  SET_BIT_AR(MOB_FLAGS(p), MOB_ISNPC);
  GET_POS(p) = POS_STANDING;
  GET_DEFAULT_POS(p) = POS_STANDING;
}

static inline void dgw_setup(void)
{
  int i;

  dgw_diag_count = 0;
  memset(dgw_world, 0, sizeof(dgw_world));
  for (i = 0; i < DGW_NUM_ROOMS; i++) {
    dgw_world[i].number = DGW_ROOM0 + i;
    dgw_world[i].name = "a test room";
  }
  world = dgw_world;
  top_of_world = DGW_NUM_ROOMS - 1;

  dgw_obj_proto_set(0, DGW_OBJ_BAG,   ITEM_CONTAINER, "zzbag bag", "a bag", 5);
  GET_OBJ_VAL(&dgw_obj_proto[0], 0) = 50;
  dgw_obj_proto_set(1, DGW_OBJ_STONE, ITEM_OTHER, "zzstone stone", "a stone", 2);
  dgw_obj_proto_set(2, DGW_OBJ_BENCH, ITEM_FURNITURE, "zzbench bench", "a bench", 30);
  GET_OBJ_VAL(&dgw_obj_proto[2], 0) = 2;
  dgw_obj_proto_set(3, DGW_OBJ_SKIN,  ITEM_DRINKCON, "zzskin waterskin", "a waterskin", 1);
  GET_OBJ_VAL(&dgw_obj_proto[3], 0) = 10;
  dgw_obj_proto_set(4, DGW_OBJ_SWORD, ITEM_WEAPON, "zzsword sword", "a sword", 4);
  dgw_obj_proto_set(5, DGW_OBJ_CHEST, ITEM_CONTAINER, "zzchest chest", "a chest", 10);
  GET_OBJ_VAL(&dgw_obj_proto[5], 0) = 100;
  dgw_obj_proto_set(6, DGW_OBJ_ROCK,  ITEM_OTHER, "zzrock rock", "a rock", 20);
  obj_proto = dgw_obj_proto;
  obj_index = dgw_obj_index;
  top_of_objt = DGW_NUM_OBJS - 1;

  dgw_mob_proto_set(0, DGW_MOB_GUARD, "zzguard guard", "a guard");
  dgw_mob_proto_set(1, DGW_MOB_OGRE,  "zzogre ogre", "an ogre");
  mob_proto = dgw_mob_proto;
  mob_index = dgw_mob_index;
  top_of_mobt = DGW_NUM_MOBS - 1;

  dgw_trig_index0.vnum = DGW_TRIG_VNUM;
  dgw_trig_index0.number = 0;
  dgw_trig_index[0] = &dgw_trig_index0;
  trig_index = dgw_trig_index;
  top_of_trigt = 1;
  dgw_num_cmdlists = 0;

}

/* An object placed in room rnum (or nowhere when rnum == NOWHERE). */
static inline struct obj_data *dgw_obj(obj_vnum vnum, room_rnum rnum)
{
  struct obj_data *o = read_object(vnum, VIRTUAL);
  TEST_ASSERT_NOT_NULL(o);
  if (rnum != NOWHERE)
    obj_to_room(o, rnum);
  return o;
}

static inline struct char_data *dgw_mob(mob_vnum vnum, room_rnum rnum)
{
  struct char_data *m = read_mobile(vnum, VIRTUAL);
  TEST_ASSERT_NOT_NULL(m);
  char_to_room(m, rnum);
  return m;
}

/* Triggers: every test trigger is an instance of trigger vnum 400 (rnum 0).
 * Command lists belong to the fixture (free_trigger() does not free them,
 * as in the server where they are shared with the prototype). */

/* A trigger whose body is the NULL-terminated array of script lines. */
static inline struct trig_data *dgw_trigger(int attach_type, long trigger_type,
                                     const char *const lines[])
{
  struct trig_data *t;
  struct cmdlist_element **tail;
  int i;

  CREATE(t, struct trig_data, 1);
  { static unsigned long serial; t->serial = 0x7e570000UL + ++serial; }
  t->nr = 0;
  t->attach_type = attach_type;
  t->trigger_type = trigger_type;
  t->narg = 100;
  t->name = strdup("dgw test trigger");
  tail = &t->cmdlist;
  /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): every caller's list ends in NULL. */
  for (i = 0; lines && lines[i]; i++) {
    CREATE(*tail, struct cmdlist_element, 1);
    (*tail)->cmd = strdup(lines[i]);
    tail = &(*tail)->next;
  }
  TEST_ASSERT_TRUE(dgw_num_cmdlists < DGW_MAX_CMDLISTS);
  dgw_cmdlists[dgw_num_cmdlists++] = t->cmdlist;
  t->next_in_world = trigger_list;
  trigger_list = t;
  dgw_trig_index0.number++;
  return t;
}

/* A script holding one trigger (inert when lines is NULL), so MOB_OR_IMPL
 * and SCRIPT() checks pass. Freed by extract_script(). */
static inline struct script_data *dgw_script_with(int attach_type, long trigger_type,
                                           const char *const lines[])
{
  struct script_data *sc;
  CREATE(sc, struct script_data, 1);
  sc->trig_list = dgw_trigger(attach_type, trigger_type, lines);
  sc->types = trigger_type;
  return sc;
}

static inline struct script_data *dgw_script(void)
{
  return dgw_script_with(MOB_TRIGGER, 0, NULL);
}

/* A connected (unswitched) player who owns a trigger. Not on
 * character_list; release with dgw_free_player(). */
static inline struct char_data *dgw_player(long idnum, room_rnum rnum)
{
  struct char_data *ch;
  CREATE(ch, struct char_data, 1);
  CREATE(ch->player_specials, struct player_special_data, 1);
  CREATE(ch->desc, struct descriptor_data, 1);
  ch->desc->character = ch;
  ch->desc->original = NULL;
  ch->player.name = strdup("Zzplayer");
  ch->player.level = 1;
  ch->nr = NOBODY;
  ch->in_room = NOWHERE;
  GET_IDNUM(ch) = idnum;
  GET_POS(ch) = POS_STANDING;
  ch->player_specials->saved.current_quest = NOTHING;
  ch->script = dgw_script();
  char_to_room(ch, rnum);
  return ch;
}

static inline void dgw_free_player(struct char_data *ch)
{
  struct obj_data *o;
  while ((o = ch->carrying) != NULL) {
    obj_from_char(o);
    extract_obj(o);
  }
  if (IN_ROOM(ch) != NOWHERE)
    char_from_room(ch);
  if (SCRIPT(ch))
    extract_script(ch, MOB_TRIGGER);
  extract_script_mem(SCRIPT_MEM(ch));
  free(ch->desc);
  free(ch->player.name);
  free(ch->player_specials);
  free(ch);
}

static inline void dgw_teardown(void)
{
  struct char_data *ch;
  int i, d;

  for (ch = character_list; ch; ch = ch->next)
    extract_char(ch);
  extract_pending_chars();
  while (object_list)
    extract_obj(object_list);
  for (i = 0; i < DGW_NUM_ROOMS; i++) {
    for (d = 0; d < NUM_OF_DIRS; d++) {
      struct room_direction_data *e = dgw_world[i].dir_option[d];
      if (e) {
        free(e->general_description);
        free(e->keyword);
        free(e);
      }
    }
    if (dgw_world[i].script)
      extract_script(&dgw_world[i], WLD_TRIGGER);
    TEST_ASSERT_NULL_MESSAGE(dgw_world[i].people, "room left occupied");
    TEST_ASSERT_NULL_MESSAGE(dgw_world[i].contents, "room left with objects");
  }
  for (i = 0; i < DGW_NUM_OBJS; i++)
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dgw_obj_index[i].number,
                                  "obj_index instance count drifted");
  for (i = 0; i < DGW_NUM_MOBS; i++)
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dgw_mob_index[i].number,
                                  "mob_index instance count drifted");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, dgw_trig_index0.number,
                                "trigger instance count drifted");
  for (i = 0; i < dgw_num_cmdlists; i++) {
    struct cmdlist_element *c = dgw_cmdlists[i], *next;
    for (; c; c = next) {
      next = c->next;
      free(c->cmd);
      free(c);
    }
  }
  dgw_num_cmdlists = 0;
  world = NULL;
  top_of_world = 0;
}

#endif
