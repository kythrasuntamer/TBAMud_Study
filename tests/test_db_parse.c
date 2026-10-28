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
  tb_test_exit_armed = 1; \
  if (setjmp(tb_test_exit_jmp) == 0) { \
    stmt; \
    tb_test_exit_armed = 0; \
    TEST_FAIL_MESSAGE("expected exit(): " #stmt); \
  } \
} while (0)

#define exit(code) tb_test_exit(code)
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
void char_to_room(struct char_data *ch, room_rnum room)
{ (void)ch; (void)room; }
void obj_to_char(struct obj_data *object, struct char_data *ch)
{ (void)object; (void)ch; }
void obj_to_obj(struct obj_data *obj, struct obj_data *obj_to)
{ (void)obj; (void)obj_to; }
void equip_char(struct char_data *ch, struct obj_data *obj, int pos)
{ (void)ch; (void)obj; (void)pos; }
void extract_obj(struct obj_data *obj) { (void)obj; }
struct obj_data *get_obj_num(obj_rnum nr) { (void)nr; return NULL; }
struct obj_data *get_obj_in_list_num(int num, struct obj_data *list)
{ (void)num; (void)list; return NULL; }
unsigned long circle_random(void) { return 0; }

/* World-file fixtures (constants.c, genolc.c, genzon.c). config.c is linked
 * whole: it is data only, and load_config() reads its defaults. */
size_t room_bits_count = 32;
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; return 0; }
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return 0; }
void dg_read_trigger(FILE *fp, void *proto, int type)
{ (void)fp; (void)proto; (void)type; }
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
void skip_spaces(char **string)                /* interpreter.c */
{ while (**string && isspace((unsigned char)**string)) (*string)++; }
void split_argument(char *argument, char *tag) /* oasis.c, verbatim logic */
{
  char *tmp = argument, *ttag = tag, *wrt = argument;
  for (; *tmp; tmp++) {
    if (*tmp != ' ' && *tmp != '=')
      *(ttag++) = *tmp;
    else if (*tmp == '=')
      break;
  }
  *ttag = '\0';
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
  rewind(fp);
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
  remove(temp_path);
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
  remove(temp_path);
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
  remove(temp_path);
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
  fclose(fp);
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
  fclose(fp);
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
  fclose(fp);
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

/* ---------------------------------------------------------------------
 * fread_string()/fread_clean_string(): '~'-terminated strings
 * ------------------------------------------------------------------- */

static char *read_tilde(char *(*reader)(FILE *, const char *),
                        const char *text, size_t len)
{
  FILE *fp = stream_of(text, len);
  char *s = reader(fp, "test-string");
  fclose(fp);
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
  fclose(fp);
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
  tb_test_exit_armed = 1;
  if (setjmp(tb_test_exit_jmp) == 0) {
    load_zones(fp, (char *)"test.zon");
    tb_test_exit_armed = 0;
    exited = 0;
  }
  fclose(fp);
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
  remove(temp_path);
}

/* B1-08: line[] was READ_SIZE - 2 bytes, but get_line() writes up to
 * READ_SIZE, so a 255+ character config line overflowed it by two bytes
 * (ASan: stack-buffer-overflow in split_argument). */
static void load_config_reads_full_length_line(void)
{
  char text[400];

  memcpy(text, "ok = ", 5);
  memset(text + 5, 'O', 300);
  memcpy(text + 305, "\n", 2);
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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(load_zones_rejects_top_nowhere);
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
  RUN_TEST(file_to_string_loads_whole_multiline_file);
  RUN_TEST(file_to_string_joins_long_lines_and_keeps_last_char);
  RUN_TEST(file_to_string_rejects_oversized_file);
  RUN_TEST(reset_zone_skips_trigger_after_load_purge);
  RUN_TEST(reset_zone_keeps_object_when_trigger_does_not_purge);
  RUN_TEST(reset_zone_wtrigger_walk_ends_at_top_max);
  RUN_TEST(reset_zone_disables_equip_with_bad_wear_pos);
  RUN_TEST(reset_zone_put_without_container_creates_nothing);
  RUN_TEST(reset_zone_object_variable_uses_arg2_context);
  return UNITY_END();
}
