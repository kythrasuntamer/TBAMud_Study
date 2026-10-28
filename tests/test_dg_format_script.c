/* Executable tests of trigedit's script formatter (dg_olc.c format_script):
 * deep nesting used to run the indentation width past the line buffer and
 * the block stack past its 256 entries.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "dg_scripts.h"
#include "dg_olc.h"

/* interpreter.c is not linked: its command table would pull in all of OLC. */
void skip_spaces(char **string)
{
  for (; **string && isspace((unsigned char)**string); (*string)++);
}

static char said[MAX_STRING_LENGTH];
size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  int n;
  (void)d;
  va_start(args, txt);
  n = vsnprintf(said, sizeof(said), txt, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}

static struct descriptor_data desc;
static struct char_data editor;
static char *text;

void setUp(void)
{
  memset(&desc, 0, sizeof(desc));
  /* An OLC descriptor always has a character; refusals name it. */
  memset(&editor, 0, sizeof(editor));
  desc.character = &editor;
  *said = '\0';
  text = NULL;
  desc.str = &text;
  desc.max_str = MAX_CMD_LENGTH;
}
void tearDown(void) { free(text); }

/* Builds n nested "if 1" lines and their "end"s. */
static char *nested_ifs(int n)
{
  size_t size = (size_t)n * 12 + 1;
  char *s = calloc(1, size);
  int i;

  TEST_ASSERT_NOT_NULL(s);
  for (i = 0; i < n; i++)
    strlcat(s, "if 1\r\n", size);
  for (i = 0; i < n; i++)
    strlcat(s, "end\r\n", size);
  return s;
}

static void ordinary_script_is_indented(void)
{
  text = strdup("if 1\r\nsay hi\r\nend\r\n");
  TEST_ASSERT_TRUE(format_script(&desc));
  TEST_ASSERT_EQUAL_STRING("if 1\r\n  say hi\r\nend\r\n", text);
}

/* 200 levels need 400 bytes of indent per line; the indented script no
 * longer fits the result buffer, so formatting is refused (whole lines
 * are written straight into it, with no per-line cap). */
static void deep_indent_is_refused_not_overflowed(void)
{
  text = nested_ifs(200);
  desc.max_str = 100000;
  TEST_ASSERT_FALSE(format_script(&desc));
  TEST_ASSERT_NOT_NULL(strstr(said, "formatting aborted"));
  /* Refused formatting leaves the text as it was. */
  TEST_ASSERT_EQUAL_INT(0, strncmp(text, "if 1\r\nif 1\r\n", 12));
}

/* More openers than the block stack holds. */
static void too_many_blocks_are_refused(void)
{
  char *s = calloc(1, 300 * 6 + 1);
  int i;

  TEST_ASSERT_NOT_NULL(s);
  for (i = 0; i < 300; i++)
    strlcat(s, "if 1\n", 300 * 6 + 1);
  text = s;
  desc.max_str = 100000;
  TEST_ASSERT_FALSE(format_script(&desc));
  TEST_ASSERT_TRUE(*said != '\0');
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(ordinary_script_is_indented);
  RUN_TEST(deep_indent_is_refused_not_overflowed);
  RUN_TEST(too_many_blocks_are_refused);
  return UNITY_END();
}
