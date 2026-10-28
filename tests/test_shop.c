/* Executable tests of the production shop buyword evaluator and shop-file
 * readers. shop.c's helpers are file-static, so this test includes shop.c
 * directly (one translation unit) and the link discards every section the
 * tested paths do not reach. Fixtures are disposable.
 */
#include <setjmp.h>
#include <stdlib.h>

/* The shop-file readers stop the boot with abort(). While a test has armed
 * shop_abort_jmp, the abort() calls compiled from shop.c below jump back to
 * it instead, so the fatal report can be asserted. stdlib.h is included
 * first so its own declaration of abort() is not rewritten. */
static jmp_buf shop_abort_jmp;
static int shop_abort_armed;
static void shop_test_abort(void) __attribute__((noreturn));
static void shop_test_abort(void)
{
  if (!shop_abort_armed)
    abort();
  shop_abort_armed = 0;
  longjmp(shop_abort_jmp, 1);
}
#define abort() shop_test_abort()

#include "unity.h"
#include "../src/shop.c" /* NOLINT(bugprone-suspicious-include): reach file-static helpers */

#include <stdlib.h>
#include "tb_error_capture.h"

/* Fixtures for externals the tested paths reach. */
struct shop_data *shop_index;
int top_shop = -1;
const char *extra_bits[] = { "GLOW", "HUM", "\n" };
void basic_mud_log(const char *format, ...) { (void)format; }
long obj_script_id(struct obj_data *obj) { return obj->script_id; }
struct obj_data *obj_proto;
const char *item_types[] = {
  "UNDEFINED", "LIGHT", "SCROLL", "WAND", "STAFF", "WEAPON", "FURNITURE",
  "FREE", "TREASURE", "ARMOR", "POTION", "WORN", "OTHER", "TRASH", "FREE2",
  "CONTAINER", "NOTE", "LIQ CONTAINER", "KEY", "FOOD", "MONEY", "PEN",
  "BOAT", "FOUNTAIN", "\n"
};
obj_rnum real_object(obj_vnum vnum) { (void)vnum; return NOTHING; }
/* Line reader with utils.c get_line()'s contract: 0 at EOF, else the line
 * (without its newline) in buf and the number of lines consumed. */
int get_line(FILE *fl, char *buf)
{
  char temp[READ_SIZE];

  if (!fgets(temp, sizeof(temp), fl))
    return 0;
  temp[strcspn(temp, "\r\n")] = '\0';
  memcpy(buf, temp, strlen(temp) + 1);
  return 1;
}
int strn_cmp(const char *a, const char *b, size_t n)
{
  for (; n > 0; a++, b++, n--) {
    if (LOWER(*a) != LOWER(*b))
      return LOWER(*a) - LOWER(*b);
    if (!*a)
      return 0;
  }
  return 0;
}
const char *drinks[] = { "water", "beer", "\n" };
char *CAP(char *txt) { *txt = UPPER(*txt); return txt; }
int count_color_chars(char *string) { (void)string; return 0; }
int str_cmp(const char *a, const char *b)
{
  for (; LOWER(*a) == LOWER(*b); a++, b++)
    if (!*a)
      return 0;
  return LOWER(*a) - LOWER(*b);
}
int isname(const char *str, const char *namelist)
{
  return namelist && strstr(namelist, str) != NULL;
}

static struct shop_data shop;
static struct shop_buy_data types[2];
static struct obj_data item;

void setUp(void)
{
  tb_error_capture_reset();
  memset(&shop, 0, sizeof(shop));
  memset(types, 0, sizeof(types));
  memset(&item, 0, sizeof(item));
  shop.vnum = 3010;
  types[0].type = ITEM_WEAPON;
  types[1].type = NOTHING;
  shop.type = types;
  shop_index = &shop;
  top_shop = 0;
  item.item_number = NOTHING;
  item.name = strdup("sword long");
}

void tearDown(void)
{
  free(item.name);
  item.name = NULL;
}

static char *repeat(char c, size_t n)
{
  char *s = malloc(n + 1);

  TEST_ASSERT_NOT_NULL(s);
  memset(s, c, n);
  s[n] = '\0';
  return s;
}

/* H2-05: more nested operators than the fixed stack holds must be rejected
 * and reported, never written past stack_data.data[]. */
static void test_buyword_deep_parens_rejected(void)
{
  char *expr = repeat('(', 150);

  TEST_ASSERT_FALSE(evaluate_expression(&item, expr, 0, 0));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("buyword-eval", tb_captured_operation);
  free(expr);
}

