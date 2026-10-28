/**
 * @file test_oasis_copy.c
 * Regression tests for src/oasis_copy.c (rcopy/dig/buildwalk) and the
 * descriptor teardown in src/oasis.c (cleanup_olc).
 *
 * The real oasis_copy.c, oasis.c and oasis_delete.c are linked; the editor
 * back ends they call (redit_setup_existing, redit_save_internally, the
 * free_* helpers) are replaced by small fakes that record what they were
 * handed.  That lets a test assert on the contract between the copy command
 * and the editor (e.g. "a new-room template never carries the source room's
 * object list") without booting a world.
 *
 * All fixture memory is heap-allocated and released in tearDown, so a leak
 * in the code under test is reported by LeakSanitizer / Valgrind.
 * Diagnostics are asserted on their stable op= token.
 */

#include "unity.h" /* before utils.h: Unity pulls in math.h, utils.h defines log */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "oasis.h"
#include "genolc.h"
#include "genzon.h"
#include "shop.h"
#include "dg_scripts.h"
#include "dg_olc.h"
#include "constants.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Globals the linked sources need that test_stubs.c does not provide.
 * --------------------------------------------------------------------- */
struct zone_data *zone_table        = NULL;
zone_rnum         top_of_zone_table = 0;
struct obj_data  *obj_proto         = NULL;
struct char_data *mob_proto         = NULL;

/* ---------------------------------------------------------------------
 * Output capture.
 * --------------------------------------------------------------------- */
static char sent[16384];

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(sent);
  (void)ch;
  va_start(args, messg);
  if (vsnprintf(sent + used, sizeof(sent) - used, messg, args) < 0)
    sent[used] = '\0';
  va_end(args);
  return strlen(sent) - used;
}

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  size_t used = strlen(sent);
  (void)d;
  va_start(args, txt);
  if (vsnprintf(sent + used, sizeof(sent) - used, txt, args) < 0)
    sent[used] = '\0';
  va_end(args);
  return strlen(sent) - used;
}

/* cleanup_olc() (oasis.c) releases the session's private trigger list
 * through zedit.c's olc_free_script_copy(), which this test does not link;
 * same behaviour. */
void olc_free_script_copy(struct descriptor_data *d)
{
  struct trig_proto_list *tp, *next_tp;

  if (!d || !d->olc)
    return;
  for (tp = OLC_SCRIPT(d); tp; tp = next_tp) {
    next_tp = tp->next;
    free(tp);
  }
  OLC_SCRIPT(d) = NULL;
}

static int act_calls;
/* act() is a macro over act_at() (call-site capture); stub the function. */
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func;
  (void)str; (void)hide_invisible; (void)ch; (void)obj; (void)vict_obj; (void)type;
  act_calls++;
  return NULL;
}

/* ---------------------------------------------------------------------
 * Minimal real implementations of interpreter/db helpers.
 * --------------------------------------------------------------------- */
void skip_spaces(char **string)
{
  while (**string && isspace((unsigned char)**string))
    (*string)++;
}

char *one_argument(char *argument, char *first_arg)
{
  skip_spaces(&argument);
  while (*argument && !isspace((unsigned char)*argument))
    *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}

char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  return one_argument(one_argument(argument, first_arg), second_arg);
}

int is_number(const char *str)
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

int search_block(char *arg, const char **list, int exact)
{
  int i;
  size_t l = strlen(arg);
  (void)exact;
  if (!l)
    return -1;
  for (i = 0; **(list + i) != '\n'; i++)
    if (!strncmp(arg, list[i], l))
      return i;
  return -1;
}

int is_name(const char *str, const char *namelist)
{
  return namelist && strstr(namelist, str) != NULL;
}

room_rnum real_room(room_vnum vnum)
{
  room_rnum i;
  if (!world)
    return NOWHERE;
  for (i = 0; i <= top_of_world; i++)
    if (world[i].number == vnum)
      return i;
  return NOWHERE;
}

zone_rnum real_zone(zone_vnum vnum)
{
  return (zone_table && zone_table[0].number == vnum) ? 0 : NOWHERE;
}

zone_rnum real_zone_by_thing(room_vnum vznum)
{
  return (zone_table && vznum >= zone_table[0].bot && vznum <= zone_table[0].top)
         ? 0 : NOWHERE;
}

