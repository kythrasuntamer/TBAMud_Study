/* Executable tests of the production configuration editor (cedit.c) and of
 * the configuration file it writes.
 *
 * cedit.c is linked with the production loader (db.c load_config()), the
 * compiled-in defaults the loader starts from (config.c), GenOLC's save list
 * (genolc.c), the line, log and string helpers (utils.c) and the tag
 * splitter the loader uses (oasis.c split_argument()).  Their unrelated code
 * is discarded with --gc-sections (tests/CMakeLists.txt); everything else
 * they reach is supplied here.  test_stubs.c is not linked because db.c and
 * config.c define the globals it stubs.
 *
 * The write-then-reload tests pin the contract between save_config() and
 * load_config(): every setting the writer emits must use a key the loader
 * reads, or the setting silently reverts to its config.c default at the
 * next boot.
 *
 * Two functions are linked with --wrap so this file supplies them:
 * tb_error_log_at(), to capture diagnostics (tb_error_capture.h), and
 * cleanup_olc(), so the editor's exits are checked here independently of
 * the oasis.c version this branch carries.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "genolc.h"
#include "oasis.h"
#include "config.h"
#include "improved-edit.h"
#include "modify.h"
#include "spec_procs.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

/* utils.c defines the real tb_error_log_at(); the linker sends every call
 * from the other objects to __wrap_tb_error_log_at, which the capture
 * header defines under that name.  The prototype carries the real one's
 * format attribute, so the capture's vsnprintf() of detail_fmt is checked
 * as a format parameter. */
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* ---------------------------------------------------------------------
 * comm.c globals and output capture
 * ------------------------------------------------------------------- */
FILE *logfile = NULL;
struct descriptor_data *descriptor_list = NULL;

static char out[MAX_STRING_LENGTH];

static void out_append(const char *fmt, va_list args)
  __attribute__ ((format (printf, 1, 0)));

static void out_append(const char *fmt, va_list args)
{
  size_t len = strlen(out);

  if (len < sizeof(out) - 1 &&
      vsnprintf(out + len, sizeof(out) - len, fmt, args) < 0)
    out[len] = '\0';
}

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  (void)d;
  va_start(args, txt);
  out_append(txt, args);
  va_end(args);
  return strlen(out);
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  (void)ch;
  va_start(args, messg);
  out_append(messg, args);
  va_end(args);
  return strlen(out);
}

char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func; (void)str;
  (void)hide_invisible; (void)ch; (void)obj; (void)vict_obj; (void)type;
  return NULL;
}

/* ---------------------------------------------------------------------
 * Collaborators outside the linked files
 * ------------------------------------------------------------------- */
char *one_argument(char *argument, char *first_arg)
{
  char *start;
  size_t n;

  while (*argument == ' ')
    argument++;
  start = argument;
  while (*argument && *argument != ' ')
    argument++;
  n = (size_t)(argument - start);
  if (n >= MAX_INPUT_LENGTH)
    n = MAX_INPUT_LENGTH - 1;
  memcpy(first_arg, start, n);
  first_arg[n] = '\0';
  return argument;
}

/* modify.c / improved-edit.c.  modify.c is not linked: with Clang's ASan
 * its string_add() dispatch table keeps every editor's cleanup hook alive
 * through --gc-sections.  The multi-line editor (MENU, WELC_MESSG,
 * START_MESSG) is not under test, so string_write() only releases the
 * old-text copy it is handed; the three string helpers below are
 * modify.c's, because the writer and loader round trip depends on them. */
void string_write(struct descriptor_data *d, char **writeto, size_t len,
                  long mailto, void *data)
{ (void)d; (void)writeto; (void)len; (void)mailto; free(data); }
void send_editor_help(struct descriptor_data *d) { (void)d; }

/* As modify.c: blank a tilde that ends a line; the loader cannot read it. */
void smash_tilde(char *str)
{
  char *p = str;

  for (; *p; p++)
    if (*p == '~' && (*(p + 1) == '\r' || *(p + 1) == '\n' || *(p + 1) == '\0'))
      *p = ' ';
}

