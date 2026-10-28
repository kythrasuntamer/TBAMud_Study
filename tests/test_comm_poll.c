/* Executable tests of game_loop()'s handling of a failed socket poll
 * (comm.c): poll_failure_recoverable() and mark_unpollable_descriptors().
 *
 * A1-14: every select() failure used to return from game_loop(), EINTR
 * included, and init_game() then saved and logged "Normal termination of
 * game." with exit status 0. Now a signal is retried, player sockets that
 * are no longer open are reported and dropped, and anything else is a
 * FATAL record (init_game() then exits nonzero after saving).
 *
 * comm.c is #included so its file-static functions can be called directly;
 * main() is renamed and unreferenced sections are discarded at link time.
 * Real descriptor numbers are used: a pipe stands in for open sockets and a
 * closed pipe end for a socket closed behind close_socket()'s back.
 */
#include "unity.h" /* before comm.c: unity.h pulls in <math.h>, whose log()
                      * clashes with utils.h's log macro */
#define main tb_comm_main_unused
#include "../src/comm.c" /* NOLINT(bugprone-suspicious-include): white-box test of static helpers */
#undef main

#include "tb_error_capture.h"

/* Fixtures for symbols the poll helpers reach outside comm.c. */
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL; /* DIAG_DESC names mob vnums */
mob_rnum top_of_mobt = 0;
void basic_mud_log(const char *format, ...) { (void)format; }

static struct descriptor_data live_desc, dead_desc;
static int live_pipe[2];  /* open: the "listening socket" and a player */
static int dead_fd;       /* a descriptor number that is not open */

void setUp(void)
{
  int scratch[2];

  TEST_ASSERT_EQUAL_INT(0, pipe(live_pipe));
  /* Open and close a pipe: its numbers are now certainly not open, and
   * nothing else opens a file before the test uses them. */
  TEST_ASSERT_EQUAL_INT(0, pipe(scratch));
  TEST_ASSERT_EQUAL_INT(0, close(scratch[0]));
  TEST_ASSERT_EQUAL_INT(0, close(scratch[1]));
  dead_fd = scratch[1];

  memset(&live_desc, 0, sizeof(live_desc));
  memset(&dead_desc, 0, sizeof(dead_desc));
  live_desc.descriptor = live_pipe[0];
  live_desc.desc_num = 1;
  STATE(&live_desc) = CON_PLAYING;
  dead_desc.descriptor = dead_fd;
  dead_desc.desc_num = 2;
  STATE(&dead_desc) = CON_PLAYING;
  live_desc.next = &dead_desc;
  descriptor_list = &live_desc;
  tb_error_capture_reset();
}

void tearDown(void)
{
  descriptor_list = NULL;
  TEST_ASSERT_EQUAL_INT(0, close(live_pipe[0]));
  TEST_ASSERT_EQUAL_INT(0, close(live_pipe[1]));
}

/* A signal (SIGCHLD from reap(), SIGUSR1 ...) interrupting the poll is not
 * a failure: poll again, report nothing, touch no connection. */
static void interrupted_poll_is_retried_quietly(void)
{
  TEST_ASSERT_TRUE(poll_failure_recoverable(live_pipe[1], EINTR, 9));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&live_desc));
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&dead_desc));
}

/* EBADF from a player socket closed outside close_socket(): that
 * connection alone is reported (naming the descriptor) and marked for
 * closing, and the loop carries on. */
static void closed_player_socket_is_reported_and_dropped(void)
{
  char expect[64];

  TEST_ASSERT_TRUE(poll_failure_recoverable(live_pipe[1], EBADF, 9));
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&dead_desc));
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&live_desc));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("select-bad-fd", tb_captured_operation);
  TEST_ASSERT_TRUE(snprintf(expect, sizeof(expect), "desc #2 fd %d no-char",
                            dead_fd) > 0);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, expect));
}

/* A dead listening socket cannot be repaired: it is named, and the poll
 * failure becomes FATAL so game_loop() stops and init_game() exits
 * nonzero after saving. */
static void closed_listening_socket_is_fatal(void)
{
  TEST_ASSERT_FALSE(poll_failure_recoverable(dead_fd, EBADF, 9));
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors); /* select-bad-fd, then select */
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("select", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "maxdesc 9"));
}

/* EBADF with no player socket to blame would recur on every pulse. */
static void unexplained_bad_descriptor_is_fatal(void)
{
  live_desc.next = NULL;
  TEST_ASSERT_FALSE(poll_failure_recoverable(live_pipe[1], EBADF, 9));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("select", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&live_desc));
}

/* Any other errno (EINVAL, ENOMEM ...) is FATAL too, never a quiet
 * "normal" shutdown. */
static void other_poll_errors_are_fatal(void)
{
  TEST_ASSERT_FALSE(poll_failure_recoverable(live_pipe[1], EINVAL, 9));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("select", tb_captured_operation);
  TEST_ASSERT_EQUAL_INT(CON_PLAYING, STATE(&dead_desc));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(interrupted_poll_is_retried_quietly);
  RUN_TEST(closed_player_socket_is_reported_and_dropped);
  RUN_TEST(closed_listening_socket_is_fatal);
  RUN_TEST(unexplained_bad_descriptor_is_fatal);
  RUN_TEST(other_poll_errors_are_fatal);
  return UNITY_END();
}
