/* Executable tests of the production spec-proc name table (spec_assign.c):
 * 'stat' passes get_spec_func_name() to %s, so it must never return NULL.
 * spec_func_list names procs from many files; these stand-ins provide the
 * addresses (only the addresses are compared).
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "spec_procs.h"
#include "ban.h"
#include "boards.h"
#include "mail.h"
#include "quest.h"
#include "shop.h"
#include "db.h"
#include <stdarg.h>
#include "tb_error_capture.h"

#define STUB_SPEC(name) SPECIAL(name) \
  { (void)ch; (void)me; (void)cmd; (void)argument; return FALSE; }
STUB_SPEC(mayor)
STUB_SPEC(snake)
STUB_SPEC(thief)
STUB_SPEC(magic_user)
STUB_SPEC(puff)
STUB_SPEC(fido)
STUB_SPEC(janitor)
STUB_SPEC(cityguard)
STUB_SPEC(postmaster)
STUB_SPEC(receptionist)
STUB_SPEC(cryogenicist)
STUB_SPEC(gen_board)
STUB_SPEC(bank)
STUB_SPEC(pet_shops)
STUB_SPEC(dump)
STUB_SPEC(guild)
STUB_SPEC(guild_guard)
STUB_SPEC(questmaster)
STUB_SPEC(shop_keeper)
STUB_SPEC(CastleGuard)
STUB_SPEC(king_welmar)
STUB_SPEC(training_master)
STUB_SPEC(James)
STUB_SPEC(cleaning)
STUB_SPEC(tim)
STUB_SPEC(tom)
STUB_SPEC(DicknDavid)
STUB_SPEC(peter)
STUB_SPEC(jerry)
static STUB_SPEC(unlisted_proc)

void setUp(void) { tb_error_capture_reset(); }
void tearDown(void) {}

/* H3-19: castle procs are named, and a proc missing from the list is
 * "Unknown" (reported once), never NULL. */
static void test_spec_func_names_are_never_null(void)
{
  TEST_ASSERT_EQUAL_STRING("Castle Guard", get_spec_func_name(CastleGuard));
  TEST_ASSERT_EQUAL_STRING("King Welmar", get_spec_func_name(king_welmar));
  TEST_ASSERT_EQUAL_STRING("Pet Shop", get_spec_func_name(pet_shops));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  TEST_ASSERT_EQUAL_STRING("Unknown", get_spec_func_name(unlisted_proc));
  TEST_ASSERT_EQUAL_STRING("Unknown", get_spec_func_name(unlisted_proc));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("unnamed-spec", tb_captured_operation);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_spec_func_names_are_never_null);
  return UNITY_END();
}
