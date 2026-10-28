/**
 * @file test_oasis_list.c
 * Regression tests for the OLC listing commands in src/oasis_list.c.
 *
 * The listings are reached only through do_oasis_list(), so each test builds
 * a tiny prototype table (obj_proto / obj_index / mob_proto / zone_table),
 * runs the command as a builder, and inspects what was paged to the builder
 * and what was written to the log.  Diagnostics are asserted on their stable
 * op= token (e.g. "op=list-truncated"), not on free text.
 *
 * Every fixture is heap-allocated with the exact number of entries the real
 * boot code would allocate (top_of_* + 1), so an off-by-one index into a
 * prototype table is an ASan heap-buffer-overflow / Valgrind invalid read.
 */

#include "unity.h" /* before utils.h: Unity pulls in math.h, utils.h defines log */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "oasis.h"
#include "shop.h"
#include "dg_scripts.h"
#include "quest.h"
#include "spells.h"
#include "constants.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Globals oasis_list.c needs that test_stubs.c does not provide.
 * --------------------------------------------------------------------- */
struct obj_data   *obj_proto         = NULL;
struct char_data  *mob_proto         = NULL;
struct zone_data  *zone_table        = NULL;
zone_rnum          top_of_zone_table = 0;
struct shop_data  *shop_index        = NULL;
int                top_shop          = -1;
struct index_data **trig_index       = NULL;
int                top_of_trigt      = 0;

/* ---------------------------------------------------------------------
 * Output capture.
 * --------------------------------------------------------------------- */
static char paged[MAX_STRING_LENGTH + 1];
static int  paged_calls;
static char sent[8192];

void page_string(struct descriptor_data *d, char *str, int keep_internal)
{
  (void)d; (void)keep_internal;
  paged_calls++;
  strlcpy(paged, str ? str : "", sizeof(paged));
}

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;
  size_t used = strlen(sent);
  (void)ch;
  va_start(args, messg);
  vsnprintf(sent + used, sizeof(sent) - used, messg, args);
  va_end(args);
  return strlen(sent) - used;
}

/* ---------------------------------------------------------------------
 * Minimal real implementations of interpreter/db helpers.
 * --------------------------------------------------------------------- */
int is_abbrev(const char *arg1, const char *arg2)
{
  if (!*arg1)
    return 0;
  for (; *arg1 && *arg2; arg1++, arg2++)
    if (LOWER(*arg1) != LOWER(*arg2))
      return 0;
  return !*arg1;
}

void skip_spaces(char **string)
{
  while (**string && isspace((unsigned char)**string))
    (*string)++;
}

char *one_argument(char *argument, char *first_arg)
{
  skip_spaces(&argument);
  while (*argument && !isspace((unsigned char)*argument))
    *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}

char *two_arguments(char *argument, char *first_arg, char *second_arg)
{
  return one_argument(one_argument(argument, first_arg), second_arg);
}

int is_name(const char *str, const char *namelist)
{
  return namelist && strstr(namelist, str) != NULL;
}

obj_rnum real_object(obj_vnum vnum)
{
  obj_rnum i;
  if (!obj_index)
    return NOTHING;
  for (i = 0; i <= top_of_objt; i++)
    if (obj_index[i].vnum == vnum)
      return i;
  return NOTHING;
}

zone_rnum real_zone(zone_vnum vnum)
{
  zone_rnum i;
  if (!zone_table)
    return NOWHERE;
  for (i = 0; i <= top_of_zone_table; i++)
    if (zone_table[i].number == vnum)
      return i;
  return NOWHERE;
}

static int read_mobile_calls;
struct char_data *read_mobile(mob_vnum nr, int type)
{
  (void)nr; (void)type;
  read_mobile_calls++;
  return NULL;
}

const char *skill_name(int num) { (void)num; return "skill"; }
int count_quests(qst_vnum low, qst_vnum high) { (void)low; (void)high; return 0; }
void list_quests(struct char_data *ch, zone_rnum zone, qst_vnum vmin, qst_vnum vmax)
{ (void)ch; (void)zone; (void)vmin; (void)vmax; }

/* ---------------------------------------------------------------------
 * Fixtures.
 * --------------------------------------------------------------------- */
static struct char_data builder;
static struct player_special_data builder_specials;
static struct descriptor_data builder_desc;
static FILE *capture_log;

static void free_objects(void)
{
  obj_rnum i;
  if (obj_proto) {
    for (i = 0; i <= top_of_objt; i++)
      free(obj_proto[i].short_description);
  }
  free(obj_proto);
  free(obj_index);
  obj_proto = NULL;
  obj_index = NULL;
  top_of_objt = 0;
}

