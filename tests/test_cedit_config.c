/* Executable tests of the production configuration editor (cedit.c) and of
 * the configuration file it writes.
 *
 * cedit.c is linked with the production loader (db.c load_config()), the
 * compiled-in defaults the loader starts from (config.c), GenOLC's save list
 * (genolc.c), the line, log and string helpers (utils.c), the tag splitter
 * the loader uses (oasis.c split_argument()) and the colour-code conversion
 * both sides apply (modify.c parse_at()/parse_tab()).  Their unrelated code
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
 * header defines under that name. */
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

/* improved-edit.c: the multi-line string editor's help text. */
void send_editor_help(struct descriptor_data *d) { (void)d; }

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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_display_closed_doors_survives_save_and_reboot);
  RUN_TEST(test_min_rent_cost_survives_save_and_reboot);
  RUN_TEST(test_every_setting_survives_save_and_reboot);
  RUN_TEST(test_turning_off_dt_dumps_clears_every_room_including_the_last);
  return UNITY_END();
}