/* As modify.c: '@' becomes the '\t' colour escape, "@@" stays '@'. */
void parse_at(char *str)
{
  char *p = str;

  for (; *p; p++)
    if (*p == '@') {
      if (*(p + 1) != '@')
        *p = '\t';
      else
        p++;
    }
}

/* As modify.c: the inverse of parse_at(), used when writing files. */
void parse_tab(char *str)
{
  char *p = str;

  for (; *p; p++)
    if (*p == '\t') {
      if (*(p + 1) != '\t')
        *p = '@';
      else
        p++;
    }
}

/* spec_assign.c / spec_procs.c: cedit re-runs assign_rooms() when the
 * dts_are_dumps option changes. */
static int assign_rooms_calls;
void assign_rooms(void) { assign_rooms_calls++; }
SPECIAL(dump)
{ (void)ch; (void)me; (void)cmd; (void)argument; return FALSE; }

/* GenOLC's save_all() table names every saver; only save_config (cedit.c)
 * is real here, and reaching another one is a test failure. */
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
int save_mobiles(zone_rnum rznum) { (void)rznum; UNREACHED(); return FALSE; }
int save_objects(zone_rnum rznum) { (void)rznum; UNREACHED(); return FALSE; }
int save_shops(zone_rnum rznum) { (void)rznum; UNREACHED(); return FALSE; }
int save_rooms(zone_rnum rznum) { (void)rznum; UNREACHED(); return FALSE; }
int save_zone(zone_rnum rznum) { (void)rznum; UNREACHED(); return FALSE; }
int save_quests(zone_rnum rznum) { (void)rznum; UNREACHED(); return FALSE; }

/* What oasis.c's cleanup_olc() does for a cedit workspace once the config
 * copy is released for every cleanup type (audit F3-07): free the copy and
 * its strings, then the workspace, and return to CON_PLAYING. */
static int cleanups;
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  struct config_data *cfg;

  (void)cleanup_type;
  if (d->olc == NULL)
    return;
  cleanups++;
  cfg = OLC_CONFIG(d);
  if (cfg) {
    free(cfg->play.OK);
    free(cfg->play.HUH);
    free(cfg->play.NOPERSON);
    free(cfg->play.NOEFFECT);
    free(cfg->operation.DFLT_IP);
    free(cfg->operation.DFLT_DIR);
    free(cfg->operation.LOGNAME);
    free(cfg->operation.MENU);
    free(cfg->operation.WELC_MESSG);
    free(cfg->operation.START_MESSG);
    free(cfg);
  }
  if (d->character)
    REMOVE_BIT_AR(PLR_FLAGS(d->character), PLR_WRITING);
  STATE(d) = CON_PLAYING;
  free(d->olc);
  d->olc = NULL;
}

/* ---------------------------------------------------------------------
 * Fixtures: one zone (save_all() looks the config's zone up), rooms for
 * the tests that need them, a builder at LVL_IMPL, and a scratch
 * directory for the configuration file.
 * ------------------------------------------------------------------- */
static struct zone_data fixture_zone;
static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;

static void world_fixture(void)
{
  memset(&fixture_zone, 0, sizeof(fixture_zone));
  fixture_zone.number = 30;
  fixture_zone.bot = 3000;
  fixture_zone.top = 3099;
  zone_table = &fixture_zone;
  top_of_zone_table = 0;
}

/* A room spec proc that the dts_are_dumps option does not control. */
static SPECIAL(other_room_proc)
{ (void)ch; (void)me; (void)cmd; (void)argument; return FALSE; }

/* Three rooms; top_of_world is the last rnum, inclusive.  Rooms 0 and 2
 * are dumps, room 1 has another spec proc.  Heap-allocated at the exact
 * size so ASan reports any index past the end. */
static void rooms_fixture(void)
{
  room_rnum i;

  top_of_world = 2;
  world = calloc((size_t)top_of_world + 1, sizeof(*world));
  TEST_ASSERT_NOT_NULL(world);
  for (i = 0; i <= top_of_world; i++)
    world[i].number = (room_vnum)(3000 + i);
  world[0].func = dump;
  world[1].func = other_room_proc;
  world[2].func = dump;
}

