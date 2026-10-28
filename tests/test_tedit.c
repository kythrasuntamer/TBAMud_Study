/* Executable tests of the production text editor command (tedit.c): one
 * editor per file, because every editor of a file writes through the same
 * global buffer.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "oasis.h"
#include "modify.h"
#include "improved-edit.h"

/* Text globals from db.c not already in test_stubs.c; this test only names
 * them. */
char *credits, *news, *help, *ihelp, *info, *handbook, *policies, *wizlist,
     *immlist;

void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}

static int writes;
void string_write(struct descriptor_data *d, char **txt, size_t len, long mailto, void *data)
{ (void)d; (void)txt; (void)len; (void)mailto; free(data); writes++; }
void clear_screen(struct descriptor_data *d) { (void)d; }
void send_editor_help(struct descriptor_data *d) { (void)d; }
static char said[256];
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
char *one_argument(char *argument, char *first_arg)
{
  strlcpy(first_arg, argument, MAX_INPUT_LENGTH);
  return argument + strlen(argument);
}

static struct char_data imp;
static struct player_special_data specials;
static struct descriptor_data mine, theirs;
static struct oasis_olc_data their_olc;

void setUp(void)
{
  memset(&imp, 0, sizeof(imp));
  memset(&specials, 0, sizeof(specials));
  memset(&mine, 0, sizeof(mine));
  memset(&theirs, 0, sizeof(theirs));
  memset(&their_olc, 0, sizeof(their_olc));
  imp.player_specials = &specials;
  imp.player.level = LVL_IMPL;
  imp.desc = &mine;
  mine.character = &imp;
  STATE(&mine) = CON_PLAYING;
  theirs.olc = &their_olc;
  their_olc.storage = NEWS_FILE;            /* someone has news open */
  STATE(&theirs) = CON_TEDIT;
  descriptor_list = &theirs;
  writes = 0;
}
void tearDown(void)
{
  descriptor_list = NULL;
  if (mine.olc) {
    free(mine.olc->storage);
    free(mine.olc);
  }
}

static void a_file_being_edited_is_refused(void)
{
  char which[] = "news";
  do_tedit(&imp, which, 0, 0);
  TEST_ASSERT_EQUAL_INT(0, writes);
  TEST_ASSERT_NULL(mine.olc);
  TEST_ASSERT_NOT_NULL(strstr(said, "already editing"));
}

static void another_file_can_be_edited(void)
{
  char which[] = "motd";
  do_tedit(&imp, which, 0, 0);
  TEST_ASSERT_EQUAL_INT(1, writes);
  TEST_ASSERT_EQUAL_INT(CON_TEDIT, STATE(&mine));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(a_file_being_edited_is_refused);
  RUN_TEST(another_file_can_be_edited);
  return UNITY_END();
}
