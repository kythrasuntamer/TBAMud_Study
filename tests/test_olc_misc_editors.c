/* Executable tests of the production OLC editors (hedit.c and friends).
 *
 * Each editor is linked from its real source file and driven through its
 * public entry points (do_oasis_*, *_parse, *_string_cleanup) with a small
 * fixture world.  Everything the editors call outside their own file is
 * supplied here; unrelated code is discarded with --gc-sections (see
 * tests/CMakeLists.txt).  Tables are heap-allocated at their exact size so
 * ASan reports any index past the end, and each guard is checked for the
 * structured diagnostic it must emit (fail loudly), not just for not
 * crashing.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "oasis.h"
#include "hedit.h"
#include "prefedit.h"
#include "protocol.h"
#include "quest.h"
#include "act.h"
#include "msgedit.h"
#include "spells.h"
#include "modify.h"
#include "improved-edit.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tb_error_capture.h"

/* ---------------------------------------------------------------------
 * Output capture
 * ------------------------------------------------------------------- */
static char out[MAX_STRING_LENGTH * 2];

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

/* act() calls made with TO_CHAR, with the message as passed. */
static int act_to_char_calls;
static char act_last[MAX_STRING_LENGTH];

char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func; (void)hide_invisible;
  (void)ch; (void)obj; (void)vict_obj;
  if (type == TO_CHAR) {
    act_to_char_calls++;
    strlcpy(act_last, str ? str : "(null)", sizeof(act_last));
  }
  return NULL;
}

void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }

/* ---------------------------------------------------------------------
 * utils.c / genolc.c / interpreter.c helpers (utils.c is not linked: it owns
 * the real tb_error_log_at, which tb_error_capture.h replaces)
 * ------------------------------------------------------------------- */
int MAX(int a, int b) { return a > b ? a : b; }
int MIN(int a, int b) { return a < b ? a : b; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  void *p;
  (void)source_file; (void)source_line; (void)source_func;
  p = calloc(count ? count : 1, element_size ? element_size : 1);
  if (!p)
    abort();
  return p;
}
char *str_udup(const char *txt)
{ return strdup((txt && *txt) ? txt : "undefined"); }
void strip_cr(char *buffer)
{
  int rpos, wpos;
  if (buffer == NULL)
    return;
  for (rpos = 0, wpos = 0; buffer[rpos]; rpos++) {
    buffer[wpos] = buffer[rpos];
    wpos += (buffer[rpos] != '\r');
  }
  buffer[wpos] = '\0';
}
char *convert_from_tabs(char *string) { return string; }
char *one_argument(char *argument, char *first_arg)
{
  char *start;
  size_t n = 0;
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
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{ return one_argument(one_argument(argument, first_arg), second_arg); }
int room_is_dark(room_rnum room) { (void)room; return 0; }

/* ---------------------------------------------------------------------
 * OLC / editor collaborators
 * ------------------------------------------------------------------- */
const char *nrm = "", *grn = "", *cyn = "", *yel = "";
void get_char_colors(struct char_data *ch) { (void)ch; }
void clear_screen(struct descriptor_data *d) { (void)d; }
int can_edit_zone(struct char_data *ch, zone_rnum rnum)
{ (void)ch; (void)rnum; return TRUE; }
void add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; }
int remove_from_save_list(zone_vnum zone, int type)
{ (void)zone; (void)type; return TRUE; }

static char **string_write_target;
static char *string_write_backstr;
static int string_writes;

void string_write(struct descriptor_data *d, char **writeto, size_t len,
                  long mailto, void *data)
{
  (void)len; (void)mailto;
  string_writes++;
  string_write_target = writeto;
  string_write_backstr = data;
  d->str = writeto;
  d->backstr = data;
}

/* The parts of oasis.c's cleanup_olc() these editors rely on. */
static int cleanups;
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  if (d->olc == NULL)
    return;
  cleanups++;
  if (OLC_HELP(d)) {
    if (cleanup_type == CLEANUP_ALL) {
      free(OLC_HELP(d)->keywords);
      if (!OLC_HELP(d)->duplicate)
        free(OLC_HELP(d)->entry);
    }
    free(OLC_HELP(d));
  }
  free(OLC_PREFS(d));
  if (OLC_MSG_LIST(d))
    free_message_list(OLC_MSG_LIST(d));
  if (OLC_ACTION(d)) {
    /* oasis.c calls only free_action() for CLEANUP_ALL, which frees the
     * strings but not the struct (a production leak outside these
     * editors); free the struct here so the fixture is leak-clean. */
    if (cleanup_type == CLEANUP_ALL)
      free_action(OLC_ACTION(d));
    free(OLC_ACTION(d));
  }
  if (OLC_QUEST(d)) {
    if (cleanup_type == CLEANUP_ALL)
      free_quest(OLC_QUEST(d));
    else
      free(OLC_QUEST(d));
  }
  free(OLC_STORAGE(d));
  STATE(d) = CON_PLAYING;
  free(d->olc);
  d->olc = NULL;
}

/* ---------------------------------------------------------------------
 * Help table fixture
 * ------------------------------------------------------------------- */
struct help_index_element *help_table = NULL;
int top_of_helpt = 0;
static int help_reboots;

static int search_result;
int search_help(const char *argument, int level)
{ (void)argument; (void)level; return search_result; }

void free_help_table(void)
{
  int i;

  for (i = 0; i < top_of_helpt; i++) {
    free(help_table[i].keywords);
    if (!help_table[i].duplicate)
      free(help_table[i].entry);
  }
  free(help_table);
  help_table = NULL;
  top_of_helpt = 0;
}

