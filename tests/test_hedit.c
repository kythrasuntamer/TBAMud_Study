/* Executable tests of the production help editor (hedit.c): declining an
 * entry offers the next match, and adding a new entry never indexes past
 * the help table.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "oasis.h"
#include "hedit.h"

#include "genolc.h"
#include "modify.h"

/* Globals from db.c and oasis.c, and editor plumbing the menus reach.
 * These tests stay in the confirm prompts, so the plumbing never runs. */
int top_of_helpt;
struct help_index_element *help_table;
const char *nrm = "", *grn = "", *cyn = "", *yel = "";
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
int add_to_save_list(zone_vnum zone, int type) { (void)zone; (void)type; UNREACHED(); return 0; }
int remove_from_save_list(zone_vnum zone, int type) { (void)zone; (void)type; UNREACHED(); return 0; }
void cleanup_olc(struct descriptor_data *d, byte type) { (void)d; (void)type; UNREACHED(); }
void clear_screen(struct descriptor_data *d) { (void)d; UNREACHED(); }
void get_char_colors(struct char_data *ch) { (void)ch; UNREACHED(); }
void free_help_table(void) { UNREACHED(); }
void index_boot(int mode) { (void)mode; UNREACHED(); }
char *str_udup(const char *txt) { (void)txt; UNREACHED(); return NULL; }
void strip_cr(char *buffer) { (void)buffer; UNREACHED(); }
void string_write(struct descriptor_data *d, char **txt, size_t len, long mailto, void *data)
{ (void)d; (void)txt; (void)len; (void)mailto; (void)data; UNREACHED(); }

/* interpreter.c is not linked; test_stubs.c's weak is_abbrev matches nothing. */
int is_abbrev(const char *arg1, const char *arg2)
{
  return *arg1 && !strncmp(arg1, arg2, strlen(arg1));
}

static struct help_index_element table[3];
static struct descriptor_data desc;
static struct oasis_olc_data olc;
static char said[MAX_STRING_LENGTH];

size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  int n;
  (void)d;
  va_start(args, txt);
  n = vsnprintf(said, sizeof(said), txt, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}

void setUp(void)
{
  memset(&desc, 0, sizeof(desc));
  memset(&olc, 0, sizeof(olc));
  table[0].keywords = "apple";
  table[1].keywords = "banana";
  table[2].keywords = "apricot";
  help_table = table;
  top_of_helpt = 3;
  desc.olc = &olc;
  OLC_STORAGE(&desc) = "ap";
  OLC_ZNUM(&desc) = 0;                   /* editing "apple" was declined */
  OLC_MODE(&desc) = HEDIT_CONFIRM_EDIT;
}
void tearDown(void) { help_table = NULL; top_of_helpt = 0; }

static void decline(void)
{
  char no[] = "n";
  hedit_parse(&desc, no);
}

/* The old loop stopped at "banana" and left OLC_ZNUM at top + 2. */
static void declining_offers_the_next_match(void)
{
  decline();
  TEST_ASSERT_EQUAL_INT(2, OLC_ZNUM(&desc));
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_EDIT, OLC_MODE(&desc));
  TEST_ASSERT_NOT_NULL(strstr(said, "apricot"));
}

/* With no match left, adding appends: the index is NOWHERE, not past the
 * end of help_table. */
static void declining_the_last_match_offers_an_add(void)
{
  OLC_ZNUM(&desc) = 2;
  decline();
  TEST_ASSERT_EQUAL_INT(NOWHERE, OLC_ZNUM(&desc));
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_ADD, OLC_MODE(&desc));
}

/* do_oasis_hedit() fixtures: a builder with a descriptor in the game.
 * Single-word arguments only. */
char *one_argument(char *argument, char *first_arg)
{
  strlcpy(first_arg, argument, MAX_INPUT_LENGTH);
  return argument + strlen(argument);
}
static int search_result;
int search_help(const char *argument, int level) { (void)argument; (void)level; return search_result; }
int can_edit_zone(struct char_data *ch, zone_rnum rnum) { (void)ch; (void)rnum; return 1; }
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  int n;
  (void)ch;
  va_start(args, messg);
  n = vsnprintf(said, sizeof(said), messg, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}

/* A missing topic used to read help_table[NOWHERE].duplicate first. */
static void editing_a_missing_topic_offers_an_add(void)
{
  static struct char_data builder;
  static struct player_special_data specials;
  char topic[] = "zzzz";

  memset(&builder, 0, sizeof(builder));
  memset(&specials, 0, sizeof(specials));
  builder.player_specials = &specials;
  builder.player.name = "Builder";
  builder.desc = &desc;
  desc.character = &builder;
  desc.olc = NULL;
  STATE(&desc) = CON_PLAYING;
  search_result = NOWHERE;

  do_oasis_hedit(&builder, topic, 0, 0);

  TEST_ASSERT_NOT_NULL(desc.olc);
  TEST_ASSERT_EQUAL_INT(NOWHERE, OLC_ZNUM(&desc));
  TEST_ASSERT_EQUAL_INT(HEDIT_CONFIRM_ADD, OLC_MODE(&desc));
  free(OLC_STORAGE(&desc));
  free(desc.olc);
  desc.olc = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(declining_offers_the_next_match);
  RUN_TEST(declining_the_last_match_offers_an_add);
  RUN_TEST(editing_a_missing_topic_offers_an_add);
  return UNITY_END();
}
