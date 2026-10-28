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
  return UNITY_END();
}