/* hedit_save_to_disk reboots the table after a committed write.  The test
 * keeps the in-memory table instead, so it can inspect what was stored. */
void index_boot(int mode) { (void)mode; help_reboots++; }

/* Rows: "alpha" (owner), "alpha2" (duplicate of row 0), "beta". */
static void help_fixture(void)
{
  free_help_table();
  top_of_helpt = 3;
  help_table = calloc((size_t)top_of_helpt, sizeof(*help_table));
  help_table[0].keywords = strdup("alpha");
  help_table[0].entry = strdup("ALPHA\r\n\r\nAlpha text.\r\n");
  help_table[1].keywords = strdup("alpha2");
  help_table[1].entry = help_table[0].entry;
  help_table[1].duplicate = 1;
  help_table[2].keywords = strdup("beta");
  help_table[2].entry = strdup("BETA\r\n\r\nBeta text.\r\n");
}

/* is_abbrev is real in interpreter.c; the editors only need prefix match. */
int is_abbrev(const char *arg1, const char *arg2)
{
  if (!*arg1)
    return 0;
  for (; *arg1 && *arg2; arg1++, arg2++)
    if (LOWER(*arg1) != LOWER(*arg2))
      return 0;
  return !*arg1;
}

/* ---------------------------------------------------------------------
 * Quest table fixture (genqst.c / quest.c collaborators)
 * ------------------------------------------------------------------- */
struct aq_data *aquest_table = NULL;
qst_rnum total_quests = 0;
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
struct char_data *mob_proto = NULL;
struct obj_data *obj_proto = NULL;
const char *quest_types[] = {
  "Object", "Room", "Find mob", "Kill mob", "Save mob", "Return object",
  "Clear room", "\n"
};
const char *aq_flags[] = { "REPEATABLE", "\n" };
static int quest_deletes, quest_adds, quest_saves;

qst_rnum real_quest(qst_vnum vnum)
{
  qst_rnum i;
  for (i = 0; i < total_quests; i++)
    if (aquest_table[i].vnum == vnum)
      return i;
  return NOTHING;
}
int delete_quest(qst_rnum rnum)
{
  /* The production delete_quest reads aquest_table[rnum] before its own
   * range test; any caller passing an invalid rnum is a bug. */
  TEST_ASSERT_TRUE_MESSAGE(rnum < total_quests, "delete_quest got an invalid rnum");
  quest_deletes++;
  return TRUE;
}
/* As genqst.c: an existing row is replaced by copies of nqst's strings;
 * the caller keeps ownership of nqst. */
int add_quest(struct aq_data *nqst)
{
  qst_rnum rnum = real_quest(nqst->vnum);
  quest_adds++;
  TEST_ASSERT_TRUE_MESSAGE(rnum != NOTHING, "fixture only replaces rows");
  free_quest_strings(&aquest_table[rnum]);
  copy_quest(&aquest_table[rnum], nqst, FALSE);
  return rnum;
}
static int save_quests_result = TRUE;
int save_quests(zone_rnum zone_num) { (void)zone_num; quest_saves++; return save_quests_result; }
void free_quest_strings(struct aq_data *quest)
{
  free(quest->name); free(quest->desc); free(quest->info);
  free(quest->done); free(quest->quit);
}
void free_quest(struct aq_data *quest) { free_quest_strings(quest); free(quest); }
int copy_quest(struct aq_data *to, struct aq_data *from, int free_old_strings)
{
  (void)free_old_strings;
  *to = *from;
  to->name = strdup(from->name); to->desc = strdup(from->desc);
  to->info = strdup(from->info); to->done = strdup(from->done);
  to->quit = strdup(from->quit);
  return TRUE;
}
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return 0; }
zone_rnum real_zone(zone_vnum vnum) { (void)vnum; return 0; }
room_vnum genolc_zone_bottom(zone_rnum rznum) { (void)rznum; return 0; }
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; return NOBODY; }
obj_rnum real_object(obj_vnum vnum) { (void)vnum; return NOTHING; }
int is_number(const char *str)
{
  if (!*str) return 0;
  for (; *str; str++) if (!isdigit((unsigned char)*str)) return 0;
  return 1;
}
void column_list(struct char_data *ch, int num_cols, const char **list,
                 int list_length, bool show_nums)
{ (void)ch; (void)num_cols; (void)list; (void)list_length; (void)show_nums; }
size_t sprintbit(bitvector_t vektor, const char *names[], char *result,
                 size_t reslen)
{ (void)vektor; (void)names; if (reslen) result[0] = '\0'; return 0; }
int genolc_checkstring(struct descriptor_data *d, char *arg)
{ (void)d; (void)arg; return TRUE; }

static struct zone_data fixture_zone;

static void quest_fixture(void)
{
  memset(&fixture_zone, 0, sizeof(fixture_zone));
  fixture_zone.number = 30;
  zone_table = &fixture_zone;
  top_of_zone_table = 0;
  total_quests = 1;
  aquest_table = calloc(1, sizeof(*aquest_table));
  aquest_table[0].vnum = 3001;
  aquest_table[0].type = AQ_OBJ_FIND;
  aquest_table[0].qm = NOBODY;
  aquest_table[0].name = strdup("Fetch");
  aquest_table[0].desc = strdup("Fetch it");
  aquest_table[0].info = strdup("info\r\n");
  aquest_table[0].done = strdup("done\r\n");
  aquest_table[0].quit = strdup("quit\r\n");
  quest_deletes = quest_adds = quest_saves = 0;
  save_quests_result = TRUE;
}

