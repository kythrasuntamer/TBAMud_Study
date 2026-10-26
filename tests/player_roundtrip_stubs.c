/* Dependencies for the isolated players.c serialization test.
 * Linked only in test_player_roundtrip, never in the actual game. */
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "dg_scripts.h"
#include "config.h"
#include "genolc.h"
#include "quest.h"

int top_of_p_table = -1;
int top_of_p_file = -1;
long top_idnum = 0;
struct pclean_criteria_data pclean_criteria[LVL_IMMORT + 1];
struct index_data **trig_index = NULL;

bitvector_t asciiflag_conv(char *flag) { (void)flag; return 0; }
int sprintascii(char *out, bitvector_t bits)
{
  (void)bits;
  out[0] = '0';
  out[1] = '\0';
  return 1;
}
char *fread_string(FILE *file, const char *error)
{
  (void)file;
  (void)error;
  return NULL;
}
trig_rnum real_trigger(trig_vnum vnum)
{
  (void)vnum;
  return NOTHING;
}
trig_data *read_trigger(int nr) { (void)nr; return NULL; }
void add_trigger(struct script_data *sc, trig_data *t, int loc)
{
  (void)sc; (void)t; (void)loc;
}
void read_saved_vars_ascii(FILE *file, struct char_data *ch, int count)
{
  (void)file; (void)ch; (void)count;
}
void save_char_vars_ascii(FILE *file, struct char_data *ch)
{
  (void)file; (void)ch;
}
void affect_total(struct char_data *ch)
{
  ch->aff_abils = ch->real_abils;
}
void affect_remove(struct char_data *ch, struct affected_type *af)
{
  (void)ch; (void)af;
}
void affect_to_char(struct char_data *ch, struct affected_type *af)
{
  (void)ch; (void)af;
}
struct obj_data *unequip_char(struct char_data *ch, int pos)
{
  (void)ch; (void)pos;
  return NULL;
}
void equip_char(struct char_data *ch, struct obj_data *obj, int pos)
{
  (void)ch; (void)obj; (void)pos;
}
void strip_cr(char *s)
{
  char *out = s;
  while (*s) {
    if (*s != '\r') *out++ = *s;
    ++s;
  }
  *out = '\0';
}
void add_completed_quest(struct char_data *ch, qst_vnum vnum)
{
  (void)ch; (void)vnum;
}