static void test_buyword_many_operands_rejected(void)
{
  size_t i, n = 150;
  char *expr = malloc(2 * n + 1);

  TEST_ASSERT_NOT_NULL(expr);
  for (i = 0; i < n; i++) {
    expr[2 * i] = 'a';
    expr[2 * i + 1] = ' ';
  }
  expr[2 * n] = '\0';
  TEST_ASSERT_FALSE(evaluate_expression(&item, expr, 0, 0));
  TEST_ASSERT_EQUAL_STRING("buyword-eval", tb_captured_operation);
  free(expr);
}

static void test_buyword_simple_matches(void)
{
  char *expr = strdup("sword | axe");

  TEST_ASSERT_NOT_NULL(expr);
  TEST_ASSERT_TRUE(evaluate_expression(&item, expr, 0, 0));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
  free(expr);
}

/* H2-21: malformed buywords are reported with the shop and buy type and
 * refuse the sale, instead of a context-free log and a partial result. */
static void test_buyword_unbalanced_paren_reported(void)
{
  char *expr = strdup("sword)");

  TEST_ASSERT_NOT_NULL(expr);
  TEST_ASSERT_FALSE(evaluate_expression(&item, expr, 0, 0));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("buyword-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop #3010"));
  free(expr);
}

static void test_buyword_missing_operand_reported(void)
{
  char *expr = strdup("sword |");

  TEST_ASSERT_NOT_NULL(expr);
  TEST_ASSERT_FALSE(evaluate_expression(&item, expr, 0, 0));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("buyword-parse", tb_captured_operation);
  free(expr);
}

static void test_buyword_extra_operands_reported(void)
{
  char *expr = strdup("sword axe");

  TEST_ASSERT_NOT_NULL(expr);
  TEST_ASSERT_FALSE(evaluate_expression(&item, expr, 0, 0));
  TEST_ASSERT_EQUAL_STRING("buyword-parse", tb_captured_operation);
  free(expr);
}

/* H2-07: a drink container whose liquid is outside drinks[] is listed as
 * "<bad liquid>" and reported, not read past the table. */
static void test_list_object_bad_liquid_reported(void)
{
  struct char_data keeper, buyer;
  char *line;

  memset(&keeper, 0, sizeof(keeper));
  memset(&buyer, 0, sizeof(buyer));
  item.short_description = strdup("a flask");
  GET_OBJ_TYPE(&item) = ITEM_DRINKCON;
  GET_OBJ_VAL(&item, 1) = 5;
  GET_OBJ_VAL(&item, 2) = 40;
  line = list_object(&item, 1, 1, 0, &keeper, &buyer);
  TEST_ASSERT_NOT_NULL(strstr(line, "<bad liquid>"));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("liquid-index", tb_captured_operation);
  free(item.short_description);
  item.short_description = NULL;
}

/* H2-08: a shop-file buy type outside item_types[] is dropped and reported
 * at load; the valid entries around it are kept. */
static void test_read_type_list_drops_out_of_range_type(void)
{
  static char data[] = "99 sword;\n5 axe;\n-1\n";
  struct shop_buy_data list[MAX_TRADE + 1];
  FILE *f = fmemopen(data, strlen(data), "r");
  int len;

  TEST_ASSERT_NOT_NULL(f);
  len = read_type_list(f, list, TRUE, MAX_TRADE);
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
  TEST_ASSERT_EQUAL_INT(2, len); /* type 5 plus the NOTHING terminator */
  TEST_ASSERT_EQUAL_INT(5, BUY_TYPE(list[0]));
  TEST_ASSERT_EQUAL_INT(NOTHING, BUY_TYPE(list[1]));
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("buytype-range", tb_captured_operation);
  free(BUY_WORD(list[0]));
}

/* H2-19: a produced vnum with no prototype is dropped loudly, naming the
 * shop (real_object() here resolves nothing). */