static void quest_fixture_free(void)
{
  qst_rnum i;
  for (i = 0; i < total_quests; i++)
    free_quest_strings(&aquest_table[i]);
  free(aquest_table);
  aquest_table = NULL;
  total_quests = 0;
}

/* ---------------------------------------------------------------------
 * Socials fixture (act.social.c / interpreter.c collaborators)
 * ------------------------------------------------------------------- */
struct social_messg *soc_mess_list = NULL;
int top_of_socialt = -1;
static struct command_info fixture_cmds[] = {
  { "RESERVED", "", 0, 0, 0, 0 },
  { "look", "look", 0, 0, 0, 0 },
  { "\n", "zzzzzzz", 0, 0, 0, 0 }
};
struct command_info *complete_cmd_info = fixture_cmds;
int *cmd_sort_info = NULL;
const char *position_types[] = {
  "Dead", "Mortally wounded", "Incapacitated", "Stunned", "Sleeping",
  "Resting", "Sitting", "Fighting", "Standing", "\n"
};
static int command_list_builds, command_sorts;
void create_command_list(void) { command_list_builds++; }
void sort_commands(void) { command_sorts++; }
void free_action(struct social_messg *mess)
{
  free(mess->command); free(mess->sort_as);
  free(mess->char_no_arg); free(mess->others_no_arg);
  free(mess->char_found); free(mess->others_found); free(mess->vict_found);
  free(mess->char_body_found); free(mess->others_body_found);
  free(mess->vict_body_found); free(mess->not_found);
  free(mess->char_auto); free(mess->others_auto);
  free(mess->char_obj_found); free(mess->others_obj_found);
  memset(mess, 0, sizeof(*mess));
}
char *delete_doubledollar(char *string) { return string; }

static void social_fixture(void)
{
  top_of_socialt = 0;
  soc_mess_list = calloc(1, sizeof(*soc_mess_list));
  soc_mess_list[0].command = strdup("smile");
  soc_mess_list[0].sort_as = strdup("smile");
  soc_mess_list[0].min_char_position = POS_RESTING;
  soc_mess_list[0].min_victim_position = POS_RESTING;
  soc_mess_list[0].char_no_arg = strdup("You smile.");
  config_info.operation.use_new_socials = 1;
}

static void social_fixture_free(void)
{
  int i;
  for (i = 0; i <= top_of_socialt; i++)
    free_action(&soc_mess_list[i]);
  free(soc_mess_list);
  soc_mess_list = NULL;
  top_of_socialt = -1;
  free(cmd_sort_info);
  cmd_sort_info = NULL;
}

/* ---------------------------------------------------------------------
 * Text files (db.c globals not already in test_stubs.c)
 * ------------------------------------------------------------------- */
char *credits = NULL, *news = NULL, *help = NULL, *ihelp = NULL,
     *info = NULL, *wizlist = NULL, *immlist = NULL, *handbook = NULL,
     *policies = NULL;

/* ---------------------------------------------------------------------
 * Combat messages (fight.c / spell_parser.c globals)
 * ------------------------------------------------------------------- */
struct message_list fight_messages[MAX_MESSAGES];
struct spell_info_type spell_info[TOP_SPELL_DEFINE + 1];

/* ---------------------------------------------------------------------
 * Builder fixture
 * ------------------------------------------------------------------- */
static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;
static protocol_t builder_protocol;
static MSDP_t builder_msdp[eMSDP_MAX];
static MSDP_t *builder_msdp_ptrs[eMSDP_MAX];

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
  {
    int i;
    memset(&builder_protocol, 0, sizeof(builder_protocol));
    memset(builder_msdp, 0, sizeof(builder_msdp));
    for (i = 0; i < eMSDP_MAX; i++)
      builder_msdp_ptrs[i] = &builder_msdp[i];
    builder_protocol.pVariables = builder_msdp_ptrs;
    builder_desc.pProtocol = &builder_protocol;
  }
  descriptor_list = &builder_desc;
  character_list = &builder;
}

/* A second, heap-allocated player for editors that target someone else;
 * freeing it lets ASan catch any pointer an editor kept. */
static struct char_data *victim;

static struct char_data *victim_create(const char *name, long idnum)
{
  struct char_data *v = calloc(1, sizeof(*v));

  v->player_specials = calloc(1, sizeof(*v->player_specials));
  v->player.name = strdup(name);
  v->player.level = 1;
  v->char_specials.saved.idnum = idnum;
  v->next = character_list;
  character_list = v;
  return v;
}

/* As extract_char_final + free_char: unlink, then free. */
static void victim_destroy(void)
{
  struct char_data **pp;

  if (!victim)
    return;
  for (pp = &character_list; *pp; pp = &(*pp)->next)
    if (*pp == victim) {
      *pp = victim->next;
      break;
    }
  free(victim->player.name);
  free(victim->player_specials);
  free(victim);
  victim = NULL;
}

struct char_data *get_player_vis(struct char_data *ch, char *name,
                                 int *number, int inroom)
{
  (void)ch; (void)number; (void)inroom;
  return (victim && !strcasecmp(name, GET_NAME(victim))) ? victim : NULL;
}

static void type(const char *line)
{
  char buf[MAX_INPUT_LENGTH];

  strlcpy(buf, line, sizeof(buf));
  out[0] = '\0';
  switch (STATE(&builder_desc)) {
  case CON_HEDIT: hedit_parse(&builder_desc, buf); break;
  case CON_PREFEDIT: prefedit_parse(&builder_desc, buf); break;
  case CON_QEDIT: qedit_parse(&builder_desc, buf); break;
  case CON_AEDIT: aedit_parse(&builder_desc, buf); break;
  case CON_MSGEDIT: msgedit_parse(&builder_desc, buf); break;
  default: TEST_FAIL_MESSAGE("typed into a descriptor that is not editing");
  }
}

