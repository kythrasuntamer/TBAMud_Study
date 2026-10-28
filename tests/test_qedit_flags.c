/* F2-28: qedit's flag menu toggled the builder's menu choice as if it were
 * the flag mask: TOGGLE_BIT(flags, number). That is right only for choice
 * 1 (AQ_REPEATABLE is 1 << 0), so the bug is latent while NUM_AQ_FLAGS is
 * 1; with a third flag, choice 3 would toggle bits 0 and 1 together.
 *
 * To make the mapping observable, quest.h is included first and
 * NUM_AQ_FLAGS raised to 3 (with a matching aq_flags[] below) before the
 * production qedit.c is #included; its include guard keeps the raised
 * value. qedit_parse() is then driven in QEDIT_FLAGS mode. Everything else
 * qedit.c references is supplied here or by test_stubs.c; unreferenced code
 * is discarded at link time.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "quest.h"

#undef NUM_AQ_FLAGS
#define NUM_AQ_FLAGS 3

#include "../src/qedit.c" /* NOLINT(bugprone-suspicious-include): white-box test of qedit_parse with more flags */

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from qedit.c to this capturing version. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* Three flags, so choices 2 and 3 exist. */
const char *aq_flags[] = { "REPEATABLE", "SECOND", "THIRD", "\n" };
const char *quest_types[] = { "Object", "\n" };
struct aq_data *aquest_table = NULL;
qst_rnum total_quests = 0;
struct zone_data *zone_table = NULL;
zone_rnum top_of_zone_table = 0;
struct char_data *mob_proto = NULL;
struct obj_data *obj_proto = NULL;

/* The flag menu's display helpers (column_list() is utils.c's). */
void get_char_colors(struct char_data *ch) { (void)ch; }
void clear_screen(struct descriptor_data *d) { (void)d; }

/* Reached only from other qedit_parse() modes, never in these tests. */
int add_quest(struct aq_data *nqst) { (void)nqst; return 0; }
int delete_quest(qst_rnum rnum) { (void)rnum; return 0; }
int copy_quest(struct aq_data *from, struct aq_data *to, int free_old_strings)
{ (void)from; (void)to; (void)free_old_strings; return 0; }
int save_quests(zone_rnum zone_num) { (void)zone_num; return 0; }
qst_rnum real_quest(qst_vnum vnum) { (void)vnum; return NOTHING; }
mob_rnum real_mobile(mob_vnum vnum) { (void)vnum; return NOBODY; }
obj_rnum real_object(obj_vnum vnum) { (void)vnum; return NOTHING; }
zone_rnum real_zone(zone_vnum vnum) { (void)vnum; return NOWHERE; }
zone_rnum real_zone_by_thing(room_vnum vznum) { (void)vznum; return NOWHERE; }
int can_edit_zone(struct char_data *ch, zone_rnum rnum)
{ (void)ch; (void)rnum; return 0; }
room_vnum genolc_zone_bottom(zone_rnum rznum) { (void)rznum; return NOWHERE; }
int genolc_checkstring(struct descriptor_data *d, char *arg)
{ (void)d; (void)arg; return 1; }
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{ (void)d; (void)cleanup_type; }
void string_write(struct descriptor_data *d, char **writeto, size_t len,
                  long mailto, void *data)
{ (void)d; (void)writeto; (void)len; (void)mailto; (void)data; }
char *str_udup(const char *txt) { return strdup((txt && *txt) ? txt : "undefined"); }

static struct descriptor_data desc;
static struct oasis_olc_data olc;
static struct aq_data quest;
static struct char_data builder;

void setUp(void)
{
  memset(&desc, 0, sizeof(desc));
  memset(&olc, 0, sizeof(olc));
  memset(&quest, 0, sizeof(quest));
  memset(&builder, 0, sizeof(builder));
  builder.player_specials = &dummy_mob;
  desc.character = &builder;
  desc.olc = &olc;
  OLC_QUEST(&desc) = &quest;
  OLC_MODE(&desc) = QEDIT_FLAGS;
  tb_error_capture_reset();
}
void tearDown(void) {}

static void choose(const char *choice)
{
  char buf[16];

  strlcpy(buf, choice, sizeof(buf));
  qedit_parse(&desc, buf);
}

/* Each menu choice n toggles exactly bit n-1, and toggling again clears
 * it. The old code made choice 3 toggle mask 3 (bits 0 and 1). */
static void each_flag_choice_toggles_its_own_bit(void)
{
  choose("3");
  TEST_ASSERT_EQUAL_INT(1L << 2, quest.flags);
  choose("2");
  TEST_ASSERT_EQUAL_INT((1L << 2) | (1L << 1), quest.flags);
  choose("1");
  TEST_ASSERT_EQUAL_INT((1L << 2) | (1L << 1) | AQ_REPEATABLE, quest.flags);
  choose("3");
  TEST_ASSERT_EQUAL_INT((1L << 1) | AQ_REPEATABLE, quest.flags);
  TEST_ASSERT_EQUAL_INT(QEDIT_FLAGS, OLC_MODE(&desc));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* A choice past the last flag changes nothing. */
static void out_of_range_choice_changes_nothing(void)
{
  quest.flags = AQ_REPEATABLE;
  choose("4");
  TEST_ASSERT_EQUAL_INT(AQ_REPEATABLE, quest.flags);
  choose("-1");
  TEST_ASSERT_EQUAL_INT(AQ_REPEATABLE, quest.flags);
  TEST_ASSERT_EQUAL_INT(QEDIT_FLAGS, OLC_MODE(&desc));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(each_flag_choice_toggles_its_own_bit);
  RUN_TEST(out_of_range_choice_changes_nothing);
  return UNITY_END();
}
