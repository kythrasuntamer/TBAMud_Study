/* Regression tests for the production bulletin-board code (boards.c):
 * board initialisation, headline bounds, message-index bookkeeping, slot
 * accounting and the board-file loader. boards.c is linked with
 * interpreter.c and utils.c; the editor (string_write), object lookup and
 * paging are faked here.
 *
 * The board files live under LIB_ETC ("etc/") relative to the working
 * directory, so main() runs the suite inside a fresh temporary directory.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "boards.h"
#include <stdarg.h>
#include <sys/stat.h>

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from the code under test to this capturing version. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

/* ---- fakes for symbols outside the linked sources --------------------- */

static char paged[MAX_STRING_LENGTH * 2];

void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  strlcpy(paged, str ? str : "", sizeof(paged));
}

void string_write(struct descriptor_data *d, char **writeto, size_t len,
                  long mailto, void *data)
{
  (void)data;
  d->str = writeto;
  d->max_str = len;
  d->mail_to = mailto;
}

/* Object prototypes: one per configured board, unless a test removes one. */
static struct index_data objs[NUM_OF_BOARDS];
static obj_vnum missing_vnum = NOTHING;

obj_rnum real_object(obj_vnum vnum)
{
  obj_rnum i;

  if (vnum == missing_vnum)
    return NOTHING;
  for (i = 0; i < NUM_OF_BOARDS; i++)
    if (objs[i].vnum == vnum)
      return i;
  return NOTHING;
}

int isname(const char *str, const char *namelist)
{
  return namelist && str && !strcmp(str, namelist);
}

/* ---- fixture ---------------------------------------------------------- */

static struct char_data writer;
static struct player_special_data writer_specials;
static struct descriptor_data writer_desc;
static struct obj_data board_obj;
static struct room_data room;

void setUp(void)
{
  int i;

  memset(&writer, 0, sizeof(writer));
  memset(&writer_specials, 0, sizeof(writer_specials));
  memset(&writer_desc, 0, sizeof(writer_desc));
  memset(&board_obj, 0, sizeof(board_obj));
  memset(&room, 0, sizeof(room));
  for (i = 0; i < NUM_OF_BOARDS; i++)
    objs[i].vnum = BOARD_VNUM(i);
  obj_index = objs;
  top_of_objt = NUM_OF_BOARDS - 1;
  missing_vnum = NOTHING;
  writer.player.name = "Writer";
  writer.player.level = 1;
  writer.player_specials = &writer_specials;
  writer.char_specials.saved.idnum = 42;
  writer.nr = NOBODY;
  writer.desc = &writer_desc;
  writer.in_room = 0;
  writer_desc.character = &writer;
  writer_desc.connected = CON_PLAYING;
  board_obj.name = "board";
  board_obj.item_number = 0;   /* board 0, vnum 3099 */
  room.number = 3001;
  world = &room;
  top_of_world = 0;
  paged[0] = '\0';
  for (i = 0; i < NUM_OF_BOARDS; i++)
    (void)remove(FILENAME(i));
  tb_error_capture_reset();
}

void tearDown(void)
{
  board_clear_all();
  descriptor_list = NULL;
  world = NULL;
  obj_index = NULL;
}

static char *line(const char *text)
{
  static char buf[MAX_INPUT_LENGTH];
  strlcpy(buf, text, sizeof(buf));
  return buf;
}

/* Finish the post the writer started: what string_add() would leave. */
static void finish_post(const char *body)
{
  TEST_ASSERT_NOT_NULL(writer_desc.str);
  *writer_desc.str = strdup(body);
  writer_desc.str = NULL;
}

/* Post on board 0 through the production writer, from a heap copy of the
 * command tail so ASan sees any write past it. */
static int write_post(const char *headline, const char *body)
{
  char *arg = strdup(headline);
  int ret;

  TEST_ASSERT_NOT_NULL(arg);
  ret = board_write_message(0, &writer, arg, &board_obj);
  free(arg);
  if (writer_desc.str)
    finish_post(body);
  return ret;
}

static void read_post(int num)
{
  char arg[16];
  snprintf(arg, sizeof(arg), "%d", num);
  paged[0] = '\0';
  board_display_msg(0, &writer, arg, &board_obj);
}

/* C1-07: before init_boards() the message index is all zeroes, every entry
 * claiming slot 0. gen_board must refuse, loudly, instead of using it. */