static void rooms_fixture_free(void)
{
  free(world);
  world = NULL;
  top_of_world = 0;
}

static void builder_fixture(void)
{
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  memset(&builder_desc, 0, sizeof(builder_desc));
  builder.player_specials = &builder_specials;
  builder.player.name = "Builder";
  builder.player.level = LVL_IMPL;
  builder.char_specials.saved.idnum = 42;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;
  builder_desc.connected = CON_PLAYING;
}

static char scratch_dir[256];
static char conf_path[512];

static void enter_scratch_dir(void)
{
  int n;

  strlcpy(scratch_dir, "/tmp/tb-cedit-test-XXXXXX", sizeof(scratch_dir));
  TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
  n = snprintf(conf_path, sizeof(conf_path), "%s/config", scratch_dir);
  TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(conf_path));
}

/* Remove path and, if it is a directory, everything below it (no symlink
 * following: lstat).  Returns 0 on success. */
static int remove_tree(const char *path)
{
  struct stat st;
  DIR *dir;
  struct dirent *ent;
  char child[1024];
  int rc = 0;

  if (lstat(path, &st) != 0)
    return -1;
  if (!S_ISDIR(st.st_mode))
    return remove(path);
  if (!(dir = opendir(path)))
    return -1;
  while ((ent = readdir(dir)) != NULL) {
    int n;

    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    if (n < 0 || (size_t)n >= sizeof(child) || remove_tree(child) != 0)
      rc = -1;
  }
  if (closedir(dir) != 0)
    rc = -1;
  if (rmdir(path) != 0)
    rc = -1;
  return rc;
}

/* Every string config_info owns, CONFFILE included. */
static void free_config_strings(void)
{
  free(CONFIG_OK);
  free(CONFIG_HUH);
  free(CONFIG_NOPERSON);
  free(CONFIG_NOEFFECT);
  free(CONFIG_DFLT_IP);
  free(CONFIG_DFLT_DIR);
  free(CONFIG_LOGNAME);
  free(CONFIG_MENU);
  free(CONFIG_WELC_MESSG);
  free(CONFIG_START_MESSG);
  free(CONFIG_CONFFILE);
  memset(&config_info, 0, sizeof(config_info));
}

/* What the next boot sees: config.c's defaults, then the file at path
 * (comm.c sets CONFFILE before load_config()). */
static void boot_config_from(const char *path)
{
  free_config_strings();
  CONFIG_CONFFILE = strdup(path);
  TEST_ASSERT_NOT_NULL(CONFIG_CONFFILE);
  load_config();
}

/* Starts every test from config.c's defaults and an empty config file. */
static void boot_default_config(void)
{
  FILE *fl = fopen(conf_path, "w");

  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  boot_config_from(conf_path);
}

/* Write the live configuration with the production writer, then reboot
 * it from the file it wrote. */
static void save_and_reboot(void)
{
  TEST_ASSERT_TRUE(save_config(NOWHERE));
  boot_config_from(conf_path);
}

void setUp(void)
{
  tb_error_capture_reset();
  out[0] = '\0';
  cleanups = assign_rooms_calls = 0;
  logfile = stderr;
  world_fixture();
  builder_fixture();
  descriptor_list = &builder_desc;
  enter_scratch_dir();
  boot_default_config();
}

void tearDown(void)
{
  if (builder_desc.olc)
    __wrap_cleanup_olc(&builder_desc, CLEANUP_ALL);
  rooms_fixture_free();
  free_save_list();
  save_list = NULL;
  free_config_strings();
  descriptor_list = NULL;
  zone_table = NULL;
  TEST_ASSERT_EQUAL_INT(0, remove_tree(scratch_dir));
}

/* ---------------------------------------------------------------------
 * save_config() -> load_config() round trips
 * ------------------------------------------------------------------- */

/* The writer emitted "disp_closed_doors" but load_config() reads
 * "display_closed_doors", so the setting reverted to its default at every
 * boot. */