room_vnum genolc_zone_bottom(zone_rnum rznum) { return zone_table[rznum].bot; }
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return TRUE; }

obj_rnum  real_object(obj_vnum vnum)   { (void)vnum; return NOTHING; }
mob_rnum  real_mobile(mob_vnum vnum)   { (void)vnum; return NOBODY; }
shop_rnum real_shop(shop_vnum vnum)    { (void)vnum; return NOWHERE; }
trig_rnum real_trigger(trig_vnum vnum) { (void)vnum; return NOTHING; }

void oedit_setup_existing(struct descriptor_data *d, int rnum) { (void)d; (void)rnum; }
void oedit_save_internally(struct descriptor_data *d) { (void)d; }
void medit_setup_existing(struct descriptor_data *d, int rnum) { (void)d; (void)rnum; }
void medit_save_internally(struct descriptor_data *d) { (void)d; }
void sedit_setup_existing(struct descriptor_data *d, int rnum) { (void)d; (void)rnum; }
void sedit_save_internally(struct descriptor_data *d) { (void)d; }
void trigedit_setup_existing(struct descriptor_data *d, int rnum) { (void)d; (void)rnum; }
void trigedit_save(struct descriptor_data *d) { (void)d; }

/* free_* helpers: release what the fakes allocate so leaks stay visible. */
void free_ex_descriptions(struct extra_descr_data *ed)
{
  struct extra_descr_data *next;
  for (; ed; ed = next) {
    next = ed->next;
    free(ed->keyword);
    free(ed->description);
    free(ed);
  }
}

static void free_trig_list(struct trig_proto_list *list)
{
  struct trig_proto_list *next;
  for (; list; list = next) {
    next = list->next;
    free(list);
  }
}

void free_proto_script(void *thing, int type)
{
  struct trig_proto_list **slot = NULL;
  switch (type) {
    case WLD_TRIGGER: slot = &((struct room_data *)thing)->proto_script; break;
    case OBJ_TRIGGER: slot = &((struct obj_data *)thing)->proto_script; break;
    case MOB_TRIGGER: slot = &((struct char_data *)thing)->proto_script; break;
    default: return;
  }
  free_trig_list(*slot);
  *slot = NULL;
}

void free_room(struct room_data *room)
{
  free_strings(room, OASIS_WLD);
  free(room);
}

void free_object_strings(struct obj_data *obj) { (void)obj; }
void free_mobile(struct char_data *mob) { free(mob); }
void free_shop(struct shop_data *shop) { free(shop); }
void free_quest(struct aq_data *quest) { free(quest); }
void free_action(struct social_messg *mess) { free(mess); }
void free_help(struct help_index_element *help) { free(help); }
void free_trigger(struct trig_data *trig) { free(trig); }
void free_olc_ibt(struct ibt_data *ibtData) { free(ibtData); }
void free_message_list(struct message_list *mlist) { free(mlist); }

/* ---------------------------------------------------------------------
 * Fake room editor back end.
 * --------------------------------------------------------------------- */
static struct obj_data source_object;  /* stands in for a live object */

/* Mirrors redit_setup_existing, except it deliberately keeps the source's
 * contents pointer when fake_setup_keeps_contents is set: that models the
 * regression F3-05 describes, so the copy command's own guard is tested. */
static int fake_setup_keeps_contents;

void redit_setup_existing(struct descriptor_data *d, int real_num)
{
  struct room_data *room;
  CREATE(room, struct room_data, 1);
  *room = world[real_num];
  room->people = NULL;
  room->events = NULL;
  if (!fake_setup_keeps_contents)
    room->contents = NULL;
  room->name = strdup(world[real_num].name);
  room->description = strdup(world[real_num].description);
  OLC_ROOM(d) = room;
  OLC_ITEM_TYPE(d) = WLD_TRIGGER;
  OLC_SCRIPT(d) = NULL;
  room->proto_script = NULL;
}

/* What redit_save_internally was handed, for the assertions. */
static int       save_calls;
static room_vnum save_seen_number;
static void     *save_seen_contents;
static int       fake_save_fails;

