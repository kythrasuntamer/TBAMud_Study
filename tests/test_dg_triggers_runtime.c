/* Executable tests of the production trigger helpers (dg_triggers.c) that
 * the narrower test_dg_memory_runtime fixture does not reach, linked against
 * the real server objects (dg_world_fixture.h).
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "dg_world_fixture.h"

void setUp(void) { dgw_setup(); }
void tearDown(void) { dgw_teardown(); }

/* E1-17: the unquoted branch of one_phrase() ignored first_arg_size. A word
 * longer than the buffer overflowed it (heap-buffer-overflow under ASan
 * with this exact-size heap buffer); now it is truncated, terminated,
 * consumed and reported. */
static void test_one_phrase_bounds_unquoted_words(void)
{
  char *arg = strdup("abcdefghijklmnop next");
  char *phrase = malloc(8);
  char *rest;

  TEST_ASSERT_NOT_NULL(arg);
  TEST_ASSERT_NOT_NULL(phrase);
  rest = one_phrase(arg, phrase, 8);
  TEST_ASSERT_EQUAL_STRING("abcdefg", phrase);
  TEST_ASSERT_EQUAL_STRING(" next", rest);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("phrase-truncated"));

  rest = one_phrase(rest, phrase, 8);
  TEST_ASSERT_EQUAL_STRING("next", phrase);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("phrase-truncated"));
  free(phrase);
  free(arg);
}

static void test_one_phrase_bounds_quoted_phrases(void)
{
  char *arg = strdup("\"open sesame now\" tail");
  char phrase[6];

  TEST_ASSERT_NOT_NULL(arg);
  TEST_ASSERT_EQUAL_STRING(" tail", one_phrase(arg, phrase, sizeof(phrase)));
  TEST_ASSERT_EQUAL_STRING("open ", phrase);
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("phrase-truncated"));
  free(arg);
}

/* E1-17: a speech trigger whose word list is longer than word_check()
 * searches is reported, once, naming the trigger. */
static void test_long_speech_wordlist_is_reported_once(void)
{
  struct char_data *m = dgw_mob(DGW_MOB_GUARD, 0);
  struct char_data *speaker = dgw_mob(DGW_MOB_OGRE, 0);
  trig_data *t;
  char *words = malloc(MAX_INPUT_LENGTH + 40);

  TEST_ASSERT_NOT_NULL(words);
  memset(words, 'w', MAX_INPUT_LENGTH + 30);
  memcpy(words, "hello ", 6);
  words[MAX_INPUT_LENGTH + 30] = '\0';

  SCRIPT(m) = dgw_script_with(MOB_TRIGGER, MTRIG_SPEECH, NULL);
  t = TRIGGERS(SCRIPT(m));
  t->arglist = words;

  speech_mtrigger(speaker, "well hello there");
  speech_mtrigger(speaker, "nothing here");
  TEST_ASSERT_EQUAL_INT(1, dgw_saw("wordlist-truncated"));
  TEST_ASSERT_NOT_NULL(strstr(dgw_detail("wordlist-truncated"), "trigger 400"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_one_phrase_bounds_unquoted_words);
  RUN_TEST(test_one_phrase_bounds_quoted_phrases);
  RUN_TEST(test_long_speech_wordlist_is_reported_once);
  return UNITY_END();
}