static void test_gen_board_before_init_is_refused(void)
{
  int handled = gen_board(&writer, &board_obj, find_command("look"), line("board"));

  TEST_ASSERT_EQUAL_INT(0, handled);
  TEST_ASSERT_EQUAL_STRING("board-init", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("ERROR", tb_captured_severity);
}

/* C1-07: a missing board object is reported and counted (boot_db makes it
 * fatal at boot), instead of exit(1) on the first board use mid-game. */
static void test_init_reports_missing_board_object(void)
{
  missing_vnum = BOARD_VNUM(6);
  TEST_ASSERT_EQUAL_INT(1, init_boards());
  TEST_ASSERT_EQUAL_STRING("board-init", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "vnum 1228"));
}

/* C1-07: the board is identified by the object whose procedure fired. */
static void test_gen_board_finds_board_by_object_vnum(void)
{
  TEST_ASSERT_EQUAL_INT(0, init_boards());
  tb_error_capture_reset();
  board_obj.item_number = 3;   /* social board, vnum 3096 */
  TEST_ASSERT_EQUAL_INT(1, gen_board(&writer, &board_obj, find_command("look"), line("board")));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* C1-02: a headline that starts late in the command line had its 80-column
 * cut written past the end of the caller's buffer. The buffer here is
 * exactly MAX_INPUT_LENGTH bytes on the heap, like game_loop's comm[]. */
static void test_late_headline_does_not_write_past_caller_buffer(void)
{
  char *comm = malloc(MAX_INPUT_LENGTH);
  size_t last = MAX_INPUT_LENGTH - 2;

  TEST_ASSERT_NOT_NULL(comm);
  memset(comm, ' ', MAX_INPUT_LENGTH);
  memcpy(comm, "write", 5);
  comm[last] = 'x';
  comm[last + 1] = '\0';

  TEST_ASSERT_EQUAL_INT(1, board_write_message(0, &writer, comm + 5, &board_obj));
  if (writer_desc.str)
    finish_post("body");
  TEST_ASSERT_EQUAL_CHAR('x', comm[last]);
  free(comm);

  read_post(1);
  TEST_ASSERT_NOT_NULL(strstr(paged, ":: x\r\n"));
}

/* C1-02: an over-long headline is cut to BOARD_HEADLINE_MAX and reported. */
static void test_long_headline_is_truncated_and_reported(void)
{
  char headline[200], expect[BOARD_HEADLINE_MAX + 8];

  memset(headline, 'h', sizeof(headline) - 1);
  headline[sizeof(headline) - 1] = '\0';
  write_post(headline, "body");

  TEST_ASSERT_EQUAL_STRING("headline-truncate", tb_captured_operation);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));

  read_post(1);
  memcpy(expect, ":: ", 3);
  memset(expect + 3, 'h', BOARD_HEADLINE_MAX);
  strcpy(expect + 3 + BOARD_HEADLINE_MAX, "\r\n");
  TEST_ASSERT_NOT_NULL(strstr(paged, expect));
}

static void remove_post(int num)
{
  char arg[16];
  snprintf(arg, sizeof(arg), "%d", num);
  TEST_ASSERT_EQUAL_INT(1, board_remove_msg(0, &writer, arg, &board_obj));
}

/* C1-03: removing a message left the vacated last index entry holding the
 * previous last entry's heading and slot, so clearing the board at shutdown
 * freed them twice (ASan double-free before the fix). */