/* Allocate exactly count prototypes (vnums 100..), as index_boot does. */
static void make_objects(int count, const char *short_desc)
{
  int i;
  free_objects();
  CREATE(obj_proto, struct obj_data, count);
  CREATE(obj_index, struct index_data, count);
  top_of_objt = count - 1;
  for (i = 0; i < count; i++) {
    obj_index[i].vnum = 100 + i;
    obj_proto[i].item_number = i;
    obj_proto[i].short_description = strdup(short_desc);
    obj_proto[i].obj_flags.type_flag = ITEM_TRASH;
  }
}

static void free_rooms(void)
{
  free(world);
  world = NULL;
  top_of_world = 0;
}

/* count rooms (vnums 300..) named name, all in zone rnum 0, no exits. */
static void make_rooms(int count, const char *name)
{
  int i;
  free_rooms();
  CREATE(world, struct room_data, count);
  top_of_world = count - 1;
  for (i = 0; i < count; i++) {
    world[i].number = 300 + i;
    world[i].zone = 0;
    world[i].name = (char *)name;
  }
}

static void free_mobiles(void)
{
  free(mob_proto);
  free(mob_index);
  mob_proto = NULL;
  mob_index = NULL;
  top_of_mobt = 0;
}

/* Allocate exactly count mob prototypes (vnums 200..), as index_boot does.
 * short_descr points at a string literal; nothing here frees it. */
static void make_mobiles(int count, const char *short_descr, int level)
{
  int i;
  free_mobiles();
  CREATE(mob_proto, struct char_data, count);
  CREATE(mob_index, struct index_data, count);
  top_of_mobt = count - 1;
  for (i = 0; i < count; i++) {
    mob_index[i].vnum = 200 + i;
    mob_proto[i].nr = i;
    mob_proto[i].player.short_descr = (char *)short_descr;
    mob_proto[i].player.level = level;
  }
}

static void run_list(int subcmd, const char *args)
{
  char argbuf[MAX_INPUT_LENGTH];
  strlcpy(argbuf, args, sizeof(argbuf));
  do_oasis_list(&builder, argbuf, 0, subcmd);
}

static int log_has(const char *needle)
{
  char line[MAX_STRING_LENGTH];
  int found = 0;
  fflush(capture_log);
  rewind(capture_log);
  while (fgets(line, sizeof(line), capture_log))
    if (strstr(line, needle))
      found = 1;
  fseek(capture_log, 0, SEEK_END);
  return found;
}

void setUp(void)
{
  memset(&builder, 0, sizeof(builder));
  memset(&builder_specials, 0, sizeof(builder_specials));
  memset(&builder_desc, 0, sizeof(builder_desc));
  builder.player_specials = &builder_specials;
  builder.player.name = (char *)"Builder";
  builder.player.level = LVL_IMPL;
  builder.in_room = NOWHERE;
  builder.desc = &builder_desc;
  builder_desc.character = &builder;

  paged[0] = '\0';
  sent[0] = '\0';
  paged_calls = 0;
  read_mobile_calls = 0;

  capture_log = tmpfile();
  logfile = capture_log;
}

void tearDown(void)
{
  free_objects();
  free_mobiles();
  free_rooms();
  logfile = NULL;
  if (capture_log)
    fclose(capture_log);
  capture_log = NULL;
}

/* ---------------------------------------------------------------------
 * F3-02: olist affect 7/8 and the generic affect rows read
 * obj_proto[top_of_objt + 1] (the stale scan index) instead of the row's
 * rnum.  The fixture table is exactly top_of_objt + 1 long, so the old code
 * is a heap-buffer-overflow under ASan.
 * --------------------------------------------------------------------- */
void test_obj_aff_list_weapon_special_case_uses_row_rnum(void)
{
  make_objects(3, "a plain rock");
  free(obj_proto[1].short_description);
  obj_proto[1].short_description = strdup("a sharp sword");
  obj_proto[1].obj_flags.type_flag = ITEM_WEAPON;
  obj_proto[1].obj_flags.value[1] = 2;
  obj_proto[1].obj_flags.value[2] = 4;

  run_list(SCMD_OASIS_OLIST, "affect 7");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "a sharp sword"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "[WEAPON]"));
}

