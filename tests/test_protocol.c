/* Executable tests of the production KaVir protocol layer (protocol.c):
 * ProtocolOutput() escape parsing and ProtocolInput() telnet framing.
 *
 * protocol.c is #included so file-static helpers are reachable; unreferenced
 * sections are discarded at link time. Inputs are heap copies (strdup) of
 * exactly their own length, so ASan reports any read past the terminator.
 */
#include "unity.h" /* before protocol.c: unity.h pulls in <math.h>, whose
                      * log() clashes with utils.h's log macro */
#include "../src/protocol.c"

#include "tb_error_capture.h"

/* Fixtures for symbols the protocol paths reach outside protocol.c. */
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL;
mob_rnum top_of_mobt = 0;
void basic_mud_log(const char *format, ...) { (void)format; }
size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{ (void)t; (void)txt; return 0; }
int write_to_descriptor(socket_t desc, const char *txt)
{ (void)desc; return (int)strlen(txt); }

static struct descriptor_data desc;

void setUp(void)
{
  memset(&desc, 0, sizeof(desc));
  desc.descriptor = 9;
  desc.desc_num = 77;
  desc.pProtocol = ProtocolCreate();
  tb_error_capture_reset();
}
void tearDown(void)
{
  ProtocolDestroy(desc.pProtocol);
}

/* Run ProtocolOutput on an exact-size heap copy of 'text'. */
static const char *translate(const char *text, int *len)
{
  static char out[MAX_OUTPUT_BUFFER + 1];
  char *heap = strdup(text);
  TEST_ASSERT_NOT_NULL(heap);
  *len = (int)strlen(text);
  strlcpy(out, ProtocolOutput(&desc, heap, len), sizeof(out));
  free(heap);
  return out;
}

/* Without UTF-8 the substitute text is shown. It used to be copied from a
 * block-scoped buffer after the block ended (stack-use-after-scope). */
static void unicode_substitute_is_copied_in_scope(void)
{
  int len;
  const char *out = translate("x\t[u65/A]y", &len);
  TEST_ASSERT_EQUAL_STRING("xAy", out);
  TEST_ASSERT_EQUAL_INT(3, len);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* Malformed codes are reported with the descriptor identity and an escaped
 * excerpt; the old ReportBug() logged the raw text with no locality. */
static void invalid_colour_is_reported_with_descriptor(void)
{
  int len;
  const char *out = translate("a\t[F9\"9]b", &len);
  TEST_ASSERT_EQUAL_STRING("ab", out);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("malformed-code", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "desc #77 fd 9 no-char -1: "));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "colour \"9?9\" invalid"));
}

/* An over-long code point overflowed int (UBSan abort); it is now bounded,
 * reported, and the substitute text is shown instead. */
static void huge_unicode_code_point_is_rejected(void)
{
  int len;
  const char *out = translate("x\t[u99999999999/A]y", &len);
  TEST_ASSERT_EQUAL_STRING("xAy", out);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("unicode-code", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "(11 digits)"));
}

/* Output that translates past MAX_OUTPUT_BUFFER used to be dropped whole. */
static void oversized_output_is_cut_with_marker(void)
{
  static char big[30001];
  const char *out;
  int len;

  memset(big, 'a', 30000);
  big[30000] = '\0';
  out = translate(big, &len);
  TEST_ASSERT_EQUAL_INT(MAX_OUTPUT_BUFFER - 1, len);
  TEST_ASSERT_EQUAL_size_t((size_t)len, strlen(out));
  TEST_ASSERT_EQUAL_CHAR('a', out[0]);
  TEST_ASSERT_EQUAL_STRING("\r\n**OVERFLOW**\r\n", out + len - 16);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("output-overflow", tb_captured_operation);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(unicode_substitute_is_copied_in_scope);
  RUN_TEST(invalid_colour_is_reported_with_descriptor);
  RUN_TEST(huge_unicode_code_point_is_rejected);
  RUN_TEST(oversized_output_is_cut_with_marker);
  return UNITY_END();
}
