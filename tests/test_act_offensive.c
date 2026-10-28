/* Regression tests for act.offensive.c's do_order(), linked against the
 * production gameplay sources (see gameplay_fixture.h). command_interpreter()
 * is replaced through the linker (--wrap) by a stand-in for the ordered
 * command, so a test can play a command trigger that changes the followers.
 */
#include "unity.h"
#include "gameplay_fixture.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "act.h"
#include "dg_scripts.h"

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* The ordered command. Like a command trigger running 'mfollow self' (or
 * one releasing another follower), it calls stop_follower(), which frees
 * that follower's follow_type node at once. */
static int commands_run;
static struct char_data *release_instead; /* NULL: the follower itself */
void __wrap_command_interpreter(struct char_data *ch, char *argument); /* NOLINT(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name */
void __wrap_command_interpreter(struct char_data *ch, char *argument) /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
{
  struct char_data *leaving = release_instead ? release_instead : ch;

  (void)argument;
  commands_run++;
  if (leaving->master)
    stop_follower(leaving);
  release_instead = NULL;
}

static char ok_text[] = "Ok.\r\n";

void setUp(void)
{
  fx_setup(1);
  tb_error_capture_reset();
  commands_run = 0;
  release_instead = NULL;
  config_info.play.OK = ok_text;
}

void tearDown(void)
{
  config_info.play.OK = NULL;
  fx_teardown();
}

static struct char_data *charmed_follower(const char *name,
                                          struct char_data *leader)
{
  struct char_data *mob = fx_new_char(name, TRUE, 0);

  add_follower(mob, leader);
  SET_BIT_AR(AFF_FLAGS(mob), AFF_CHARM);
  return mob;
}

static void order(struct char_data *ch, const char *text)
{
  char buf[MAX_INPUT_LENGTH];

  strlcpy(buf, text, sizeof(buf));
  do_order(ch, buf, 0, 0);
}

/* G1: 'order followers' read k->next after the ordered command, which had
 * just freed k when the follower stopped following (ASan
 * heap-use-after-free). Every follower is still ordered exactly once. */
static void test_order_survives_follower_leaving(void)
{
  struct char_data *leader = fx_new_char("hero", FALSE, 0);
  struct char_data *first = charmed_follower("dog", leader);
  struct char_data *second = charmed_follower("cat", leader);

  order(leader, "followers sit");

  TEST_ASSERT_EQUAL_INT(2, commands_run);
  TEST_ASSERT_NULL(first->master);
  TEST_ASSERT_NULL(second->master);
  TEST_ASSERT_NULL(leader->followers);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  fx_free_char(second);
  fx_free_char(first);
  fx_free_char(leader);
}

/* A command that releases the follower still to be ordered: that one is
 * skipped and reported, not ordered through a freed node. */
static void test_order_skips_follower_released_by_trigger(void)
{
  struct char_data *leader = fx_new_char("hero", FALSE, 0);
  struct char_data *first = charmed_follower("dog", leader);
  struct char_data *second = charmed_follower("cat", leader);

  release_instead = first;             /* second's command releases first */
  order(leader, "followers sit");

  TEST_ASSERT_EQUAL_INT(1, commands_run);
  TEST_ASSERT_NULL(first->master);
  TEST_ASSERT_EQUAL_PTR(leader, second->master);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("follower-changed", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "was released by a trigger"));
  stop_follower(second);
  fx_free_char(second);
  fx_free_char(first);
  fx_free_char(leader);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_order_survives_follower_leaving);
  RUN_TEST(test_order_skips_follower_released_by_trigger);
  return UNITY_END();
}
