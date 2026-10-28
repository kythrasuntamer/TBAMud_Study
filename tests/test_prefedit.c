/* Executable tests of the production preference editor (prefedit.c): an
 * editor never keeps a pointer to a character that has been freed.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "oasis.h"
#include "prefedit.h"

static int cleanups;
static struct descriptor_data *cleaned;
void cleanup_olc(struct descriptor_data *d, byte type)
{
  (void)type;
  cleanups++;
  cleaned = d;
}

static struct char_data imp, victim;
static struct descriptor_data imp_desc, own_desc;
static struct oasis_olc_data imp_olc, own_olc;
static struct prefs_data imp_prefs, own_prefs;

void setUp(void)
{
  memset(&imp, 0, sizeof(imp));
  memset(&victim, 0, sizeof(victim));
  imp.player.name = "Imp";
  victim.player.name = "Victim";
  GET_IDNUM(&imp) = 101;
  GET_IDNUM(&victim) = 202;
  memset(&imp_desc, 0, sizeof(imp_desc));
  memset(&own_desc, 0, sizeof(own_desc));
  imp_desc.olc = &imp_olc;
  own_desc.olc = &own_olc;
  imp_olc.prefs = &imp_prefs;
  own_olc.prefs = &own_prefs;
  imp_desc.character = &imp;
  own_desc.character = &victim;
  STATE(&imp_desc) = STATE(&own_desc) = CON_PREFEDIT;
  imp_prefs.idnum = GET_IDNUM(&victim);  /* the implementor edits Victim */
  own_prefs.idnum = GET_IDNUM(&victim);  /* and Victim edits themself */
  imp_desc.next = &own_desc;
  descriptor_list = &imp_desc;
  cleanups = 0;
  cleaned = NULL;
}
void tearDown(void) { descriptor_list = NULL; }

/* Saving after the victim left wrote through a freed character. */
static void editors_of_a_freed_character_are_abandoned(void)
{
  prefedit_forget_char(&victim);
  TEST_ASSERT_EQUAL_INT(1, cleanups);
  TEST_ASSERT_EQUAL_PTR(&imp_desc, cleaned);   /* not Victim's own session */
}

static void other_characters_leave_editors_alone(void)
{
  prefedit_forget_char(&imp);
  TEST_ASSERT_EQUAL_INT(0, cleanups);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(editors_of_a_freed_character_are_abandoned);
  RUN_TEST(other_characters_leave_editors_alone);
  return UNITY_END();
}