void test_obj_aff_list_generic_affect_uses_row_rnum(void)
{
  make_objects(3, "a dagger");
  obj_proto[2].affected[0].location = APPLY_STR;
  obj_proto[2].affected[0].modifier = 2;

  run_list(SCMD_OASIS_OLIST, "affect 1");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "a dagger"));
  TEST_ASSERT_FALSE(log_has("op=list-index"));
}

/* ---------------------------------------------------------------------
 * F3-01: 100 rows of ~560 bytes overflowed the 49152-byte stack buffer in
 * olist affect (unbounded len += snprintf).  Now the list stops at the last
 * whole row, shows a trailer, and logs op=list-truncated.
 * --------------------------------------------------------------------- */
void test_obj_aff_list_truncates_loudly_instead_of_overflowing(void)
{
  char longdesc[501];
  int i;

  memset(longdesc, 'x', sizeof(longdesc) - 1);
  longdesc[sizeof(longdesc) - 1] = '\0';
  make_objects(100, longdesc);
  for (i = 0; i <= top_of_objt; i++) {
    obj_proto[i].affected[0].location = APPLY_STR;
    obj_proto[i].affected[0].modifier = 1 + i;
  }

  run_list(SCMD_OASIS_OLIST, "affect 1");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_TRUE(strlen(paged) < MAX_STRING_LENGTH);
  TEST_ASSERT_NOT_NULL(strstr(paged, "*** list truncated ***"));
  TEST_ASSERT_TRUE(log_has("op=list-truncated"));
  TEST_ASSERT_TRUE(log_has("resource=olist-affect"));
}

void test_obj_aff_list_short_list_is_not_reported_truncated(void)
{
  make_objects(3, "a dagger");
  obj_proto[1].affected[0].location = APPLY_STR;
  obj_proto[1].affected[0].modifier = 2;

  run_list(SCMD_OASIS_OLIST, "affect 1");

  TEST_ASSERT_NOT_NULL(strstr(paged, "a dagger"));
  TEST_ASSERT_NULL(strstr(paged, "*** list truncated ***"));
  TEST_ASSERT_FALSE(log_has("op=list-truncated"));
}

/* ---------------------------------------------------------------------
 * F3-03: olist type <n> indexed item_types[] with an unchecked builder
 * number, and valid-but-unhandled types (FURNITURE, FREE, FREE2) returned
 * from inside the loop, discarding the listing.
 * --------------------------------------------------------------------- */
void test_obj_type_list_rejects_out_of_range_type_loudly(void)
{
  make_objects(2, "a chair");

  run_list(SCMD_OASIS_OLIST, "type 100000");

  TEST_ASSERT_EQUAL_INT(0, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(sent, "Invalid item type"));
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
  TEST_ASSERT_TRUE(log_has("resource=olist-type"));
}

void test_obj_type_list_rejects_negative_and_non_numeric_type(void)
{
  make_objects(2, "a chair");

  run_list(SCMD_OASIS_OLIST, "type -1");
  TEST_ASSERT_EQUAL_INT(0, paged_calls);
  run_list(SCMD_OASIS_OLIST, "type 3x");
  TEST_ASSERT_EQUAL_INT(0, paged_calls);
  TEST_ASSERT_TRUE(log_has("op=arg-range"));
}

void test_obj_type_list_lists_types_the_switch_does_not_special_case(void)
{
  make_objects(2, "a rocking chair");
  obj_proto[0].obj_flags.type_flag = ITEM_FURNITURE;
  obj_proto[1].obj_flags.type_flag = ITEM_FURNITURE;

  run_list(SCMD_OASIS_OLIST, "type 6");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "[FURNITURE]"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "  2) "));
  TEST_ASSERT_NULL(strstr(sent, "Not a valid item type"));
}

/* ---------------------------------------------------------------------
 * F3-17: mlist level/flags instantiated every match with read_mobile() and
 * parked it in room 0 until the deferred extraction ran.  The listing must
 * come from the prototype table and never create a live mob.
 * --------------------------------------------------------------------- */
void test_mob_level_list_reads_prototypes_without_instantiating(void)
{
  make_mobiles(3, "the town guard", 12);
  mob_proto[1].player.level = 3;

  run_list(SCMD_OASIS_MLIST, "level 12");

  TEST_ASSERT_EQUAL_INT(0, read_mobile_calls);
  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "the town guard"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "200"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "202"));
  TEST_ASSERT_NULL(strstr(paged, "201"));
}

