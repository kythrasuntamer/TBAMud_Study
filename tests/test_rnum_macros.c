/* Regression tests for the VALID_*_RNUM checks: utils.h's VALID_ROOM_RNUM,
 * VALID_MOB_RNUM, VALID_ZONE_RNUM and VALID_OBJ_RNUM, dg_scripts.h's
 * VALID_TRIG_RNUM, olc_checks.h's VALID_OBJ_RNUM_NUM and oasis.h's
 * VALID_OBJ_RNUM_IDX, which all follow tb_rnum_valid(). genzon.h's and
 * oasis.h's VALID_ZONE_RNUM copies are token-identical to utils.h's (a
 * difference would draw a "redefined" warning here).
 *
 * With the default unsigned index type the sentinels (NOWHERE, NOTHING,
 * NOBODY) are 65535, so a plain int or long holding -1 used to pass both
 * the sentinel test and the "<= top" test: a zone-command argument zedit
 * had not set yet reached mob_proto[-1], and VALID_TRIG_RNUM read
 * trig_index[-1] (UBSan). The checks now refuse every negative value of
 * every integer type, and evaluate the argument once (VALID_TRIG_RNUM,
 * which then indexes trig_index, twice).
 *
 * This file is built with -Werror=type-limits: the checks compare as long,
 * never "rnum >= 0" in an unsigned rnum type, so typed rnum arguments must
 * not draw an "always true" warning.
 */
#include "unity.h"

#include <limits.h>

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "genzon.h"
#include "oasis.h"
#include "dg_scripts.h"
#include "olc_checks.h"

/* Tables the checks read; test_stubs.c supplies world, top_of_world,
 * top_of_mobt and top_of_objt. */
zone_rnum top_of_zone_table = 0;
struct index_data **trig_index = NULL;
int top_of_trigt = 0;

void setUp(void)
{
  top_of_world = 2;
  top_of_mobt = 3;
  top_of_zone_table = 1;
  top_of_objt = 4;
  trig_index = NULL;
  top_of_trigt = 0;
}

void tearDown(void)
{
  free(trig_index);
  trig_index = NULL;
  top_of_trigt = 0;
}

/* -1 in an int or a long, the most negative values, and an unsigned long
 * that converts to -1, for one check. */
#define ASSERT_REFUSES_NEGATIVES(check) do { \
  int neg_int = -1; \
  long neg_long = -1L; \
  int min_int = INT_MIN; \
  long min_long = LONG_MIN; \
  unsigned long wrapped = ULONG_MAX; \
  TEST_ASSERT_FALSE(check(neg_int)); \
  TEST_ASSERT_FALSE(check(neg_long)); \
  TEST_ASSERT_FALSE(check(min_int)); \
  TEST_ASSERT_FALSE(check(min_long)); \
  TEST_ASSERT_FALSE(check(wrapped)); \
} while (0)

void test_room_rnum_refuses_negative_and_sentinel(void)
{
  room_rnum typed_nowhere = NOWHERE, typed_last = 2;
  int int_nowhere = NOWHERE;

  ASSERT_REFUSES_NEGATIVES(VALID_ROOM_RNUM);
  TEST_ASSERT_FALSE(VALID_ROOM_RNUM(typed_nowhere));
  TEST_ASSERT_FALSE(VALID_ROOM_RNUM(int_nowhere));
  TEST_ASSERT_FALSE(VALID_ROOM_RNUM(3));
  TEST_ASSERT_TRUE(VALID_ROOM_RNUM(0));
  TEST_ASSERT_TRUE(VALID_ROOM_RNUM(typed_last));
  TEST_ASSERT_TRUE(VALID_ROOM_RNUM(2L));
}

void test_mob_rnum_refuses_negative_and_sentinel(void)
{
  mob_rnum typed_nobody = NOBODY, typed_last = 3;
  int int_nobody = NOBODY;

  ASSERT_REFUSES_NEGATIVES(VALID_MOB_RNUM);
  TEST_ASSERT_FALSE(VALID_MOB_RNUM(typed_nobody));
  TEST_ASSERT_FALSE(VALID_MOB_RNUM(int_nobody));
  TEST_ASSERT_FALSE(VALID_MOB_RNUM(4));
  TEST_ASSERT_TRUE(VALID_MOB_RNUM(0));
  TEST_ASSERT_TRUE(VALID_MOB_RNUM(typed_last));
}