/* Mirrors the parts of redit_save_internally + add_room the copy commands
 * rely on: the template is copied into a new world[] slot and its strings
 * are duplicated (copy_room_strings); the template keeps its own strings. */
void redit_save_internally(struct descriptor_data *d)
{
  struct room_data *slot;

  save_calls++;
  save_seen_number = OLC_ROOM(d)->number;
  save_seen_contents = OLC_ROOM(d)->contents;
  if (fake_save_fails) {
    write_to_output(d, "Something went wrong...\r\n");
    return;
  }
  RECREATE(world, struct room_data, top_of_world + 2);
  top_of_world++;
  slot = &world[top_of_world];
  *slot = *OLC_ROOM(d);
  slot->number = OLC_NUM(d);
  slot->name = strdup(OLC_ROOM(d)->name ? OLC_ROOM(d)->name : "");
  slot->description = strdup(OLC_ROOM(d)->description ? OLC_ROOM(d)->description : "");
  memset(slot->dir_option, 0, sizeof(slot->dir_option));
  slot->ex_description = NULL;
  slot->proto_script = OLC_SCRIPT(d);
  OLC_SCRIPT(d) = NULL;
}

/* ---------------------------------------------------------------------
 * Fixtures.
 * --------------------------------------------------------------------- */
static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;
static FILE *capture_log;

/* world != NULL means it holds top_of_world + 1 rooms. */
static void add_fixture_room(room_vnum vnum, const char *name)
{
  if (world == NULL) {
    CREATE(world, struct room_data, 1);
    top_of_world = 0;
  } else {
    RECREATE(world, struct room_data, top_of_world + 2);
    top_of_world++;
    memset(&world[top_of_world], 0, sizeof(world[top_of_world]));
  }
  world[top_of_world].number = vnum;
  world[top_of_world].zone = 0;
  world[top_of_world].name = strdup(name);
  world[top_of_world].description = strdup("A fixture room.\r\n");
}

static void free_fixture_world(void)
{
  room_rnum i;
  if (world) {
    for (i = 0; i <= top_of_world; i++) {
      world[i].contents = NULL;
      free_strings(&world[i], OASIS_WLD);
      free_trig_list(world[i].proto_script);
    }
  }
  free(world);
  world = NULL;
  top_of_world = 0;
}

static void run_cmd(void (*cmd)(struct char_data *, char *, int, int),
                    const char *args, int subcmd)
{
  char argbuf[MAX_INPUT_LENGTH];
  strlcpy(argbuf, args, sizeof(argbuf));
  cmd(&builder, argbuf, 0, subcmd);
}

static int log_has(const char *needle)
{
  char line[MAX_STRING_LENGTH];
  int found = 0;
  TEST_ASSERT_EQUAL_INT(0, fflush(capture_log));
  TEST_ASSERT_EQUAL_INT(0, fseek(capture_log, 0L, SEEK_SET));
  while (fgets(line, sizeof(line), capture_log))
    if (strstr(line, needle))
      found = 1;
  TEST_ASSERT_EQUAL_INT(0, fseek(capture_log, 0L, SEEK_END));
  return found;
}

void setUp(void)
{
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  memset(&builder_desc, 0, sizeof(builder_desc));
  builder.player_specials = &builder_specials;
  builder.player.name = (char *)"Builder";
  builder.player.level = LVL_IMPL;
  builder_specials.saved.olc_zone = NOWHERE;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
  STATE(&builder_desc) = CON_PLAYING;
  descriptor_list = &builder_desc;

  CREATE(zone_table, struct zone_data, 1);
  top_of_zone_table = 0;
  zone_table[0].number = 30;
  zone_table[0].bot = 3000;
  zone_table[0].top = 3099;

  add_fixture_room(3001, "The Temple");
  add_fixture_room(3002, "The Altar");
  builder.in_room = 0;

  sent[0] = '\0';
  act_calls = 0;
  save_calls = 0;
  save_seen_number = 0;
  save_seen_contents = NULL;
  fake_save_fails = 0;
  fake_setup_keeps_contents = 0;

  capture_log = tmpfile();
  logfile = capture_log;
}

