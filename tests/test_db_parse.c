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
  errno = 0; \
  tb_test_exit_armed = 1; \
  if (setjmp(tb_test_exit_jmp) == 0) { \
    stmt; \
    tb_test_exit_armed = 0; \
    TEST_FAIL_MESSAGE("expected exit(): " #stmt); \
  } \
} while (0)

#define exit(code) tb_test_exit(code)
/* NOLINTNEXTLINE(bugprone-suspicious-include): reach db.c's statics. */
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
void paginate_string(char *str, struct descriptor_data *d)  /* modify.c */
{
  if (d->showstr_count)
    *(d->showstr_vector) = str;
  d->showstr_page = 0;
}

/* DG fixtures for reset_zone(). The object UID table is a single slot: the
 * tests load one object at a time. A load trigger "purges" its object the
 * way extract_obj() does, by unregistering and freeing it immediately. */
static long fixture_uid;
static const void *fixture_uid_owner;
static int fixture_purge_on_load, fixture_load_triggers, fixture_triggers_added;
static int fixture_vars_added;
static long fixture_last_var_context;

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
  if (type != OBJ_TRIGGER)	/* mobs from the 'M' fixture get no script */
    return;
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
trig_rnum real_trigger(trig_vnum vnum) { (void)vnum; return NOTHING; }
void add_trigger(struct script_data *sc, trig_data *t, int loc)
{ (void)sc; (void)t; (void)loc; fixture_triggers_added++; }
void add_var(struct trig_var_data **var_list, const char *name,
             const char *value, long id)
{
  (void)var_list; (void)name; (void)value;
  fixture_vars_added++;
  fixture_last_var_context = id;
}
void reset_wtrigger(room_data *room) { (void)room; }
void load_mtrigger(char_data *ch) { (void)ch; }
void char_to_room_at(const char *src_file, int src_line, const char *src_func, struct char_data *ch, room_rnum room)
{ (void)src_file; (void)src_line; (void)src_func;  (void)ch; (void)room; }
void obj_to_char(struct obj_data *object, struct char_data *ch)
{ (void)object; (void)ch; }
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to)
{ (void)obj; (void)obj_to; }
void equip_char_at(const char *src_file, int src_line, const char *src_func, struct char_data *ch, struct obj_data *obj, int pos)
{ (void)src_file; (void)src_line; (void)src_func;  (void)ch; (void)obj; (void)pos; }
void extract_obj(struct obj_data *obj) { (void)obj; }
struct obj_data *get_obj_num(obj_rnum nr) { (void)nr; return NULL; }
struct obj_data *get_obj_in_list_num(int num, struct obj_data *list)
{ (void)num; (void)list; return NULL; }
unsigned long circle_random(void) { return 0; }

/* World-file fixtures (constants.c, genolc.c, genzon.c). config.c is linked
 * whole: it is data only, and load_config() reads its defaults. */
size_t room_bits_count = 32;
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return 0; }
/* genzon.c's contract: the zone whose bot..top holds vznum, else NOWHERE. */
zone_rnum real_zone_by_thing(room_vnum vznum)
{
  zone_rnum z;

  for (z = 0; zone_table && z <= top_of_zone_table; z++)
    if (zone_table[z].bot <= vznum && vznum <= zone_table[z].top)
      return z;
  return NOWHERE;
}
/* dg_db_scripts.c's trigger readers name a missing trigger's owner by
 * GET_MOB_VNUM(), the room's number or GET_OBJ_VNUM(); these fixtures
 * record that vnum (dg_read_trigger() also consumes its "T <vnum>" line). */
static int fixture_trigger_owner_vnum;
void dg_read_trigger(FILE *fp, void *proto, int type)
{
  char line[READ_SIZE];

  (void)get_line(fp, line);
  fixture_trigger_owner_vnum = type == MOB_TRIGGER ?
    (int) GET_MOB_VNUM((char_data *) proto) : (int) ((room_data *) proto)->number;
}
/* check_object() fixtures (constants.c, spell_parser.c, interpreter.c,
 * handler.c). Zero bit counts make check_bitvector_names() accept nothing
 * set, and the test objects set no flags. */
size_t affected_bits_count = 0;
size_t extra_bits_count = 0;
size_t wear_bits_count = 0;
const char *drinknames[] = { "water", "\n" };
int scheck = 0;
const char *unused_spellname = "!UNUSED!";
const char *skill_name(int num) { (void)num; return unused_spellname; }
int search_block(char *arg, const char **list, int exact)
{
  int i;
  (void)exact;
  for (i = 0; *list[i] != '\n'; i++)
    if (!strcmp(arg, list[i]))
      return i;
  return -1;
}
char *any_one_arg(char *argument, char *first_arg)
{ *first_arg = '\0'; return argument; }
char *find_exdesc(char *word, struct extra_descr_data *list)
{ (void)word; (void)list; return NULL; }
/* discrete_load() reaches every record parser; the tests feed it rooms only. */
size_t action_bits_count = 0;
void parse_trigger(FILE *trig_f, int nr) { (void)trig_f; (void)nr; }
void parse_quest(FILE *quest_f, int nr) { (void)quest_f; (void)nr; }
void dg_obj_trigger(char *line, struct obj_data *obj)
{ (void)line; fixture_trigger_owner_vnum = (int) GET_OBJ_VNUM(obj); }
void skip_spaces(char **string)                /* interpreter.c */
{ while (**string && isspace((unsigned char)**string)) (*string)++; }
/* oasis.c's logic, without its tag-too-long report; load_config() calls it
 * through the oasis.h split_argument(argument, tag) macro, which passes
 * sizeof(tag). */
void split_argument_bounded(char *argument, char *tag, size_t tag_size)
{
  char *tmp = argument, *wrt = argument;
  size_t key_len = 0;

  if (tag_size == 0)
    return;
  for (; *tmp; tmp++) {
    if (*tmp != ' ' && *tmp != '=') {
      if (key_len + 1 < tag_size)
        tag[key_len] = *tmp;
      key_len++;
    } else if (*tmp == '=')
      break;
  }
  tag[key_len < tag_size ? key_len : 0] = '\0';
  while (*tmp == '=' || *tmp == ' ')
    tmp++;
  while (*tmp)
    *(wrt++) = *(tmp++);
  *wrt = '\0';
}

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
static struct char_data zone_mob_proto[1];
static struct index_data zone_mob_index[1];

static void zone_fixture_setup(struct reset_com *cmds)
{
  memset(zone_obj_proto, 0, sizeof(zone_obj_proto));
  memset(zone_obj_index, 0, sizeof(zone_obj_index));
  memset(zone_rooms, 0, sizeof(zone_rooms));
  memset(zone_fixture, 0, sizeof(zone_fixture));
  memset(zone_mob_proto, 0, sizeof(zone_mob_proto));
  memset(zone_mob_index, 0, sizeof(zone_mob_index));
  zone_mob_index[0].vnum = 3005;
  zone_mob_proto[0].nr = 0;
  SET_BIT_AR(MOB_FLAGS(&zone_mob_proto[0]), MOB_ISNPC);
  mob_proto = zone_mob_proto;
  mob_index = zone_mob_index;
  top_of_mobt = 0;
  character_list = NULL;
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
  while (character_list) {
    struct char_data *next = character_list->next;
    free(character_list);
    character_list = next;
  }
  mob_proto = NULL;
  mob_index = NULL;
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

/* B1-07: with top == 65535 the room_vnum counter in the reset_wtrigger walk
 * wrapped to 0 and the loop never ended. */
static void reset_zone_wtrigger_walk_ends_at_top_max(void)
{
  struct reset_com cmds[] = { { 'S', FALSE, 0, 0, 0, 1, NULL, NULL } };

  zone_fixture_setup(cmds);
  zone_fixture[0].bot = 65530;
  zone_fixture[0].top = 65535;
  reset_zone(0);
  TEST_ASSERT_EQUAL_INT(0, zone_fixture[0].age);
  zone_fixture_teardown();
}

/* B1-10: an 'E' with a bad wear slot named the object from obj_index[arg2],
 * but arg2 is the max-existing count, not an rnum (ASan: heap-buffer-
 * overflow), and the command stayed enabled so the read repeated on every
 * reset. Now the object comes from arg1 and the command is disabled. */
static void reset_zone_disables_equip_with_bad_wear_pos(void)
{
  struct reset_com cmds[] = {
    { 'M', FALSE, 0, 5, 0, 1, NULL, NULL },
    { 'E', TRUE, 0, 4765, 99, 2, NULL, NULL },
    { 'S', FALSE, 0, 0, 0, 3, NULL, NULL },
  };

  zone_fixture_setup(cmds);
  reset_zone(0);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-reset", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3005"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 3022"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "pos 99"));
  TEST_ASSERT_EQUAL_CHAR('*', cmds[1].command);
  TEST_ASSERT_NULL(object_list);
  zone_fixture_teardown();
}

/* B1-25: 'P' created its object before looking for the container, so a
 * missing container left a new object with no location in object_list.
 * get_obj_num() is stubbed to find nothing. */
static void reset_zone_put_without_container_creates_nothing(void)
{
  struct reset_com cmds[] = {
    { 'P', FALSE, 0, 99, 0, 1, NULL, NULL },
    { 'S', FALSE, 0, 0, 0, 2, NULL, NULL },
  };

  zone_fixture_setup(cmds);
  reset_zone(0);
  TEST_ASSERT_NULL(object_list);
  TEST_ASSERT_EQUAL_INT(0, zone_obj_index[0].number);
  TEST_ASSERT_EQUAL_CHAR('*', cmds[0].command);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-reset", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 3022"));
  zone_fixture_teardown();
}

/* B1-V02: 'V' for an object or mob passed arg3 (the room rnum after
 * renum_zone_table()) as the variable context instead of arg2, so a global
 * assigned with context 7 landed in context <room rnum>. */
static void reset_zone_object_variable_uses_arg2_context(void)
{
  struct reset_com cmds[] = {
    { 'O', FALSE, 0, 99, 0, 1, NULL, NULL },
    { 'V', TRUE, OBJ_TRIGGER, 7, 0, 2, (char *)"foo", (char *)"bar" },
    { 'S', FALSE, 0, 0, 0, 3, NULL, NULL },
  };

  zone_fixture_setup(cmds);
  fixture_purge_on_load = 0;
  fixture_last_var_context = -1;
  reset_zone(0);
  TEST_ASSERT_EQUAL_INT(1, fixture_vars_added);
  TEST_ASSERT_EQUAL_INT(7, (int)fixture_last_var_context);
  zone_fixture_teardown();
}

/* B1-28: renum_zone_table() stored a 'V' command's resolved room in b, so a
 * missing room was reported with arg2 (the context, here 0) instead of the
 * room vnum 34999. */
