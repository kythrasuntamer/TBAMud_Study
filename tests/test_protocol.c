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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(unicode_substitute_is_copied_in_scope);
  return UNITY_END();
}
