/* Executable tests of the production improved editor (improved-edit.c).
 * Edit buffers can hold world-file text loaded by fread_string(), which need
 * not end in a newline and can end in a lone '\t' colour introducer. Inputs
 * are exact-size heap copies so ASan sees any access past the terminator.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "interpreter.h"
#include "improved-edit.h"
#include "modify.h"
#include <stdarg.h>

/* utils.c owns tb_error_log_at(); the link wraps it (--wrap) so the tests can
 * assert which structured diagnostic the editor emitted. */
static int captured_errors;
static char captured_operation[64];
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func; (void)detail_fmt;
  captured_errors++;
  strlcpy(captured_operation, operation ? operation : "",
          sizeof(captured_operation));
}

static char output[MAX_STRING_LENGTH];

size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{
  va_list args;
  size_t used = strlen(output);
  (void)t;
  va_start(args, txt);
  if (vsnprintf(output + used, sizeof(output) - used, txt, args) < 0)
    output[used] = '\0';
  va_end(args);
  return strlen(output);
}

static struct descriptor_data d;
static char *text;

void setUp(void)
{
  memset(&d, 0, sizeof(d));
  d.desc_num = 3;
  d.max_str = MAX_STRING_LENGTH;
  d.str = &text;
  text = NULL;
  output[0] = '\0';
  captured_errors = 0;
  captured_operation[0] = '\0';
}

void tearDown(void)
{
  free(text);
  text = NULL;
}

static int format_heap(const char *src)
{
  text = strdup(src);
  TEST_ASSERT_NOT_NULL(text);
  return format_text(&text, 0, &d, (unsigned int)d.max_str, 1, 999999);
}

/* Finding A2-V02: a lone trailing '\t' made the word scan step past the
 * terminator, then read and write bytes outside the buffer. */
void test_format_drops_dangling_trailing_colour_code(void)
{
  TEST_ASSERT_EQUAL_INT(1, format_heap("Aroom\t"));
  TEST_ASSERT_EQUAL_STRING("Aroom\r\n", text);
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-color", captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(output, "colour code"));
}

/* strchr(".!?", '\0') matches the terminator: text with no final newline
 * used to step past it in the sentence-delimiter loop. */
void test_format_text_without_trailing_newline(void)
{
  TEST_ASSERT_EQUAL_INT(1, format_heap("a quiet room"));
  TEST_ASSERT_EQUAL_STRING("A quiet room\r\n", text);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

void test_format_sentence_end_without_trailing_newline(void)
{
  TEST_ASSERT_EQUAL_INT(1, format_heap("one. two!"));
  TEST_ASSERT_EQUAL_STRING("One.  Two!\r\n", text);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

void test_format_keeps_complete_colour_codes(void)
{
  TEST_ASSERT_EQUAL_INT(1, format_heap("a \tRred\tn room.\r\n"));
  TEST_ASSERT_EQUAL_STRING("A \tRred\tn room.\r\n", text);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

/* Finding A2-03: a bare "/" has no command letter; actions used to be copied
 * from str + 2, past the line's terminator. An exact-size heap copy makes
 * that read visible to ASan. */
void test_bare_slash_is_rejected_without_reading_past_it(void)
{
  char *line = strdup("/");

  TEST_ASSERT_NOT_NULL(line);
  TEST_ASSERT_EQUAL_INT(STRINGADD_ACTION, improved_editor_execute(&d, line));
  TEST_ASSERT_EQUAL_STRING("Invalid option.\r\n", output);
  TEST_ASSERT_EQUAL_STRING("", line);
  free(line);
}

void test_save_command_still_parses(void)
{
  char *line = strdup("/s");

  TEST_ASSERT_NOT_NULL(line);
  TEST_ASSERT_EQUAL_INT(STRINGADD_SAVE, improved_editor_execute(&d, line));
  free(line);
}

/* replace_str(): an overflowing "/ra" used to leave the buffer cut at the
 * failing match while the player was told the replace was aborted. */
void test_replace_all_overflow_leaves_buffer_unchanged(void)
{
  text = strdup("aXaXaX");
  TEST_ASSERT_NOT_NULL(text);

  TEST_ASSERT_EQUAL_INT(-1, replace_str(&text, (char *)"X", (char *)"YYYYY", TRUE, 12));
  TEST_ASSERT_EQUAL_STRING("aXaXaX", text);
}

void test_replace_all_exact_fit_keeps_every_byte(void)
{
  /* 3 + 3*3 = 12 bytes of text + NUL = 13 = max_size. */
  text = strdup("aXaXaX");
  TEST_ASSERT_NOT_NULL(text);

  TEST_ASSERT_EQUAL_INT(3, replace_str(&text, (char *)"X", (char *)"YYY", TRUE, 13));
  TEST_ASSERT_EQUAL_STRING("aYYYaYYYaYYY", text);
}

void test_replace_first_only_and_shrink(void)
{
  text = strdup("one two one");
  TEST_ASSERT_NOT_NULL(text);

  TEST_ASSERT_EQUAL_INT(1, replace_str(&text, (char *)"one", (char *)"1", FALSE, 64));
  TEST_ASSERT_EQUAL_STRING("1 two one", text);
  TEST_ASSERT_EQUAL_INT(0, replace_str(&text, (char *)"three", (char *)"3", TRUE, 64));
  TEST_ASSERT_EQUAL_STRING("1 two one", text);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_format_drops_dangling_trailing_colour_code);
  RUN_TEST(test_format_text_without_trailing_newline);
  RUN_TEST(test_format_sentence_end_without_trailing_newline);
  RUN_TEST(test_format_keeps_complete_colour_codes);
  RUN_TEST(test_bare_slash_is_rejected_without_reading_past_it);
  RUN_TEST(test_save_command_still_parses);
  RUN_TEST(test_replace_all_overflow_leaves_buffer_unchanged);
  RUN_TEST(test_replace_all_exact_fit_keeps_every_byte);
  RUN_TEST(test_replace_first_only_and_shrink);
  return UNITY_END();
}