static void renum_zone_table_reports_missing_variable_room(void)
{
  struct reset_com cmds[] = {
    { 'V', FALSE, WLD_TRIGGER, 0, 34999, 7, (char *)"foo", (char *)"bar" },
    { 'S', FALSE, 0, 0, 0, 8, NULL, NULL },
  };

  zone_fixture_setup(cmds);
  renum_zone_table();
  TEST_ASSERT_EQUAL_CHAR('*', cmds[0].command);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-renum", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 30 line 7 cmd V: arg3 vnum 34999"));
  zone_fixture_teardown();
}

/* ---------------------------------------------------------------------
 * Helpers: named temporary files and FILE streams with given contents.
 * ------------------------------------------------------------------- */

static char temp_path[64];

/* Write len bytes to a fresh temporary file; its name is in temp_path. */
static void write_temp_file(const char *data, size_t len)
{
  int fd;
  FILE *fp;

  strlcpy(temp_path, "/tmp/tb_db_parse_XXXXXX", sizeof(temp_path));
  fd = mkstemp(temp_path);
  TEST_ASSERT_TRUE(fd >= 0);
  fp = fdopen(fd, "wb");
  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_EQUAL_size_t(len, fwrite(data, 1, len, fp));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* A read stream over len bytes of data (may contain NUL bytes). */
static FILE *stream_of(const char *data, size_t len)
{
  FILE *fp = tmpfile();
  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_EQUAL_size_t(len, fwrite(data, 1, len, fp));
  TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0L, SEEK_SET));
  return fp;
}

/* ---------------------------------------------------------------------
 * file_to_string(): bounded by the real buffer size (B1-01)
 * ------------------------------------------------------------------- */

/* Before the fix strlcat() was given sizeof(char *) == 8, so every text file
 * loaded as its first 7 bytes and the load still reported success. */