static void test_display_closed_doors_survives_save_and_reboot(void)
{
  int changed = !display_closed_doors;

  CONFIG_DISP_CLOSED_DOORS = changed;
  save_and_reboot();
  TEST_ASSERT_EQUAL_INT(changed, CONFIG_DISP_CLOSED_DOORS);
}

/* min_rent_cost (Crashsave/Rent option C) was never written, so a changed
 * surcharge reverted to config.c's default at every boot. */
static void test_min_rent_cost_survives_save_and_reboot(void)
{
  int changed = min_rent_cost + 4321;

  CONFIG_MIN_RENT_COST = changed;
  save_and_reboot();
  TEST_ASSERT_EQUAL_INT(changed, CONFIG_MIN_RENT_COST);
}

/* Every setting cedit edits, each with a value that is not its config.c
 * default.  The same lists drive both setting and checking, so a field
 * cannot be set without being checked. */
#define EVERY_NUMERIC_SETTING(X) \
  X(CONFIG_PK_SETTING, 2) \
  X(CONFIG_PT_SETTING, 1) \
  X(CONFIG_LEVEL_CAN_SHOUT, 7) \
  X(CONFIG_HOLLER_MOVE_COST, 33) \
  X(CONFIG_TUNNEL_SIZE, 5) \
  X(CONFIG_MAX_EXP_GAIN, 123456) \
  X(CONFIG_MAX_EXP_LOSS, 654321) \
  X(CONFIG_MAX_NPC_CORPSE_TIME, 9) \
  X(CONFIG_MAX_PC_CORPSE_TIME, 17) \
  X(CONFIG_IDLE_VOID, 11) \
  X(CONFIG_IDLE_RENT_TIME, 52) \
  X(CONFIG_IDLE_MAX_LEVEL, LVL_IMMORT + 2) \
  X(CONFIG_DTS_ARE_DUMPS, !dts_are_dumps) \
  X(CONFIG_LOAD_INVENTORY, !load_into_inventory) \
  X(CONFIG_TRACK_T_DOORS, !track_through_doors) \
  X(CONFIG_NO_MORT_TO_IMMORT, !no_mort_to_immort) \
  X(CONFIG_DISP_CLOSED_DOORS, !display_closed_doors) \
  X(CONFIG_DIAGONAL_DIRS, !diagonal_dirs) \
  X(CONFIG_MAP, map_option == 1 ? 0 : 1) \
  X(CONFIG_MAP_SIZE, 9) \
  X(CONFIG_MINIMAP_SIZE, 4) \
  X(CONFIG_SCRIPT_PLAYERS, !script_players) \
  X(CONFIG_FREE_RENT, !free_rent) \
  X(CONFIG_MAX_OBJ_SAVE, 45) \
  X(CONFIG_MIN_RENT_COST, 250) \
  X(CONFIG_AUTO_SAVE, !auto_save) \
  X(CONFIG_AUTOSAVE_TIME, 12) \
  X(CONFIG_CRASH_TIMEOUT, 14) \
  X(CONFIG_RENT_TIMEOUT, 40) \
  X(CONFIG_MORTAL_START, 3005) \
  X(CONFIG_IMMORTAL_START, 1210) \
  X(CONFIG_FROZEN_START, 1203) \
  X(CONFIG_DON_ROOM_1, 3070) \
  X(CONFIG_DON_ROOM_2, 5520) \
  X(CONFIG_DON_ROOM_3, NOWHERE) \
  X(CONFIG_DFLT_PORT, 4242) \
  X(CONFIG_MAX_PLAYING, 120) \
  X(CONFIG_MAX_FILESIZE, 64000) \
  X(CONFIG_MAX_BAD_PWS, 5) \
  X(CONFIG_SITEOK_ALL, !siteok_everyone) \
  X(CONFIG_NS_IS_SLOW, !nameserver_is_slow) \
  X(CONFIG_NEW_SOCIALS, !use_new_socials) \
  X(CONFIG_OLC_SAVE, !auto_save_olc) \
  X(CONFIG_MEDIT_ADVANCED, !medit_advanced_stats) \
  X(CONFIG_IBT_AUTOSAVE, !ibt_autosave) \
  X(CONFIG_PROTOCOL_NEGOTIATION, !protocol_negotiation) \
  X(CONFIG_SPECIAL_IN_COMM, !special_in_comm) \
  X(CONFIG_DEBUG_MODE, 2) \
  X(CONFIG_USE_AUTOWIZ, !use_autowiz) \
  X(CONFIG_MIN_WIZLIST_LEV, LVL_GOD + 2)

