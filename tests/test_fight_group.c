/* Regression tests for perform_violence()'s group autoassist loop
 * (fight.c), linked against the production fight.c, handler.c and
 * lists.c. See gameplay_fixture.h for what is real and what is faked. */
#include "unity.h"
#include "gameplay_fixture.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "fight.h"
#include "lists.h"

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

void setUp(void)
{
  fx_setup(2);
  tb_error_capture_reset();
  fx_rand_value = 50;
  /* boot_db() makes these; the first create_list() is global_lists. */
  if (global_lists == NULL)
    global_lists = create_list();
  if (group_list == NULL)
    group_list = create_list();
}

void tearDown(void)
{
  fx_teardown();
}

/* P2 gate: the autoassist loop called merge_iterator() on the fighter's
 * group member list and never remove_iterator(), so each round leaked one
 * live iterator and free_group() later reported "freed with N live
 * iterators". After a round the member list must have none. */
static void test_round_with_grouped_fighter_releases_iterator(void)
{
  struct char_data *hero = fx_new_char("hero", FALSE, 0);
  struct char_data *squire = fx_new_char("squire", FALSE, 0);
  struct char_data *rat = fx_new_char("rat", TRUE, 0);
  struct group_data *grp;

  GET_MAX_HIT(rat) = GET_HIT(rat) = 30000;
  grp = create_group(hero);
  join_group(squire, grp);
  TEST_ASSERT_EQUAL_UINT(2, (unsigned) grp->members->iSize);

  set_fighting(hero, rat);
  perform_violence();
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, (unsigned) grp->members->iIterators,
                                 "round 1 left a live iterator");
  perform_violence();
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, (unsigned) grp->members->iIterators,
                                 "round 2 left a live iterator");

  if (FIGHTING(hero))
    stop_fighting(hero);
  if (FIGHTING(rat))
    stop_fighting(rat);
  leave_group(squire);
  leave_group(hero);   /* last member: frees the group and its list */
  TEST_ASSERT_EQUAL_UINT(0, (unsigned) group_list->iSize);
  TEST_ASSERT_NULL_MESSAGE(tb_captured_operation[0] ? tb_captured_operation : NULL,
                           "free_group reported an error");
  fx_free_char(rat);
  fx_free_char(squire);
  fx_free_char(hero);
}

int main(void)
{
  int failures;

  UNITY_BEGIN();
  RUN_TEST(test_round_with_grouped_fighter_releases_iterator);
  failures = UNITY_END();
  free_list(group_list);
  group_list = NULL;
  free_list(global_lists);
  global_lists = NULL;
  return failures;
}
