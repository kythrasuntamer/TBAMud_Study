/* Executable tests of the production shop buyword evaluator and shop-file
 * readers. shop.c's helpers are file-static, so this test includes shop.c
 * directly (one translation unit) and the link discards every section the
 * tested paths do not reach. Fixtures are disposable.
 */
#include "unity.h"
#include "../src/shop.c"

#include <stdlib.h>
#include "tb_error_capture.h"

/* Fixtures for externals the tested paths reach. */
struct shop_data *shop_index;
int top_shop = -1;
const char *extra_bits[] = { "GLOW", "HUM", "\n" };
void basic_mud_log(const char *format, ...) { (void)format; }
long obj_script_id(struct obj_data *obj) { return obj->script_id; }
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

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_buyword_unbalanced_paren_reported);
  RUN_TEST(test_buyword_missing_operand_reported);
  RUN_TEST(test_buyword_extra_operands_reported);
  RUN_TEST(test_buyword_deep_parens_rejected);
  RUN_TEST(test_buyword_many_operands_rejected);
  RUN_TEST(test_buyword_simple_matches);
  return UNITY_END();
}