static void test_read_list_reports_missing_produce_vnum(void)
{
  static char data[] = "3020\n-1\n";
  struct shop_buy_data list[MAX_PROD + 1];
  FILE *f = fmemopen(data, strlen(data), "r");
  int len;

  TEST_ASSERT_NOT_NULL(f);
  len = read_list(f, list, TRUE, MAX_PROD, LIST_PRODUCE);
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
  TEST_ASSERT_EQUAL_INT(1, len); /* only the NOTHING terminator */
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("produce-vnum", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop #3010"));
}

/* H2-18: a type-list line that starts with a NUL byte must not make the
 * newline strip write before the buffer; it is reported instead. */
static void test_read_type_list_leading_nul_reported(void)
{
  static char data[] = "\0junk\n-1\n";
  struct shop_buy_data list[MAX_TRADE + 1];
  FILE *f = fmemopen(data, sizeof(data) - 1, "r");
  int len;

  TEST_ASSERT_NOT_NULL(f);
  len = read_type_list(f, list, TRUE, MAX_TRADE);
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
  TEST_ASSERT_EQUAL_INT(1, len); /* nothing but the terminator */
  TEST_ASSERT_EQUAL_INT(1, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("shop-file-parse", tb_captured_operation);
}

/* H2-20: builder text spliced into an act() format has every '$' doubled
 * so perform_act prints it literally, and truncation never leaves half of
 * a "$$" pair (which would form a new $-code). */
static void test_act_escape_doubles_dollar(void)
{
  char out[32];

  TEST_ASSERT_EQUAL_INT(1, shop_act_escape(out, sizeof(out), "a $t charm"));
  TEST_ASSERT_EQUAL_STRING("a $$t charm", out);
}

static void test_act_escape_truncates_on_pair_boundary(void)
{
  char out[4];

  shop_act_escape(out, sizeof(out), "ab$t");
  TEST_ASSERT_EQUAL_STRING("ab", out);
}

/* A buy type that does not fit an int was cast with (int): 4294967301
 * became type 5 (weapon) and -4294967296 type 0, without a word. Both are
 * now dropped and reported, and the entry after them is kept. */
static void test_read_type_list_drops_type_past_int(void)
{
  static char data[] = "4294967301 sword;\n-4294967296;\n9 armor;\n-1\n";
  struct shop_buy_data list[MAX_TRADE + 1];
  FILE *f = fmemopen(data, strlen(data), "r");
  int len;

  TEST_ASSERT_NOT_NULL(f);
  len = read_type_list(f, list, TRUE, MAX_TRADE);
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
  TEST_ASSERT_EQUAL_INT(2, len); /* type 9 plus the NOTHING terminator */
  TEST_ASSERT_EQUAL_INT(9, BUY_TYPE(list[0]));
  TEST_ASSERT_EQUAL_STRING("armor", BUY_WORD(list[0]));
  TEST_ASSERT_EQUAL_INT(NOTHING, BUY_TYPE(list[1]));
  TEST_ASSERT_EQUAL_INT(2, tb_captured_errors);
  TEST_ASSERT_EQUAL_STRING("buytype-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop #3010 buy type"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "does not fit an int"));
  free(BUY_WORD(list[0]));
}

/* ---- shop-file numbers: strtol/strtof with a range check per field ---- */

/* Run STMT, which must end in the readers' abort(); fail if it returns. */
#define EXPECT_SHOP_ABORT(stmt) do { \
  shop_abort_armed = 1; \
  if (setjmp(shop_abort_jmp) == 0) { \
    stmt; \
    shop_abort_armed = 0; \
    TEST_FAIL_MESSAGE("expected abort(): " #stmt); \
  } \
} while (0)

/* read_line()'s vsscanf("%d") was undefined for a number outside int, and
 * glibc stored 4294967301 as 5: a produced vnum, room vnum or keeper wrapped
 * into another, real vnum without a word. Now the boot stops, naming the
 * shop, the field and the value. */
static void test_read_list_aborts_on_produce_vnum_past_int(void)
{
  static char data[] = "4294967301\n-1\n";
  struct shop_buy_data list[MAX_PROD + 1];
  FILE *volatile f = fmemopen(data, strlen(data), "r");

  TEST_ASSERT_NOT_NULL(f);
  EXPECT_SHOP_ABORT((void)read_list(f, list, TRUE, MAX_PROD, LIST_PRODUCE));
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
  TEST_ASSERT_EQUAL_STRING("FATAL", tb_captured_severity);
  TEST_ASSERT_EQUAL_STRING("shop-field-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "shop #3010: produced obj vnum 4294967301 out of range"));
}

/* A room vnum fits an int but not the IDXTYPE room_vnum it is stored in. */
static void test_read_list_aborts_on_room_vnum_past_idxtype(void)
{
  static char data[] = "70000\n-1\n";
  struct shop_buy_data list[2];
  FILE *volatile f = fmemopen(data, strlen(data), "r");

  TEST_ASSERT_NOT_NULL(f);
  EXPECT_SHOP_ABORT((void)read_list(f, list, TRUE, 1, LIST_ROOM));
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "shop #3010: room vnum 70000 out of range"));
}

