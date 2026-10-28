/* Executable tests of the boot-time world checks in comm.c
 * (report_world_gaps(), run after the world is loaded, in syntax-check mode
 * too).
 *
 * H1-16: on a world without the DG caster proxy mob, damage() used to
 * exempt every player from ROOM_PEACEFUL (fixed in damage() itself), and
 * nothing ever told the operator that mob vnum DG_CASTER_PROXY was gone.
 * H1-27: configured donation rooms were never checked against the world.
 *
 * comm.c is #included so its file-static functions can be called directly;
 * main() is renamed and unreferenced sections are discarded at link time.
 */
#include "unity.h" /* before comm.c: unity.h pulls in <math.h>, whose log()
                      * clashes with utils.h's log macro */
#define main tb_comm_main_unused
#include "../src/comm.c" /* NOLINT(bugprone-suspicious-include): white-box test of static helpers */
#undef main

#include "tb_error_capture.h"

/* db.c's real_mobile(): the caster proxy exists only when proxy_loaded. */
static int proxy_loaded;
mob_rnum real_mobile(mob_vnum vnum)
{
  return (proxy_loaded && vnum == DG_CASTER_PROXY) ? 0 : NOBODY;
}

/* act.item.c's donation-room check (tested in test_act_item): count calls. */
static int donation_checks;
void check_donation_rooms(void) { donation_checks++; }

void setUp(void)
{
  proxy_loaded = TRUE;
  donation_checks = 0;
  tb_error_capture_reset();
}
void tearDown(void) {}

/* A world with the proxy mob boots without a word. */
static void complete_world_reports_nothing(void)
{
  report_world_gaps();
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* H1-27: the configured donation rooms are checked against the world once
 * per boot. */
static void donation_rooms_are_checked_at_boot(void)
{
  report_world_gaps();
  TEST_ASSERT_EQUAL_INT(1, donation_checks);
}

/* H1-16: a missing caster proxy is named once, at boot. */
static void missing_caster_proxy_is_reported(void)
{
  char expect[32];

  proxy_loaded = FALSE;
  report_world_gaps();
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("caster-proxy-missing", tb_captured_operation);
  TEST_ASSERT_TRUE(snprintf(expect, sizeof(expect), "vnum %d ",
                            DG_CASTER_PROXY) > 0);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, expect));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(complete_world_reports_nothing);
  RUN_TEST(missing_caster_proxy_is_reported);
  RUN_TEST(donation_rooms_are_checked_at_boot);
  return UNITY_END();
}
