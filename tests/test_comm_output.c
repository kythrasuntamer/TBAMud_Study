/* Executable tests of the production descriptor output path in comm.c:
 * vwrite_to_output() (buffer state machine) and process_output() (sending,
 * partial writes, carrying unsent tails over).
 *
 * comm.c is #included so its file-static functions can be called directly;
 * main() is renamed and unreferenced sections are discarded at link time.
 * The test links with -Wl,--wrap=write, so comm.c's socket writes reach
 * __wrap_write() below, which accepts a scripted number of bytes and then
 * reports EAGAIN, exactly as a full kernel socket buffer would.
 */
#include "unity.h" /* before comm.c: unity.h pulls in <math.h>, whose log()
                      * clashes with utils.h's log macro */
#define main tb_comm_main_unused
#include "../src/comm.c"
#undef main

#include "protocol.h"
#include "tb_error_capture.h"

/* Fixtures for symbols the output path reaches outside comm.c. */
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL; /* DIAG_DESC names mob vnums */
mob_rnum top_of_mobt = 0;
time_t motdmod = 0;
time_t newsmod = 0;
void basic_mud_log(const char *format, ...) { (void)format; }
const char *ProtocolOutput(descriptor_t *d, const char *data, int *length)
{ (void)d; (void)length; return data; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}

/* Scripted socket: accept up to write_budget bytes in total, then EAGAIN. */
static size_t write_budget;
static char written[MAX_SOCK_BUF * 2];
static size_t written_len;

ssize_t __wrap_write(int fd, const void *buf, size_t count);
ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
  size_t n = count < write_budget ? count : write_budget;
  (void)fd;
  if (n == 0) {
    errno = EAGAIN;
    return -1;
  }
  if (written_len + n < sizeof(written)) {
    memcpy(written + written_len, buf, n);
    written_len += n;
    written[written_len] = '\0';
  }
  write_budget -= n;
  return (ssize_t)n;
}

static struct descriptor_data desc;
static protocol_t proto;
static char *edit_target; /* d->str only needs to be non-NULL: prompt "] " */

void setUp(void)
{
  memset(&desc, 0, sizeof(desc));
  memset(&proto, 0, sizeof(proto));
  desc.descriptor = 7;
  desc.desc_num = 42;
  desc.pProtocol = &proto;
  desc.output = desc.small_outbuf;
  desc.bufspace = SMALL_BUFSIZE - 1;
  desc.bufptr = 0;
  desc.str = &edit_target;
  STATE(&desc) = CON_PLAYING;
  write_budget = 0;
  written_len = 0;
  written[0] = '\0';
  tb_error_capture_reset();
}
void tearDown(void)
{
  outbuf_reset_small(&desc);
  free_bufpool();
}

/* The kernel takes all of the queued output but only part of the prompt.
 * The unsent prompt tail must become the new buffer contents with a
 * non-negative bufptr; the old code subtracted its length from 0. */
static void partial_prompt_tail_is_carried_over(void)
{
  write_to_output(&desc, "ABC");
  write_budget = 4;                         /* "ABC" + "]" of "] " */
  TEST_ASSERT_EQUAL_INT(4, process_output(&desc));
  TEST_ASSERT_EQUAL_STRING("ABC]", written);
  TEST_ASSERT_EQUAL_INT(1, desc.bufptr);
  TEST_ASSERT_EQUAL_INT(SMALL_BUFSIZE - 2, desc.bufspace);
  TEST_ASSERT_EQUAL_STRING(" ", desc.output);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  /* The next write appends inside small_outbuf, not into last_input. */
  write_to_output(&desc, "XY");
  TEST_ASSERT_EQUAL_STRING(" XY", desc.output);
  TEST_ASSERT_EQUAL_INT(3, desc.bufptr);
  TEST_ASSERT_EQUAL_INT(0, desc.last_input[MAX_INPUT_LENGTH - 1]);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* With the prepended CRLF (has_prompt), a 1-byte write sent no output. */
static void partial_crlf_counts_as_nothing_sent(void)
{
  write_to_output(&desc, "ABC");
  desc.has_prompt = TRUE;
  write_budget = 1;
  TEST_ASSERT_EQUAL_INT(0, process_output(&desc));
  TEST_ASSERT_EQUAL_STRING("ABC", desc.output);
  TEST_ASSERT_EQUAL_INT(3, desc.bufptr);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* A corrupt offset is reported and the buffer reset, never written through. */
static void corrupt_bufptr_is_reported_not_used(void)
{
  memset(desc.last_input, 'L', sizeof(desc.last_input) - 1);
  desc.last_input[sizeof(desc.last_input) - 1] = '\0';
  desc.bufptr = -2;
  desc.bufspace = SMALL_BUFSIZE + 1;

  write_to_output(&desc, "AB");

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("outbuf-invariant", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "desc #42 fd 7"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "bufptr=-2"));
  TEST_ASSERT_EQUAL_CHAR('L', desc.last_input[sizeof(desc.last_input) - 2]);
  TEST_ASSERT_EQUAL_STRING("AB", desc.output);
  TEST_ASSERT_EQUAL_INT(2, desc.bufptr);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(partial_prompt_tail_is_carried_over);
  RUN_TEST(partial_crlf_counts_as_nothing_sent);
  RUN_TEST(corrupt_bufptr_is_reported_not_used);
  return UNITY_END();
}