/* Overflow past long, a malformed line and end of file all stop the boot;
 * in-range values, including the -1 "no keeper", are returned. */
static void test_read_shop_number_reports_each_failure(void)
{
  static char data[] = "-1\n65535\n99999999999999999999\nkeeper\n";
  FILE *volatile f = fmemopen(data, strlen(data), "r");

  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL_INT(-1, (int)read_shop_number(f, "keeper vnum", -1, IDXTYPE_MAX));
  TEST_ASSERT_EQUAL_INT(65535, (int)read_shop_number(f, "keeper vnum", -1, IDXTYPE_MAX));
  TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);

  EXPECT_SHOP_ABORT((void)read_shop_number(f, "shop flags", 0, LONG_MAX));
  TEST_ASSERT_EQUAL_STRING("shop-field-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "(past the range of long)"));

  EXPECT_SHOP_ABORT((void)read_shop_number(f, "trade-with flags", INT_MIN, INT_MAX));
  TEST_ASSERT_EQUAL_STRING("shop-file-parse", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
                              "expected a number for trade-with flags, got an unparsable line"));

  EXPECT_SHOP_ABORT((void)read_shop_number(f, "first opening hour", INT_MIN, INT_MAX));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "got end of file"));
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

/* Profit factors are finite floats, as sedit requires. */
static void test_read_shop_profit_rejects_non_finite(void)
{
  static char data[] = "1.15\n1e40\nnan\n";
  FILE *volatile f = fmemopen(data, strlen(data), "r");

  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL_FLOAT(1.15f, read_shop_profit(f, "buy profit"));
  EXPECT_SHOP_ABORT((void)read_shop_profit(f, "sell profit"));
  TEST_ASSERT_EQUAL_STRING("shop-field-range", tb_captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "shop #3010: sell profit"));
  EXPECT_SHOP_ABORT((void)read_shop_profit(f, "sell profit"));
  TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "is not a finite float"));
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

/* ---- shopping_buy(): the keeper is paid for every unit sold ---- */

/* Fixtures for the purchase path (db.c, constants.c, handler.c, limits.c,
 * utils.c). Carrying and gold follow the real bookkeeping closely enough
 * for one keeper and one buyer in a lit room. */
struct time_info_data time_info;
const struct str_app_type str_app[31] = { [11] = { 0, 0, 1000, 100 } };
int room_is_dark(room_rnum room) { (void)room; return FALSE; }
int increase_gold(struct char_data *ch, int amt) { return GET_GOLD(ch) += amt; }
int decrease_gold(struct char_data *ch, int amt) { return GET_GOLD(ch) -= amt; }
struct obj_data *read_object(obj_vnum nr, int type) { (void)nr; (void)type; return NULL; }
void extract_obj(struct obj_data *obj) { (void)obj; }
char *fname(const char *namelist) { return (char *)namelist; }
int get_number(char **name) { (void)name; return 1; }	/* no "2." prefix */
struct obj_data *get_obj_in_list_num(int num, struct obj_data *list)
{ (void)num; (void)list; return NULL; }
int is_number(const char *str)
{
  if (!*str)
    return FALSE;
  for (; *str; str++)
    if (!isdigit((unsigned char)*str))
      return FALSE;
  return TRUE;
}
char *one_argument(char *argument, char *first_arg)
{
  while (*argument == ' ')
    argument++;
  while (*argument && *argument != ' ')
    *first_arg++ = *argument++;
  *first_arg = '\0';
  return argument;
}
void obj_from_char(struct obj_data *object)
{
  struct char_data *ch = object->carried_by;
  struct obj_data **pp;

  for (pp = &ch->carrying; *pp; pp = &(*pp)->next_content)
    if (*pp == object) {
      *pp = object->next_content;
      break;
    }
  IS_CARRYING_N(ch)--;
  IS_CARRYING_W(ch) -= GET_OBJ_WEIGHT(object);
  object->carried_by = NULL;
  object->next_content = NULL;
}
void obj_to_char(struct obj_data *object, struct char_data *ch)
{
  object->next_content = ch->carrying;
  ch->carrying = object;
  object->carried_by = ch;
  IS_CARRYING_N(ch)++;
  IS_CARRYING_W(ch) += GET_OBJ_WEIGHT(object);
}

static struct char_data shop_keeper_mob, shop_buyer;
static struct player_special_data shop_buyer_specials;

/* A keeper (mob) in an open shop holding item, the only unit, for 100
 * coins; a level 1 player with 1000 coins wants it. */