static void file_to_string_loads_whole_multiline_file(void)
{
  static const char text[] = "Welcome to the game.\nSecond line here.\n";
  char buf[MAX_STRING_LENGTH];

  write_temp_file(text, sizeof(text) - 1);
  TEST_ASSERT_EQUAL_INT(0, file_to_string(temp_path, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("Welcome to the game.\r\nSecond line here.\r\n", buf);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  (void)remove(temp_path);
}

/* Lines longer than one fgets() chunk are joined, and a final line without
 * a newline keeps its last character. */
static void file_to_string_joins_long_lines_and_keeps_last_char(void)
{
  char text[READ_SIZE * 2 + 8], expect[READ_SIZE * 2 + 16];
  char buf[MAX_STRING_LENGTH];

  memset(text, 'x', READ_SIZE + 40);
  memcpy(text + READ_SIZE + 40, "\nend", 5);
  memset(expect, 'x', READ_SIZE + 40);
  memcpy(expect + READ_SIZE + 40, "\r\nend\r\n", 8);
  write_temp_file(text, strlen(text));
  TEST_ASSERT_EQUAL_INT(0, file_to_string(temp_path, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(expect, buf);
  (void)remove(temp_path);
}

/* A file larger than the buffer is rejected whole, loudly. */
static void file_to_string_rejects_oversized_file(void)
{
  static const char text[] = "0123456789\nabcdefghij\n";
  char buf[16];

  write_temp_file(text, sizeof(text) - 1);
  TEST_ASSERT_EQUAL_INT(-1, file_to_string(temp_path, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("text-file-load", tb_captured_operation);
  (void)remove(temp_path);
}

/* B1-30: file_to_string_alloc() returned -1 whenever a pager was reading the
 * old text in place, so the copy-on-reload code after it never ran and
 * "reload" failed while anyone paged the file. Now the pager gets its own
 * copy and the reload succeeds. */
static void file_to_string_alloc_reloads_while_paged(void)
{
  static const char text[] = "New text.\n";
  struct descriptor_data d;
  char *vector[1];
  char *global = strdup("Old text.\r\n");

  memset(&d, 0, sizeof(d));
  vector[0] = global;
  d.showstr_vector = vector;
  d.showstr_count = 1;
  d.showstr_page = 0;
  descriptor_list = &d;

  write_temp_file(text, sizeof(text) - 1);
  TEST_ASSERT_EQUAL_INT(0, file_to_string_alloc(temp_path, &global));
  TEST_ASSERT_EQUAL_STRING("New text.\r\n", global);
  TEST_ASSERT_NOT_NULL(d.showstr_head);
  TEST_ASSERT_EQUAL_STRING("Old text.\r\n", d.showstr_head);
  TEST_ASSERT_TRUE(vector[0] == d.showstr_head);
  (void)remove(temp_path);
  descriptor_list = NULL;
  free(d.showstr_head);
  free(global);
}

/* ---------------------------------------------------------------------
 * parse_room()/setup_dir(): exit direction range (B1-03)
 * ------------------------------------------------------------------- */

/* parse_room() keeps its room and zone cursors in statics, so every test
 * shares one world array large enough for all rooms the tests parse. */
#define ROOM_FIXTURES 16
static struct room_data room_fixture[ROOM_FIXTURES];
static struct zone_data room_zone_fixture[1];

static void room_fixture_use(void)
{
  room_zone_fixture[0].number = 1;
  room_zone_fixture[0].bot = 0;
  room_zone_fixture[0].top = 65000;
  zone_table = room_zone_fixture;
  top_of_zone_table = 0;
  world = room_fixture;
}

static void room_fixture_free(void)
{
  int r, d;
  for (r = 0; r < ROOM_FIXTURES; r++) {
    struct room_data *room = &room_fixture[r];
    free(room->name);
    free(room->description);
    for (d = 0; d < NUM_OF_DIRS; d++)
      if (room->dir_option[d]) {
        free(room->dir_option[d]->general_description);
        free(room->dir_option[d]->keyword);
        free(room->dir_option[d]);
      }
    free_extra_descriptions(room->ex_description);
    memset(room, 0, sizeof(*room));
  }
  world = NULL;
  zone_table = NULL;
}

static void parse_room_text(const char *text, int vnum)
{
  FILE *fp = stream_of(text, strlen(text));
  parse_room(fp, vnum);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* "D11" used to CREATE() into dir_option[11], i.e. room_data's func/script
 * fields: an intra-object write invisible to ASan and Valgrind. */
static void parse_room_rejects_out_of_range_direction(void)
{
  static const char text[] =
    "Hall~\nA hall.\n~\n0 0 0 0 0 0\nD11\n~\n~\n0 -1 0\nS\n$\n";
  FILE *volatile fp = stream_of(text, sizeof(text) - 1);

  room_fixture_use();
  EXPECT_EXIT(parse_room(fp, 100));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_INT(1, tb_test_exit_code);
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("room-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 100"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "direction 11"));
  room_fixture_free();
}

static void parse_room_rejects_negative_direction(void)
{
  static const char text[] =
    "Hall~\nA hall.\n~\n0 0 0 0 0 0\nD-1\n~\n~\n0 -1 0\nS\n$\n";
  FILE *volatile fp = stream_of(text, sizeof(text) - 1);

  room_fixture_use();
  EXPECT_EXIT(parse_room(fp, 101));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_STRING("room-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "direction -1"));
  room_fixture_free();
}

/* A duplicate exit replaces the earlier one without leaking it, loudly. */
static void parse_room_reports_duplicate_exit(void)
{
  static const char text[] =
    "Hall~\nA hall.\n~\n0 0 0 0 0 0\n"
    "D0\nfirst~\ndoor~\n0 -1 3001\n"
    "D0\nsecond~\ngate~\n0 -1 3002\n"
    "S\n$\n";

  room_fixture_use();
  parse_room_text(text, 102);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("room-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "duplicate exit D0"));
  TEST_ASSERT_EQUAL_INT(3002, world[top_of_world].dir_option[0]->to_room);
  room_fixture_free();
}

/* B1-06: sector 10 (== NUM_ROOM_SECTORS) and negative sectors passed the old
 * "> NUM_ROOM_SECTORS" check and later indexed movement_loss[] out of
 * bounds. Both are now replaced by SECT_INSIDE, loudly. */
static void parse_room_rejects_sector_out_of_range(void)
{
  static const char text[] = "Hall~\nA hall.\n~\n0 0 0 0 0 10\nS\n$\n";

  room_fixture_use();
  parse_room_text(text, 103);
  TEST_ASSERT_EQUAL_INT(SECT_INSIDE, world[top_of_world].sector_type);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("room-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 103"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "sector 10"));
  room_fixture_free();
}

static void parse_room_rejects_negative_legacy_sector(void)
{
  static const char text[] = "Hall~\nA hall.\n~\n0 0 -3\nS\n$\n";

  room_fixture_use();
  parse_room_text(text, 104);
  TEST_ASSERT_EQUAL_INT(SECT_INSIDE, world[top_of_world].sector_type);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "sector -3"));
  room_fixture_free();
}

static void parse_room_keeps_valid_sector(void)
{
  static const char text[] = "Hall~\nA hall.\n~\n0 0 0 0 0 9\nS\n$\n";

  room_fixture_use();
  parse_room_text(text, 105);
  TEST_ASSERT_EQUAL_INT(9, world[top_of_world].sector_type);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  room_fixture_free();
}

/* B1-23: with diagonal directions off, setup_dir() returned without reading
 * the exit's strings, so parse_room() took the exit description for the
 * next D/E/S line and exited. The exit is now skipped whole, loudly. */
static void parse_room_skips_disabled_diagonal_exit(void)
{
  static const char text[] =
    "Hall~\nA hall.\n~\n0 0 0 0 0 0\n"
    "D6\nA path northwest.\n~\npath~\n0 -1 3001\n"
    "D0\nNorth.\n~\n~\n0 -1 3002\n"
    "S\n$\n";

  room_fixture_use();
  CONFIG_DIAGONAL_DIRS = FALSE;
  parse_room_text(text, 106);
  TEST_ASSERT_NULL(world[top_of_world].dir_option[6]);
  TEST_ASSERT_NOT_NULL(world[top_of_world].dir_option[0]);
  TEST_ASSERT_EQUAL_INT(3002, world[top_of_world].dir_option[0]->to_room);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 106: D6 skipped"));
  room_fixture_free();
}

/* B1-24: flag letter 'G' is bit 32, which the int room-flag slot cannot
 * hold; it used to vanish without a word. 'F' (bit 31) still fits. */
static void parse_room_reports_flag_bit_past_slot(void)
{
  static const char text[] = "Hall~\nA hall.\n~\n0 aFG 0 0 0 0\nS\n$\n";

  room_fixture_use();
  parse_room_text(text, 107);
  TEST_ASSERT_EQUAL_UINT32(0x80000001u, (unsigned int)world[top_of_world].room_flags[0]);
  TEST_ASSERT_EQUAL_STRING("flag-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "room vnum 107: flags 'aFG' set bit 32"));
  room_fixture_free();
}

/* ---------------------------------------------------------------------
 * fread_string()/fread_clean_string(): '~'-terminated strings
 * ------------------------------------------------------------------- */

static char *read_tilde(char *(*reader)(FILE *, const char *),
                        const char *text, size_t len)
{
  FILE *fp = stream_of(text, len);
  char *s = reader(fp, "test-string");
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  return s;
}

/* B1-04: a body line that is only "\r\n" walked below tmp[] (ASan:
 * stack-buffer-underflow) and the blank line came back as "\r\r\n". */
static void fread_clean_string_keeps_blank_crlf_line(void)
{
  static const char text[] = "a\r\n\r\nb~\n";
  char *s = read_tilde(fread_clean_string, text, sizeof(text) - 1);

  TEST_ASSERT_EQUAL_STRING("a\r\n\r\nb", s);
  free(s);
}

static void fread_string_keeps_blank_crlf_line(void)
{
  static const char text[] = "a\r\n\r\nb~\n";
  char *s = read_tilde(fread_string, text, sizeof(text) - 1);

  TEST_ASSERT_EQUAL_STRING("a\r\n\r\nb", s);
  free(s);
}

/* B1-V01: a chunk whose first byte is NUL made the trailing-newline scan read
 * tmp[-1] (ASan: stack-buffer-underflow). It is now reported and skipped. */
static void fread_string_reports_nul_at_line_start(void)
{
  static const char text[] = "a\n\0zz\nb~\n";
  char *s = read_tilde(fread_string, text, sizeof(text) - 1);

  TEST_ASSERT_EQUAL_STRING("a\r\nb", s);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("fread-string", tb_captured_operation);
  free(s);
}

static void fread_clean_string_reports_nul_at_line_start(void)
{
  static const char text[] = "a\n\0zz\nb~\n";
  char *s = read_tilde(fread_clean_string, text, sizeof(text) - 1);

  TEST_ASSERT_EQUAL_STRING("a\r\nb", s);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("fread-string", tb_captured_operation);
  free(s);
}

/* B1-05: a 600-character line with no newline in the first fgets() window
 * used to get "\r\n" written at tmp[511..513], one byte past the array
 * (ASan: stack-buffer-overflow). The long line now comes back whole. */
static void check_long_line(char *(*reader)(FILE *, const char *))
{
  char text[700], expect[700];
  char *s;

  memset(text, 'q', 600);
  memcpy(text + 600, "\nend~\n", 7);
  memset(expect, 'q', 600);
  memcpy(expect + 600, "\r\nend", 6);
  s = read_tilde(reader, text, strlen(text));
  TEST_ASSERT_EQUAL_STRING(expect, s);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(s);
}

static void fread_string_rejoins_long_line(void)
{ check_long_line(fread_string); }

static void fread_clean_string_rejoins_long_line(void)
{ check_long_line(fread_clean_string); }

/* A 511-character final chunk with no newline and no '~' at EOF used to take
 * the "\r\n" splice to tmp[513]; it now ends in the format-error exit. */
static void fread_string_long_unterminated_tail_exits(void)
{
  char text[520];
  FILE *volatile fp;

  memset(text, 'z', 511);
  fp = stream_of(text, 511);
  EXPECT_EXIT(free(fread_string(fp, "test-string")));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_INT(1, tb_test_exit_code);
}

/* ---------------------------------------------------------------------
 * load_zones(): zone header and reset command ranges
 * ------------------------------------------------------------------- */

/* load_zones() fills zone_table[zone] for a static cursor that only moves on
 * success, so the tests give it a table with a slot per successful load. */
#define ZONE_LOAD_FIXTURES 8
static struct zone_data zone_load_fixture[ZONE_LOAD_FIXTURES];

static void zone_load_fixture_free(void)
{
  int z, c;
  for (z = 0; z < ZONE_LOAD_FIXTURES; z++) {
    struct zone_data *zd = &zone_load_fixture[z];
    if (zd->cmd)
      for (c = 0; zd->cmd[c].command != 'S' && zd->cmd[c].command; c++) {
        free(zd->cmd[c].sarg1);
        free(zd->cmd[c].sarg2);
      }
    free(zd->cmd);
    free(zd->name);
    free(zd->builders);
    memset(zd, 0, sizeof(*zd));
  }
  zone_table = NULL;
}

/* Parse a zone file; returns 1 if load_zones() exit()ed. */
static int load_zone_text(const char *text)
{
  FILE *volatile fp = stream_of(text, strlen(text));
  volatile int exited = 1;

  zone_table = zone_load_fixture;
  errno = 0;
  tb_test_exit_armed = 1;
  if (setjmp(tb_test_exit_jmp) == 0) {
    load_zones(fp, (char *)"test.zon");
    tb_test_exit_armed = 0;
    exited = 0;
  }
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  return exited;
}

static void load_zones_rejects_top_nowhere(void)
{
  static const char text[] =
    "#30\nbuilder~\nZone~\n65000 65535 30 2\nS\n$\n";

  TEST_ASSERT_EQUAL_INT(1, load_zone_text(text));
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("zone-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 30 top 65535"));
  zone_load_fixture_free();
}

/* ---------------------------------------------------------------------
 * load_config(): line buffer size and empty multi-line values
 * ------------------------------------------------------------------- */

static void config_fixture_free(void)
{
  free(CONFIG_OK); free(CONFIG_HUH); free(CONFIG_NOPERSON);
  free(CONFIG_NOEFFECT); free(CONFIG_DFLT_IP); free(CONFIG_DFLT_DIR);
  free(CONFIG_LOGNAME); free(CONFIG_MENU); free(CONFIG_WELC_MESSG);
  free(CONFIG_START_MESSG);
  memset(&config_info, 0, sizeof(config_info));
}

static void load_config_text(const char *text)
{
  write_temp_file(text, strlen(text));
  CONFIG_CONFFILE = temp_path;
  load_config();
  (void)remove(temp_path);
}

/* B1-08: line[] was READ_SIZE - 2 bytes, but get_line() writes up to
 * READ_SIZE, so a 255+ character config line overflowed it by two bytes
 * (ASan: stack-buffer-overflow in split_argument). */
static void load_config_reads_full_length_line(void)
{
  char text[400];

  strlcpy(text, "ok = ", sizeof(text));
  memset(text + 5, 'O', 300);
  strlcpy(text + 305, "\n", sizeof(text) - 305);
  load_config_text(text);
  TEST_ASSERT_NOT_NULL(CONFIG_OK);
  TEST_ASSERT_EQUAL_CHAR('O', CONFIG_OK[0]);
  /* The value keeps its re-added "\r\n" even at the maximum line length. */
  TEST_ASSERT_NOT_NULL(strstr(CONFIG_OK, "\r\n"));
  config_fixture_free();
}

/* B1-09: an empty "menu" or "start_messg" left the value NULL (fread_string()
 * returns NULL for ""), and the extra parse_at() call then dereferenced it. */
static void load_config_restores_default_for_empty_menu(void)
{
  load_config_text("menu\n~\n");
  TEST_ASSERT_NOT_NULL(CONFIG_MENU);
  TEST_ASSERT_EQUAL_STRING(MENU, CONFIG_MENU);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("config-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "'menu'"));
  config_fixture_free();
}

static void load_config_restores_default_for_empty_start_messg(void)
{
  load_config_text("start_messg\n~\n");
  TEST_ASSERT_NOT_NULL(CONFIG_START_MESSG);
  TEST_ASSERT_EQUAL_STRING(START_MESSG, CONFIG_START_MESSG);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "'start_messg'"));
  config_fixture_free();
}

/* ---------------------------------------------------------------------
 * check_object(): values that later index tables
 * ------------------------------------------------------------------- */

/* B1-11: a fountain or drink container with liquid 99 passed check_object()
 * and later indexed drinks[]/drink_aff[] out of range in do_drink. */
static void check_object_repairs_liquid_out_of_range(void)
{
  struct obj_data obj;
  static struct index_data idx[1];

  memset(&obj, 0, sizeof(obj));
  idx[0].vnum = 3133;
  obj_index = idx;
  obj.item_number = 0;
  obj.name = (char *)"fountain";
  obj.short_description = (char *)"a fountain";
  GET_OBJ_TYPE(&obj) = ITEM_FOUNTAIN;
  GET_OBJ_VAL(&obj, 2) = 99;
  TEST_ASSERT_TRUE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(LIQ_WATER, GET_OBJ_VAL(&obj, 2));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("object-check", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 3133 liquid 99"));
  obj_index = NULL;
}

/* ---------------------------------------------------------------------
 * parse_simple_mob(): byte-sized position and sex fields
 * ------------------------------------------------------------------- */

static struct char_data simple_mob_fixture[1];

/* Parse a simple-mob numeric block into mob_proto[0]; 1 if it exit()ed. */
static int parse_simple_mob_text(const char *text, int nr)
{
  FILE *volatile fp = stream_of(text, strlen(text));
  volatile int exited = 1;

  memset(simple_mob_fixture, 0, sizeof(simple_mob_fixture));
  mob_proto = simple_mob_fixture;
  errno = 0;
  tb_test_exit_armed = 1;
  if (setjmp(tb_test_exit_jmp) == 0) {
    parse_simple_mob(fp, 0, nr);
    tb_test_exit_armed = 0;
    exited = 0;
  }
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  mob_proto = NULL;
  return exited;
}

/* B1-12: position 8 / default 8 / sex 7 were stored into byte fields with no
 * check; sex 7 later indexes the three-entry gender tables. */
static void parse_simple_mob_rejects_sex_out_of_range(void)
{
  TEST_ASSERT_EQUAL_INT(1, parse_simple_mob_text(
    "1 20 10 1d1+1 1d1+0\n0 0\n8 8 7\n", 3005));
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("mob-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3005"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "sex 7"));
}

static void parse_simple_mob_rejects_negative_position(void)
{
  TEST_ASSERT_EQUAL_INT(1, parse_simple_mob_text(
    "1 20 10 1d1+1 1d1+0\n0 0\n-1 8 0\n", 3006));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "position -1"));
}

static void parse_simple_mob_accepts_valid_position(void)
{
  TEST_ASSERT_EQUAL_INT(0, parse_simple_mob_text(
    "1 20 10 1d1+1 1d1+0\n0 0\n8 8 2\n", 3007));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* ---------------------------------------------------------------------
 * boot_social_messages(): parsed socials never exceed the allocation
 * ------------------------------------------------------------------- */

static char social_dir[64];
static char social_cwd[PATH_MAX];

/* Write misc/socials.new under a fresh directory and chdir() into it. */
static void social_fixture_use(const char *text)
{
  char path[128];
  FILE *fp;

  strlcpy(social_dir, "/tmp/tb_soc_XXXXXX", sizeof(social_dir));
  TEST_ASSERT_NOT_NULL(mkdtemp(social_dir));
  TEST_ASSERT_NOT_NULL(getcwd(social_cwd, sizeof(social_cwd)));
  TEST_ASSERT_EQUAL_INT(0, chdir(social_dir));
  TEST_ASSERT_EQUAL_INT(0, mkdir("misc", 0700));
  strlcpy(path, SOCMESS_FILE_NEW, sizeof(path));
  fp = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(fp);
  TEST_ASSERT_EQUAL_INT(1, fputs(text, fp) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  CONFIG_NEW_SOCIALS = TRUE;
  top_of_socialt = 0;
  soc_mess_list = NULL;
}

static void social_fixture_free(int slots)
{
  int k;
  for (k = 0; k < slots; k++) {
    struct social_messg *soc = &soc_mess_list[k];

    free(soc->command);
    free(soc->sort_as);
    free(soc->char_no_arg);       free(soc->others_no_arg);
    free(soc->char_found);        free(soc->others_found);
    free(soc->vict_found);        free(soc->not_found);
    free(soc->char_auto);         free(soc->others_auto);
    free(soc->char_body_found);   free(soc->others_body_found);
    free(soc->vict_body_found);   free(soc->char_obj_found);
    free(soc->others_obj_found);
  }
  free(soc_mess_list);
  soc_mess_list = NULL;
  (void)remove(SOCMESS_FILE_NEW);
  (void)rmdir("misc");
  TEST_ASSERT_EQUAL_INT(0, chdir(social_cwd));
  (void)rmdir(social_dir);
  CONFIG_NEW_SOCIALS = FALSE;
}

#define SOCIAL_BODY "#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n"
/* A complete social that takes no target: only char_no_arg is required. */
#define SOCIAL_SOLO "You grin.\n#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n"

/* B1-13: the count sees only lines starting with '~', but fscanf(" %s ")
 * skips leading blanks, so " ~b" and " ~c" were parsed without being counted
 * and curr_soc ran past soc_mess_list[] (2 slots for 1 counted social). */
static void boot_social_messages_rejects_uncounted_socials(void)
{
  social_fixture_use("~a a 0 0 0 0\n" SOCIAL_BODY "\n"
                     " ~b b 0 0 0 0\n" SOCIAL_BODY "\n"
                     " ~c c 0 0 0 0\n" SOCIAL_BODY "\n$\n");
  EXPECT_EXIT(boot_social_messages());
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("social-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "social '~c'"));
  social_fixture_free(2);
}

static void boot_social_messages_loads_counted_socials(void)
{
  social_fixture_use("~a a 0 0 0 0\n" SOCIAL_SOLO "\n"
                     "~b b 0 0 0 0\n" SOCIAL_SOLO "\n$\n");
  boot_social_messages();
  TEST_ASSERT_EQUAL_INT(1, top_of_socialt);
  TEST_ASSERT_EQUAL_STRING("b", soc_mess_list[1].command);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  social_fixture_free(2);
}

/* ---------------------------------------------------------------------
 * discrete_load(): records never exceed the counted allocation (B1-14)
 * ------------------------------------------------------------------- */

/* Run discrete_load() over text with the given slot capacity; 1 on exit(). */
static int discrete_load_text(const char *text, int mode, int capacity)
{
  FILE *volatile fp = stream_of(text, strlen(text));
  volatile int exited = 1;

  boot_capacity = capacity;
  boot_records = 0;
  boot_last_vnum = -1;
  errno = 0;
  tb_test_exit_armed = 1;
  if (setjmp(tb_test_exit_jmp) == 0) {
    discrete_load(fp, mode, (char *)"test.wld");
    tb_test_exit_armed = 0;
    exited = 0;
  }
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  return exited;
}

/* The counting pass and the parser can disagree about where records start
 * (a '#' inside a long line); a record past the allocation was written to
 * world[rec_count]. */
static void discrete_load_rejects_record_past_capacity(void)
{
  static const char text[] =
    "#200\nA~\nA.\n~\n0 0 0 0 0 0\nS\n"
    "#201\nB~\nB.\n~\n0 0 0 0 0 0\nS\n$\n";

  room_fixture_use();
  TEST_ASSERT_EQUAL_INT(1, discrete_load_text(text, DB_BOOT_WLD, 1));
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("proto-slot", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "world #201"));
  room_fixture_free();
}

static void discrete_load_accepts_records_within_capacity(void)
{
  static const char text[] =
    "#202\nA~\nA.\n~\n0 0 0 0 0 0\nS\n"
    "#203\nB~\nB.\n~\n0 0 0 0 0 0\nS\n$\n";

  room_fixture_use();
  TEST_ASSERT_EQUAL_INT(0, discrete_load_text(text, DB_BOOT_WLD, 2));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(2, boot_records);
  room_fixture_free();
}

/* B1-15: #70000 was stored as vnum 4464 and a descending number broke the
 * binary search in real_room(). The first is fatal, the second (present in
 * the stock world) is reported. */
static void discrete_load_rejects_vnum_past_idxtype(void)
{
  room_fixture_use();
  TEST_ASSERT_EQUAL_INT(1, discrete_load_text(
    "#70000\nA~\nA.\n~\n0 0 0 0 0 0\nS\n$\n", DB_BOOT_WLD, 4));
  TEST_ASSERT_EQUAL_STRING("record-number", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "world #70000"));
  room_fixture_free();
}

static void discrete_load_reports_descending_vnum(void)
{
  room_fixture_use();
  TEST_ASSERT_EQUAL_INT(0, discrete_load_text(
    "#211\nA~\nA.\n~\n0 0 0 0 0 0\nS\n"
    "#210\nB~\nB.\n~\n0 0 0 0 0 0\nS\n$\n", DB_BOOT_WLD, 4));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("record-number", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "previous #211"));
  room_fixture_free();
}

static void discrete_load_warns_on_99999_stop(void)
{
  room_fixture_use();
  TEST_ASSERT_EQUAL_INT(0, discrete_load_text("#99999\n", DB_BOOT_WLD, 4));
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("record-number", tb_captured_operation);
  room_fixture_free();
}

/* A line padded to exactly one READ_SIZE chunk and then "#99" on the same
 * physical line: get_line() returns "#99" as a record, so the count must see
 * it too. The old 127-byte chunking did not. */
static void count_hash_records_counts_hash_at_get_line_chunk(void)
{
  char text[READ_SIZE + 16];
  FILE *fp;

  memset(text, 'x', READ_SIZE - 1);
  memcpy(text + READ_SIZE - 1, "#99\n", 5);
  fp = stream_of(text, strlen(text));
  TEST_ASSERT_EQUAL_INT(1, count_hash_records(fp));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* B1-16: an 'M' naming mob 70000 was narrowed to vnum 4464 by real_mobile()
 * and reset the wrong mob. load_zones() now disables the command loudly. */
static void load_zones_disables_command_with_vnum_past_idxtype(void)
{
  static const char text[] =
    "#31\nbuilder~\nZone~\n3100 3199 30 2\n"
    "M 0 70000 1 3100\n"
    "O 0 3100 1 3100\n"
    "S\n$\n";

  TEST_ASSERT_EQUAL_INT(0, load_zone_text(text));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "zone 31 line 5 cmd M arg1=70000"));
  TEST_ASSERT_EQUAL_CHAR('*', zone_table[top_of_zone_table].cmd[0].command);
  TEST_ASSERT_EQUAL_CHAR('O', zone_table[top_of_zone_table].cmd[1].command);
  zone_load_fixture_free();
}

/* B1-18: parse_mobile()/parse_object() ran fname() on the short description,
 * which copied the whole leading alphabetic run into a READ_SIZE static
 * (ASan: global-buffer-overflow for a 300+ letter word). */
static void starts_with_article_handles_long_first_word(void)
{
  char text[400];

  memset(text, 'a', 399);
  text[399] = '\0';
  TEST_ASSERT_FALSE(starts_with_article(text));
  TEST_ASSERT_TRUE(starts_with_article("A long sword"));
  TEST_ASSERT_TRUE(starts_with_article("an apple"));
  TEST_ASSERT_TRUE(starts_with_article("The Mayor"));
  TEST_ASSERT_TRUE(starts_with_article("the"));
  TEST_ASSERT_FALSE(starts_with_article("Anvil of doom"));
  TEST_ASSERT_FALSE(starts_with_article("theft"));
  TEST_ASSERT_FALSE(starts_with_article(""));
}

/* ---------------------------------------------------------------------
 * get_one_line(): help-file lines
 * ------------------------------------------------------------------- */

/* B1-21: a line starting with NUL made strlen() 0 and the newline strip wrote
 * buf[-1] (ASan: stack-buffer-underflow). */
static void get_one_line_reports_leading_nul(void)
{
  static const char text[] = "\0abc\nnext\n";
  char buf[READ_SIZE];
  FILE *fp = stream_of(text, sizeof(text) - 1);

  get_one_line(fp, buf);
  TEST_ASSERT_EQUAL_STRING("", buf);
  TEST_ASSERT_EQUAL_STRING("help-load", tb_captured_operation);
  get_one_line(fp, buf);
  TEST_ASSERT_EQUAL_STRING("next", buf);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* A line longer than one chunk kept losing its last character; now it is
 * split whole and reported, and CRLF endings are removed entirely. */
static void get_one_line_keeps_long_line_characters(void)
{
  char text[READ_SIZE + 16], buf[READ_SIZE];
  FILE *fp;

  memset(text, 'h', READ_SIZE + 4);
  memcpy(text + READ_SIZE + 4, "\r\n", 3);
  fp = stream_of(text, strlen(text));
  get_one_line(fp, buf);
  TEST_ASSERT_EQUAL_size_t(READ_SIZE - 1, strlen(buf));
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  get_one_line(fp, buf);
  TEST_ASSERT_EQUAL_STRING("hhhhh", buf);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* B1-27: E-spec values were parsed with tb_parse_int_or_zero() (garbage
 * became 0) and clamped by RANGE() without a word. */
static void interpret_espec_reports_clamp_and_garbage(void)
{
  /* Arrays, not literals: parse_espec() hands interpret_espec() a pointer
   * into a line buffer, and the analyzer misreads literals here. */
  char v_str[] = "40", v_add[] = "x7", v_dex[] = "18";

  memset(simple_mob_fixture, 0, sizeof(simple_mob_fixture));
  mob_proto = simple_mob_fixture;

  interpret_espec("Str", v_str, 0, 3008);
  TEST_ASSERT_EQUAL_INT(25, mob_proto[0].real_abils.str);
  TEST_ASSERT_EQUAL_STRING("mob-espec", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3008 Str value '40' clamped to [3,25]"));

  tb_error_capture_reset();
  interpret_espec("StrAdd", v_add, 0, 3008);
  TEST_ASSERT_EQUAL_INT(0, mob_proto[0].real_abils.str_add);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "'x7' is not a number"));

  tb_error_capture_reset();
  interpret_espec("Dex", v_dex, 0, 3008);
  TEST_ASSERT_EQUAL_INT(18, mob_proto[0].real_abils.dex);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  mob_proto = NULL;
}

/* ---------------------------------------------------------------------
 * fread_number(): int range (B1-31)
 * ------------------------------------------------------------------- */

static int read_number(const char *text)
{
  FILE *fp = stream_of(text, strlen(text));
  int n = fread_number(fp);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  return n;
}

/* "number * 10 + digit" on an int overflowed (UBSan: signed-integer-
 * overflow) for a long digit string; now it is reported and read as 0. */
static void fread_number_reports_out_of_range(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_number("99999999999999999999 "));
  TEST_ASSERT_EQUAL_STRING("fread-number", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "out of int range"));
}

