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

/* "/i" and "/e" split "<line> <text>" with half_chop(); the shared weak
 * stub (test_stubs.c) blanks both halves. This mirrors interpreter.c's: the
 * first word, then the rest after the separating spaces. */
void half_chop(char *string, char *arg1, char *arg2)
{
  while (isspace((unsigned char)*string))
    string++;
  while (*string && !isspace((unsigned char)*string))
    *arg1++ = *string++;
  *arg1 = '\0';
  while (isspace((unsigned char)*string))
    string++;
  memmove(arg2, string, strlen(string) + 1);
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

/* Formatting adds sentence spacing; a result longer than max_str used to be
 * cut to fit, losing the end of the text without a word. */
void test_format_too_long_leaves_text_unchanged(void)
{
  d.max_str = 20;
  text = strdup("one. two. three.\r\n");     /* 18 bytes; formatted is 20 */
  TEST_ASSERT_NOT_NULL(text);

  TEST_ASSERT_EQUAL_INT(0, format_text(&text, 0, &d, (unsigned int)d.max_str, 1, 999999));
  TEST_ASSERT_EQUAL_STRING("one. two. three.\r\n", text);
  TEST_ASSERT_NOT_NULL(strstr(output, "too long"));
}

void test_format_result_that_fits_is_applied(void)
{
  d.max_str = 21;
  text = strdup("one. two. three.\r\n");
  TEST_ASSERT_NOT_NULL(text);

  TEST_ASSERT_EQUAL_INT(1, format_text(&text, 0, &d, (unsigned int)d.max_str, 1, 999999));
  TEST_ASSERT_EQUAL_STRING("One.  Two.  Three.\r\n", text);
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

/* Soak-test finding: "/d1" on a one-line buffer shrank it to "" in a 3-byte
 * block whose spare bytes held stale text, and "/i1" then measured the tail
 * with strlen(s + 1) from the terminator, reading past the block (ASan
 * heap-buffer-overflow). Inserting after the last line of an exact-size
 * buffer read past it the same way. */
void test_delete_last_line_then_insert_stays_in_bounds(void)
{
  char *del = strdup("/d1"), *ins = strdup("/i1 new text"),
       *append = strdup("/i2 second");

  TEST_ASSERT_NOT_NULL(del);
  TEST_ASSERT_NOT_NULL(ins);
  TEST_ASSERT_NOT_NULL(append);
  text = strdup("only line\r\n");
  TEST_ASSERT_NOT_NULL(text);

  improved_editor_execute(&d, del);
  TEST_ASSERT_EQUAL_STRING("", text);
  improved_editor_execute(&d, ins);
  TEST_ASSERT_EQUAL_STRING("new text\r\n", text);

  free(text);
  text = strdup("first\r\n");                /* exact size: no spare bytes */
  TEST_ASSERT_NOT_NULL(text);
  improved_editor_execute(&d, append);
  TEST_ASSERT_EQUAL_STRING("first\r\nsecond\r\n", text);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
  free(append);
  free(ins);
  free(del);
}

/* Ledger observation A2 (oos:improved-edit.c escaped \r\n): the range
 * errors of /f, /d, /l and /n were written as "\\r\\n", so the player saw
 * a literal backslash-r-backslash-n and no line break. */
static void expect_range_message(const char *command, const char *expected)
{
  char line[32];    /* on the stack: a failed assertion must not leak it */

  strlcpy(line, command, sizeof(line));
  output[0] = '\0';
  TEST_ASSERT_EQUAL_INT(STRINGADD_ACTION, improved_editor_execute(&d, line));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, output, command);
}

void test_range_errors_end_with_a_real_line_break(void)
{
  text = strdup("one\r\ntwo\r\n");
  TEST_ASSERT_NOT_NULL(text);

  expect_range_message("/f5-2", "That range is invalid.\r\n");
  expect_range_message("/fx", "Invalid line range.\r\n");
  expect_range_message("/dx", "Invalid line range.\r\n");
  expect_range_message("/lx", "Invalid line range.\r\n");
  expect_range_message("/nx", "Invalid line range.\r\n");
  TEST_ASSERT_EQUAL_STRING("one\r\ntwo\r\n", text);
}

/* help.hlp documents "/d# - deletes line #, or a range #-#". Guard the range
 * form: deleting inside the buffer, past its end, and a reversed range. */
void test_delete_range_removes_those_lines(void)
{
  char line[32];

  text = strdup("one\r\ntwo\r\nthree\r\nfour\r\n");
  TEST_ASSERT_NOT_NULL(text);
  strlcpy(line, "/d2-3", sizeof(line));
  TEST_ASSERT_EQUAL_INT(STRINGADD_ACTION, improved_editor_execute(&d, line));
  TEST_ASSERT_EQUAL_STRING("one\r\nfour\r\n", text);
  TEST_ASSERT_EQUAL_STRING("2 lines deleted.\r\n", output);

  /* A range running past the last line deletes to the end. */
  output[0] = '\0';
  strlcpy(line, "/d2-9", sizeof(line));
  improved_editor_execute(&d, line);
  TEST_ASSERT_EQUAL_STRING("one\r\n", text);
  TEST_ASSERT_EQUAL_STRING("1 line deleted.\r\n", output);

  /* A reversed range is refused out loud and changes nothing. */
  output[0] = '\0';
  strlcpy(line, "/d3-1", sizeof(line));
  improved_editor_execute(&d, line);
  TEST_ASSERT_EQUAL_STRING("one\r\n", text);
  TEST_ASSERT_EQUAL_STRING("That range is invalid.\r\n", output);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_format_drops_dangling_trailing_colour_code);
  RUN_TEST(test_format_text_without_trailing_newline);
  RUN_TEST(test_format_sentence_end_without_trailing_newline);
  RUN_TEST(test_format_keeps_complete_colour_codes);
  RUN_TEST(test_format_too_long_leaves_text_unchanged);
  RUN_TEST(test_format_result_that_fits_is_applied);
  RUN_TEST(test_bare_slash_is_rejected_without_reading_past_it);
  RUN_TEST(test_save_command_still_parses);
  RUN_TEST(test_replace_all_overflow_leaves_buffer_unchanged);
  RUN_TEST(test_replace_all_exact_fit_keeps_every_byte);
  RUN_TEST(test_replace_first_only_and_shrink);
  RUN_TEST(test_delete_last_line_then_insert_stays_in_bounds);
  RUN_TEST(test_range_errors_end_with_a_real_line_break);
  RUN_TEST(test_delete_range_removes_those_lines);
  return UNITY_END();
}
