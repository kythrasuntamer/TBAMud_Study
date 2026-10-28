/* Executable tests of comm.c's close_socket() releasing a descriptor's OLC
 * workspace when the connection closes in the middle of an editor.
 *
 * comm.c is linked with its main() renamed and unreferenced sections
 * discarded (tests/CMakeLists.txt); what close_socket() reaches outside
 * comm.c is supplied here.  The descriptors have no character, so only the
 * socket, queue, protocol, event-list and OLC teardown run.
 */
#undef main   /* this target renames comm.c's main(); the test needs its own */

#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include "lists.h"
#include "oasis.h"
#include "prefedit.h"
#include "ibt.h"
#include "protocol.h"
#include <fcntl.h>
#include "tb_error_capture.h"

/* db.c globals and collaborators the act() code in comm.c reaches. */
struct room_data *world = NULL;
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL;
mob_rnum top_of_mobt = 0;
/* act()'s target checks and diagnostics on this tree name rooms, objects
 * and script ids; the descriptors here have no character, so none runs. */
room_rnum top_of_world = 0;
struct index_data *obj_index = NULL;
obj_rnum top_of_objt = 0;
long char_script_id(char_data *ch) { (void)ch; return 0; }
struct char_data *find_char(long n) { (void)n; return NULL; }
/* CHECK_PLAYER_SPECIAL's reporter (utils.c). */
void tb_player_special_on_mob_at(const struct char_data *ch, const char *field,
                                 const char *source_file, int source_line,
                                 const char *source_func)
{ (void)ch; (void)field; (void)source_file; (void)source_line; (void)source_func; }
void basic_mud_log(const char *format, ...) { (void)format; }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
int MAX(int a, int b) { return a > b ? a : b; }
/* room_is_dark(), simple_list(), free_list() and event_cancel() are call-site
 * macros over *_at() functions on this tree (file/line/func capture). */
int room_is_dark_at(room_rnum room, const char *source_file, int source_line,
                    const char *source_func)
{ (void)room; (void)source_file; (void)source_line; (void)source_func; return 0; }
char *fname(const char *namelist) { return (char *)namelist; }
char *CAP(char *txt) { return txt; }
void act_mtrigger(const char_data *ch, char *str, char_data *actor,
                  char_data *victim, obj_data *object, obj_data *target,
                  char *arg)
{ (void)ch; (void)str; (void)actor; (void)victim; (void)object; (void)target; (void)arg; }
const char *ProtocolOutput(descriptor_t *d, const char *data, int *length)
{ (void)d; (void)length; return data; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *allocation;

  (void)file; (void)line; (void)func;
  allocation = calloc(count, element_size);
  if (!allocation)
    abort();
  return allocation;
}

/* Character teardown: the descriptors here have no character. */
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
void add_llog_entry(struct char_data *ch, int type) { (void)ch; (void)type; UNREACHED(); }
void save_char(struct char_data *ch) { (void)ch; UNREACHED(); }
void free_char(struct char_data *ch) { (void)ch; UNREACHED(); }

/* Protocol and event-list teardown for a descriptor with neither. */
void ProtocolDestroy(protocol_t *apProtocol) { (void)apProtocol; }
void *simple_list_at(struct list_data *pList, const char *file, int line,
                     const char *func)
{ (void)pList; (void)file; (void)line; (void)func; UNREACHED(); return NULL; }
void event_cancel_at(struct event *event, const char *file, int line,
                     const char *func)
{ (void)event; (void)file; (void)line; (void)func; UNREACHED(); }
void free_list_at(struct list_data *pList, const char *file, int line,
                  const char *func)
{ (void)file; (void)line; (void)func; free(pList); }

/* What oasis.c's cleanup_olc() frees for the workspaces built here, with
 * the cedit config copy released for every cleanup type (audit F3-07): the
 * config copy, prefedit's preferences, ibtedit's copy of the entry, iedit's
 * copy of the object (never the live object, OLC_IEDIT_OBJ), each with its
 * strings, then the workspace.  Calls are counted so a missed state shows. */
static int cleanups;
static int cleanup_all_seen;     /* the last call passed CLEANUP_ALL */
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{
  if (d->olc == NULL)
    return;
  cleanups++;
  cleanup_all_seen = (cleanup_type == CLEANUP_ALL);
  if (OLC_CONFIG(d)) {
    free(OLC_CONFIG(d)->play.OK);
    free(OLC_CONFIG(d)->operation.MENU);
    free(OLC_CONFIG(d));
  }
  free(OLC_PREFS(d));
  if (OLC_IBT(d)) {
    free(OLC_IBT(d)->text);
    free(OLC_IBT(d)->body);
    free(OLC_IBT(d)->name);
    free(OLC_IBT(d)->notes);
    free(OLC_IBT(d));
  }
  if (OLC_OBJ(d)) {
    free(OLC_OBJ(d)->name);
    free(OLC_OBJ(d)->short_description);
    free(OLC_OBJ(d));
  }
  free(d->olc);
  d->olc = NULL;
}

/* A descriptor as close_socket() gets it when the link drops: on the
 * descriptor list, with an open socket, an empty event list, and in editor
 * state 'state' with an empty workspace the test fills in.  close_socket()
 * frees the descriptor. */
