/* Executable tests of the production pager in modify.c: page_string(),
 * count_pages()/next_page() and show_string(). Pager text comes from world,
 * help and lib files, so a colour introducer can be its last byte; the scan
 * must stop at the terminator and report it. Inputs are heap copies so ASan
 * sees any over-read.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "modify.h"
#include <stdarg.h>
#include "tb_error_capture.h"

static char sent[MAX_STRING_LENGTH * 2];
static size_t sent_len;
static int sends;

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  int n;
  (void)ch;
  va_start(args, messg);
  n = vsnprintf(sent, sizeof(sent), messg, args);
  va_end(args);
  sent_len = n < 0 ? 0 : (size_t)n;
  sends++;
  return sent_len;
}
void basic_mud_log(const char *format, ...) { (void)format; }
/* show_string() only needs the first word; the tests page with "". */
char *any_one_arg(char *argument, char *first_arg)
{
  *first_arg = '\0';
  return argument;
}

static struct descriptor_data d;
static struct char_data ch;
static struct player_special_data ps;

void setUp(void)
{
  memset(&d, 0, sizeof(d));
  memset(&ch, 0, sizeof(ch));
  memset(&ps, 0, sizeof(ps));
  ch.player_specials = &ps;
  ch.desc = &d;
  GET_IDNUM(&ch) = 42;
  GET_PAGE_LENGTH(&ch) = 22;
  GET_SCREEN_WIDTH(&ch) = 80;
  d.character = &ch;
  d.desc_num = 7;
  sends = 0;
  sent_len = 0;
  sent[0] = '\0';
  tb_error_capture_reset();
}

void tearDown(void)
{
  if (d.showstr_count)
    free(d.showstr_vector);
  free(d.showstr_head);
  d.showstr_vector = NULL;
  d.showstr_head = NULL;
  d.showstr_count = 0;
}

static void page_heap(const char *text)
{
  char *copy = strdup(text);

  TEST_ASSERT_NOT_NULL(copy);
  page_string(&d, copy, TRUE);
  free(copy);
}

/* Finding A2-V01: fread_string() turns a trailing '@' into '\t'. */
void test_trailing_tab_colour_code_stops_at_terminator(void)
{
  page_heap("A room.\t");

  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("WARNING", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("color-scan", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "player idnum 42"));
}

/* Finding A2-02: an ESC with no closing 'm' before the terminator. */
void test_unterminated_ansi_escape_stops_at_terminator(void)
{
  page_heap("A room.\x1B");

  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("ansi-scan", tb_captured_operation);
}

void test_unterminated_ansi_escape_with_parameters(void)
{
  page_heap("A room.\x1B[1;3");

  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("ansi-scan", tb_captured_operation);
}

/* Well-formed colour codes are zero-width and report nothing. */
void test_well_formed_colour_codes_are_silent(void)
{
  page_heap("\tRred\tn \t\ttab \x1B[0m plain\r\n");

  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* A trailing escaped tab ("\t\t") is complete, not dangling. */
void test_trailing_escaped_tab_is_complete(void)
{
  page_heap("text\t\t");

  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Finding A2-08: a page larger than show_string()'s buffer is clamped; the
 * lost bytes must be reported. One page of colour codes (zero-width) plus a
 * short second page. */
void test_oversized_page_is_reported(void)
{
  size_t codes = MAX_STRING_LENGTH / 2 + 16, i;
  size_t len = codes * 2 + 512;
  char *text = malloc(len), *p = text;

  TEST_ASSERT_NOT_NULL(text);
  for (i = 0; i < codes; i++) {
    *p++ = '\t';
    *p++ = 'n';
  }
  for (i = 0; i < 25; i++) {   /* page length 22: forces a second page */
    *p++ = 'x';
    *p++ = '\n';
  }
  *p = '\0';

  page_string(&d, text, TRUE);
  free(text);

  TEST_ASSERT_EQUAL_INT(2, d.showstr_count);
  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_size_t(MAX_STRING_LENGTH - 1, sent_len);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("page-truncate", tb_captured_operation);
}

void test_normal_page_is_silent(void)
{
  char text[64 * 3 + 1];
  int i;

  for (i = 0; i < 64; i++)
    memcpy(text + i * 3, "x\r\n", 3);
  text[64 * 3] = '\0';
  page_string(&d, text, TRUE);

  TEST_ASSERT_TRUE(d.showstr_count > 1);
  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_trailing_tab_colour_code_stops_at_terminator);
  RUN_TEST(test_unterminated_ansi_escape_stops_at_terminator);
  RUN_TEST(test_unterminated_ansi_escape_with_parameters);
  RUN_TEST(test_well_formed_colour_codes_are_silent);
  RUN_TEST(test_trailing_escaped_tab_is_complete);
  RUN_TEST(test_oversized_page_is_reported);
  RUN_TEST(test_normal_page_is_silent);
  return UNITY_END();
}
