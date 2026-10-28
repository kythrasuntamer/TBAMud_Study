/* Executable test of cleanup_olc() (oasis.c) releasing an aedit workspace.
 * oasis.c and act.social.c (for the real free_action) are linked with
 * unreferenced sections discarded (see tests/CMakeLists.txt). cleanup_olc
 * can reach every editor's release function; an aedit workspace reaches
 * none of them, so they fail the test if called. Run under LeakSanitizer
 * or Valgrind: the point is that nothing of the workspace is left behind.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "oasis.h"
#include "genmob.h"
#include "genobj.h"
#include "shop.h"
#include "genshp.h"
#include "quest.h"
#include "ibt.h"
#include "msgedit.h"
#include "dg_scripts.h"
#include "tb_error_capture.h"

#define UNREACHED() TEST_FAIL_MESSAGE(__func__)

/* db.c / utils.c globals and helpers (not linked). */
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
struct char_data *mob_proto = NULL;
struct obj_data *obj_proto = NULL;
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void basic_mud_log(const char *format, ...) { (void)format; }
int MAX(int a, int b) { return a > b ? a : b; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  void *p;

  (void)source_file; (void)source_line; (void)source_func;
  p = calloc(count, element_size);
  if (!p)
    abort();
  return p;
}
obj_rnum real_object(obj_vnum vnum) { (void)vnum; UNREACHED(); return NOTHING; }
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; UNREACHED(); return NOBODY; }

/* The other editors' release functions. */
void free_help(struct help_index_element *help) { (void)help; UNREACHED(); }
void free_message_list(struct message_list *mlist) { (void)mlist; UNREACHED(); }
int free_mobile(struct char_data *mob) { (void)mob; UNREACHED(); return 0; }
void free_object_strings(struct obj_data *obj) { (void)obj; UNREACHED(); }
void free_olc_ibt(IBT_DATA *toFree) { (void)toFree; UNREACHED(); }
void free_proto_script(void *thing, int type) { (void)thing; (void)type; UNREACHED(); }
void free_quest(struct aq_data *quest) { (void)quest; UNREACHED(); }
void free_room(struct room_data *room) { (void)room; UNREACHED(); }
void free_shop(struct shop_data *shop) { (void)shop; UNREACHED(); }
void free_trigger(trig_data *trig) { (void)trig; UNREACHED(); }
int free_strings(void *data, int type) { (void)data; (void)type; UNREACHED(); return 0; }

void setUp(void) { tb_error_capture_reset(); }
void tearDown(void) {}

/* An aedit workspace as aedit_setup_new() builds it. */
static struct social_messg *new_action_workspace(struct descriptor_data *d)
{
  memset(d, 0, sizeof(*d));
  STATE(d) = CON_PLAYING;            /* no character: no stop message */
  CREATE(d->olc, struct oasis_olc_data, 1);
  CREATE(OLC_ACTION(d), struct social_messg, 1);
  OLC_ACTION(d)->command = strdup("smile");
  OLC_ACTION(d)->sort_as = strdup("smile");
  OLC_ACTION(d)->char_no_arg = strdup("This action is unfinished.");
  TEST_ASSERT_NOT_NULL(OLC_ACTION(d)->command);
  TEST_ASSERT_NOT_NULL(OLC_ACTION(d)->sort_as);
  TEST_ASSERT_NOT_NULL(OLC_ACTION(d)->char_no_arg);
  return OLC_ACTION(d);
}

/* free_action() frees a social's strings but not the struct (it is also
 * used on soc_mess_list[] entries), so CLEANUP_ALL leaked the workspace's
 * struct social_messg on every aedit quit or link loss (LeakSanitizer
 * reports it on the parent commit). */
static void aedit_workspace_is_released_whole(void)
{
  struct descriptor_data d;

  (void)new_action_workspace(&d);
  cleanup_olc(&d, CLEANUP_ALL);
  TEST_ASSERT_NULL(d.olc);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* After a save the strings belong to soc_mess_list: CLEANUP_STRUCTS frees
 * the struct and leaves them (here the test still holds and frees them). */
static void aedit_saved_workspace_keeps_its_strings(void)
{
  struct descriptor_data d;
  struct social_messg *act = new_action_workspace(&d);
  char *command = act->command, *sort_as = act->sort_as;
  char *char_no_arg = act->char_no_arg;

  cleanup_olc(&d, CLEANUP_STRUCTS);
  TEST_ASSERT_NULL(d.olc);
  TEST_ASSERT_EQUAL_STRING("smile", command);   /* still valid */
  free(command);
  free(sort_as);
  free(char_no_arg);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(aedit_workspace_is_released_whole);
  RUN_TEST(aedit_saved_workspace_keeps_its_strings);
  return UNITY_END();
}