/* The one-line messages gain "\r\n" when loaded, and the multi-line ones
 * keep the "\r\n" the writer turns into plain newlines. */
#define EVERY_STRING_SETTING(X) \
  X(CONFIG_OK, "Done that.\r\n") \
  X(CONFIG_HUH, "Pardon?\r\n") \
  X(CONFIG_NOPERSON, "Nobody by that name.\r\n") \
  X(CONFIG_NOEFFECT, "Nothing happens at all.\r\n") \
  X(CONFIG_DFLT_IP, "127.0.0.1") \
  X(CONFIG_DFLT_DIR, "otherlib") \
  X(CONFIG_LOGNAME, "log/othersyslog") \
  X(CONFIG_MENU, "Menu line one\r\nMenu line two\r\n") \
  X(CONFIG_WELC_MESSG, "Welcome, traveller.\r\n") \
  X(CONFIG_START_MESSG, "Your journey begins.\r\n")

#define SET_NUMERIC(field, value) \
  TEST_ASSERT_NOT_EQUAL_INT_MESSAGE((value), (field), "already the default: " #field); \
  (field) = (value);
#define CHECK_NUMERIC(field, value) \
  TEST_ASSERT_EQUAL_INT_MESSAGE((value), (field), #field);
#define SET_STRING(field, value) \
  TEST_ASSERT_TRUE_MESSAGE(!(field) || strcmp((field), (value)) != 0, \
                           "already the default: " #field); \
  free(field); \
  (field) = strdup(value); \
  TEST_ASSERT_NOT_NULL(field);
#define CHECK_STRING(field, value) \
  TEST_ASSERT_EQUAL_STRING_MESSAGE((value), (field), #field);

/* The whole save_config() / load_config() contract: no setting cedit can
 * change may be lost or altered by a save and a reboot. */
static void test_every_setting_survives_save_and_reboot(void)
{
  EVERY_NUMERIC_SETTING(SET_NUMERIC)
  EVERY_STRING_SETTING(SET_STRING)
  save_and_reboot();
  EVERY_NUMERIC_SETTING(CHECK_NUMERIC)
  EVERY_STRING_SETTING(CHECK_STRING)
}

/* ---------------------------------------------------------------------
 * The editor, driven through its entry points
 * ------------------------------------------------------------------- */
static void open_cedit(void)
{
  char cmd[] = "";

  do_oasis_cedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_CEDIT, STATE(&builder_desc));
}

static void type(const char *line)
{
  char buf[MAX_INPUT_LENGTH];

  TEST_ASSERT_EQUAL_INT_MESSAGE(CON_CEDIT, STATE(&builder_desc),
                                "typed into a descriptor that is not in cedit");
  strlcpy(buf, line, sizeof(buf));
  out[0] = '\0';
  cedit_parse(&builder_desc, buf);
}

/* From the main menu: quit, and answer yes to keeping the changes. */
static void quit_and_apply(void)
{
  type("q");
  type("y");
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&builder_desc));
}

/* Turning "Death Traps Junk Items" off makes cedit_save_internally() run
 * reassign_rooms(), which must take the dump proc off every room, the
 * last one included (its loop stopped at top_of_world - 1, audit F2-20),
 * and leave other spec procs alone before assign_rooms() runs again. */
static void test_turning_off_dt_dumps_clears_every_room_including_the_last(void)
{
  CONFIG_DTS_ARE_DUMPS = YES;
  CONFIG_OLC_SAVE = NO;
  rooms_fixture();
  open_cedit();
  type("g");          /* Game Play Options */
  type("m");          /* toggle Death Traps Junk Items */
  type("q");          /* back to the main menu */
  quit_and_apply();

  TEST_ASSERT_EQUAL_INT(NO, CONFIG_DTS_ARE_DUMPS);
  TEST_ASSERT_EQUAL_INT(1, assign_rooms_calls);
  TEST_ASSERT_NULL(world[0].func);
  TEST_ASSERT_TRUE_MESSAGE(world[1].func == other_room_proc,
                           "a non-dump spec proc was cleared");
  TEST_ASSERT_NULL_MESSAGE(world[top_of_world].func,
                           "the last room kept its dump proc");
}

/* ---------------------------------------------------------------------
 * The OLC save list: configuration changes that are only in memory must
 * be listed by 'olc' and written by save_all() ('saveall', shutdown).
 * ------------------------------------------------------------------- */
static int config_entries_queued(void)
{
  struct save_list_data *entry;
  int n = 0;

  for (entry = save_list; entry; entry = entry->next)
    if (entry->type == SL_CFG)
      n++;
  return n;
}

/* One cedit session that toggles Free Rent and keeps the change. */
static void toggle_free_rent_and_apply(void)
{
  open_cedit();
  type("c");          /* Crashsave/Rent Options */
  type("a");          /* toggle Free Rent */
  type("q");          /* back to the main menu */
  quit_and_apply();
}

/* cedit_save_internally() called add_to_save_list(NOWHERE, SL_CFG), which
 * refuses SL_CFG, so a change kept only in memory was never queued:
 * 'olc' said everything was saved and save_all() never wrote it. */
static void test_change_kept_in_memory_is_queued_and_listed(void)
{
  char none[] = "";

  CONFIG_OLC_SAVE = NO;
  toggle_free_rent_and_apply();
  TEST_ASSERT_NOT_NULL(strstr(out, "saved to memory"));
  TEST_ASSERT_TRUE(in_save_list(NOWHERE, SL_CFG));

  out[0] = '\0';
  do_show_save_list(&builder, none, 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "Game configuration data"));

  toggle_free_rent_and_apply();    /* a second change: still one entry */
  TEST_ASSERT_EQUAL_INT(1, config_entries_queued());
}