void setUp(void)
{
  tb_error_capture_reset();
  out[0] = '\0';
  cleanups = help_reboots = string_writes = 0;
  act_to_char_calls = 0;
  act_last[0] = '\0';
  string_write_target = NULL;
  string_write_backstr = NULL;
  builder_fixture();
  help_fixture();
  quest_fixture();
  social_fixture();
  spell_info[0].name = "!UNUSED!";
  command_list_builds = command_sorts = 0;
}

void tearDown(void)
{
  if (builder_desc.olc)
    cleanup_olc(&builder_desc, CLEANUP_ALL);
  free_help_table();
  quest_fixture_free();
  free_messages();
  social_fixture_free();
  victim_destroy();
  descriptor_list = NULL;
  character_list = NULL;
}

/* ---------------------------------------------------------------------
 * hedit
 * ------------------------------------------------------------------- */

/* F2-01: answering 'n' when the next row does not match must move to
 * "add" with OLC_ZNUM == NOWHERE; it used to leave top_of_helpt + 2 there and
 * the save wrote past help_table. */
static void test_hedit_no_then_add_appends_instead_of_writing_past_table(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_HEDIT, STATE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_EDIT, OLC_MODE(&builder_desc));

  type("n");
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_ADD, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(NOWHERE, OLC_ZNUM(&builder_desc));

  type("y");
  type("2");
  type("5");
  type("q");
  type("y");
  TEST_ASSERT_EQUAL_INT(4, top_of_helpt);
  TEST_ASSERT_EQUAL_STRING("beta", help_table[3].keywords);
  TEST_ASSERT_EQUAL_INT(5, help_table[3].min_level);
  TEST_ASSERT_NULL(builder_desc.olc);
}

/* F2-01: a matching later row is offered as an edit, not skipped.
 * F2-19: duplicate rows (row 1) are not offered; their owner is. */
static void test_hedit_no_offers_next_matching_row(void)
{
  char cmd[] = " alpha";

  free(help_table[2].keywords);
  help_table[2].keywords = strdup("alphabet");
  search_result = 0;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("n");
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_EDIT, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(2, OLC_ZNUM(&builder_desc));
  type("n");
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_ADD, OLC_MODE(&builder_desc));
  type("q");
}

/* F2-19: replacing a row frees the old strings and moves the duplicate
 * rows that shared the old entry onto the new one (ASan/Valgrind see the
 * leak or a dangling duplicate).  The disk write fails in the test tree,
 * so the table is not rebooted and can be inspected. */
static void test_hedit_replace_frees_old_row_and_keeps_duplicates_valid(void)
{
  char cmd[] = " alpha";

  search_result = 0;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  type("2");
  type("7");
  type("q");
  type("y");
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(3, top_of_helpt);
  TEST_ASSERT_EQUAL_INT(7, help_table[0].min_level);
  TEST_ASSERT_TRUE(help_table[1].entry == help_table[0].entry);
  TEST_ASSERT_EQUAL_STRING("ALPHA\r\n\r\nAlpha text.\r\n", help_table[1].entry);
}

/* F2-19: a save aimed at a duplicate row would be skipped by the writer;
 * it is refused loudly and the table is unchanged. */
static void test_hedit_save_to_duplicate_row_is_refused_loudly(void)
{
  char cmd[] = " alpha";

  search_result = 0;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  type("2");
  type("9");
  type("q");
  OLC_ZNUM(&builder_desc) = 1;   /* simulate a walk that landed on a duplicate */
  tb_error_capture_reset();
  type("y");
  TEST_ASSERT_EQUAL_STRING("hedit-duplicate-save", tb_captured_operation);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(0, help_table[1].min_level);
  TEST_ASSERT_TRUE(help_table[1].entry == help_table[0].entry);
}

