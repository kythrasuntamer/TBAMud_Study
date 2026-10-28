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
  memset(&imp_desc, 0, sizeof(imp_desc));
  memset(&own_desc, 0, sizeof(own_desc));
  imp_desc.olc = &imp_olc;
  own_desc.olc = &own_olc;
  imp_olc.prefs = &imp_prefs;
  own_olc.prefs = &own_prefs;
  imp_desc.character = &imp;
  own_desc.character = &victim;
  STATE(&imp_desc) = STATE(&own_desc) = CON_PREFEDIT;
  imp_prefs.ch = &victim;               /* the implementor edits Victim */
  own_prefs.ch = &victim;               /* and Victim edits themself */
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

/* do_oasis_prefedit() fixtures; the self-edit path reaches none of the
 * world or logging code these satisfy. */
#define UNREACHED() TEST_FAIL_MESSAGE(__func__)
void basic_mud_log(const char *format, ...) { (void)format; UNREACHED(); }
void mudlog(int type, int level, int file, const char *str, ...)
{ (void)type; (void)level; (void)file; (void)str; UNREACHED(); }
struct char_data *get_player_vis(struct char_data *ch, char *name, int *number, int inroom)
{ (void)ch; (void)name; (void)number; (void)inroom; UNREACHED(); return NULL; }
int room_is_dark(room_rnum room) { (void)room; return 0; }
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  (void)file; (void)line; (void)func;
  void *allocation = calloc(count, element_size);
  if (!allocation) abort();
  return allocation;
}
static int acts, sends;
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void)src_file; (void)src_line; (void)src_func; (void)str;
  (void)hide_invisible; (void)ch; (void)obj; (void)vict_obj; (void)type;
  acts++;
  return NULL;
}
size_t send_to_char(struct char_data *ch, const char *messg, ...)
{ (void)ch; (void)messg; sends++; return 0; }
char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  *first_arg = *second_arg = '\0';
  return argument;
}

/* A player checking their own preferences while an implementor edits them
 * gets the direct message only; act() used to run too, on a buffer that
 * was never written. */
static void own_prefs_being_edited_sends_one_message(void)
{
  static struct player_special_data specials, imp_specials;
  char none[] = "";

  memset(&specials, 0, sizeof(specials));
  memset(&imp_specials, 0, sizeof(imp_specials));
  victim.player_specials = &specials;
  imp.player_specials = &imp_specials;     /* PERS() checks visibility */
  /* Victim is playing on their own connection; only the implementor's
   * session is editing them. */
  static struct descriptor_data playing;
  memset(&playing, 0, sizeof(playing));
  playing.character = &victim;
  STATE(&playing) = CON_PLAYING;
  victim.desc = &playing;
  imp_desc.next = NULL;
  acts = sends = 0;

  do_oasis_prefedit(&victim, none, 0, 0);
  victim.desc = NULL;

  TEST_ASSERT_EQUAL_INT(1, sends);
  TEST_ASSERT_EQUAL_INT(0, acts);
}

/* A character without a connection of its own (a forced mob, a switched
 * body) has no descriptor to hang the editor on; the old code
 * dereferenced ch->desc anyway. */
static void prefedit_without_a_descriptor_does_nothing(void)
{
  char none[] = "";

  victim.desc = NULL;
  acts = sends = 0;
  do_oasis_prefedit(&victim, none, 0, 0);
  TEST_ASSERT_EQUAL_INT(0, sends);
  TEST_ASSERT_EQUAL_INT(0, cleanups);
}

/* Restoring defaults: immortals keep (or gain) nohassle, holylight and
 * show-vnums from the lowest immortal level up, and the "on" defaults are
 * really turned on.  The old code switched the immortal toggles off for an
 * immortal who had them and never turned automap and friends on. */
static void restore_defaults_sets_what_it_documents(void)
{
  int i;

  memset(&imp_prefs, 0, sizeof(imp_prefs));
  imp_prefs.ch = &victim;
  victim.player.level = LVL_IMMORT;
  SET_BIT_AR(imp_prefs.pref_flags, PRF_HOLYLIGHT);   /* already on */

  prefedit_Restore_Defaults(&imp_desc);

  {
    const int on[] = { PRF_NOHASSLE, PRF_HOLYLIGHT, PRF_SHOWVNUMS,
                       PRF_AUTOMAP, PRF_AUTOKEY, PRF_AUTODOOR, PRF_VERBOSE };
    for (i = 0; i < (int)(sizeof(on) / sizeof(*on)); i++)
      TEST_ASSERT_TRUE_MESSAGE(IS_SET_AR(imp_prefs.pref_flags, on[i]), "flag off");
  }

  victim.player.level = LVL_IMMORT - 1;
  prefedit_Restore_Defaults(&imp_desc);
  TEST_ASSERT_FALSE(IS_SET_AR(imp_prefs.pref_flags, PRF_HOLYLIGHT));
  victim.player.level = 0;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(editors_of_a_freed_character_are_abandoned);
  RUN_TEST(other_characters_leave_editors_alone);
  RUN_TEST(own_prefs_being_edited_sends_one_message);
  RUN_TEST(prefedit_without_a_descriptor_does_nothing);
  RUN_TEST(restore_defaults_sets_what_it_documents);
  return UNITY_END();
}