void test_zone_rnum_refuses_negative_and_sentinel(void)
{
  zone_rnum typed_nowhere = NOWHERE, typed_last = 1;

  ASSERT_REFUSES_NEGATIVES(VALID_ZONE_RNUM);
  TEST_ASSERT_FALSE(VALID_ZONE_RNUM(typed_nowhere));
  TEST_ASSERT_FALSE(VALID_ZONE_RNUM(2));
  TEST_ASSERT_TRUE(VALID_ZONE_RNUM(0));
  TEST_ASSERT_TRUE(VALID_ZONE_RNUM(typed_last));
}

void test_obj_rnum_checks_refuse_negative_and_sentinel(void)
{
  struct obj_data obj;
  obj_rnum typed_nothing = NOTHING, typed_last = 4;

  memset(&obj, 0, sizeof(obj));
  obj.item_number = NOTHING;
  TEST_ASSERT_FALSE(VALID_OBJ_RNUM(&obj));
  obj.item_number = 5;
  TEST_ASSERT_FALSE(VALID_OBJ_RNUM(&obj));
  obj.item_number = 4;
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM(&obj));
  obj.item_number = 0;
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM(&obj));

  ASSERT_REFUSES_NEGATIVES(VALID_OBJ_RNUM_NUM);
  ASSERT_REFUSES_NEGATIVES(VALID_OBJ_RNUM_IDX);
  TEST_ASSERT_FALSE(VALID_OBJ_RNUM_NUM(typed_nothing));
  TEST_ASSERT_FALSE(VALID_OBJ_RNUM_IDX(typed_nothing));
  TEST_ASSERT_FALSE(VALID_OBJ_RNUM_NUM(5));
  TEST_ASSERT_FALSE(VALID_OBJ_RNUM_IDX(5));
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM_NUM(typed_last));
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM_IDX(typed_last));
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM_NUM(0));
}

/* trig_index is a heap array here, so a read of trig_index[-1] or of
 * trig_index[top_of_trigt] is a heap-buffer-overflow under ASan and an
 * invalid read under Valgrind, not just a wrong answer. */
void test_trig_rnum_refuses_negative_without_indexing(void)
{
  static struct index_data loaded_a, loaded_b;
  trig_rnum typed_nothing = NOTHING, typed_first = 0;

  trig_index = calloc(3, sizeof(*trig_index));
  TEST_ASSERT_NOT_NULL(trig_index);
  trig_index[0] = &loaded_a;
  trig_index[1] = NULL;             /* a deleted trigger's empty slot */
  trig_index[2] = &loaded_b;
  top_of_trigt = 3;                 /* a count: rnums 0..2 */

  ASSERT_REFUSES_NEGATIVES(VALID_TRIG_RNUM);
  TEST_ASSERT_FALSE(VALID_TRIG_RNUM(typed_nothing));
  TEST_ASSERT_FALSE(VALID_TRIG_RNUM(3));
  TEST_ASSERT_FALSE(VALID_TRIG_RNUM(1));
  TEST_ASSERT_TRUE(VALID_TRIG_RNUM(typed_first));
  TEST_ASSERT_TRUE(VALID_TRIG_RNUM(2));
}

/* No triggers loaded: nothing is valid and trig_index (NULL) is never
 * dereferenced. */
void test_trig_rnum_empty_table_accepts_nothing(void)
{
  trig_index = NULL;
  top_of_trigt = 0;

  TEST_ASSERT_FALSE(VALID_TRIG_RNUM(0));
  TEST_ASSERT_FALSE(VALID_TRIG_RNUM(-1));
}

/* The range checks evaluate their argument once, so an argument with a
 * side effect is safe (clang-tidy's bugprone-macro-repeated-side-effects
 * has nothing to report on them). */
void test_range_checks_evaluate_argument_once(void)
{
  int i = -1;

  TEST_ASSERT_FALSE(VALID_ROOM_RNUM(i++));
  TEST_ASSERT_TRUE(VALID_MOB_RNUM(i++));
  TEST_ASSERT_TRUE(VALID_ZONE_RNUM(i++));
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM_NUM(i++));
  TEST_ASSERT_TRUE(VALID_OBJ_RNUM_IDX(i++));
  TEST_ASSERT_EQUAL_INT(4, i);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_room_rnum_refuses_negative_and_sentinel);
  RUN_TEST(test_mob_rnum_refuses_negative_and_sentinel);
  RUN_TEST(test_zone_rnum_refuses_negative_and_sentinel);
  RUN_TEST(test_obj_rnum_checks_refuse_negative_and_sentinel);
  RUN_TEST(test_trig_rnum_refuses_negative_without_indexing);
  RUN_TEST(test_trig_rnum_empty_table_accepts_nothing);
  RUN_TEST(test_range_checks_evaluate_argument_once);
  return UNITY_END();
}
