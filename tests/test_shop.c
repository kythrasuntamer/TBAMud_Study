/* Executable tests of the production shop buyword evaluator and shop-file
 * readers. shop.c's helpers are file-static, so this test includes shop.c
 * directly (one translation unit) and the link discards every section the
 * tested paths do not reach. Fixtures are disposable.
 */
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
  return UNITY_END();
}
