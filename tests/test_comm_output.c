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
#include "../src/comm.c" /* NOLINT(bugprone-suspicious-include): white-box test of static helpers */
#undef main

#include "protocol.h"
#include "tb_error_capture.h"

/* Fixtures for symbols the output path reaches outside comm.c. */
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL; /* DIAG_DESC names mob vnums */
mob_rnum top_of_mobt = 0;
struct room_data *world = NULL;
room_rnum top_of_world = 0;
time_t motdmod = 0;
time_t newsmod = 0;
/* CHECK_PLAYER_SPECIAL's reporter (utils.c, not linked here). */
void tb_player_special_on_mob_at(const struct char_data *ch, const char *field,
                                 const char *source_file, int source_line,
                                 const char *source_func)
{ (void)ch; (void)field; (void)source_file; (void)source_line; (void)source_func; }

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

/* __wrap_ names are fixed by the linker's --wrap option. */
ssize_t __wrap_write(int fd, const void *buf, size_t count);  /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
ssize_t __wrap_write(int fd, const void *buf, size_t count)  /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
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

/* Scripted socket input for process_input(): one read returns it all. */
static char read_script[MAX_RAW_INPUT_LENGTH];
static size_t read_script_len;

ssize_t __wrap_read(int fd, void *buf, size_t count);  /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
ssize_t __wrap_read(int fd, void *buf, size_t count)  /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
{
  size_t n = read_script_len < count ? read_script_len : count;
  (void)fd;
  if (n == 0) {
    errno = EAGAIN;
    return -1;
  }
  memcpy(buf, read_script, n);
  read_script_len = 0;
  return (ssize_t)n;
}

/* With _FORTIFY_SOURCE (the hardened RelWithDebInfo build) glibc turns
 * comm.c's read() into __read_chk(), which --wrap=read does not catch: the
 * real read() then ran on the fake descriptor and returned -1. Keep the
 * fortify check (count must fit the destination) and read the script. */
ssize_t __wrap___read_chk(int fd, void *buf, size_t count, size_t buflen);  /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
ssize_t __wrap___read_chk(int fd, void *buf, size_t count, size_t buflen)  /* NOLINT(cert-dcl37-c,cert-dcl51-cpp) */
{
  if (count > buflen)
    abort();
  return __wrap_read(fd, buf, count);
}

/* History ('!') helpers from interpreter.c; unused by these inputs. */
int is_abbrev(const char *arg1, const char *arg2) { (void)arg1; (void)arg2; return 0; }
void skip_spaces(char **string) { (void)string; }

/* The protocol layer passes plain text through unchanged. */
ssize_t ProtocolInput(descriptor_t *d, char *data, int size, char *out,
                      size_t out_size)
{
  size_t before = strlen(out);
  (void)d; (void)size;
  strlcat(out, data, out_size);
  return (ssize_t)(strlen(out) - before);
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
  buf_largecount = 0; /* tearDown() freed every pooled buffer */
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

/* A write that overflows an already-large buffer must not take a second
 * large buffer (the first one leaked); it fills the live one and enters the
 * reported overflow state. */
static void overflow_of_large_buffer_keeps_it(void)
{
  static char big[30001];
  struct txt_block *first;

  memset(big, 'a', 2000);
  big[2000] = '\0';
  write_to_output(&desc, "%s", big);
  TEST_ASSERT_NOT_NULL(desc.large_outbuf);
  first = desc.large_outbuf;
  TEST_ASSERT_EQUAL_INT(1, buf_largecount);

  memset(big, 'b', 30000);
  big[30000] = '\0';
  write_to_output(&desc, "%s", big);

  TEST_ASSERT_EQUAL_PTR(first, desc.large_outbuf);
  TEST_ASSERT_EQUAL_PTR(first->text, desc.output);
  TEST_ASSERT_EQUAL_INT(1, buf_largecount);
  TEST_ASSERT_EQUAL_INT(0, desc.bufspace);
  TEST_ASSERT_EQUAL_INT(LARGE_BUFSIZE - 1, desc.bufptr);
  TEST_ASSERT_EQUAL_size_t(LARGE_BUFSIZE - 1, strlen(desc.output));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("output-overflow", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);

  /* Overflow state: later output is discarded until the next flush. */
  TEST_ASSERT_EQUAL_size_t(0, write_to_output(&desc, "more"));
  TEST_ASSERT_EQUAL_INT(LARGE_BUFSIZE - 1, desc.bufptr);
}

/* A mob built with 0d0+0 hit dice has max_hit 0; the MSDP opponent health
 * once divided by it every second (SIGFPE). */
static void opponent_with_zero_max_hit_is_reported(void)
{
  static struct index_data index[1];
  static char_data mob;

  memset(&mob, 0, sizeof(mob));
  index[0].vnum = 3001;
  mob_index = index;
  top_of_mobt = 0;
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_ISNPC);
  mob.nr = 0;
  GET_HIT(&mob) = 5;
  GET_MAX_HIT(&mob) = 0;

  TEST_ASSERT_EQUAL_INT(0, msdp_opponent_health_percent(&desc, &mob));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("opponent-health", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "opponent mob vnum 3001 has max_hit 0"));

  /* Normal case, with hit * 100 beyond what sh_int arithmetic would hold. */
  GET_MAX_HIT(&mob) = 30000;
  GET_HIT(&mob) = 15000;
  TEST_ASSERT_EQUAL_INT(50, msdp_opponent_health_percent(&desc, &mob));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  mob_index = NULL;
}