static void fread_number_reads_limits_and_sums(void)
{
  TEST_ASSERT_EQUAL_INT(INT_MAX, read_number("2147483647 "));
  TEST_ASSERT_EQUAL_INT(INT_MIN, read_number("-2147483648 "));
  TEST_ASSERT_EQUAL_INT(7, read_number("3|4 "));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(0, read_number("2147483647|1 "));
  TEST_ASSERT_EQUAL_STRING("fread-number", tb_captured_operation);
}

static void fread_number_reports_bad_format(void)
{
  TEST_ASSERT_EQUAL_INT(0, read_number("x12"));
  TEST_ASSERT_EQUAL_STRING("fread-number", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "expected a digit"));
}

/* ---------------------------------------------------------------------
 * Empty ("~") world strings the game prints unchecked (G2-03)
 * ------------------------------------------------------------------- */

/* parse_object()/parse_mobile() keep their slot cursors in statics, like
 * parse_room(): the tests share one prototype table of each kind. */
#define PROTO_FIXTURES 8
static struct obj_data obj_proto_fixture[PROTO_FIXTURES];
static struct index_data obj_index_fixture[PROTO_FIXTURES];
static struct char_data mob_proto_fixture[PROTO_FIXTURES];
static struct index_data mob_index_fixture[PROTO_FIXTURES];