void tearDown(void)
{
  if (builder_desc.olc)
    cleanup_olc(&builder_desc, CLEANUP_ALL);
  free_fixture_world();
  free(zone_table);
  zone_table = NULL;
  descriptor_list = NULL;
  logfile = NULL;
  if (capture_log)
    (void)fclose(capture_log);
  capture_log = NULL;
}

/* ---------------------------------------------------------------------
 * F3-05: rcopy inserted a template still pointing at the source room's
 * object list, so the new room shared (and later dangled into) it.
 * --------------------------------------------------------------------- */
void test_rcopy_new_room_never_inherits_source_contents(void)
{
  world[0].contents = &source_object;
  fake_setup_keeps_contents = 1;   /* model the regressed editor */

  run_cmd(do_oasis_copy, "3001 3050", CON_REDIT);

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_NULL(save_seen_contents);
  TEST_ASSERT_TRUE(log_has("op=room-copy"));
  TEST_ASSERT_NULL(world[real_room(3050)].contents);
  TEST_ASSERT_NULL(builder_desc.olc);
}

void test_rcopy_with_clean_template_logs_nothing(void)
{
  world[0].contents = &source_object;

  run_cmd(do_oasis_copy, "3001 3050", CON_REDIT);

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_NULL(save_seen_contents);
  TEST_ASSERT_FALSE(log_has("op=room-copy"));
}

/* ---------------------------------------------------------------------
 * F3-06: rcopy left the source vnum in the template, so the editor skipped
 * its new-room renumber pass; and an int vnum above IDXTYPE_MAX was
 * silently narrowed (68538 -> 3002).
 * --------------------------------------------------------------------- */
void test_rcopy_saves_template_as_new_room(void)
{
  run_cmd(do_oasis_copy, "3001 3050", CON_REDIT);

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_EQUAL_INT(NOWHERE, save_seen_number);
  TEST_ASSERT_NOT_EQUAL(NOWHERE, real_room(3050));
}

void test_rcopy_rejects_vnum_that_would_be_narrowed(void)
{
  run_cmd(do_oasis_copy, "3001 68538", CON_REDIT);

  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_EQUAL_INT(1, top_of_world);   /* nothing was added */
  TEST_ASSERT_NOT_NULL(strstr(sent, "Vnums must be numbers"));
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
  TEST_ASSERT_TRUE(log_has("resource=rcopy"));
  TEST_ASSERT_NULL(builder_desc.olc);
}

void test_rcopy_rejects_nowhere_sentinel_and_negative_target(void)
{
  run_cmd(do_oasis_copy, "3001 65535", CON_REDIT);
  TEST_ASSERT_EQUAL_INT(0, save_calls);
  run_cmd(do_oasis_copy, "3001 -1", CON_REDIT);
  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
}

/* ---------------------------------------------------------------------
 * F3-07: cleanup_olc freed OLC_CONFIG only inside the OLC_ROOM branch,
 * which cedit never populates, so the config copy and its strings leaked
 * on every cedit exit.  The leak is what fails this test (LeakSanitizer /
 * Valgrind definite leak) if the release regresses.
 * --------------------------------------------------------------------- */
static void attach_config_editor(void)
{
  CREATE(builder_desc.olc, struct oasis_olc_data, 1);
  CREATE(OLC_CONFIG(&builder_desc), struct config_data, 1);
  OLC_CONFIG(&builder_desc)->play.OK = strdup("Ok.\r\n");
  OLC_CONFIG(&builder_desc)->operation.MENU = strdup("menu");
  OLC_CONFIG(&builder_desc)->operation.LOGNAME = strdup("log/syslog");
  STATE(&builder_desc) = CON_CEDIT;
}

void test_cleanup_config_releases_config_copy(void)
{
  attach_config_editor();

  cleanup_olc(&builder_desc, CLEANUP_CONFIG);

  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&builder_desc));
}

void test_cleanup_all_releases_config_copy_on_link_loss(void)
{
  attach_config_editor();

  cleanup_olc(&builder_desc, CLEANUP_ALL);   /* what close_socket passes */

  TEST_ASSERT_NULL(builder_desc.olc);
}

/* ---------------------------------------------------------------------
 * F3-08: OLC_SCRIPT (the editor's private trigger list) was freed only on
 * the explicit 'n' paths; quitting without changes or losing link leaked it.
 * --------------------------------------------------------------------- */
