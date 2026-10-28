/* Executable tests of production zone-command editing (genzon.c).  The test
 * build compiles genzon.c with -ftrivial-auto-var-init=pattern where the
 * compiler has it, so a local left uninitialised reads as a pattern.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "genzon.h"

void setUp(void) {}
void tearDown(void) {}

/* new_command() set only the command letter of the new entry; its args and
 * string pointers (later freed on delete) were stack garbage. */
static void new_command_is_blank(void)
{
  struct zone_data zone;
  struct reset_com *c;

  memset(&zone, 0, sizeof(zone));
  CREATE(zone.cmd, struct reset_com, 1);
  zone.cmd[0].command = 'S';

  TEST_ASSERT_EQUAL_INT(1, new_command(&zone, 0));

  c = &zone.cmd[0];
  TEST_ASSERT_EQUAL_CHAR('N', c->command);
  TEST_ASSERT_FALSE(c->if_flag);
  TEST_ASSERT_EQUAL_INT(0, c->arg1);
  TEST_ASSERT_EQUAL_INT(0, c->arg2);
  TEST_ASSERT_EQUAL_INT(0, c->arg3);
  TEST_ASSERT_NULL(c->sarg1);
  TEST_ASSERT_NULL(c->sarg2);
  TEST_ASSERT_EQUAL_CHAR('S', zone.cmd[1].command);
  free(zone.cmd);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(new_command_is_blank);
  return UNITY_END();
}
