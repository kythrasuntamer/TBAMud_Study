/* Executable tests of the production act()/perform_act() formatter.
 * comm.c is linked with main() renamed and unreferenced sections discarded.
 * The recipient has an act-trigger script and no descriptor, so SENDOK()
 * passes and act() returns the formatted line without network output.
 * That copy is not capitalized; CAP() applies only to descriptor output.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "dg_scripts.h"

#include "protocol.h"
#include "tb_error_capture.h"

/* Fixtures for symbols the formatter reaches outside comm.c (db.c globals
 * are defined here because db.c is not linked). */
struct room_data *world = NULL;
struct player_special_data dummy_mob;
struct index_data *mob_index = NULL; /* recipient diagnostics name mob vnums */
mob_rnum top_of_mobt = 0;
void basic_mud_log(const char *format, ...) { (void)format; }
int room_is_dark(room_rnum room) { (void)room; return 0; }
char *fname(const char *namelist) { return (char *)namelist; }
/* Reached only for descriptor output, which these tests never produce. */
char *CAP(char *txt) { return txt; }
void act_mtrigger(const char_data *ch, char *str, char_data *actor,
                  char_data *victim, obj_data *object, obj_data *target,
                  char *arg)
{ (void)ch; (void)str; (void)actor; (void)victim; (void)object; (void)target; (void)arg; }
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

static char_data viewer;
static struct script_data viewer_script;

void setUp(void)
{
  memset(&viewer, 0, sizeof(viewer));
  memset(&viewer_script, 0, sizeof(viewer_script));
  viewer_script.types = MTRIG_ACT;
  viewer.script = &viewer_script;
  viewer.player_specials = &dummy_mob;
  GET_POS(&viewer) = POS_STANDING;
  tb_error_capture_reset();
}
void tearDown(void) {}

static void plain_message_gets_line_ending(void)
{
  const char *out = act("hello", FALSE, &viewer, NULL, NULL, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQUAL_STRING("hello\r\n", out);
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}
static void text_substitution_is_copied(void)
{
  const char *out = act("says '$T'", FALSE, &viewer, NULL, "hi", TO_CHAR);
  TEST_ASSERT_EQUAL_STRING("says 'hi'\r\n", out);
}
/* A trailing '$' previously advanced past the format's terminator. */
static void trailing_dollar_stops_at_terminator(void)
{
  /* Heap copy so ASan sees any read past the end of the format. */
  char *fmt = strdup("cost 5$");
  TEST_ASSERT_NOT_NULL(fmt);
  const char *out = act(fmt, FALSE, &viewer, NULL, NULL, TO_CHAR);
  free(fmt); /* out is the formatter's own copy */
  TEST_ASSERT_EQUAL_STRING("cost 5\r\n", out);
  /* Reported with the format as the call-site key. */
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format=\"cost 5$\""));
  /* The act() caller's own location is named. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "test_act_format.c:"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, " trailing_dollar_stops_at_terminator"));
}
/* $T text longer than the output buffer previously overflowed the stack. */
static void oversized_substitution_is_truncated(void)
{
  size_t len = MAX_STRING_LENGTH * 2;
  char *huge = malloc(len + 1);
  TEST_ASSERT_NOT_NULL(huge);
  memset(huge, 'x', len);
  huge[len] = '\0';
  const char *out = act("$T tail", FALSE, &viewer, NULL, huge, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  size_t out_len = strlen(out);
  TEST_ASSERT_EQUAL_size_t(MAX_STRING_LENGTH - 1, out_len);
  TEST_ASSERT_EQUAL_STRING("\r\n", out + out_len - 2);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-output", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format=\"$T tail\""));
  free(huge);
}
static void illegal_code_is_reported_escaped(void)
{
  const char *out = act("bad $Z \"quote\"\n", FALSE, &viewer, NULL, NULL, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("format-parse", tb_captured_operation);
  /* Quotes and control characters cannot break the log record. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "format=\"bad $Z ?quote??\""));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "0x5a at offset 5"));
}
static void oversized_literal_is_truncated(void)
{
  size_t len = MAX_STRING_LENGTH * 2;
  char *huge = malloc(len + 1);
  TEST_ASSERT_NOT_NULL(huge);
  memset(huge, 'y', len);
  huge[len] = '\0';
  const char *out = act(huge, FALSE, &viewer, NULL, NULL, TO_CHAR);
  TEST_ASSERT_NOT_NULL(out);
  TEST_ASSERT_EQUAL_size_t(MAX_STRING_LENGTH - 1, strlen(out));
  free(huge);
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  /* The excerpt is bounded and marked as cut. */
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "yyy...\""));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(plain_message_gets_line_ending);
  RUN_TEST(text_substitution_is_copied);
  RUN_TEST(trailing_dollar_stops_at_terminator);
  RUN_TEST(oversized_substitution_is_truncated);
  RUN_TEST(illegal_code_is_reported_escaped);
  RUN_TEST(oversized_literal_is_truncated);
  return UNITY_END();
}