static void proto_fixture_free(void)
{
  int k;

  for (k = 0; k < PROTO_FIXTURES; k++) {
    struct obj_data *obj = &obj_proto_fixture[k];
    struct char_data *mob = &mob_proto_fixture[k];

    free(obj->name);
    free(obj->short_description);
    free(obj->description);
    free(obj->action_description);
    free_extra_descriptions(obj->ex_description);
    memset(obj, 0, sizeof(*obj));
    free(mob->player.name);
    free(mob->player.short_descr);
    free(mob->player.long_descr);
    free(mob->player.description);
    memset(mob, 0, sizeof(*mob));
  }
  obj_proto = NULL;
  obj_index = NULL;
  mob_proto = NULL;
  mob_index = NULL;
}

/* fread_string() returns NULL for "~"; parse_room() stored it as the room
 * name and description and as an extra description keyword, which 'look'
 * and find_exdesc() dereference. */
static void parse_room_replaces_empty_strings(void)
{
  static const char text[] =
    "~\n~\n0 0 0 0 0 0\nE\n~\nA brass plaque.\n~\nS\n$\n";
  struct room_data *room;

  room_fixture_use();
  parse_room_text(text, 108);
  room = &world[top_of_world];
  TEST_ASSERT_EQUAL_STRING("undefined", room->name);
  TEST_ASSERT_EQUAL_STRING("undefined", room->description);
  TEST_ASSERT_NOT_NULL(room->ex_description);
  TEST_ASSERT_EQUAL_STRING("undefined", room->ex_description->keyword);
  TEST_ASSERT_EQUAL_STRING("A brass plaque.\r\n", room->ex_description->description);
  TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("world-string-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "room vnum 108: empty extra description keyword"));
  room_fixture_free();
}

/* An object with an empty short and long description: list_obj_to_char()
 * read *description and strcmp()ed short descriptions (UBSan: load of null
 * pointer), and an empty extra description keyword reached find_exdesc(). */
static void parse_object_replaces_empty_strings(void)
{
  static const char text[] =
    "sword~\n~\n~\n~\n"
    "12 0 0 0 0 0 0 0 0 0 0 0 0\n0 0 0 0\n1 2 3 4 5\n"
    "E\n~\nThe blade is notched.\n~\n$\n";
  struct obj_data *obj;
  FILE *fp = stream_of(text, sizeof(text) - 1);

  obj_proto = obj_proto_fixture;
  obj_index = obj_index_fixture;
  (void)parse_object(fp, 3020);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  obj = &obj_proto[top_of_objt];
  TEST_ASSERT_EQUAL_STRING("sword", obj->name);
  TEST_ASSERT_EQUAL_STRING("undefined", obj->short_description);
  TEST_ASSERT_EQUAL_STRING("Undefined", obj->description);  /* CAP()ed */
  TEST_ASSERT_NULL(obj->action_description);  /* optional: stays NULL */
  TEST_ASSERT_NOT_NULL(obj->ex_description);
  TEST_ASSERT_EQUAL_STRING("undefined", obj->ex_description->keyword);
  TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("world-string-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "obj vnum 3020: empty extra description keyword"));
  proto_fixture_free();
}

/* A mob with an empty short description: list_one_char() ran
 * UPPER(*short_descr) and act() prints it as the mob's name. */
static void parse_mobile_replaces_empty_short_description(void)
{
  static const char text[] =
    "guard~\n~\nA guard stands here.\n~\nHe looks tough.\n~\n"
    "0 0 0 0 0 0 0 0 0 S\n1 20 10 1d1+1 1d1+0\n0 0\n8 8 1\n$\n";
  struct char_data *mob;
  FILE *fp = stream_of(text, sizeof(text) - 1);

  mob_proto = mob_proto_fixture;
  mob_index = mob_index_fixture;
  parse_mobile(fp, 3010);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  mob = &mob_proto[top_of_mobt];
  TEST_ASSERT_EQUAL_STRING("undefined", mob->player.short_descr);
  TEST_ASSERT_EQUAL_STRING("guard", mob->player.name);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("world-string-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "mob vnum 3010: empty short description"));
  proto_fixture_free();
}

/* ---------------------------------------------------------------------
 * check_object(): weapon values hit() uses
 * ------------------------------------------------------------------- */

static struct index_data weapon_index_fixture[1];

/* A weapon prototype with the given damage dice and attack type. */
static void weapon_fixture(struct obj_data *obj, int ndice, int sdice, int type)
{
  memset(obj, 0, sizeof(*obj));
  weapon_index_fixture[0].vnum = 3021;
  obj_index = weapon_index_fixture;
  obj->item_number = 0;
  obj->name = (char *)"dagger";
  obj->short_description = (char *)"a dagger";
  GET_OBJ_TYPE(obj) = ITEM_WEAPON;
  GET_OBJ_VAL(obj, 1) = ndice;
  GET_OBJ_VAL(obj, 2) = sdice;
  GET_OBJ_VAL(obj, 3) = type;
}

/* H1-08: weapon value 3 = 40 passed check_object() and later indexed
 * attack_hit_text[] (NUM_ATTACK_TYPES entries) in dam_message() and
 * 'stat'. It is now replaced by 0 (hit), loudly. */
static void check_object_repairs_attack_type_out_of_range(void)
{
  struct obj_data obj;

  weapon_fixture(&obj, 1, 4, 40);
  TEST_ASSERT_TRUE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(&obj, 3));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("object-check", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "obj vnum 3021 attack type 40"));

  tb_error_capture_reset();
  weapon_fixture(&obj, 1, 4, -1);
  TEST_ASSERT_TRUE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(&obj, 3));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "attack type -1"));

  tb_error_capture_reset();
  weapon_fixture(&obj, 1, 4, NUM_ATTACK_TYPES - 1);
  TEST_ASSERT_FALSE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(NUM_ATTACK_TYPES - 1, GET_OBJ_VAL(&obj, 3));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  obj_index = NULL;
}

/* H1-18: weapon dice 2000000000d6 made dice() loop two billion times on
 * every blow, and negative dice were kept. Both are now clamped into
 * 0..MAX_WEAPON_NDICE d 0..MAX_WEAPON_SDICE, loudly; 0d0 and the oedit
 * maximum pass silently. */
static void check_object_clamps_weapon_dice(void)
{
  struct obj_data obj;

  weapon_fixture(&obj, 2000000000, 6, 0);
  TEST_ASSERT_TRUE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(MAX_WEAPON_NDICE, GET_OBJ_VAL(&obj, 1));
  TEST_ASSERT_EQUAL_INT(6, GET_OBJ_VAL(&obj, 2));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("object-check", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "obj vnum 3021 damage dice 2000000000d6"));

  tb_error_capture_reset();
  weapon_fixture(&obj, -3, 1000, 0);
  TEST_ASSERT_TRUE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_VAL(&obj, 1));
  TEST_ASSERT_EQUAL_INT(MAX_WEAPON_SDICE, GET_OBJ_VAL(&obj, 2));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);

  tb_error_capture_reset();
  weapon_fixture(&obj, 0, 0, 0);
  TEST_ASSERT_FALSE(check_object(&obj));
  weapon_fixture(&obj, MAX_WEAPON_NDICE, MAX_WEAPON_SDICE, 0);
  TEST_ASSERT_FALSE(check_object(&obj));
  TEST_ASSERT_EQUAL_INT(MAX_WEAPON_NDICE, GET_OBJ_VAL(&obj, 1));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  obj_index = NULL;
}

/* F1-16: load_zones() kept 'E 1 3201 1 40' and 'D 0 3200 42 1', and zedit's
 * command list then read equipment_types[40] and dirs[42]. Both commands
 * are now disabled at load, loudly; the last valid slot and direction
 * (NUM_WEARS - 1, NUM_OF_DIRS - 1) stay. */