/* The auto prompt compared 'hit << 2' with max_hit: undefined for the
 * negative hit points of an incapacitated or dying player (UBSan abort). */
static void auto_prompt_handles_negative_hit_points(void)
{
  static char_data player;
  static struct player_special_data specials;

  memset(&player, 0, sizeof(player));
  memset(&specials, 0, sizeof(specials));
  player.player_specials = &specials;
  SET_BIT_AR(PRF_FLAGS(&player), PRF_DISPAUTO);
  GET_HIT(&player) = -5;
  GET_MAX_HIT(&player) = 100;
  GET_MANA(&player) = GET_MAX_MANA(&player) = 100;
  GET_MOVE(&player) = GET_MAX_MOVE(&player) = 100;
  desc.str = NULL;
  desc.character = &player;

  TEST_ASSERT_EQUAL_STRING("-5H > ", make_prompt(&desc));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  desc.character = NULL;
}

/* Formatted text longer than vwrite_to_output's buffer was cut with no
 * report (the size check was dead code once ProtocolOutput rewrote the
 * size through an int * aliasing a size_t). Runs after
 * overflow_of_large_buffer_keeps_it, so the rate-limited output-overflow
 * report from the large-buffer clamp is suppressed here and the
 * truncation report is the only record. */
static void oversized_format_is_reported(void)
{
  static char big[MAX_STRING_LENGTH + 100];

  memset(big, 'z', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  write_to_output(&desc, "%s", big);

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("output-truncate", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "desc #42 fd 7"));
  TEST_ASSERT_EQUAL_INT(0, desc.bufspace); /* overflow state */
}

/* A typed line longer than the command buffer was cut without the notice
 * or any record, because the notice tested space_left <= 0 while the copy
 * loop stops at space_left == 1. */
static void long_input_line_is_reported(void)
{
  static char *history[HISTORY_SIZE];
  size_t n = MAX_INPUT_LENGTH + 100;

  memset(read_script, 'q', n);
  read_script[n] = '\n';
  read_script_len = n + 1;
  desc.history = history;
  desc.str = NULL;
  write_budget = sizeof(written) - 1;

  TEST_ASSERT_EQUAL_INT(1, process_input(&desc));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("input-truncate", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(written, "Line too long."));
  TEST_ASSERT_EQUAL_size_t(MAX_INPUT_LENGTH - 2, strlen(desc.input.head->text));

  flush_input_queue(&desc);
  free(history[0]);
  history[0] = NULL;
  desc.history = NULL;
}

/* Commands beyond the pending-command bound were dropped silently. */
static void input_flood_is_reported(void)
{
  static char *history[HISTORY_SIZE];
  int i;

  for (i = 0; i < TB_MAX_PENDING_COMMANDS; i++)
    TEST_ASSERT_EQUAL_INT(1, write_to_q("north", &desc.input, 0));
  TEST_ASSERT_EQUAL_INT(0, write_to_q("north", &desc.input, 0));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors); /* reported by the caller */

  strlcpy(read_script, "look\n", sizeof(read_script));
  read_script_len = 5;
  desc.history = history;
  desc.str = NULL;
  TEST_ASSERT_EQUAL_INT(1, process_input(&desc));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("input-flood", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(desc.output, "Too many commands queued"));

  flush_input_queue(&desc);
  free(history[0]);
  history[0] = NULL;
  desc.history = NULL;
}

/* send_to_range() looped 'j < top_of_world' and never reached the highest
 * room; a reversed range was logged with no detail. */