void test_mob_flag_list_reads_prototypes_without_instantiating(void)
{
  make_mobiles(2, "a snarling wolf", 5);
  SET_BIT_AR(mob_proto[1].char_specials.saved.act, MOB_AGGRESSIVE);

  run_list(SCMD_OASIS_MLIST, "flags 5");

  TEST_ASSERT_EQUAL_INT(0, read_mobile_calls);
  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "a snarling wolf"));
  TEST_ASSERT_NOT_NULL(strstr(paged, "201"));
  TEST_ASSERT_NULL(strstr(paged, "200"));
}

/* ---------------------------------------------------------------------
 * F3-15: every list builder stopped at the buffer limit without telling
 * the builder or the log.  A full list now ends in the truncation trailer
 * after the last whole row and logs op=list-truncated naming the command.
 * --------------------------------------------------------------------- */
static char *long_text(char *buf, size_t size)
{
  memset(buf, 'y', size - 1);
  buf[size - 1] = '\0';
  return buf;
}

void test_obj_name_list_reports_truncation(void)
{
  char desc[501];
  int i;

  struct zone_data zone0;

  /* do_oasis_list resolves a lone argument as a zone first; a world always
   * has zone 0, and only then does a non-numeric argument become a name. */
  memset(&zone0, 0, sizeof(zone0));
  zone_table = &zone0;
  top_of_zone_table = 0;

  make_objects(120, long_text(desc, sizeof(desc)));
  for (i = 0; i <= top_of_objt; i++)
    obj_proto[i].name = (char *)"widget";

  run_list(SCMD_OASIS_OLIST, "widget");
  zone_table = NULL;

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_TRUE(strlen(paged) < MAX_STRING_LENGTH);
  TEST_ASSERT_NOT_NULL(strstr(paged, "*** list truncated ***"));
  TEST_ASSERT_TRUE(log_has("op=list-truncated"));
  TEST_ASSERT_TRUE(log_has("resource=olist-name"));
  for (i = 0; i <= top_of_objt; i++)
    obj_proto[i].name = NULL;
}

void test_obj_range_list_reports_truncation(void)
{
  char desc[501];

  make_objects(120, long_text(desc, sizeof(desc)));

  run_list(SCMD_OASIS_OLIST, "100 300");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "*** list truncated ***"));
  TEST_ASSERT_TRUE(log_has("resource=olist "));
}

void test_room_list_truncates_on_a_whole_row(void)
{
  char name[501];
  const char *trailer;

  make_rooms(120, long_text(name, sizeof(name)));

  run_list(SCMD_OASIS_RLIST, "300 500");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  trailer = strstr(paged, "\r\n*** list truncated ***");
  TEST_ASSERT_NOT_NULL(trailer);
  /* The row before the trailer is complete: it ends in its own CRLF. */
  TEST_ASSERT_TRUE(trailer - paged >= 2);
  TEST_ASSERT_EQUAL_MEMORY("\r\n", trailer - 2, 2);
  TEST_ASSERT_TRUE(log_has("resource=rlist "));
}

void test_short_room_list_is_not_reported_truncated(void)
{
  make_rooms(3, "a quiet glade");

  run_list(SCMD_OASIS_RLIST, "300 500");

  TEST_ASSERT_EQUAL_INT(1, paged_calls);
  TEST_ASSERT_NOT_NULL(strstr(paged, "a quiet glade"));
  TEST_ASSERT_NULL(strstr(paged, "*** list truncated ***"));
  TEST_ASSERT_FALSE(log_has("op=list-truncated"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_obj_aff_list_weapon_special_case_uses_row_rnum);
  RUN_TEST(test_obj_aff_list_generic_affect_uses_row_rnum);
  RUN_TEST(test_obj_aff_list_truncates_loudly_instead_of_overflowing);
  RUN_TEST(test_obj_aff_list_short_list_is_not_reported_truncated);
  RUN_TEST(test_obj_type_list_rejects_out_of_range_type_loudly);
  RUN_TEST(test_obj_type_list_rejects_negative_and_non_numeric_type);
  RUN_TEST(test_obj_type_list_lists_types_the_switch_does_not_special_case);
  RUN_TEST(test_mob_level_list_reads_prototypes_without_instantiating);
  RUN_TEST(test_mob_flag_list_reads_prototypes_without_instantiating);
  RUN_TEST(test_obj_name_list_reports_truncation);
  RUN_TEST(test_obj_range_list_reports_truncation);
  RUN_TEST(test_room_list_truncates_on_a_whole_row);
  RUN_TEST(test_short_room_list_is_not_reported_truncated);
  return UNITY_END();
}