static void load_zones_disables_bad_wear_position_and_direction(void)
{
  static const char text[] =
    "#32\nbuilder~\nZone~\n3200 3299 30 2\n"
    "M 0 3200 1 3200\n"
    "E 1 3201 1 40\n"
    "D 0 3200 42 1\n"
    "E 1 3201 1 17\n"
    "D 0 3200 9 1\n"
    "S\n$\n";
  struct reset_com *cmd;

  TEST_ASSERT_EQUAL_INT(0, load_zone_text(text));
  cmd = zone_table[top_of_zone_table].cmd;
  TEST_ASSERT_EQUAL_CHAR('M', cmd[0].command);
  TEST_ASSERT_EQUAL_CHAR('*', cmd[1].command);
  TEST_ASSERT_EQUAL_CHAR('*', cmd[2].command);
  TEST_ASSERT_EQUAL_CHAR('E', cmd[3].command);
  TEST_ASSERT_EQUAL_CHAR('D', cmd[4].command);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "zone 32 line 7 cmd D direction 42"));
  zone_load_fixture_free();
}

/* G2-13 (boot half): do_action needs char_no_arg, and do_gmote needs
 * others_found once char_found is set; a '#' line left them NULL and the
 * social failed only when a player used it. Boot now names each one. */
static void boot_social_messages_reports_missing_required_messages(void)
{
  social_fixture_use("~a a 0 0 0 0\n" SOCIAL_BODY "\n"
                     "~b b 0 0 0 0\n"
                     "You smile.\n#\nYou smile at $N.\n"
                     "#\n#\n#\n#\n#\n#\n#\n#\n#\n#\n\n"
                     "~c c 0 0 0 0\n" SOCIAL_SOLO "\n$\n");
  boot_social_messages();
  TEST_ASSERT_EQUAL_INT(2, top_of_socialt);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("social-field-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "social 'b' has char_found but no others_found"));
  social_fixture_free(3);
}

/* ---------------------------------------------------------------------
 * sort_boot_records(): tables loaded out of vnum order (B1-15)
 * ------------------------------------------------------------------- */

/* Rooms with the given vnums in world[]; light records the file slot. */
static void sorted_room_fixture(const int *vnums, int n)
{
  int k;

  memset(room_fixture, 0, sizeof(room_fixture));
  world = room_fixture;
  for (k = 0; k < n; k++) {
    room_fixture[k].number = vnums[k];
    room_fixture[k].light = (byte)k;
  }
  top_of_world = n - 1;
}

/* B1-15: discrete_load() only reported a number that did not ascend, and
 * real_room()'s binary search then missed rooms (the stock 301.wld lists
 * #30170 after #30172, which hid 30171 and 30172). Here 102 between 104 and
 * 105 is not found until index_boot()'s sort puts it back. */
static void sort_boot_records_restores_room_order(void)
{
  static const int vnums[] = { 100, 101, 103, 104, 102, 105, 106 };
  const int n = (int)(sizeof(vnums) / sizeof(vnums[0]));
  int k;

  sorted_room_fixture(vnums, n);
  TEST_ASSERT_EQUAL_INT(NOWHERE, real_room(102));
  sort_boot_records(DB_BOOT_WLD, "world/wld/index");
  for (k = 0; k < n; k++) {
    TEST_ASSERT_EQUAL_INT(100 + k, world[k].number);
    TEST_ASSERT_EQUAL_INT(k, real_room(100 + k));
  }
  TEST_ASSERT_EQUAL_INT(4, world[2].light);	/* 102 came from slot 4 */
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  room_fixture_free();
}

/* A shared vnum cannot be sorted apart: both records stay, in file order,
 * and the duplicate is reported. */
static void sort_boot_records_reports_duplicate_vnum(void)
{
  static const int vnums[] = { 5, 7, 5 };

  sorted_room_fixture(vnums, 3);
  sort_boot_records(DB_BOOT_WLD, "world/wld/index");
  TEST_ASSERT_EQUAL_INT(5, world[0].number);
  TEST_ASSERT_EQUAL_INT(0, world[0].light);
  TEST_ASSERT_EQUAL_INT(5, world[1].number);
  TEST_ASSERT_EQUAL_INT(2, world[1].light);
  TEST_ASSERT_EQUAL_INT(7, world[2].number);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("record-number", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "vnum 5 is used by more than one record"));
  room_fixture_free();
}

/* Mobs and objects live in two parallel arrays and store their own rnum
 * (nr, item_number); triggers store it in their prototype. Each moves with
 * its index entry and is renumbered. */
static void sort_boot_records_renumbers_prototypes(void)
{
  static const int vnums[] = { 20, 10, 30 };
  static struct index_data trig_idx[2];
  static struct trig_data trig_protos[2];
  struct index_data *trig_slots[2];
  int k;

  mob_proto = mob_proto_fixture;
  mob_index = mob_index_fixture;
  obj_proto = obj_proto_fixture;
  obj_index = obj_index_fixture;
  for (k = 0; k < 3; k++) {
    mob_index[k].vnum = obj_index[k].vnum = vnums[k];
    mob_proto[k].nr = obj_proto[k].item_number = k;
    GET_LEVEL(&mob_proto[k]) = vnums[k];	/* follows its prototype */
    GET_OBJ_WEIGHT(&obj_proto[k]) = vnums[k];
  }
  top_of_mobt = top_of_objt = 2;
  sort_boot_records(DB_BOOT_MOB, "world/mob/index");
  sort_boot_records(DB_BOOT_OBJ, "world/obj/index");
  for (k = 0; k < 3; k++) {
    TEST_ASSERT_EQUAL_INT(10 * (k + 1), mob_index[k].vnum);
    TEST_ASSERT_EQUAL_INT(10 * (k + 1), GET_LEVEL(&mob_proto[k]));
    TEST_ASSERT_EQUAL_INT(k, mob_proto[k].nr);
    TEST_ASSERT_EQUAL_INT(k, real_mobile(10 * (k + 1)));
    TEST_ASSERT_EQUAL_INT(10 * (k + 1), obj_index[k].vnum);
    TEST_ASSERT_EQUAL_INT(10 * (k + 1), GET_OBJ_WEIGHT(&obj_proto[k]));
    TEST_ASSERT_EQUAL_INT(k, obj_proto[k].item_number);
    TEST_ASSERT_EQUAL_INT(k, real_object(10 * (k + 1)));
  }
  proto_fixture_free();

  for (k = 0; k < 2; k++) {
    trig_idx[k].vnum = (k == 0) ? 9 : 5;
    trig_idx[k].proto = &trig_protos[k];
    trig_protos[k].nr = k;
    trig_slots[k] = &trig_idx[k];
  }
  trig_index = trig_slots;
  top_of_trigt = 2;
  sort_boot_records(DB_BOOT_TRG, "world/trg/index");
  TEST_ASSERT_EQUAL_INT(5, trig_index[0]->vnum);
  TEST_ASSERT_TRUE(trig_index[0]->proto == &trig_protos[1]);
  TEST_ASSERT_EQUAL_INT(0, trig_index[0]->proto->nr);
  TEST_ASSERT_EQUAL_INT(1, trig_index[1]->proto->nr);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  trig_index = NULL;
  top_of_trigt = 0;
}

/* discrete_load() marks the pass so index_boot() sorts it. */
static void discrete_load_marks_descending_vnum_for_sorting(void)
{
  room_fixture_use();
  boot_disordered = FALSE;
  TEST_ASSERT_EQUAL_INT(0, discrete_load_text(
    "#221\nA~\nA.\n~\n0 0 0 0 0 0\nS\n"
    "#220\nB~\nB.\n~\n0 0 0 0 0 0\nS\n$\n", DB_BOOT_WLD, 4));
  TEST_ASSERT_TRUE(boot_disordered);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "#220 does not ascend (previous #221); the "
                              "table is sorted after loading"));
  boot_disordered = FALSE;
  room_fixture_free();
}

/* An empty ("~") mob alias list stayed NULL; isname() tolerates that, but
 * medit, DG name lookups and the pet shop print it with %s. */
static void parse_mobile_replaces_empty_alias_list(void)
{
  static const char text[] =
    "~\nthe guard~\nA guard stands here.\n~\nHe looks tough.\n~\n"
    "0 0 0 0 0 0 0 0 0 S\n1 20 10 1d1+1 1d1+0\n0 0\n8 8 1\n$\n";
  struct char_data *mob;
  FILE *fp = stream_of(text, sizeof(text) - 1);

  mob_proto = mob_proto_fixture;
  mob_index = mob_index_fixture;
  parse_mobile(fp, 3011);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  mob = &mob_proto[top_of_mobt];
  TEST_ASSERT_EQUAL_STRING("undefined", mob->player.name);
  TEST_ASSERT_EQUAL_STRING("the guard", mob->player.short_descr);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("world-string-missing", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "mob vnum 3011: empty alias list"));
  proto_fixture_free();
}

/* parse_simple_mob() stored its numbers into byte and sh_int fields
 * unchecked: damage dice 200d6 wrapped to -56d6, and armor class 300000000
 * overflowed "10 * t[2]" (UBSan: signed-integer-overflow). Each bad field is
 * now reported with the mob vnum, and then the boot stops. */
static void parse_simple_mob_rejects_values_that_would_wrap(void)
{
  TEST_ASSERT_EQUAL_INT(1, parse_simple_mob_text(
    "1 20 300000000 1d1+1 200d6+0\n0 0\n8 8 1\n", 3012));
  TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);	/* two fields, then fatal */
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("mob-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3012: 2 value(s)"));

  tb_error_capture_reset();
  TEST_ASSERT_FALSE(mob_value_in_range(3012, "damage dice count", 200, 0, SCHAR_MAX));
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "mob vnum 3012: damage dice count 200 out of range 0..127"));

  /* Each hit dice value fits an sh_int, but read_mobile() would roll
   * 200d200 into the sh_int max_hit. */
  tb_error_capture_reset();
  TEST_ASSERT_EQUAL_INT(1, parse_simple_mob_text(
    "1 20 10 200d200+0 1d1+0\n0 0\n8 8 1\n", 3013));
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mob vnum 3013: 1 value(s)"));
}

/* The extreme values each field can hold load silently. */
static void parse_simple_mob_accepts_field_limits(void)
{
  struct char_data *mob = &simple_mob_fixture[0];

  TEST_ASSERT_EQUAL_INT(0, parse_simple_mob_text(
    "255 -107 3276 1d1+32766 127d127+127\n0 0\n8 8 1\n", 3014));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(255, GET_LEVEL(mob));
  TEST_ASSERT_EQUAL_INT(127, GET_HITROLL(mob));
  TEST_ASSERT_EQUAL_INT(32760, GET_AC(mob));
  TEST_ASSERT_EQUAL_INT(127, mob->mob_specials.damnodice);

  TEST_ASSERT_EQUAL_INT(0, parse_simple_mob_text(
    "0 148 -3276 0d0+-32768 0d0+-128\n0 0\n8 8 1\n", 3015));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(-128, GET_HITROLL(mob));
  TEST_ASSERT_EQUAL_INT(-32760, GET_AC(mob));
  TEST_ASSERT_EQUAL_INT(-32768, GET_MOVE(mob));
  TEST_ASSERT_EQUAL_INT(-128, GET_DAMROLL(mob));
}