static void send_to_range_includes_last_room(void)
{
  static struct room_data rooms[2];
  static char_data listener;

  memset(rooms, 0, sizeof(rooms));
  memset(&listener, 0, sizeof(listener));
  rooms[0].number = 100;
  rooms[1].number = 101;
  world = rooms;
  top_of_world = 1;
  listener.in_room = 1;
  listener.desc = &desc;
  rooms[1].people = &listener;

  send_to_range(100, 101, "echo");
  TEST_ASSERT_EQUAL_STRING("echo", desc.output);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  send_to_range(101, 100, "reversed");
  TEST_ASSERT_EQUAL_STRING("echo", desc.output);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("send-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "start vnum 101 > finish vnum 100"));
  world = NULL;
  top_of_world = 0;
}

/* A failed log-file open used to leave the redirected stream closed (by
 * freopen), and the next fallback name was then opened on that closed
 * FILE *. Now the stream survives a failure and is redirected on success. */
static void failed_log_open_keeps_stream(void)
{
  char path[] = "/tmp/tb_comm_logXXXXXX";
  char line[64] = "";
  FILE *stream, *check;
  int fd = mkstemp(path);

  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
  stream = fopen("/dev/null", "w");
  TEST_ASSERT_NOT_NULL(stream);

  TEST_ASSERT_FALSE(open_logfile("/nonexistent-dir/tb/syslog", stream));
  TEST_ASSERT_TRUE(fprintf(stream, "still open\n") > 0);
  TEST_ASSERT_EQUAL_INT(0, fflush(stream));

  TEST_ASSERT_TRUE(open_logfile(path, stream));
  TEST_ASSERT_EQUAL_PTR(stream, logfile);
  TEST_ASSERT_TRUE(fprintf(logfile, "logged\n") > 0);
  TEST_ASSERT_EQUAL_INT(0, fflush(logfile));
  check = fopen(path, "r");
  TEST_ASSERT_NOT_NULL(check);
  TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), check));
  TEST_ASSERT_EQUAL_STRING("logged\n", line);
  TEST_ASSERT_EQUAL_INT(0, fclose(check));
  TEST_ASSERT_EQUAL_INT(0, fclose(stream));
  logfile = NULL;
  TEST_ASSERT_EQUAL_INT(0, remove(path));
}

/* The snoop copy used "%*s" (a field width, so the whole buffer including
 * unsent bytes) and ended with a literal "%%". */
static void snooper_sees_only_sent_bytes(void)
{
  static struct descriptor_data snooper;
  static protocol_t snooper_proto;

  memset(&snooper, 0, sizeof(snooper));
  memset(&snooper_proto, 0, sizeof(snooper_proto));
  snooper.pProtocol = &snooper_proto;
  snooper.output = snooper.small_outbuf;
  snooper.bufspace = SMALL_BUFSIZE - 1;
  desc.snoop_by = &snooper;

  write_to_output(&desc, "ABCDEF");
  write_budget = 3;
  TEST_ASSERT_EQUAL_INT(3, process_output(&desc));
  TEST_ASSERT_EQUAL_STRING("% ABC%", snooper.output);
  TEST_ASSERT_EQUAL_STRING("DEF", desc.output);
  desc.snoop_by = NULL;
}

/* The port was narrowed to ush_int before its range check, so 70000
 * became 4464 and was accepted; '-Cx' became fd 0. */
static void command_line_numbers_are_range_checked(void)
{
  long value = -1;

  TEST_ASSERT_FALSE(parse_long_arg("70000", 1025, 65535, &value));
  TEST_ASSERT_FALSE(parse_long_arg("1024", 1025, 65535, &value));
  TEST_ASSERT_FALSE(parse_long_arg("4000x", 1025, 65535, &value));
  TEST_ASSERT_FALSE(parse_long_arg("", 0, MOTHER_DESC_MAX, &value));
  TEST_ASSERT_FALSE(parse_long_arg("x", 0, MOTHER_DESC_MAX, &value));
  TEST_ASSERT_EQUAL_INT(-1, (int)value);
  TEST_ASSERT_TRUE(parse_long_arg("4000", 1025, 65535, &value));
  TEST_ASSERT_EQUAL_INT(4000, (int)value);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(partial_prompt_tail_is_carried_over);
  RUN_TEST(partial_crlf_counts_as_nothing_sent);
  RUN_TEST(corrupt_bufptr_is_reported_not_used);
  RUN_TEST(overflow_of_large_buffer_keeps_it);
  RUN_TEST(opponent_with_zero_max_hit_is_reported);
  RUN_TEST(auto_prompt_handles_negative_hit_points);
  RUN_TEST(oversized_format_is_reported);
  RUN_TEST(long_input_line_is_reported);
  RUN_TEST(input_flood_is_reported);
  RUN_TEST(send_to_range_includes_last_room);
  RUN_TEST(failed_log_open_keeps_stream);
  RUN_TEST(snooper_sees_only_sent_bytes);
  RUN_TEST(command_line_numbers_are_range_checked);
  return UNITY_END();
}