static void test_saveall_writes_the_queued_config(void)
{
  int changed = !free_rent;

  CONFIG_OLC_SAVE = NO;
  toggle_free_rent_and_apply();
  TEST_ASSERT_EQUAL_INT(changed, CONFIG_FREE_RENT);

  TEST_ASSERT_TRUE(save_all());
  TEST_ASSERT_NULL(save_list);
  boot_config_from(conf_path);
  TEST_ASSERT_EQUAL_INT(changed, CONFIG_FREE_RENT);
}

static void test_cedit_save_command_clears_the_queued_config(void)
{
  char save[] = " save";

  CONFIG_OLC_SAVE = NO;
  toggle_free_rent_and_apply();
  TEST_ASSERT_TRUE(in_save_list(NOWHERE, SL_CFG));

  do_oasis_cedit(&builder, save, 0, 0);
  TEST_ASSERT_FALSE(in_save_list(NOWHERE, SL_CFG));
  TEST_ASSERT_NULL(strstr(out, "FAILED"));
}

/* OLC autosave on, but the file cannot be written: the builder is told,
 * the failure is reported, and the change stays queued for a later
 * 'cedit save' or save_all(). */
static void test_failed_autosave_keeps_the_config_queued(void)
{
  char missing[600];
  int n = snprintf(missing, sizeof(missing), "%s/missing/config", scratch_dir);

  TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(missing));
  free(CONFIG_CONFFILE);
  CONFIG_CONFFILE = strdup(missing);
  TEST_ASSERT_NOT_NULL(CONFIG_CONFFILE);
  CONFIG_OLC_SAVE = YES;

  toggle_free_rent_and_apply();
  TEST_ASSERT_NOT_NULL(strstr(out, "FAILED"));
  TEST_ASSERT_EQUAL_STRING("cedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
  TEST_ASSERT_TRUE(in_save_list(NOWHERE, SL_CFG));
}