/* ---------------------------------------------------------------------
 * room_zone(): every room lies inside a zone
 * ------------------------------------------------------------------- */

static struct zone_data gap_zone_fixture[2];
static int gap_zone_cursor;	/* static: survives the EXPECT_EXIT longjmp */

/* Zone 10 holds rooms 1000-1099 and zone 20 rooms 2000-2099. */
static void gap_zone_fixture_use(void)
{
  gap_zone_cursor = 0;
  memset(gap_zone_fixture, 0, sizeof(gap_zone_fixture));
  gap_zone_fixture[0].number = 10;
  gap_zone_fixture[0].bot = 1000;
  gap_zone_fixture[0].top = 1099;
  gap_zone_fixture[1].number = 20;
  gap_zone_fixture[1].bot = 2000;
  gap_zone_fixture[1].top = 2099;
  zone_table = gap_zone_fixture;
  top_of_zone_table = 1;
}

/* Room 1500 lies between the zones. parse_room() attached it to zone 20,
 * whose reset and redit save walk 2000-2099, so it was never reset and
 * vanished on the next zone save. It is now fatal, naming both zones. */
static void room_zone_rejects_room_between_zones(void)
{
  gap_zone_fixture_use();
  EXPECT_EXIT((void)room_zone(1500, &gap_zone_cursor));
  TEST_ASSERT_EQUAL_INT(1, tb_test_exit_code);
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("room-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "room vnum 1500 lies between zone 10 (1000-1099) "
                              "and zone 20 (2000-2099)"));
  zone_table = NULL;
  top_of_zone_table = 0;
}

/* Rooms inside a zone move the cursor forward only; rooms below the
 * cursor's zone or above the last zone stay fatal. */
static void room_zone_follows_ascending_rooms(void)
{
  gap_zone_fixture_use();
  TEST_ASSERT_EQUAL_INT(0, room_zone(1000, &gap_zone_cursor));
  TEST_ASSERT_EQUAL_INT(0, room_zone(1099, &gap_zone_cursor));
  TEST_ASSERT_EQUAL_INT(1, room_zone(2000, &gap_zone_cursor));
  TEST_ASSERT_EQUAL_INT(1, gap_zone_cursor);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  EXPECT_EXIT((void)room_zone(1050, &gap_zone_cursor));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "room vnum 1050 is below zone 20 (2000-2099)"));
  gap_zone_cursor = 1;
  EXPECT_EXIT((void)room_zone(2100, &gap_zone_cursor));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "room vnum 2100 is above the last zone 20"));
  zone_table = NULL;
  top_of_zone_table = 0;
}

/* setup_dir()'s format errors named the room through a context string that
 * used to read GET_ROOM_VNUM(room) + 1, which is NOWHERE + 1 during boot.
 * They now stop the boot naming the room's own vnum and the direction. */
static void setup_dir_reports_bad_exit_line_with_room_and_direction(void)
{
  static const char bad[] =
    "Hall~\nA hall.\n~\n0 0 0 0 0 0\nD2\nA door.\n~\ndoor~\nnot numbers\nS\n$\n";
  static const char cut[] =
    "Hall~\nA hall.\n~\n0 0 0 0 0 0\nD3\nA door.\n~\ndoor~\n";
  FILE *volatile fp = stream_of(bad, sizeof(bad) - 1);

  room_fixture_use();
  EXPECT_EXIT(parse_room(fp, 110));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_INT(1, tb_test_exit_code);
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("room-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "room vnum 110 direction D2: malformed"));
  room_fixture_free();

  fp = stream_of(cut, sizeof(cut) - 1);
  room_fixture_use();
  EXPECT_EXIT(parse_room(fp, 111));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "room vnum 111 direction D3: file ends"));
  room_fixture_free();
}

/* ---------------------------------------------------------------------
 * Legacy-record conversion: the converted record's zone must exist
 * ------------------------------------------------------------------- */

/* FUZZ-W2 (tests/fuzz/known-crashes/world/obj-convert-zone-nowhere): a
 * legacy 3-field object line is converted to 128-bit flags, and with
 * bitsavetodisk on (the default) parse_object() read
 * zone_table[real_zone_by_thing(vnum)]: zone_table[NOWHERE] for a vnum in
 * no zone, 4.7 MB past the table (ASan: SEGV). Object 1500 lies between
 * zones 10 and 20; it now stops the boot, and 1050 converts as before. */
static void parse_object_rejects_converted_record_in_no_zone(void)
{
  static const char text[] =
    "sword~\na sword~\nA sword lies here.~\n~\n"
    "12 0 0\n0 0 0 0\n1 2 3 4 5\n$\n";
  FILE *volatile fp = stream_of(text, sizeof(text) - 1);

  gap_zone_fixture_use();
  obj_proto = obj_proto_fixture;
  obj_index = obj_index_fixture;
  EXPECT_EXIT((void)parse_object(fp, 1500));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("world-convert", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "obj vnum 1500 is in no zone"));
  proto_fixture_free();

  tb_error_capture_reset();
  fp = stream_of(text, sizeof(text) - 1);
  obj_proto = obj_proto_fixture;
  obj_index = obj_index_fixture;
  (void)parse_object(fp, 1050);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_STRING("sword", obj_proto[top_of_objt].name);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  proto_fixture_free();
  zone_table = NULL;
  top_of_zone_table = 0;
}

/* FUZZ-W4 (mob-convert-zone-nowhere): the same zone_table[NOWHERE] read in
 * parse_mobile() for a legacy 4-field flag line. */
static void parse_mobile_rejects_converted_record_in_no_zone(void)
{
  static const char text[] =
    "guard~\nthe guard~\nA guard stands here.\n~\nHe looks tough.\n~\n"
    "0 0 0 S\n1 20 10 1d1+1 1d1+0\n0 0\n8 8 1\n$\n";
  FILE *volatile fp = stream_of(text, sizeof(text) - 1);

  gap_zone_fixture_use();
  mob_proto = mob_proto_fixture;
  mob_index = mob_index_fixture;
  EXPECT_EXIT(parse_mobile(fp, 1500));
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("world-convert", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "mob vnum 1500 is in no zone"));
  proto_fixture_free();

  tb_error_capture_reset();
  fp = stream_of(text, sizeof(text) - 1);
  mob_proto = mob_proto_fixture;
  mob_index = mob_index_fixture;
  parse_mobile(fp, 1050);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_STRING("guard", mob_proto[top_of_mobt].player.name);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  proto_fixture_free();
  zone_table = NULL;
  top_of_zone_table = 0;
}

/* load_zones() over len bytes of data (which may hold NUL bytes); 1 if it
 * exit()ed. */
static int load_zone_bytes(const char *data, size_t len)
{
  FILE *volatile fp = stream_of(data, len);
  volatile int exited = 1;

  zone_table = zone_load_fixture;
  errno = 0;
  tb_test_exit_armed = 1;
  if (setjmp(tb_test_exit_jmp) == 0) {
    load_zones(fp, (char *)"test.zon");
    tb_test_exit_armed = 0;
    exited = 0;
  }
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  return exited;
}

/* A line of only blanks, or one starting with a NUL byte, got the command
 * letter '\0' and was then parsed from what the previous line left in the
 * buffer, creating a junk command (or a misleading format error). Both are
 * now skipped and reported with their line numbers. */
static void load_zones_skips_blank_and_nul_lines(void)
{
  static const char text[] =
    "#33\nbuilder~\nZone~\n3300 3399 30 2\n"
    "M 0 3300 1 3300\n"
    "   \n"
    "\0M 0 3301 1 3301\n"
    "O 0 3300 1 3300\n"
    "S\n$\n";
  struct reset_com *cmd;

  TEST_ASSERT_EQUAL_INT(0, load_zone_bytes(text, sizeof(text) - 1));
  cmd = zone_table[top_of_zone_table].cmd;
  TEST_ASSERT_EQUAL_CHAR('M', cmd[0].command);
  TEST_ASSERT_EQUAL_INT(3300, cmd[0].arg1);
  TEST_ASSERT_EQUAL_CHAR('O', cmd[1].command);
  TEST_ASSERT_EQUAL_CHAR('S', cmd[2].command);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("zone-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "zone 33 line 7: NUL byte where a command was expected"));
  zone_load_fixture_free();
}

/* dg_read_trigger() and dg_obj_trigger() name a missing trigger's owner by
 * GET_MOB_VNUM()/GET_OBJ_VNUM(). parse_mobile() set the mob's rnum and
 * top_of_mobt only after its 'T' lines, and parse_object() top_of_objt
 * only after its own, so both reports named 65535 (NOBODY/NOTHING; for
 * objects from the second one on). The owner's real vnum is seen now. */
static void trigger_lines_see_their_owner_vnum(void)
{
  static const char mob_text[] =
    "guard~\nthe guard~\nA guard stands here.\n~\nHe looks tough.\n~\n"
    "0 0 0 0 0 0 0 0 0 S\n1 20 10 1d1+1 1d1+0\n0 0\n8 8 1\nT 999\n$\n";
  static const char obj_text[] =
    "sword~\na sword~\nA sword lies here.~\n~\n"
    "12 0 0 0 0 0 0 0 0 0 0 0 0\n0 0 0 0\n1 2 3 4 5\nT 999\n$\n";
  FILE *fp;
  int k;

  mob_proto = mob_proto_fixture;
  mob_index = mob_index_fixture;
  fixture_trigger_owner_vnum = -1;
  fp = stream_of(mob_text, sizeof(mob_text) - 1);
  parse_mobile(fp, 3016);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  TEST_ASSERT_EQUAL_INT(3016, fixture_trigger_owner_vnum);

  /* Two objects: the first slot also passed when top_of_objt was 0. */
  obj_proto = obj_proto_fixture;
  obj_index = obj_index_fixture;
  for (k = 0; k < 2; k++) {
    fixture_trigger_owner_vnum = -1;
    fp = stream_of(obj_text, sizeof(obj_text) - 1);
    (void)parse_object(fp, 3017 + k);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    TEST_ASSERT_EQUAL_INT(3017 + k, fixture_trigger_owner_vnum);
  }
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  proto_fixture_free();
}

/* A legacy 3-field object line ("type extra wear") has no affect field,
 * but the conversion parsed f3 for the affect flags anyway: sscanf() had
 * left it unset, so it read uninitialised stack bytes (Valgrind:
 * conditional jump on uninitialised value in asciiflag_conv_aff()). The
 * 4-field form keeps its affect letters. */
static void parse_object_converts_legacy_affect_field(void)
{
  static const char three[] =
    "sword~\na sword~\nA sword lies here.~\n~\n"
    "12 a b\n0 0 0 0\n1 2 3 4 5\n$\n";
  static const char four[] =
    "sword~\na sword~\nA sword lies here.~\n~\n"
    "12 a b c\n0 0 0 0\n1 2 3 4 5\n$\n";
  struct obj_data *obj;
  FILE *fp;

  gap_zone_fixture_use();
  obj_proto = obj_proto_fixture;
  obj_index = obj_index_fixture;
  fp = stream_of(three, sizeof(three) - 1);
  (void)parse_object(fp, 1060);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  obj = &obj_proto[top_of_objt];
  TEST_ASSERT_EQUAL_INT(1, GET_OBJ_EXTRA(obj)[0]);	/* 'a' */
  TEST_ASSERT_EQUAL_INT(2, GET_OBJ_WEAR(obj)[0]);	/* 'b' */
  TEST_ASSERT_EQUAL_INT(0, GET_OBJ_AFFECT(obj)[0]);

  fp = stream_of(four, sizeof(four) - 1);
  (void)parse_object(fp, 1061);
  TEST_ASSERT_EQUAL_INT(0, fclose(fp));
  obj = &obj_proto[top_of_objt];
  TEST_ASSERT_EQUAL_INT(8, GET_OBJ_AFFECT(obj)[0]);	/* 'c', affect bit 3 */
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  proto_fixture_free();
  zone_table = NULL;
  top_of_zone_table = 0;
}

/* ---------------------------------------------------------------------
 * load_config(): unknown keys and the old cedit key name
 * ------------------------------------------------------------------- */

/* load_config() ignored unknown keys without a word, which hid cedit
 * writing "disp_closed_doors" while the loader read "display_closed_doors".
 * Each unknown key is now reported with its line (comment lines and the
 * lines of a multi-line value count), and loading goes on. */
static void load_config_reports_unknown_keys(void)
{
  load_config_text("* comment\n"
                   "max_playing = 77\n"
                   "bogus_key = 5\n"
                   "menu\nLine one\nLine two\n~\n"
                   "zzz = 1\n"
                   "tunnel_size = 3\n");
  TEST_ASSERT_EQUAL_INT(77, CONFIG_MAX_PLAYING);
  TEST_ASSERT_EQUAL_INT(3, CONFIG_TUNNEL_SIZE);
  TEST_ASSERT_EQUAL_STRING("Line one\r\nLine two\r\n", CONFIG_MENU);
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);	/* bogus_key, zzz */
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("config-unknown-key", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "line 8: unknown key 'zzz' ignored"));
  config_fixture_free();
}