static struct descriptor_data *add_editing_descriptor(int state)
{
  struct descriptor_data *d;

  CREATE(d, struct descriptor_data, 1);
  d->descriptor = open("/dev/null", O_RDONLY);
  TEST_ASSERT_TRUE(d->descriptor >= 0);
  CREATE(d->events, struct list_data, 1);
  CREATE(d->olc, struct oasis_olc_data, 1);
  STATE(d) = state;
  d->next = descriptor_list;
  descriptor_list = d;
  return d;
}

/* The config copy do_oasis_cedit() puts in the workspace. */
static void give_config_copy(struct descriptor_data *d)
{
  CREATE(OLC_CONFIG(d), struct config_data, 1);
  OLC_CONFIG(d)->play.OK = strdup("Okay.\r\n");
  OLC_CONFIG(d)->operation.MENU = strdup("Menu\r\n");
  TEST_ASSERT_NOT_NULL(OLC_CONFIG(d)->play.OK);
  TEST_ASSERT_NOT_NULL(OLC_CONFIG(d)->operation.MENU);
}

void setUp(void)
{
  cleanups = 0;
  cleanup_all_seen = FALSE;
  descriptor_list = NULL;
  tb_error_capture_reset();
}
void tearDown(void) {}

/* Finding 4 (cedit group): CON_CEDIT was missing from close_socket()'s list
 * of editor states, so a builder who lost link in cedit leaked the
 * workspace and its config copy.  On the integrated tree close_socket()
 * releases any attached workspace whatever the state (F3-11), which this
 * pins for cedit. */
static void test_link_loss_in_cedit_releases_the_workspace(void)
{
  give_config_copy(add_editing_descriptor(CON_CEDIT));
  close_socket(descriptor_list);
  TEST_ASSERT_EQUAL_INT(1, cleanups);
  TEST_ASSERT_TRUE(cleanup_all_seen);
  TEST_ASSERT_NULL(descriptor_list);
}

/* A state that was already listed keeps working. */
static void test_link_loss_in_redit_still_releases_the_workspace(void)
{
  give_config_copy(add_editing_descriptor(CON_REDIT));
  close_socket(descriptor_list);
  TEST_ASSERT_EQUAL_INT(1, cleanups);
  TEST_ASSERT_TRUE(cleanup_all_seen);
}

/* CON_PREFEDIT, CON_IBTEDIT and CON_IEDIT were missing too.  Each of
 * those editors leaves through cleanup_olc(d, CLEANUP_ALL) on a normal
 * exit, so a dropped link must release its workspace the same way. */
static void test_link_loss_in_prefedit_releases_the_workspace(void)
{
  struct descriptor_data *d = add_editing_descriptor(CON_PREFEDIT);

  CREATE(OLC_PREFS(d), struct prefs_data, 1);
  close_socket(d);
  TEST_ASSERT_EQUAL_INT(1, cleanups);
  TEST_ASSERT_TRUE(cleanup_all_seen);
}

static void test_link_loss_in_ibtedit_releases_the_workspace(void)
{
  struct descriptor_data *d = add_editing_descriptor(CON_IBTEDIT);

  CREATE(OLC_IBT(d), IBT_DATA, 1);
  OLC_IBT(d)->text = strdup("Typo in the temple");
  OLC_IBT(d)->name = strdup("Builder");
  TEST_ASSERT_NOT_NULL(OLC_IBT(d)->text);
  TEST_ASSERT_NOT_NULL(OLC_IBT(d)->name);
  close_socket(d);
  TEST_ASSERT_EQUAL_INT(1, cleanups);
  TEST_ASSERT_TRUE(cleanup_all_seen);
}

/* iedit edits a copy of a live object: the copy goes, the object stays. */
static void test_link_loss_in_iedit_releases_the_copy_not_the_object(void)
{
  char live_name[] = "sword";
  struct obj_data live;
  struct descriptor_data *d = add_editing_descriptor(CON_IEDIT);

  memset(&live, 0, sizeof(live));
  live.name = live_name;
  OLC_IEDIT_OBJ(d) = &live;
  CREATE(OLC_OBJ(d), struct obj_data, 1);
  OLC_OBJ(d)->name = strdup("sword");
  OLC_OBJ(d)->short_description = strdup("a sword");
  TEST_ASSERT_NOT_NULL(OLC_OBJ(d)->name);
  TEST_ASSERT_NOT_NULL(OLC_OBJ(d)->short_description);
  close_socket(d);
  TEST_ASSERT_EQUAL_INT(1, cleanups);
  TEST_ASSERT_TRUE(cleanup_all_seen);
  TEST_ASSERT_EQUAL_STRING("sword", live.name);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_link_loss_in_cedit_releases_the_workspace);
  RUN_TEST(test_link_loss_in_redit_still_releases_the_workspace);
  RUN_TEST(test_link_loss_in_prefedit_releases_the_workspace);
  RUN_TEST(test_link_loss_in_ibtedit_releases_the_workspace);
  RUN_TEST(test_link_loss_in_iedit_releases_the_copy_not_the_object);
  return UNITY_END();
}