/* F2-01: a save whose row went stale is refused with a diagnostic. */
static void test_hedit_save_with_stale_rnum_is_refused_loudly(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  type("2");
  type("5");
  type("q");
  OLC_ZNUM(&builder_desc) = 7;  /* simulate a corrupted index */
  type("y");
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("hedit-rnum-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
  TEST_ASSERT_EQUAL_INT(3, top_of_helpt);
  TEST_ASSERT_NULL(builder_desc.olc);
}

/* F2-02: a search miss (NOWHERE) must offer "add" without indexing
 * help_table[NOWHERE]. */
static void test_hedit_new_topic_does_not_index_nowhere(void)
{
  char cmd[] = " gamma";

  search_result = NOWHERE;
  do_oasis_hedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_ADD, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  type("n");
  TEST_ASSERT_NULL(builder_desc.olc);
}

/* F2-02: a duplicate keyword is redirected to its owning row. */
static void test_hedit_duplicate_redirects_to_owner(void)
{
  char cmd[] = " alpha2";

  search_result = 1;
  do_oasis_hedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_EDIT, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(0, OLC_ZNUM(&builder_desc));
}

/* F2-02: an orphan duplicate is refused with a warning. */
static void test_hedit_orphan_duplicate_is_refused_loudly(void)
{
  char cmd[] = " alpha2";
  char *owner_entry = help_table[0].entry;

  help_table[0].entry = strdup("other");      /* row 1 loses its owner */
  search_result = 1;
  do_oasis_hedit(&builder, cmd, 0, 0);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("hedit-duplicate-owner", tb_captured_operation);
  free(help_table[0].entry);
  help_table[0].entry = owner_entry;
}

/* F2-03: with a NULL entry, '1' must hand the editor a NULL (or heap)
 * backstr, never a string literal that string_add later frees; and a save
 * or abort that leaves the entry NULL puts back a placeholder, loudly. */
static void test_hedit_null_entry_round_trip(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  free(OLC_HELP(&builder_desc)->entry);
  OLC_HELP(&builder_desc)->entry = NULL;  /* as after '/c' then '/s' */
  type("1");
  TEST_ASSERT_EQUAL_INT(1, string_writes);
  TEST_ASSERT_NULL(string_write_backstr);
  TEST_ASSERT_TRUE(string_write_target == &OLC_HELP(&builder_desc)->entry);

  hedit_string_cleanup(&builder_desc, STRINGADD_SAVE);
  TEST_ASSERT_NOT_NULL(OLC_HELP(&builder_desc)->entry);
  TEST_ASSERT_EQUAL_STRING("hedit-empty-entry", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(HEDIT_MAIN_MENU, OLC_MODE(&builder_desc));
}

/* F2-29: an impossible editor mode is reported with the mode and editor,
 * not a mudlog whose arguments were in the wrong order. */
static void test_hedit_bad_mode_is_reported(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  OLC_MODE(&builder_desc) = 99;
  tb_error_capture_reset();
  type("x");
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("bad-mode", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "mode 99"));
  TEST_ASSERT_EQUAL_INT(HEDIT_MAIN_MENU, OLC_MODE(&builder_desc));
}

/* F2-21: the help index cannot be written in the test tree (no text/help
 * directory); the builder is told and the failure names the editor. */
static void test_hedit_failed_disk_save_is_loud(void)
{
  char cmd[] = " beta";

  search_result = 2;
  do_oasis_hedit(&builder, cmd, 0, 0);
  type("y");
  type("2");
  type("5");
  type("q");
  tb_error_capture_reset();
  type("y");
  TEST_ASSERT_EQUAL_STRING("hedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
  TEST_ASSERT_NOT_NULL(strstr(out, "FAILED"));
  TEST_ASSERT_NULL(strstr(out, "saved to disk"));
  TEST_ASSERT_EQUAL_INT(5, help_table[2].min_level);
}

/* ---------------------------------------------------------------------
 * prefedit
 * ------------------------------------------------------------------- */

/* F2-06: the edited player is extracted while an IMPL's editor is open;
 * the next input must not touch the freed character. */
static void test_prefedit_target_extracted_mid_edit(void)
{
  char cmd[] = " bob";

  victim = victim_create("Bob", 77);
  do_oasis_prefedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_PREFEDIT, STATE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(77, OLC_PREFS(&builder_desc)->idnum);

  victim_destroy();
  type("t");
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&builder_desc));
  TEST_ASSERT_EQUAL_STRING("prefedit-target-gone", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 77"));
}

/* F2-10: the player whose prefs an IMPL is editing types "prefedit".  They
 * get the send_to_char notice only: act() used to run as well, with a
 * buffer formatted only in the other branch. */
static void test_prefedit_self_while_being_edited_sends_one_message(void)
{
  char cmd[] = " bob", none[] = "";
  struct descriptor_data victim_desc;

  victim = victim_create("Bob", 77);
  memset(&victim_desc, 0, sizeof(victim_desc));
  victim_desc.character = victim;
  victim_desc.connected = CON_PLAYING;
  victim->desc = &victim_desc;

  do_oasis_prefedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_PREFEDIT, STATE(&builder_desc));
  out[0] = '\0';
  do_oasis_prefedit(victim, none, 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "Your preferences are currently being edited"));
  TEST_ASSERT_EQUAL_INT(0, act_to_char_calls);
  TEST_ASSERT_NULL(victim_desc.olc);
  victim->desc = NULL;
}

/* F2-06: editing one's own preferences still works. */
static void test_prefedit_self_still_resolves(void)
{
  char cmd[] = "";

  do_oasis_prefedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_PREFEDIT, STATE(&builder_desc));
  type("t");
  TEST_ASSERT_NOT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* ---------------------------------------------------------------------
 * qedit
 * ------------------------------------------------------------------- */

/* F2-07: a new quest is AQ_UNDEFINED (-1); the menu must not index
 * quest_types[-1]. */