static void test_remove_first_of_two_then_clear(void)
{
  write_post("first", "body one");
  write_post("second", "body two");
  remove_post(1);
  read_post(1);
  TEST_ASSERT_NOT_NULL(strstr(paged, "body two"));
  tb_error_capture_reset();
  board_clear_all();
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* C1-03: removing the last message left its freed heading in the entry. */
static void test_remove_last_then_clear(void)
{
  write_post("first", "body one");
  write_post("second", "body two");
  remove_post(2);
  tb_error_capture_reset();
  board_clear_all();
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Write a one-message board file for board 0 with the given record. */
static void write_board_file(const struct board_msginfo *rec,
                             const char *heading, size_t heading_bytes)
{
  FILE *fl = fopen(FILENAME(0), "wb");
  int count = 1;

  TEST_ASSERT_NOT_NULL(fl);
  TEST_ASSERT_EQUAL_size_t(1, fwrite(&count, sizeof(count), 1, fl));
  TEST_ASSERT_EQUAL_size_t(1, fwrite(rec, sizeof(*rec), 1, fl));
  if (heading_bytes)
    TEST_ASSERT_EQUAL_size_t(heading_bytes, fwrite(heading, 1, heading_bytes, fl));
  TEST_ASSERT_EQUAL_INT(0, fclose(fl));
}

/* C1-04: the loader used the heading pointer and slot number stored in the
 * file. A bad heading length reset the board, which freed the on-disk
 * pointer and indexed the slot pool with the on-disk slot (invalid free /
 * out-of-bounds write before the fix). */
static void test_load_ignores_on_disk_pointer_and_slot(void)
{
  struct board_msginfo rec;

  memset(&rec, 0, sizeof(rec));
  rec.slot_num = 12345;
  rec.heading = (char *)(uintptr_t)0x4141414141410ULL;
  rec.heading_len = 0;
  write_board_file(&rec, NULL, 0);

  board_load_board(0);
  TEST_ASSERT_EQUAL_STRING("board-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "heading_len 0"));
  board_clear_all();
}

/* C1-04: the same, with the failure on the heading read itself. */
static void test_load_short_heading_read(void)
{
  struct board_msginfo rec;

  memset(&rec, 0, sizeof(rec));
  rec.slot_num = 12345;
  rec.heading = (char *)(uintptr_t)0x4141414141410ULL;
  rec.heading_len = 50;
  write_board_file(&rec, "short", 5);

  board_load_board(0);
  TEST_ASSERT_EQUAL_STRING("board-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "field heading"));
  board_clear_all();
}

/* C1-04: lengths beyond what the writer can produce are refused before any
 * allocation (an INT_MAX heading made CREATE() abort the server). */
static void test_load_rejects_oversized_lengths(void)
{
  struct board_msginfo rec;

  memset(&rec, 0, sizeof(rec));
  rec.heading_len = 0x7fffffff;
  write_board_file(&rec, NULL, 0);
  board_load_board(0);
  TEST_ASSERT_EQUAL_STRING("board-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, TB_ERRID_PARSER_RANGE));
}

/* C1-05: a heading without its terminator was kept, and the owner check in
 * board_remove_msg() strstr'd past the 5-byte block (ASan before the fix). */
static void test_load_rejects_unterminated_heading(void)
{
  struct board_msginfo rec;

  memset(&rec, 0, sizeof(rec));
  rec.heading_len = 5;
  write_board_file(&rec, "hello", 5);
  board_load_board(0);
  TEST_ASSERT_EQUAL_STRING("board-load", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "heading not NUL-terminated"));

  {
    char arg[] = "1";
    board_remove_msg(0, &writer, arg, &board_obj);
  }
}

/* Saved boards load back with their headings and bodies. */
static void test_save_load_round_trip(void)
{
  write_post("first", "body one");
  write_post("second", "body two");
  board_save_board(0);
  board_clear_all();
  tb_error_capture_reset();

  board_load_board(0);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  read_post(2);
  TEST_ASSERT_NOT_NULL(strstr(paged, ":: second"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "body two"));
}

int main(void)
{
  char dir[] = "/tmp/tb_test_boards.XXXXXX";

  if (!mkdtemp(dir) || chdir(dir) != 0 || mkdir("etc", 0700) != 0) {
    perror("test_boards fixture directory");
    return 1;
  }
  logfile = stderr;
  complete_cmd_info = (struct command_info *)cmd_info;

  UNITY_BEGIN();
  RUN_TEST(test_gen_board_before_init_is_refused);
  RUN_TEST(test_init_reports_missing_board_object);
  RUN_TEST(test_gen_board_finds_board_by_object_vnum);
  RUN_TEST(test_late_headline_does_not_write_past_caller_buffer);
  RUN_TEST(test_long_headline_is_truncated_and_reported);
  RUN_TEST(test_remove_first_of_two_then_clear);
  RUN_TEST(test_remove_last_then_clear);
  RUN_TEST(test_load_ignores_on_disk_pointer_and_slot);
  RUN_TEST(test_load_short_heading_read);
  RUN_TEST(test_load_rejects_oversized_lengths);
  RUN_TEST(test_load_rejects_unterminated_heading);
  RUN_TEST(test_save_load_round_trip);
  return UNITY_END();
}