static struct trig_proto_list *make_script(int n)
{
  struct trig_proto_list *head = NULL, *node;
  while (n-- > 0) {
    CREATE(node, struct trig_proto_list, 1);
    node->vnum = 3000 + n;
    node->next = head;
    head = node;
  }
  return head;
}

static void attach_room_editor(room_vnum vnum)
{
  CREATE(builder_desc.olc, struct oasis_olc_data, 1);
  CREATE(OLC_ROOM(&builder_desc), struct room_data, 1);
  OLC_ROOM(&builder_desc)->name = strdup("scratch");
  OLC_ROOM(&builder_desc)->description = strdup("scratch\r\n");
  OLC_NUM(&builder_desc) = vnum;
  OLC_ZNUM(&builder_desc) = 0;
  OLC_SCRIPT(&builder_desc) = make_script(2);
  STATE(&builder_desc) = CON_REDIT;
}

void test_cleanup_frees_unsaved_script_on_link_loss(void)
{
  attach_room_editor(3001);

  cleanup_olc(&builder_desc, CLEANUP_ALL);

  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_FALSE(log_has("op=script-owner"));
}

void test_cleanup_frees_script_reattached_to_scratch_once(void)
{
  attach_room_editor(3001);
  /* redit/medit 'n' path: hand the list back to the scratch entity. */
  OLC_ROOM(&builder_desc)->proto_script = OLC_SCRIPT(&builder_desc);

  cleanup_olc(&builder_desc, CLEANUP_ALL);   /* ASan: no double free */

  TEST_ASSERT_NULL(builder_desc.olc);
}

void test_cleanup_never_frees_script_owned_by_live_room(void)
{
  attach_room_editor(3001);
  /* A save path that forgot to clear OLC_SCRIPT after the hand-off. */
  world[0].proto_script = OLC_SCRIPT(&builder_desc);

  cleanup_olc(&builder_desc, CLEANUP_ALL);

  TEST_ASSERT_TRUE(log_has("op=script-owner"));
  TEST_ASSERT_NOT_NULL(world[0].proto_script);
  TEST_ASSERT_EQUAL_INT(3000, world[0].proto_script->vnum); /* still valid */
}

/* ---------------------------------------------------------------------
 * F3-09: buildwalk released its template with CLEANUP_STRUCTS, leaking the
 * strdup'd name and description (add_room copies them).  LeakSanitizer /
 * Valgrind fail this test if that regresses.
 * --------------------------------------------------------------------- */
void test_buildwalk_releases_template_strings(void)
{
  SET_BIT_AR(PRF_FLAGS(&builder), PRF_BUILDWALK);

  TEST_ASSERT_EQUAL_INT(1, buildwalk(&builder, NORTH));

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_NOT_EQUAL(NOWHERE, real_room(3000));
  TEST_ASSERT_NOT_NULL(world[real_room(3000)].name);
}

/* ---------------------------------------------------------------------
 * F3-10: dig/buildwalk free()d an existing d->olc and replaced it, leaking
 * the open editor's scratch entity (reachable via do_force on a builder
 * who is in medit).  They must refuse and leave the editor intact.
 * --------------------------------------------------------------------- */
static struct oasis_olc_data *attach_mob_editor(void)
{
  CREATE(builder_desc.olc, struct oasis_olc_data, 1);
  CREATE(OLC_MOB(&builder_desc), struct char_data, 1);
  OLC_NUM(&builder_desc) = 3001;
  STATE(&builder_desc) = CON_MEDIT;
  return builder_desc.olc;
}