static void test_qedit_new_quest_menu_names_undefined_type(void)
{
  char cmd[] = " 3050";

  do_oasis_qedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_QEDIT, STATE(&builder_desc));
  TEST_ASSERT_NOT_NULL(strstr(out, "Quest Type     : \tcUndefined"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* F2-07: an out-of-range stored type is shown as Undefined, loudly. */
static void test_qedit_bad_stored_type_is_reported(void)
{
  char cmd[] = " 3001";

  aquest_table[0].type = 99;
  do_oasis_qedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_STRING("quest-type-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "quest vnum 3001 type 99"));
}

/* F2-08: deleting a quest that was never saved must not call delete_quest
 * with NOTHING (it read aquest_table[NOTHING] before its range test). */
static void test_qedit_delete_unsaved_quest_is_refused_loudly(void)
{
  char cmd[] = " 3050";

  do_oasis_qedit(&builder, cmd, 0, 0);
  type("x");
  TEST_ASSERT_EQUAL_INT(QEDIT_CONFIRM_DELETE, OLC_MODE(&builder_desc));
  type("y");
  TEST_ASSERT_EQUAL_INT(0, quest_deletes);
  TEST_ASSERT_EQUAL_INT(0, quest_saves);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_STRING("qedit-delete-unsaved", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "quest vnum 3050"));
}

/* F2-08: deleting a saved quest still works. */
static void test_qedit_delete_saved_quest(void)
{
  char cmd[] = " 3001";

  do_oasis_qedit(&builder, cmd, 0, 0);
  type("x");
  type("y");
  TEST_ASSERT_EQUAL_INT(1, quest_deletes);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* F2-13: add_quest copies the strings, so saving must free the editor's
 * copies (CLEANUP_ALL).  CLEANUP_STRUCTS leaked them: LeakSanitizer fails
 * this test on the original code. */
static void test_qedit_save_frees_editor_strings(void)
{
  char cmd[] = " 3001";

  do_oasis_qedit(&builder, cmd, 0, 0);
  type("1");
  type("Renamed");
  type("q");
  type("y");
  TEST_ASSERT_EQUAL_INT(1, quest_adds);
  TEST_ASSERT_EQUAL_STRING("Renamed", aquest_table[0].name);
  TEST_ASSERT_NULL(builder_desc.olc);
}

/* F2-21: a quest file that could not be written is reported to the builder
 * and in the log, not announced as "saved to disk". */
static void test_qedit_failed_disk_save_is_loud(void)
{
  char cmd[] = " 3001";

  config_info.operation.auto_save_olc = 1;
  save_quests_result = FALSE;
  do_oasis_qedit(&builder, cmd, 0, 0);
  type("1");
  type("Renamed");
  type("q");
  type("y");
  config_info.operation.auto_save_olc = 0;
  TEST_ASSERT_EQUAL_INT(1, quest_saves);
  TEST_ASSERT_EQUAL_STRING("qedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "quest vnum 3001"));
  TEST_ASSERT_NOT_NULL(strstr(out, "FAILED"));
  TEST_ASSERT_NULL(strstr(out, "saved to disk"));
  TEST_ASSERT_NULL(builder_desc.olc);
}

/* ---------------------------------------------------------------------
 * aedit
 * ------------------------------------------------------------------- */

/* F2-21: the socials file cannot be written in the test tree (no misc/
 * directory); the save must say so instead of "Action saved to disk". */
static void test_aedit_failed_disk_save_is_loud(void)
{
  char cmd[] = " smile";

  do_oasis_aedit(&builder, cmd, 0, 0);
  type("y");
  type("4");
  type("5");
  type("q");
  tb_error_capture_reset();
  type("y");
  TEST_ASSERT_EQUAL_STRING("aedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(out, "FAILED"));
  TEST_ASSERT_NULL(strstr(out, "saved to disk"));
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_INT(5, soc_mess_list[0].min_level_char);
}

/* F2-09: out-of-range positions and levels are re-prompted, loudly; the old
 * '&&' tests accepted any int and the menu then indexed position_types[]
 * with it. */
static void test_aedit_rejects_out_of_range_position_and_level(void)
{
  char cmd[] = " smile";

  do_oasis_aedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_AEDIT, STATE(&builder_desc));
  type("y");
  type("2");
  type("10");
  TEST_ASSERT_EQUAL_INT(AEDIT_MIN_CHAR_POS, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_STRING("aedit-input-range", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(POS_RESTING, OLC_ACTION(&builder_desc)->min_char_position);
  type("junk");
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  type("8");
  TEST_ASSERT_EQUAL_INT(POS_STANDING, OLC_ACTION(&builder_desc)->min_char_position);
  TEST_ASSERT_EQUAL_INT(AEDIT_MAIN_MENU, OLC_MODE(&builder_desc));

  type("4");
  type("-1");
  TEST_ASSERT_EQUAL_INT(AEDIT_MIN_CHAR_LEVEL, OLC_MODE(&builder_desc));
  TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);
  type("5");
  TEST_ASSERT_EQUAL_INT(5, OLC_ACTION(&builder_desc)->min_level_char);
}

/* F2-09: a bad stored position (from the socials file) is shown as invalid
 * by the level-0 astat, with a warning, not read out of bounds. */
static void test_astat_bad_stored_position_is_reported(void)
{
  char cmd[] = " smile";

  soc_mess_list[0].min_victim_position = 99;
  do_astat(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_STRING("social-position-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(out, "<invalid"));  /* %-8.8s */
}

/* ---------------------------------------------------------------------
 * tedit
 * ------------------------------------------------------------------- */

/* Editors that save text files write relative to the lib directory.  Run
 * those tests inside a fresh temporary directory; with_text_dir creates
 * text/ so saves can commit. */
static char saved_cwd[4096];
static char scratch_dir[256];

static void enter_scratch_dir(int with_text_dir)
{
  TEST_ASSERT_NOT_NULL(getcwd(saved_cwd, sizeof(saved_cwd)));
  strlcpy(scratch_dir, "/tmp/tb-olc-test-XXXXXX", sizeof(scratch_dir));
  TEST_ASSERT_NOT_NULL(mkdtemp(scratch_dir));
  TEST_ASSERT_EQUAL_INT(0, chdir(scratch_dir));
  if (with_text_dir)
    TEST_ASSERT_EQUAL_INT(0, mkdir("text", 0700));
}

static void leave_scratch_dir(void)
{
  char cmd[512];

  TEST_ASSERT_EQUAL_INT(0, chdir(saved_cwd));
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", scratch_dir);
  TEST_ASSERT_EQUAL_INT(0, system(cmd));
}

static char *slurp(const char *path)
{
  static char buf[4096];
  FILE *fl = fopen(path, "r");
  size_t n;

  if (!fl)
    return NULL;
  n = fread(buf, 1, sizeof(buf) - 1, fl);
  buf[n] = '\0';
  fclose(fl);
  return buf;
}

/* F2-04: the editor works on a private copy.  '/c' must not free or NULL
 * the live GREETINGS, and saving an empty buffer publishes "" (comm.c runs
 * strlen(GREETINGS) on each connection) with a warning. */
static void test_tedit_clear_then_save_keeps_greetings_non_null(void)
{
  char cmd[] = " greetings";

  enter_scratch_dir(TRUE);
  GREETINGS = strdup("Welcome!\r\n");
  do_tedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_TEDIT, STATE(&builder_desc));
  TEST_ASSERT_TRUE(builder_desc.str != &GREETINGS);

  /* improved-edit '/c': free the buffer being edited and NULL it. */
  free(*builder_desc.str);
  *builder_desc.str = NULL;
  TEST_ASSERT_EQUAL_STRING("Welcome!\r\n", GREETINGS);

  free(builder_desc.backstr);    /* string_add's STRINGADD_SAVE */
  builder_desc.backstr = NULL;
  tedit_string_cleanup(&builder_desc, STRINGADD_SAVE);
  TEST_ASSERT_NOT_NULL(GREETINGS);
  TEST_ASSERT_EQUAL_STRING("", GREETINGS);
  TEST_ASSERT_EQUAL_STRING("tedit-empty", tb_captured_operation);
  TEST_ASSERT_NULL(builder_desc.olc);
  TEST_ASSERT_EQUAL_STRING("", slurp("text/greetings"));
  free(GREETINGS);
  GREETINGS = NULL;
  leave_scratch_dir();
}

/* F2-15: a save that cannot be written is reported with errno, the builder
 * is told, and neither the file nor the live text changes. */
static void test_tedit_failed_save_is_loud_and_changes_nothing(void)
{
  char cmd[] = " motd";
  char *before;

  enter_scratch_dir(FALSE);          /* no text/ directory: fopen fails */
  motd = strdup("Old motd\r\n");
  before = motd;
  do_tedit(&builder, cmd, 0, 0);
  free(*builder_desc.str);
  *builder_desc.str = strdup("New motd\r\n");
  free(builder_desc.backstr);
  builder_desc.backstr = NULL;
  tedit_string_cleanup(&builder_desc, STRINGADD_SAVE);
  TEST_ASSERT_TRUE(motd == before);
  TEST_ASSERT_EQUAL_STRING("Old motd\r\n", motd);
  TEST_ASSERT_EQUAL_STRING("tedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(out, "Save FAILED"));
  TEST_ASSERT_NULL(builder_desc.olc);
  free(motd);
  motd = NULL;
  leave_scratch_dir();
}

/* F2-15: a successful save replaces the file and then the live text. */
static void test_tedit_save_commits_file_then_text(void)
{
  char cmd[] = " motd";

  enter_scratch_dir(TRUE);
  motd = strdup("Old motd\r\n");
  do_tedit(&builder, cmd, 0, 0);
  free(*builder_desc.str);
  *builder_desc.str = strdup("New motd\r\n");
  free(builder_desc.backstr);
  builder_desc.backstr = NULL;
  tedit_string_cleanup(&builder_desc, STRINGADD_SAVE);
  TEST_ASSERT_EQUAL_STRING("New motd\n", motd);
  TEST_ASSERT_EQUAL_STRING("New motd\n", slurp("text/motd"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(motd);
  motd = NULL;
  leave_scratch_dir();
}

/* F2-04: an abort leaves the live text exactly as it was. */
static void test_tedit_abort_leaves_live_text(void)
{
  char cmd[] = " motd";
  char *before;

  motd = strdup("Old motd\r\n");
  before = motd;
  do_tedit(&builder, cmd, 0, 0);
  /* string_add's abort: restore backstr into the edited buffer. */
  free(*builder_desc.str);
  *builder_desc.str = builder_desc.backstr;
  builder_desc.backstr = NULL;
  tedit_string_cleanup(&builder_desc, STRINGADD_ABORT);
  TEST_ASSERT_TRUE(motd == before);
  TEST_ASSERT_EQUAL_STRING("Old motd\r\n", motd);
  TEST_ASSERT_NULL(builder_desc.olc);
  free(motd);
  motd = NULL;
}

/* ---------------------------------------------------------------------
 * Entry points without a descriptor
 * ------------------------------------------------------------------- */

/* F2-11: a linkdead player forced (e.g. by a DG mforce) to run an editor
 * has no descriptor; each entry point refuses, loudly, before touching
 * ch->desc. */
static void test_editors_without_descriptor_refuse_loudly(void)
{
  char none[] = "", quest[] = " 3001", msg[] = " 1";

  victim = victim_create("Bob", 77);
  victim->player.level = LVL_IMPL;

  do_oasis_prefedit(victim, none, 0, 0);
  TEST_ASSERT_EQUAL_STRING("olc-no-descriptor", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 77 has_desc 0"));

  tb_error_capture_reset();
  do_oasis_qedit(victim, quest, 0, 0);
  TEST_ASSERT_EQUAL_STRING("olc-no-descriptor", tb_captured_operation);

  tb_error_capture_reset();
  do_msgedit(victim, msg, 0, 0);
  TEST_ASSERT_EQUAL_STRING("olc-no-descriptor", tb_captured_operation);

  tb_error_capture_reset();
  do_msgedit(victim, none, 0, 0);
  TEST_ASSERT_EQUAL_STRING("olc-no-descriptor", tb_captured_operation);
}

/* ---------------------------------------------------------------------
 * msgedit
 * ------------------------------------------------------------------- */

/* F2-14: a combat-message save that cannot be written is reported and the
 * builder told; it used to exit(1) the whole server. */
static void test_msgedit_failed_save_is_loud_and_does_not_exit(void)
{
  char cmd[] = " 1";

  enter_scratch_dir(FALSE);          /* no misc/ directory */
  do_msgedit(&builder, cmd, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_MSGEDIT, STATE(&builder_desc));
  type("a");
  type("You slay $N!");
  type("s");
  type("y");
  TEST_ASSERT_EQUAL_STRING("msgedit-save", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "slot 1 editor player idnum 42"));
  TEST_ASSERT_NOT_NULL(strstr(out, "FAILED"));
  TEST_ASSERT_NOT_NULL(builder_desc.olc);
  leave_scratch_dir();
}

/* F2-14: a writable lib/misc commits the file. */
static void test_msgedit_save_commits_file(void)
{
  char cmd[] = " 1";

  enter_scratch_dir(FALSE);
  TEST_ASSERT_EQUAL_INT(0, mkdir("misc", 0700));
  do_msgedit(&builder, cmd, 0, 0);
  type("a");
  type("You slay $N!");
  type("s");
  type("y");
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_NOT_NULL(strstr(slurp("misc/messages"), "You slay $N!"));
  leave_scratch_dir();
}

/* F2-16: one GOD's 'Q' (save prompt that ends the session) must not make
 * another GOD's 'S' prompt end theirs; the intent used to be a static. */
static void test_msgedit_quit_intent_is_per_descriptor(void)
{
  char cmd1[] = " 1", cmd2[] = " 2", buf[MAX_INPUT_LENGTH];
  struct descriptor_data other;

  do_msgedit(&builder, cmd1, 0, 0);
  type("a");
  type("You slay $N!");
  type("q");
  TEST_ASSERT_EQUAL_INT(MSGEDIT_CONFIRM_SAVE_QUIT, OLC_MODE(&builder_desc));

  /* A second GOD on another descriptor edits slot 2 and answers 'S'/'n'. */
  memset(&other, 0, sizeof(other));
  other.character = &builder;
  other.connected = CON_PLAYING;
  other.next = descriptor_list;
  descriptor_list = &other;
  builder.desc = &other;
  do_msgedit(&builder, cmd2, 0, 0);
  TEST_ASSERT_NOT_NULL(other.olc);
  strlcpy(buf, "s", sizeof(buf));
  msgedit_parse(&other, buf);
  TEST_ASSERT_EQUAL_INT(MSGEDIT_CONFIRM_SAVE, OLC_MODE(&other));
  strlcpy(buf, "n", sizeof(buf));
  msgedit_parse(&other, buf);
  TEST_ASSERT_NOT_NULL_MESSAGE(other.olc, "'S' then 'n' must not end the session");
  TEST_ASSERT_EQUAL_INT(MSGEDIT_MAIN_MENU, OLC_MODE(&other));
  cleanup_olc(&other, CLEANUP_ALL);
  descriptor_list = other.next;
  builder.desc = &builder_desc;

  /* The first GOD's answer still ends their session. */
  type("n");
  TEST_ASSERT_NULL(builder_desc.olc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_msgedit_failed_save_is_loud_and_does_not_exit);
  RUN_TEST(test_msgedit_save_commits_file);
  RUN_TEST(test_msgedit_quit_intent_is_per_descriptor);
  RUN_TEST(test_editors_without_descriptor_refuse_loudly);
  RUN_TEST(test_tedit_clear_then_save_keeps_greetings_non_null);
  RUN_TEST(test_tedit_abort_leaves_live_text);
  RUN_TEST(test_tedit_failed_save_is_loud_and_changes_nothing);
  RUN_TEST(test_tedit_save_commits_file_then_text);
  RUN_TEST(test_aedit_rejects_out_of_range_position_and_level);
  RUN_TEST(test_astat_bad_stored_position_is_reported);
  RUN_TEST(test_qedit_delete_unsaved_quest_is_refused_loudly);
  RUN_TEST(test_qedit_delete_saved_quest);
  RUN_TEST(test_qedit_save_frees_editor_strings);
  RUN_TEST(test_qedit_new_quest_menu_names_undefined_type);
  RUN_TEST(test_qedit_bad_stored_type_is_reported);
  RUN_TEST(test_prefedit_target_extracted_mid_edit);
  RUN_TEST(test_prefedit_self_still_resolves);
  RUN_TEST(test_prefedit_self_while_being_edited_sends_one_message);
  RUN_TEST(test_hedit_null_entry_round_trip);
  RUN_TEST(test_hedit_no_then_add_appends_instead_of_writing_past_table);
  RUN_TEST(test_hedit_no_offers_next_matching_row);
  RUN_TEST(test_hedit_save_with_stale_rnum_is_refused_loudly);
  RUN_TEST(test_hedit_replace_frees_old_row_and_keeps_duplicates_valid);
  RUN_TEST(test_hedit_save_to_duplicate_row_is_refused_loudly);
  RUN_TEST(test_hedit_bad_mode_is_reported);
  RUN_TEST(test_hedit_failed_disk_save_is_loud);
  RUN_TEST(test_qedit_failed_disk_save_is_loud);
  RUN_TEST(test_aedit_failed_disk_save_is_loud);
  RUN_TEST(test_hedit_new_topic_does_not_index_nowhere);
  RUN_TEST(test_hedit_duplicate_redirects_to_owner);
  RUN_TEST(test_hedit_orphan_duplicate_is_refused_loudly);
  return UNITY_END();
}
