/* Executable tests of the production text editor command (tedit.c): one
 * editor per file. Each editor edits a private copy and a save replaces the
 * whole file and its global, so a second editor's save would silently
 * discard the first one's work.
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
#include "tb_error_capture.h"

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
/* Only do_tedit()'s stale-session recovery calls it; no test opens one. */
void cleanup_olc(struct descriptor_data *d, byte cleanup_type)
{ (void)d; (void)cleanup_type; TEST_FAIL_MESSAGE("stale OLC session"); }
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

static struct char_data imp, other;
static struct player_special_data specials, other_specials;
static struct descriptor_data mine, theirs;

static void playing_immortal(struct char_data *ch, struct player_special_data *sp,
                             struct descriptor_data *d)
{
  memset(ch, 0, sizeof(*ch));
  memset(sp, 0, sizeof(*sp));
  memset(d, 0, sizeof(*d));
  ch->player_specials = sp;
  ch->player.level = LVL_IMPL;
  ch->desc = d;
  d->character = ch;
  STATE(d) = CON_PLAYING;
}

void setUp(void)
{
  playing_immortal(&imp, &specials, &mine);
  playing_immortal(&other, &other_specials, &theirs);
  theirs.next = &mine;
  descriptor_list = &theirs;
  writes = 0;
  said[0] = '\0';
  tb_error_capture_reset();
}
static void release(struct descriptor_data *d)
{
  if (d->olc) {
    free(d->olc->storage);
    free(d->olc);
    d->olc = NULL;
  }
}
void tearDown(void)
{
  descriptor_list = NULL;
  release(&mine);
  release(&theirs);
}

/* Someone already has news open (through the production command), so a
 * second editor of news is refused. */
static void a_file_being_edited_is_refused(void)
{
  char news_arg[] = "news", again[] = "news";
  do_tedit(&other, news_arg, 0, 0);
  TEST_ASSERT_EQUAL_INT(CON_TEDIT, STATE(&theirs));
  TEST_ASSERT_EQUAL_INT(1, writes);

  do_tedit(&imp, again, 0, 0);
  TEST_ASSERT_EQUAL_INT(1, writes);
  TEST_ASSERT_NULL(mine.olc);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&mine));
  TEST_ASSERT_NOT_NULL(strstr(said, "already editing"));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

static void another_file_can_be_edited(void)
{
  char news_arg[] = "news", motd_arg[] = "motd";
  do_tedit(&other, news_arg, 0, 0);
  do_tedit(&imp, motd_arg, 0, 0);
  TEST_ASSERT_EQUAL_INT(2, writes);
  TEST_ASSERT_EQUAL_INT(CON_TEDIT, STATE(&mine));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(a_file_being_edited_is_refused);
  RUN_TEST(another_file_can_be_edited);
  return UNITY_END();
}