static void test_successful_autosave_leaves_nothing_queued(void)
{
  CONFIG_OLC_SAVE = YES;
  toggle_free_rent_and_apply();
  TEST_ASSERT_NOT_NULL(strstr(out, "saved to disk"));
  TEST_ASSERT_FALSE(in_save_list(NOWHERE, SL_CFG));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* ---------------------------------------------------------------------
 * One-line settings.  save_config() writes each as "<key> = <value>" on
 * one line, and load_config() reads the line back with get_line(), which
 * returns at most READ_SIZE - 1 characters; a longer value came back cut.
 * ------------------------------------------------------------------- */
struct one_line_setting {
  const char *menu;       /* main-menu choice */
  const char *option;     /* sub-menu choice that asks for the value */
  int mode;               /* OLC_MODE while the editor waits for it */
  const char *key;        /* what save_config() writes before the value */
  const char *field;      /* the field the diagnostic names */
  char **(*live)(void);   /* the config_info string the value ends up in */
};

static char **live_ok(void) { return &CONFIG_OK; }
static char **live_huh(void) { return &CONFIG_HUH; }
static char **live_noperson(void) { return &CONFIG_NOPERSON; }
static char **live_noeffect(void) { return &CONFIG_NOEFFECT; }
static char **live_dflt_ip(void) { return &CONFIG_DFLT_IP; }
static char **live_dflt_dir(void) { return &CONFIG_DFLT_DIR; }
static char **live_logname(void) { return &CONFIG_LOGNAME; }

static const struct one_line_setting one_line_settings[] = {
  { "g", "1", CEDIT_OK,       "ok = ",       "ok",       live_ok },
  { "g", "2", CEDIT_HUH,      "huh = ",      "huh",      live_huh },
  { "g", "3", CEDIT_NOPERSON, "noperson = ", "noperson", live_noperson },
  { "g", "4", CEDIT_NOEFFECT, "noeffect = ", "noeffect", live_noeffect },
  { "o", "b", CEDIT_DFLT_IP,  "DFLT_IP = ",  "DFLT_IP",  live_dflt_ip },
  { "o", "c", CEDIT_DFLT_DIR, "DFLT_DIR = ", "DFLT_DIR", live_dflt_dir },
  { "o", "d", CEDIT_LOGNAME,  "LOGNAME = ",  "LOGNAME",  live_logname },
};
#define ONE_LINE_SETTINGS (sizeof(one_line_settings) / sizeof(one_line_settings[0]))

/* The longest value that fits after s->key in one line get_line() returns
 * whole (READ_SIZE - 1 characters). */
static size_t line_cap(const struct one_line_setting *s)
{
  return (size_t)(READ_SIZE - 1) - strlen(s->key);
}

/* n copies of c in dst, NUL-terminated. */
static void fill_text(char *dst, size_t dst_size, char c, size_t n)
{
  TEST_ASSERT_TRUE(n < dst_size);
  memset(dst, c, n);
  dst[n] = '\0';
}

/* The written file has exactly one line for key, it is key followed by
 * value, and get_line() can return it whole. */
static void assert_config_line(const char *key, const char *value,
                               const char *field)
{
  char line[1024], expect[1024];
  FILE *fl;
  int found = 0;
  int n = snprintf(expect, sizeof(expect), "%s%s", key, value);

  TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(expect));
  fl = fopen(conf_path, "r");
  TEST_ASSERT_NOT_NULL(fl);
  while (fgets(line, sizeof(line), fl)) {
    char *newline = strchr(line, '\n');

    if (newline)
      *newline = '\0';
    if (strncmp(line, key, strlen(key)) != 0)
      continue;
    found++;
    TEST_ASSERT_TRUE_MESSAGE(strlen(line) <= (size_t)(READ_SIZE - 1), field);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect, line, field);
  }
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, found, field);
}

/* Each one-line setting: one character past what a config line can hold
 * after its key is refused, reported and asked for again; a value at the
 * limit is kept, saved, and written on a line get_line() reads whole.
 * Over-long values used to be stored and saved, then cut at the next
 * boot. */
