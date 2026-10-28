/* Executable test of the production social editor (aedit.c): a minimum
 * position outside POS_DEAD..POS_STANDING is refused.  The old check used
 * && and accepted anything, which the menu then used to index
 * position_types[].
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "oasis.h"

#include "genolc.h"
#include "interpreter.h"
#include "act.h"

/* Globals and plumbing from other modules; these prompts never reach them. */
const char *nrm = "", *grn = "", *cyn = "", *yel = "";
struct command_info *complete_cmd_info;
struct social_messg *soc_mess_list;
int top_of_socialt;
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; UNREACHED(); return 0; }
int remove_from_save_list(zone_vnum zone, int type) { (void)zone; (void)type; UNREACHED(); return 0; }
void cleanup_olc(struct descriptor_data *d, byte type) { (void)d; (void)type; UNREACHED(); }
void create_command_list(void) { UNREACHED(); }
void sort_commands(void) { UNREACHED(); }
char *delete_doubledollar(char *string) { UNREACHED(); return string; }
void free_action(struct social_messg *mess) { (void)mess; UNREACHED(); }
void get_char_colors(struct char_data *ch) { (void)ch; }

static struct descriptor_data desc;
static struct oasis_olc_data olc;
static struct social_messg action;
static struct char_data builder;
static struct player_special_data specials;

void setUp(void)
{
  memset(&desc, 0, sizeof(desc));
  memset(&olc, 0, sizeof(olc));
  memset(&action, 0, sizeof(action));
  memset(&builder, 0, sizeof(builder));
  memset(&specials, 0, sizeof(specials));
  builder.player_specials = &specials;
  desc.character = &builder;
  desc.olc = &olc;
  OLC_ACTION(&desc) = &action;
  action.command = "smile";
  action.sort_as = "smile";
  action.min_char_position = POS_RESTING;
  action.min_victim_position = POS_RESTING;
}
void tearDown(void) {}

static void out_of_range_positions_are_refused(void)
{
  char bad[] = "99", low[] = "-1", ok[] = "8";

  OLC_MODE(&desc) = AEDIT_MIN_CHAR_POS;
  aedit_parse(&desc, bad);
  TEST_ASSERT_EQUAL_INT(POS_RESTING, action.min_char_position);

  OLC_MODE(&desc) = AEDIT_MIN_VICT_POS;
  aedit_parse(&desc, low);
  TEST_ASSERT_EQUAL_INT(POS_RESTING, action.min_victim_position);

  OLC_MODE(&desc) = AEDIT_MIN_CHAR_POS;
  aedit_parse(&desc, ok);
  TEST_ASSERT_EQUAL_INT(POS_STANDING, action.min_char_position);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(out_of_range_positions_are_refused);
  return UNITY_END();
}