/* The old name keeps its setting (the last line wins, as for any key) and
 * is reported once per load; the current name loads silently. */
static void load_config_reads_old_closed_doors_key(void)
{
  load_config_text("disp_closed_doors = 1\ndisp_closed_doors = 0\n");
  TEST_ASSERT_EQUAL_INT(0, CONFIG_DISP_CLOSED_DOORS);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("config-key-alias", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "line 1: 'disp_closed_doors'"));
  config_fixture_free();

  tb_error_capture_reset();
  load_config_text("display_closed_doors = 1\n");
  TEST_ASSERT_EQUAL_INT(1, CONFIG_DISP_CLOSED_DOORS);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  config_fixture_free();
}

/* ---------------------------------------------------------------------
 * free_char(): a mob's remember() list
 * ------------------------------------------------------------------- */

/* free_char() fixtures (interpreter.c, act.comm.c, dg_*.c, handler.c,
 * lists.c, mud_event.c, mobact.c); clearMemory() frees like mobact.c's and
 * counts the records it freed. */
static int fixture_memory_freed;
void free_alias(struct alias_data *a) { (void)a; }
void free_history(struct char_data *ch, int type) { (void)ch; (void)type; }
void free_proto_script(void *thing, int type) { (void)thing; (void)type; }
void affect_remove(struct char_data *ch, struct affected_type *af)
{ (void)af; ch->affected = NULL; }
void extract_script(void *thing, int type) { (void)thing; (void)type; }
void *simple_list(struct list_data *pList) { (void)pList; return NULL; }
void free_list(struct list_data *pList) { (void)pList; }
void event_cancel(struct event *event) { (void)event; }
void remove_from_lookup_table(long uid, const void *c) { (void)uid; (void)c; }
void clearMemory(struct char_data *ch)
{
  memory_rec *curr, *next;

  for (curr = MEMORY(ch); curr; curr = next) {
    next = curr->next;
    free(curr);
    fixture_memory_freed++;
  }
  MEMORY(ch) = NULL;
}

/* damage() makes a MOB_MEMORY victim remember() its attacker, and only
 * extract_char_final() cleared that list: destroy_db() frees the mobs still
 * alive at shutdown with free_char(), which leaked every record
 * (LeakSanitizer: remember <- damage <- hit <- do_hit). */
static void free_char_releases_mob_memory(void)
{
  struct char_data *mob;
  memory_rec *rec;
  int k;

  CREATE(mob, struct char_data, 1);
  SET_BIT_AR(MOB_FLAGS(mob), MOB_ISNPC);
  mob->nr = NOBODY;
  mob->player_specials = &dummy_mob;
  for (k = 0; k < 2; k++) {
    CREATE(rec, memory_rec, 1);
    rec->id = k + 1;
    rec->next = MEMORY(mob);
    MEMORY(mob) = rec;
  }
  fixture_memory_freed = 0;
  free_char(mob);
  TEST_ASSERT_EQUAL_INT(2, fixture_memory_freed);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(load_zones_rejects_top_nowhere);
  RUN_TEST(fread_number_reports_out_of_range);
  RUN_TEST(fread_number_reads_limits_and_sums);
  RUN_TEST(fread_number_reports_bad_format);
  RUN_TEST(get_one_line_reports_leading_nul);
  RUN_TEST(get_one_line_keeps_long_line_characters);
  RUN_TEST(starts_with_article_handles_long_first_word);
  RUN_TEST(load_zones_disables_command_with_vnum_past_idxtype);
  RUN_TEST(discrete_load_rejects_record_past_capacity);
  RUN_TEST(discrete_load_accepts_records_within_capacity);
  RUN_TEST(count_hash_records_counts_hash_at_get_line_chunk);
  RUN_TEST(discrete_load_rejects_vnum_past_idxtype);
  RUN_TEST(discrete_load_reports_descending_vnum);
  RUN_TEST(discrete_load_warns_on_99999_stop);
  RUN_TEST(boot_social_messages_rejects_uncounted_socials);
  RUN_TEST(boot_social_messages_loads_counted_socials);
  RUN_TEST(parse_simple_mob_rejects_sex_out_of_range);
  RUN_TEST(parse_simple_mob_rejects_negative_position);
  RUN_TEST(parse_simple_mob_accepts_valid_position);
  RUN_TEST(interpret_espec_reports_clamp_and_garbage);
  RUN_TEST(check_object_repairs_liquid_out_of_range);
  RUN_TEST(load_config_reads_full_length_line);
  RUN_TEST(load_config_restores_default_for_empty_menu);
  RUN_TEST(load_config_restores_default_for_empty_start_messg);
  RUN_TEST(fread_string_rejoins_long_line);
  RUN_TEST(fread_clean_string_rejoins_long_line);
  RUN_TEST(fread_string_long_unterminated_tail_exits);
  RUN_TEST(fread_string_reports_nul_at_line_start);
  RUN_TEST(fread_clean_string_reports_nul_at_line_start);
  RUN_TEST(fread_clean_string_keeps_blank_crlf_line);
  RUN_TEST(fread_string_keeps_blank_crlf_line);
  RUN_TEST(parse_room_rejects_out_of_range_direction);
  RUN_TEST(parse_room_rejects_negative_direction);
  RUN_TEST(parse_room_reports_duplicate_exit);
  RUN_TEST(parse_room_rejects_sector_out_of_range);
  RUN_TEST(parse_room_rejects_negative_legacy_sector);
  RUN_TEST(parse_room_keeps_valid_sector);
  RUN_TEST(parse_room_skips_disabled_diagonal_exit);
  RUN_TEST(parse_room_reports_flag_bit_past_slot);
  RUN_TEST(file_to_string_loads_whole_multiline_file);
  RUN_TEST(file_to_string_joins_long_lines_and_keeps_last_char);
  RUN_TEST(file_to_string_rejects_oversized_file);
  RUN_TEST(file_to_string_alloc_reloads_while_paged);
  RUN_TEST(reset_zone_skips_trigger_after_load_purge);
  RUN_TEST(reset_zone_keeps_object_when_trigger_does_not_purge);
  RUN_TEST(reset_zone_wtrigger_walk_ends_at_top_max);
  RUN_TEST(reset_zone_disables_equip_with_bad_wear_pos);
  RUN_TEST(reset_zone_put_without_container_creates_nothing);
  RUN_TEST(reset_zone_object_variable_uses_arg2_context);
  RUN_TEST(renum_zone_table_reports_missing_variable_room);
  RUN_TEST(parse_room_replaces_empty_strings);
  RUN_TEST(parse_object_replaces_empty_strings);
  RUN_TEST(parse_mobile_replaces_empty_short_description);
  RUN_TEST(check_object_repairs_attack_type_out_of_range);
  RUN_TEST(check_object_clamps_weapon_dice);
  RUN_TEST(load_zones_disables_bad_wear_position_and_direction);
  RUN_TEST(boot_social_messages_reports_missing_required_messages);
  RUN_TEST(sort_boot_records_restores_room_order);
  RUN_TEST(sort_boot_records_reports_duplicate_vnum);
  RUN_TEST(sort_boot_records_renumbers_prototypes);
  RUN_TEST(discrete_load_marks_descending_vnum_for_sorting);
  RUN_TEST(parse_mobile_replaces_empty_alias_list);
  RUN_TEST(parse_simple_mob_rejects_values_that_would_wrap);
  RUN_TEST(parse_simple_mob_accepts_field_limits);
  RUN_TEST(room_zone_rejects_room_between_zones);
  RUN_TEST(room_zone_follows_ascending_rooms);
  RUN_TEST(setup_dir_reports_bad_exit_line_with_room_and_direction);
  RUN_TEST(parse_object_rejects_converted_record_in_no_zone);
  RUN_TEST(parse_mobile_rejects_converted_record_in_no_zone);
  RUN_TEST(load_zones_skips_blank_and_nul_lines);
  RUN_TEST(trigger_lines_see_their_owner_vnum);
  RUN_TEST(parse_object_converts_legacy_affect_field);
  RUN_TEST(load_config_reports_unknown_keys);
  RUN_TEST(load_config_reads_old_closed_doors_key);
  RUN_TEST(free_char_releases_mob_memory);
  return UNITY_END();
}