void test_dig_refuses_while_another_editor_is_open(void)
{
  struct oasis_olc_data *open_editor = attach_mob_editor();

  run_cmd(do_dig, "north 3050", 0);

  TEST_ASSERT_EQUAL_PTR(open_editor, builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_NOT_NULL(strstr(sent, "already editing"));
  TEST_ASSERT_TRUE(log_has("op=olc-busy"));
  TEST_ASSERT_TRUE(log_has("resource=dig"));
}

void test_buildwalk_refuses_while_another_editor_is_open(void)
{
  struct oasis_olc_data *open_editor = attach_mob_editor();
  SET_BIT_AR(PRF_FLAGS(&builder), PRF_BUILDWALK);

  TEST_ASSERT_EQUAL_INT(0, buildwalk(&builder, NORTH));

  TEST_ASSERT_EQUAL_PTR(open_editor, builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_TRUE(log_has("op=olc-busy"));
  TEST_ASSERT_TRUE(log_has("resource=buildwalk"));
}

void test_dig_creates_room_when_idle(void)
{
  run_cmd(do_dig, "north 3050", 0);

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_NOT_EQUAL(NOWHERE, real_room(3050));
  TEST_ASSERT_FALSE(log_has("op=olc-busy"));
}

/* ---------------------------------------------------------------------
 * F3-14: do_oasis_copy returned silently for an unknown subcmd and for a
 * caller it refuses, and printed "Done." even when the save failed.
 * --------------------------------------------------------------------- */
void test_copy_reports_failed_save_instead_of_done(void)
{
  fake_save_fails = 1;

  run_cmd(do_oasis_copy, "3001 3050", CON_REDIT);

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_NULL(strstr(sent, "Done."));
  TEST_ASSERT_NOT_NULL(strstr(sent, "The copy failed"));
  TEST_ASSERT_TRUE(log_has("op=copy-failed"));
  TEST_ASSERT_TRUE(log_has("resource=rcopy"));
  TEST_ASSERT_NULL(builder_desc.olc);
}

void test_copy_success_still_says_done(void)
{
  run_cmd(do_oasis_copy, "3001 3050", CON_REDIT);

  TEST_ASSERT_NOT_NULL(strstr(sent, "Done."));
  TEST_ASSERT_FALSE(log_has("op=copy-failed"));
}

void test_copy_unknown_subcmd_is_reported(void)
{
  run_cmd(do_oasis_copy, "3001 3050", CON_ZEDIT);

  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_TRUE(log_has("op=copy-dispatch"));
}

void test_copy_while_editing_tells_the_builder(void)
{
  struct oasis_olc_data *open_editor = attach_mob_editor();

  run_cmd(do_oasis_copy, "3001 3050", CON_REDIT);

  TEST_ASSERT_EQUAL_PTR(open_editor, builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_NOT_NULL(strstr(sent, "while editing"));
  TEST_ASSERT_TRUE(log_has("op=copy-refused"));
}

/* ---------------------------------------------------------------------
 * F3-12: a 'V' command's sarg strings typed in zedit (or copied from the
 * zone by zedit_setup) were never freed when the editor closed.  The leak
 * is what fails this test (LeakSanitizer / Valgrind) if it regresses.
 * --------------------------------------------------------------------- */
void test_cleanup_frees_zone_command_sargs(void)
{
  struct zone_data *zone;

  CREATE(builder_desc.olc, struct oasis_olc_data, 1);
  CREATE(zone, struct zone_data, 1);
  zone->name = strdup("Scratch Zone");
  CREATE(zone->cmd, struct reset_com, 3);
  zone->cmd[0].command = 'V';
  zone->cmd[0].sarg1 = strdup("questflag");
  zone->cmd[0].sarg2 = strdup("1");
  zone->cmd[1].command = 'M';             /* no strings: NULL sargs */
  zone->cmd[2].command = 'S';
  OLC_ZONE(&builder_desc) = zone;
  STATE(&builder_desc) = CON_ZEDIT;

  cleanup_olc(&builder_desc, CLEANUP_ALL);

  TEST_ASSERT_NULL(builder_desc.olc);
}

/* ---------------------------------------------------------------------
 * F3-18: dig narrowed its target vnum to room_vnum with no range check
 * (68001 -> 2465; 65535 == NOWHERE deleted the exit) and treated a
 * non-numeric target as limbo.
 * --------------------------------------------------------------------- */
void test_dig_rejects_vnum_that_would_be_narrowed(void)
{
  run_cmd(do_dig, "north 68001", 0);

  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_NULL(world[0].dir_option[NORTH]);
  TEST_ASSERT_NOT_NULL(strstr(sent, "is not a room vnum"));
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
  TEST_ASSERT_TRUE(log_has("resource=dig"));
}

void test_dig_nowhere_sentinel_does_not_delete_exit(void)
{
  struct room_direction_data *exit;

  CREATE(exit, struct room_direction_data, 1);
  exit->to_room = 1;
  world[0].dir_option[NORTH] = exit;

  run_cmd(do_dig, "north 65535", 0);

  TEST_ASSERT_EQUAL_PTR(exit, world[0].dir_option[NORTH]);
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
  world[0].dir_option[NORTH] = NULL;
  free(exit);
}

void test_dig_rejects_non_numeric_target(void)
{
  run_cmd(do_dig, "north temple", 0);

  TEST_ASSERT_EQUAL_INT(0, save_calls);
  TEST_ASSERT_NULL(strstr(sent, "limbo"));
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
}

/* ---------------------------------------------------------------------
 * F3-13: an unknown cleanup type logged a bare "Unknown type!" and leaked
 * the room; internal one-shot users (dig/buildwalk/copy) broadcast
 * "$n stops using OLC." for an editor they never opened.
 * --------------------------------------------------------------------- */
void test_cleanup_unknown_type_reports_and_still_frees(void)
{
  CREATE(builder_desc.olc, struct oasis_olc_data, 1);
  CREATE(OLC_ROOM(&builder_desc), struct room_data, 1);  /* no strings */
  OLC_NUM(&builder_desc) = 3001;
  STATE(&builder_desc) = CON_REDIT;

  cleanup_olc(&builder_desc, 99);

  TEST_ASSERT_NULL(builder_desc.olc);         /* room struct freed: LSan */
  TEST_ASSERT_TRUE(log_has("op=cleanup-type"));
  TEST_ASSERT_TRUE(log_has("olc_num 3001"));
}

void test_internal_dig_does_not_announce_leaving_olc(void)
{
  run_cmd(do_dig, "north 3050", 0);

  TEST_ASSERT_EQUAL_INT(1, save_calls);
  TEST_ASSERT_EQUAL_INT(0, act_calls);
}

void test_real_editor_exit_still_announces_and_resets_state(void)
{
  attach_room_editor(3001);
  SET_BIT_AR(PLR_FLAGS(&builder), PLR_WRITING);

  cleanup_olc(&builder_desc, CLEANUP_ALL);

  TEST_ASSERT_EQUAL_INT(1, act_calls);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&builder_desc));
  TEST_ASSERT_FALSE(PLR_FLAGGED(&builder, PLR_WRITING));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_cleanup_unknown_type_reports_and_still_frees);
  RUN_TEST(test_internal_dig_does_not_announce_leaving_olc);
  RUN_TEST(test_real_editor_exit_still_announces_and_resets_state);
  RUN_TEST(test_dig_refuses_while_another_editor_is_open);
  RUN_TEST(test_buildwalk_refuses_while_another_editor_is_open);
  RUN_TEST(test_dig_creates_room_when_idle);
  RUN_TEST(test_cleanup_frees_zone_command_sargs);
  RUN_TEST(test_copy_reports_failed_save_instead_of_done);
  RUN_TEST(test_copy_success_still_says_done);
  RUN_TEST(test_copy_unknown_subcmd_is_reported);
  RUN_TEST(test_copy_while_editing_tells_the_builder);
  RUN_TEST(test_dig_rejects_vnum_that_would_be_narrowed);
  RUN_TEST(test_dig_nowhere_sentinel_does_not_delete_exit);
  RUN_TEST(test_dig_rejects_non_numeric_target);
  RUN_TEST(test_buildwalk_releases_template_strings);
  RUN_TEST(test_cleanup_frees_unsaved_script_on_link_loss);
  RUN_TEST(test_cleanup_frees_script_reattached_to_scratch_once);
  RUN_TEST(test_cleanup_never_frees_script_owned_by_live_room);
  RUN_TEST(test_cleanup_config_releases_config_copy);
  RUN_TEST(test_cleanup_all_releases_config_copy_on_link_loss);
  RUN_TEST(test_rcopy_saves_template_as_new_room);
  RUN_TEST(test_rcopy_rejects_vnum_that_would_be_narrowed);
  RUN_TEST(test_rcopy_rejects_nowhere_sentinel_and_negative_target);
  RUN_TEST(test_rcopy_new_room_never_inherits_source_contents);
  RUN_TEST(test_rcopy_with_clean_template_logs_nothing);
  return UNITY_END();
}
