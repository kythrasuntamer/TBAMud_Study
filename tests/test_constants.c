/* Name tables in src/constants.c, linked as built: each table entry must
 * line up with the structs.h constant that indexes it.  constants.c also
 * asserts the table lengths at compile time; these tests pin the names. */
#include "unity.h"

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "constants.h"

void setUp(void) {}
void tearDown(void) {}

/* I2-12: CON_IEDIT had no entry, so it showed "Protocol Detection" and
 * CON_GET_PROTOCOL showed the "\n" sentinel. */
void test_connected_types_line_up_with_con_states(void)
{
  TEST_ASSERT_EQUAL_STRING("Message edit", connected_types[CON_MSGEDIT]);
  TEST_ASSERT_EQUAL_STRING("Object inst edit", connected_types[CON_IEDIT]);
  TEST_ASSERT_EQUAL_STRING("Protocol Detection",
                           connected_types[CON_GET_PROTOCOL]);
  TEST_ASSERT_EQUAL_STRING("\n", connected_types[CON_GET_PROTOCOL + 1]);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_connected_types_line_up_with_con_states);
  return UNITY_END();
}