static void test_one_line_settings_refuse_more_than_a_config_line_holds(void)
{
  size_t i;

  CONFIG_OLC_SAVE = YES;
  for (i = 0; i < ONE_LINE_SETTINGS; i++) {
    const struct one_line_setting *s = &one_line_settings[i];
    size_t cap = line_cap(s);
    char too_long[MAX_INPUT_LENGTH], at_cap[MAX_INPUT_LENGTH], limit[64];
    const char *kept;
    int n;

    fill_text(too_long, sizeof(too_long), 'x', cap + 1);
    fill_text(at_cap, sizeof(at_cap), (char)('a' + i), cap);
    n = snprintf(limit, sizeof(limit), "at most %zu characters", cap);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(limit));

    open_cedit();
    type(s->menu);
    type(s->option);
    TEST_ASSERT_EQUAL_INT_MESSAGE(s->mode, OLC_MODE(&builder_desc), s->field);

    tb_error_capture_reset();
    type(too_long);
    TEST_ASSERT_EQUAL_INT_MESSAGE(s->mode, OLC_MODE(&builder_desc), s->field);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, limit), s->field);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cedit-value-too-long", tb_captured_operation,
                                     s->field);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tb_captured_detail, s->field), s->field);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tb_captured_detail, "player idnum 42"),
                                 s->field);

    type(at_cap);
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(s->mode, OLC_MODE(&builder_desc), s->field);
    type("q");                       /* back to the main menu */
    quit_and_apply();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "saved to disk"), s->field);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tb_captured_errors, s->field);

    /* The four messages are kept with the "\r\n" the editor adds. */
    kept = *s->live();
    TEST_ASSERT_NOT_NULL_MESSAGE(kept, s->field);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(kept, at_cap, cap), s->field);
    TEST_ASSERT_TRUE_MESSAGE(!strcmp(kept + cap, "") || !strcmp(kept + cap, "\r\n"),
                             s->field);
    assert_config_line(s->key, at_cap, s->field);
  }
}

/* Values reach config_info without the editor's check too: comm.c's -o
 * sets the log name, and a hand-written config file may use a shorter
 * "key=" than save_config() writes.  save_config() must refuse to write a
 * value the loader would cut, report it, and leave the file alone. */
static void test_save_config_refuses_a_value_the_loader_would_cut(void)
{
  char temp_path[600];
  struct stat st;
  size_t i;
  int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", conf_path);

  TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(temp_path));
  for (i = 0; i < ONE_LINE_SETTINGS; i++) {
    const struct one_line_setting *s = &one_line_settings[i];
    char too_long[MAX_INPUT_LENGTH];
    char **live = s->live();
    char *kept = *live;
    int saved;

    fill_text(too_long, sizeof(too_long), 'x', line_cap(s) + 1);
    *live = too_long;               /* borrowed: restored before any check */
    tb_error_capture_reset();
    saved = save_config(NOWHERE);
    *live = kept;

    TEST_ASSERT_FALSE_MESSAGE(saved, s->field);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cedit-value-too-long", tb_captured_operation,
                                     s->field);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tb_captured_detail, s->field), s->field);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, stat(conf_path, &st), s->field);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)st.st_size, s->field);  /* setUp's empty file */
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(0, stat(temp_path, &st), s->field);
  }
  TEST_ASSERT_TRUE(save_config(NOWHERE));     /* every value restored: saves */
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_display_closed_doors_survives_save_and_reboot);
  RUN_TEST(test_min_rent_cost_survives_save_and_reboot);
  RUN_TEST(test_every_setting_survives_save_and_reboot);
  RUN_TEST(test_turning_off_dt_dumps_clears_every_room_including_the_last);
  RUN_TEST(test_change_kept_in_memory_is_queued_and_listed);
  RUN_TEST(test_saveall_writes_the_queued_config);
  RUN_TEST(test_cedit_save_command_clears_the_queued_config);
  RUN_TEST(test_failed_autosave_keeps_the_config_queued);
  RUN_TEST(test_successful_autosave_leaves_nothing_queued);
  RUN_TEST(test_one_line_settings_refuse_more_than_a_config_line_holds);
  RUN_TEST(test_save_config_refuses_a_value_the_loader_would_cut);
  return UNITY_END();
}
