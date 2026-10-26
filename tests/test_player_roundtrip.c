/* Isolated native save/load regression. Never uses the live player directory. */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "spells.h"
#include <sys/stat.h>
#include <unistd.h>

static char original_cwd[4096];
static char sandbox[128];
static struct char_data before, after;
static struct player_special_data before_specials, after_specials;

void setUp(void)
{
  TEST_ASSERT_NOT_NULL(getcwd(original_cwd, sizeof(original_cwd)));
  TEST_ASSERT_TRUE(snprintf(sandbox, sizeof(sandbox),
      "/tmp/tbamud-player-roundtrip-XXXXXX") > 0);
  TEST_ASSERT_NOT_NULL(mkdtemp(sandbox));
  TEST_ASSERT_EQUAL_INT(0, chdir(sandbox));
  TEST_ASSERT_EQUAL_INT(0, mkdir("plrfiles", 0700));
  TEST_ASSERT_EQUAL_INT(0, mkdir("plrfiles/P-T", 0700));
  memset(&before, 0, sizeof(before));
  memset(&after, 0, sizeof(after));
  memset(&before_specials, 0, sizeof(before_specials));
  memset(&after_specials, 0, sizeof(after_specials));
  before.player_specials = &before_specials;
  after.player_specials = &after_specials;
  before.player.name = "Roundtrip";
  top_of_p_table = -1;
  TEST_ASSERT_EQUAL_INT(0, create_entry("Roundtrip"));
  TEST_ASSERT_NOT_NULL(player_table);
  before.pfilepos = 0;
  before.player.time.logon = 1;
  before.char_specials.saved.idnum = 19301;
}
void tearDown(void)
{
  char path[256];
  TEST_ASSERT_EQUAL_INT(0, chdir(original_cwd));
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/plrfiles/P-T/roundtrip.plr",
      sandbox) > 0);
  TEST_ASSERT_EQUAL_INT(0, unlink(path));
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/plrfiles/index",
      sandbox) > 0);
  (void)unlink(path);
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/plrfiles/P-T",
      sandbox) > 0);
  TEST_ASSERT_EQUAL_INT(0, rmdir(path));
  TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/plrfiles",
      sandbox) > 0);
  TEST_ASSERT_EQUAL_INT(0, rmdir(path));
  TEST_ASSERT_EQUAL_INT(0, rmdir(sandbox));
  free_player_index();
}
void test_character_class_and_endgame_roundtrip(void)
{
  int klass;
  /* Save and reload a separate synthetic account for every class. */
  for (klass = 0; klass < NUM_CLASSES; ++klass) {
    GET_CLASS(&before) = klass;
    GET_LEVEL(&before) = 50;
    GET_ENDGAME_AP(&before) = 7;
    GET_ENDGAME_XP(&before) = 321;
    GET_ENDGAME_HP_RANK(&before) = 3;
    GET_ENDGAME_MP_RANK(&before) = 4;
    save_char(&before);
    TEST_ASSERT_EQUAL_INT(0, load_char("Roundtrip", &after));
    TEST_ASSERT_EQUAL_INT(klass, GET_CLASS(&after));
    TEST_ASSERT_EQUAL_INT(50, GET_LEVEL(&after));
    TEST_ASSERT_EQUAL_INT(7, GET_ENDGAME_AP(&after));
    TEST_ASSERT_EQUAL_INT(321, GET_ENDGAME_XP(&after));
    TEST_ASSERT_EQUAL_INT(3, GET_ENDGAME_HP_RANK(&after));
    TEST_ASSERT_EQUAL_INT(4, GET_ENDGAME_MP_RANK(&after));
  }
}
int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_character_class_and_endgame_roundtrip);
  return UNITY_END();
}
