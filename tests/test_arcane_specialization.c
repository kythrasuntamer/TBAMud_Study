/* Native regression tests execute the same pure calculations used by magic.c. */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "spells.h"
#include "arcane_specialization.h"

void setUp(void) {}
void tearDown(void) {}

void test_wizard_mastery_boundaries(void)
{
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(CLASS_MAGIC_USER, 19, 1, 100, 0, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(CLASS_MAGIC_USER, 20, 15, 100, 0, 0));
  TEST_ASSERT_EQUAL_INT(80, arcane_adjust_mana(CLASS_MAGIC_USER, 20, 10, 100, 0, 0));
  TEST_ASSERT_EQUAL_INT(1, arcane_adjust_mana(CLASS_MAGIC_USER, 50, 1, 1, 0, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(CLASS_MAGIC_USER, 50, 1, 100, 0, 1));
}
void test_sorcerer_mana_boundaries(void)
{
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(CLASS_SORCERER, 19, 1, 100, 1, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(CLASS_SORCERER, 20, 1, 100, 0, 0));
  TEST_ASSERT_EQUAL_INT(150, arcane_adjust_mana(CLASS_SORCERER, 20, 1, 100, 1, 0));
  TEST_ASSERT_EQUAL_INT(2, arcane_adjust_mana(CLASS_SORCERER, 20, 1, 1, 1, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(CLASS_SORCERER, 20, 1, 100, 1, 1));
}
void test_other_classes_have_no_adjustment(void)
{
  int klass;
  for (klass = CLASS_CLERIC; klass < NUM_CLASSES; ++klass) {
    if (klass == CLASS_SORCERER) continue;
    TEST_ASSERT_EQUAL_INT(100, arcane_adjust_mana(klass, 50, 1, 100, 1, 0));
    TEST_ASSERT_EQUAL_INT(100, arcane_adjust_damage(klass, 50, 100, CAST_SPELL, 0));
  }
}
void test_damage_applies_only_to_direct_sorcerer_spells(void)
{
  TEST_ASSERT_EQUAL_INT(125, arcane_adjust_damage(CLASS_SORCERER, 20, 100, CAST_SPELL, 0));
  TEST_ASSERT_EQUAL_INT(6, arcane_adjust_damage(CLASS_SORCERER, 20, 5, CAST_SPELL, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_damage(CLASS_SORCERER, 19, 100, CAST_SPELL, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_damage(CLASS_SORCERER, 50, 100, CAST_WAND, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_damage(CLASS_SORCERER, 50, 100, CAST_SCROLL, 0));
  TEST_ASSERT_EQUAL_INT(100, arcane_adjust_damage(CLASS_SORCERER, 50, 100, CAST_SPELL, 1));
  TEST_ASSERT_EQUAL_INT(0, arcane_adjust_damage(CLASS_SORCERER, 50, 0, CAST_SPELL, 0));
}
int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_wizard_mastery_boundaries);
  RUN_TEST(test_sorcerer_mana_boundaries);
  RUN_TEST(test_other_classes_have_no_adjustment);
  RUN_TEST(test_damage_applies_only_to_direct_sorcerer_spells);
  return UNITY_END();
}