static void purchase_fixture(void)
{
  memset(&shop_keeper_mob, 0, sizeof(shop_keeper_mob));
  memset(&shop_buyer, 0, sizeof(shop_buyer));
  memset(&shop_buyer_specials, 0, sizeof(shop_buyer_specials));
  SET_BIT_AR(MOB_FLAGS(&shop_keeper_mob), MOB_ISNPC);
  shop_keeper_mob.player_specials = &dummy_mob;
  shop_keeper_mob.player.short_descr = (char *)"the shopkeeper";
  shop_keeper_mob.aff_abils.cha = 11;
  shop_buyer.player.name = (char *)"Buyer";
  shop_buyer.player_specials = &shop_buyer_specials;
  shop_buyer.player.level = 1;
  shop_buyer.aff_abils.cha = 11;
  shop_buyer.aff_abils.str = 11;
  shop_buyer.aff_abils.dex = 11;
  GET_GOLD(&shop_buyer) = 1000;

  shop.profit_buy = 1.0f;
  shop.profit_sell = 1.0f;
  shop.close1 = 28;
  time_info.hours = 12;
  shop.message_buy = (char *)"%s That'll be %d coins, thanks.";

  item.short_description = (char *)"a long sword";
  GET_OBJ_COST(&item) = 100;
  GET_OBJ_WEIGHT(&item) = 1;
  obj_to_char(&item, &shop_keeper_mob);
  SHOP_SORT(0) = 1;
}

/* shopping_buy() paid the keeper only when "obj", by then the next unit on
 * offer, was not NULL: buying the last unit of an item the shop does not
 * produce took the buyer's 100 coins and paid nobody. */
static void test_buying_last_unit_pays_keeper(void)
{
  char arg[] = "sword";

  purchase_fixture();
  shopping_buy(arg, &shop_buyer, &shop_keeper_mob, 0);
  TEST_ASSERT_TRUE(shop_buyer.carrying == &item);
  TEST_ASSERT_NULL(shop_keeper_mob.carrying);
  TEST_ASSERT_EQUAL_INT(900, GET_GOLD(&shop_buyer));
  TEST_ASSERT_EQUAL_INT(100, GET_GOLD(&shop_keeper_mob));
  item.short_description = NULL;
}

/* A quest item is paid in quest points, which go to nobody: the keeper's
 * gold stays as it was, last unit or not. */
static void test_buying_quest_item_pays_no_gold(void)
{
  char arg[] = "sword";

  purchase_fixture();
  SET_BIT_AR(GET_OBJ_EXTRA(&item), ITEM_QUEST);
  GET_QUESTPOINTS(&shop_buyer) = 150;
  shopping_buy(arg, &shop_buyer, &shop_keeper_mob, 0);
  TEST_ASSERT_TRUE(shop_buyer.carrying == &item);
  TEST_ASSERT_EQUAL_INT(50, GET_QUESTPOINTS(&shop_buyer));
  TEST_ASSERT_EQUAL_INT(1000, GET_GOLD(&shop_buyer));
  TEST_ASSERT_EQUAL_INT(0, GET_GOLD(&shop_keeper_mob));
  item.short_description = NULL;
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_act_escape_doubles_dollar);
  RUN_TEST(test_act_escape_truncates_on_pair_boundary);
  RUN_TEST(test_read_type_list_leading_nul_reported);
  RUN_TEST(test_read_list_reports_missing_produce_vnum);
  RUN_TEST(test_read_type_list_drops_out_of_range_type);
  RUN_TEST(test_list_object_bad_liquid_reported);
  RUN_TEST(test_buyword_unbalanced_paren_reported);
  RUN_TEST(test_buyword_missing_operand_reported);
  RUN_TEST(test_buyword_extra_operands_reported);
  RUN_TEST(test_buyword_deep_parens_rejected);
  RUN_TEST(test_buyword_many_operands_rejected);
  RUN_TEST(test_buyword_simple_matches);
  RUN_TEST(test_read_type_list_drops_type_past_int);
  RUN_TEST(test_read_list_aborts_on_produce_vnum_past_int);
  RUN_TEST(test_read_list_aborts_on_room_vnum_past_idxtype);
  RUN_TEST(test_read_shop_number_reports_each_failure);
  RUN_TEST(test_read_shop_profit_rejects_non_finite);
  RUN_TEST(test_buying_last_unit_pays_keeper);
  RUN_TEST(test_buying_quest_item_pays_no_gold);
  return UNITY_END();
}
