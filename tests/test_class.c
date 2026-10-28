/**
 * @file test_class.c
 * Unit tests for pure functions in src/class.c:
 *   parse_class, thaco, backstab_mult, level_exp
 * and get_class_by_name() from src/utils.c (class names and legacy aliases).
 */

#include "unity.h"

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "class.h"
#include "interpreter.h" /* is_abbrev() */
#include "spells.h" /* SAVING_PARA and other saving throw identifiers */

extern FILE *logfile;

/* utils.c is linked, so its tb_error_log_at() exists; the test build wraps
 * calls from class.c to this capturing version. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
#define tb_error_log_at __wrap_tb_error_log_at
#include "tb_error_capture.h"
#undef tb_error_log_at

void setUp(void)    { logfile = stderr; }
void tearDown(void) { logfile = NULL; }

/* =========================================================
 * parse_class
 * ========================================================= */

void test_parse_class_magic_user_lowercase(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, parse_class('m'));
}

/* test_stubs.c's weak is_abbrev() always fails; get_class_by_name() needs
 * the real prefix match, so this mirrors interpreter.c's is_abbrev(). */
int is_abbrev(const char *arg1, const char *arg2)
{
    if (!*arg1)
        return 0;
    for (; *arg1 && *arg2; arg1++, arg2++)
        if (LOWER(*arg1) != LOWER(*arg2))
            return 0;
    return !*arg1;
}

/* Current names in pc_class_types[] resolve, in full and abbreviated. */
void test_get_class_by_name_current_names(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("Wizard"));
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("wiz"));
    TEST_ASSERT_EQUAL_INT(CLASS_CLERIC, get_class_by_name("cleric"));
    TEST_ASSERT_EQUAL_INT(CLASS_THIEF, get_class_by_name("THIEF"));
    TEST_ASSERT_EQUAL_INT(CLASS_WARRIOR, get_class_by_name("warrior"));
    TEST_ASSERT_EQUAL_INT(CLASS_MONK, get_class_by_name("monk"));
}

/* "Magic User" was renamed "Wizard"; the old names still mean the wizard. */
void test_get_class_by_name_legacy_magic_user_aliases(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("magic user"));
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("Magic User"));
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("magic"));
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("mage"));
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, get_class_by_name("ma"));
}

/* A current class keeps an abbreviation it shares with a legacy alias, and
 * unknown or empty names are refused. */
void test_get_class_by_name_current_names_win_and_misses(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_MONK, get_class_by_name("m"));
    TEST_ASSERT_EQUAL_INT(-1, get_class_by_name("ranger"));
    TEST_ASSERT_EQUAL_INT(-1, get_class_by_name("magic users"));
    TEST_ASSERT_EQUAL_INT(-1, get_class_by_name(""));
    TEST_ASSERT_EQUAL_INT(-1, get_class_by_name(NULL));
}

void test_parse_class_cleric_lowercase(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_CLERIC, parse_class('c'));
}

void test_parse_class_warrior_lowercase(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_WARRIOR, parse_class('w'));
}

void test_parse_class_thief_lowercase(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_THIEF, parse_class('t'));
}

void test_parse_class_uppercase(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_MAGIC_USER, parse_class('M'));
    TEST_ASSERT_EQUAL_INT(CLASS_CLERIC,     parse_class('C'));
    TEST_ASSERT_EQUAL_INT(CLASS_WARRIOR,    parse_class('W'));
    TEST_ASSERT_EQUAL_INT(CLASS_THIEF,      parse_class('T'));
}

void test_parse_class_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_UNDEFINED, parse_class('x'));
    TEST_ASSERT_EQUAL_INT(CLASS_UNDEFINED, parse_class('?'));
    TEST_ASSERT_EQUAL_INT(CLASS_UNDEFINED, parse_class(' '));
}

/* =========================================================
 * thaco
 * ========================================================= */

void test_thaco_magic_user_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(20, thaco(CLASS_MAGIC_USER, 1));
}

void test_thaco_magic_user_level_10(void)
{
    TEST_ASSERT_EQUAL_INT(17, thaco(CLASS_MAGIC_USER, 10));
}

void test_thaco_cleric_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(20, thaco(CLASS_CLERIC, 1));
}

void test_thaco_warrior_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(20, thaco(CLASS_WARRIOR, 1));
}

void test_thaco_warrior_level_20(void)
{
    TEST_ASSERT_EQUAL_INT(2, thaco(CLASS_WARRIOR, 20));
}

void test_thaco_warrior_high_level_is_one(void)
{
    /* Warriors hit thac0=1 around level 21 and stay there */
    TEST_ASSERT_EQUAL_INT(1, thaco(CLASS_WARRIOR, 21));
    TEST_ASSERT_EQUAL_INT(1, thaco(CLASS_WARRIOR, LVL_IMPL));
}

void test_thaco_thief_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(20, thaco(CLASS_THIEF, 1));
}

/* =========================================================
 * backstab_mult
 * ========================================================= */

void test_backstab_mult_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(2, backstab_mult(1));
}

void test_backstab_mult_level_7(void)
{
    TEST_ASSERT_EQUAL_INT(2, backstab_mult(7));
}

void test_backstab_mult_level_8(void)
{
    TEST_ASSERT_EQUAL_INT(3, backstab_mult(8));
}

void test_backstab_mult_level_13(void)
{
    TEST_ASSERT_EQUAL_INT(3, backstab_mult(13));
}

void test_backstab_mult_level_14(void)
{
    TEST_ASSERT_EQUAL_INT(4, backstab_mult(14));
}

void test_backstab_mult_level_20(void)
{
    TEST_ASSERT_EQUAL_INT(4, backstab_mult(20));
}

void test_backstab_mult_level_21(void)
{
    TEST_ASSERT_EQUAL_INT(5, backstab_mult(21));
}

void test_backstab_mult_immortal(void)
{
    TEST_ASSERT_EQUAL_INT(20, backstab_mult(LVL_IMMORT));
}

/* =========================================================
 * level_exp
 * ========================================================= */

void test_level_exp_magic_user_level_0(void)
{
    TEST_ASSERT_EQUAL_INT(0, level_exp(CLASS_MAGIC_USER, 0));
}

void test_level_exp_magic_user_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(1, level_exp(CLASS_MAGIC_USER, 1));
}

void test_level_exp_magic_user_level_2(void)
{
    TEST_ASSERT_EQUAL_INT(2500, level_exp(CLASS_MAGIC_USER, 2));
}

void test_level_exp_cleric_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(1, level_exp(CLASS_CLERIC, 1));
}

void test_level_exp_thief_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(1, level_exp(CLASS_THIEF, 1));
}

void test_level_exp_warrior_level_1(void)
{
    TEST_ASSERT_EQUAL_INT(1, level_exp(CLASS_WARRIOR, 1));
}

void test_level_exp_invalid_level_returns_zero(void)
{
    /* Level > LVL_IMPL or level < 0 → logs error and returns 0 */
    TEST_ASSERT_EQUAL_INT(0, level_exp(CLASS_MAGIC_USER, -1));
    TEST_ASSERT_EQUAL_INT(0, level_exp(CLASS_MAGIC_USER, LVL_IMPL + 1));
}

void test_level_exp_immortal_level(void)
{
    /* Level 51 follows the extended XP curve, not the old level-31 cap. */
    TEST_ASSERT_EQUAL_INT(9710000, level_exp(CLASS_MAGIC_USER, LVL_IMMORT));
}

void test_level_exp_extended_boundaries(void)
{
    /* Preserve level-30 thresholds and add 110,000 XP per level through 51. */
    TEST_ASSERT_EQUAL_INT(7400000, level_exp(CLASS_MAGIC_USER, 30));
    TEST_ASSERT_EQUAL_INT(7510000, level_exp(CLASS_MAGIC_USER, 31));
    TEST_ASSERT_EQUAL_INT(9600000, level_exp(CLASS_MAGIC_USER, 50));
    TEST_ASSERT_EQUAL_INT(9710000, level_exp(CLASS_MAGIC_USER, 51));

    TEST_ASSERT_EQUAL_INT(6400000, level_exp(CLASS_CLERIC, 30));
    TEST_ASSERT_EQUAL_INT(6510000, level_exp(CLASS_CLERIC, 31));
    TEST_ASSERT_EQUAL_INT(8600000, level_exp(CLASS_CLERIC, 50));
    TEST_ASSERT_EQUAL_INT(8710000, level_exp(CLASS_CLERIC, 51));

    TEST_ASSERT_EQUAL_INT(6650000, level_exp(CLASS_THIEF, 30));
    TEST_ASSERT_EQUAL_INT(6760000, level_exp(CLASS_THIEF, 31));
    TEST_ASSERT_EQUAL_INT(8850000, level_exp(CLASS_THIEF, 50));
    TEST_ASSERT_EQUAL_INT(8960000, level_exp(CLASS_THIEF, 51));

    TEST_ASSERT_EQUAL_INT(7400000, level_exp(CLASS_WARRIOR, 30));
    TEST_ASSERT_EQUAL_INT(7510000, level_exp(CLASS_WARRIOR, 31));
    TEST_ASSERT_EQUAL_INT(9600000, level_exp(CLASS_WARRIOR, 50));
    TEST_ASSERT_EQUAL_INT(9710000, level_exp(CLASS_WARRIOR, 51));
}

void test_level_exp_extended_progression_is_monotonic(void)
{
    int classes[] = { CLASS_MAGIC_USER, CLASS_CLERIC, CLASS_THIEF, CLASS_WARRIOR };
    size_t i;
    for (i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        int level;
        for (level = 31; level <= LVL_IMPL; ++level)
            TEST_ASSERT_TRUE(level_exp(classes[i], level) > level_exp(classes[i], level - 1));
    }
}

void test_level_exp_mortal_boundary(void)
{
    TEST_ASSERT_EQUAL_INT(9600000, level_exp(CLASS_MAGIC_USER, LVL_IMMORT - 1));
    TEST_ASSERT_EQUAL_INT(9710000, level_exp(CLASS_MAGIC_USER, LVL_IMMORT));
}

void test_monk_class_selection_and_combat_curves(void)
{
    TEST_ASSERT_EQUAL_INT(CLASS_MONK, parse_class('o'));
    TEST_ASSERT_EQUAL_INT(CLASS_MONK, parse_class('O'));
    TEST_ASSERT_EQUAL_INT(thaco(CLASS_WARRIOR, 1), thaco(CLASS_MONK, 1));
    TEST_ASSERT_EQUAL_INT(thaco(CLASS_WARRIOR, 30), thaco(CLASS_MONK, 30));
    TEST_ASSERT_EQUAL_INT(level_exp(CLASS_WARRIOR, 25), level_exp(CLASS_MONK, 25));
    TEST_ASSERT_EQUAL_INT(level_exp(CLASS_WARRIOR, 50), level_exp(CLASS_MONK, 50));
    TEST_ASSERT_EQUAL_INT(saving_throws(CLASS_WARRIOR, SAVING_PARA, 10), saving_throws(CLASS_MONK, SAVING_PARA, 10));
}

/* =========================================================
 * saving_throws
 * ========================================================= */

/* C1-18: the mage/cleric/thief tables stop at 40 and the warrior table at
 * 50, but levels run to LVL_IMPL. Past the table every save returned 100
 * ("never saves") and logged once per call. Now the last row is used. */
void test_saving_throws_past_table_use_last_row(void)
{
    tb_error_capture_reset();
    TEST_ASSERT_EQUAL_INT(saving_throws(CLASS_MAGIC_USER, SAVING_PARA, 40),
                          saving_throws(CLASS_MAGIC_USER, SAVING_PARA, 45));
    TEST_ASSERT_EQUAL_INT(saving_throws(CLASS_CLERIC, SAVING_SPELL, 40),
                          saving_throws(CLASS_CLERIC, SAVING_SPELL, LVL_IMMORT - 1));
    TEST_ASSERT_EQUAL_INT(saving_throws(CLASS_THIEF, SAVING_ROD, 40),
                          saving_throws(CLASS_THIEF, SAVING_ROD, LVL_IMPL));
    TEST_ASSERT_EQUAL_INT(saving_throws(CLASS_WARRIOR, SAVING_PARA, 50),
                          saving_throws(CLASS_WARRIOR, SAVING_PARA, LVL_IMMORT));
    TEST_ASSERT_TRUE(saving_throws(CLASS_MAGIC_USER, SAVING_BREATH, 48) < 100);
    TEST_ASSERT_EQUAL_INT(0, tb_captured_errors);
}

/* C1-18: a level outside 0..LVL_IMPL is still an error, and says so. */
void test_saving_throws_invalid_level_is_reported(void)
{
    tb_error_capture_reset();
    TEST_ASSERT_EQUAL_INT(100, saving_throws(CLASS_MAGIC_USER, SAVING_PARA, LVL_IMPL + 1));
    TEST_ASSERT_EQUAL_STRING("class-table", tb_captured_operation);
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "level 61"));
}

/* C1-19: table misses named nothing ("XP tables not set up correctly");
 * the record now carries the table, class, type and level. */
void test_class_table_misses_name_their_inputs(void)
{
    tb_error_capture_reset();
    TEST_ASSERT_EQUAL_INT(123456, level_exp(7, 10));
    TEST_ASSERT_EQUAL_STRING("class-table", tb_captured_operation);
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "class 7 type -1 level 10"));
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "level-exp"));

    TEST_ASSERT_EQUAL_INT(100, thaco(9, 5));
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "class 9 type -1 level 5 is outside the thac0"));

    TEST_ASSERT_EQUAL_INT(100, saving_throws(CLASS_CLERIC, 42, 10));
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "class 1 type 42 level 10"));
    TEST_ASSERT_NULL(strstr(tb_captured_detail, " for ")); /* no character */
}

/* C1-19: a lookup made for a character also names it, so a player whose
 * saved class or level is outside a table is identified by the record
 * itself (fight.c's compute_thaco() uses char_thaco()). Three reports: the
 * class tables write at most ten a minute. */
void test_class_table_misses_name_the_character(void)
{
    struct char_data ch;

    memset(&ch, 0, sizeof(ch));
    GET_CLASS(&ch) = 7;            /* corrupt player file data */
    GET_LEVEL(&ch) = 10;
    GET_IDNUM(&ch) = 1234;

    tb_error_capture_reset();
    TEST_ASSERT_EQUAL_INT(100, char_thaco(&ch));
    TEST_ASSERT_EQUAL_STRING("class-table", tb_captured_operation);
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
        "class 7 type -1 level 10 is outside the thac0 table for player idnum 1234"));

    TEST_ASSERT_EQUAL_INT(123456, char_level_exp(&ch, 11));
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail,
        "class 7 type -1 level 11 is outside the level-exp table for player idnum 1234"));

    TEST_ASSERT_EQUAL_INT(100, char_saving_throw(&ch, 7, SAVING_SPELL));
    TEST_ASSERT_NOT_NULL(strstr(tb_captured_detail, "saving-throw table for player idnum 1234"));
    TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);

    /* A character whose class and level are in range is not reported. */
    GET_CLASS(&ch) = CLASS_WARRIOR;
    TEST_ASSERT_EQUAL_INT(thaco(CLASS_WARRIOR, 10), char_thaco(&ch));
    TEST_ASSERT_EQUAL_INT(3, tb_captured_errors);
}

/* =========================================================
 * main
 * ========================================================= */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_monk_class_selection_and_combat_curves);

    /* saving_throws */
    RUN_TEST(test_saving_throws_past_table_use_last_row);
    RUN_TEST(test_saving_throws_invalid_level_is_reported);
    RUN_TEST(test_class_table_misses_name_their_inputs);
    RUN_TEST(test_class_table_misses_name_the_character);

    /* parse_class */
    RUN_TEST(test_parse_class_magic_user_lowercase);
    RUN_TEST(test_parse_class_cleric_lowercase);
    RUN_TEST(test_parse_class_warrior_lowercase);
    RUN_TEST(test_parse_class_thief_lowercase);
    RUN_TEST(test_parse_class_uppercase);
    RUN_TEST(test_parse_class_invalid);

    /* get_class_by_name (utils.c) */
    RUN_TEST(test_get_class_by_name_current_names);
    RUN_TEST(test_get_class_by_name_legacy_magic_user_aliases);
    RUN_TEST(test_get_class_by_name_current_names_win_and_misses);

    /* thaco */
    RUN_TEST(test_thaco_magic_user_level_1);
    RUN_TEST(test_thaco_magic_user_level_10);
    RUN_TEST(test_thaco_cleric_level_1);
    RUN_TEST(test_thaco_warrior_level_1);
    RUN_TEST(test_thaco_warrior_level_20);
    RUN_TEST(test_thaco_warrior_high_level_is_one);
    RUN_TEST(test_thaco_thief_level_1);

    /* backstab_mult */
    RUN_TEST(test_backstab_mult_level_1);
    RUN_TEST(test_backstab_mult_level_7);
    RUN_TEST(test_backstab_mult_level_8);
    RUN_TEST(test_backstab_mult_level_13);
    RUN_TEST(test_backstab_mult_level_14);
    RUN_TEST(test_backstab_mult_level_20);
    RUN_TEST(test_backstab_mult_level_21);
    RUN_TEST(test_backstab_mult_immortal);

    /* level_exp */
    RUN_TEST(test_level_exp_magic_user_level_0);
    RUN_TEST(test_level_exp_magic_user_level_1);
    RUN_TEST(test_level_exp_magic_user_level_2);
    RUN_TEST(test_level_exp_cleric_level_1);
    RUN_TEST(test_level_exp_thief_level_1);
    RUN_TEST(test_level_exp_warrior_level_1);
    RUN_TEST(test_level_exp_invalid_level_returns_zero);
    RUN_TEST(test_level_exp_immortal_level);
    RUN_TEST(test_level_exp_extended_boundaries);
    RUN_TEST(test_level_exp_extended_progression_is_monotonic);
    RUN_TEST(test_level_exp_mortal_boundary);

    return UNITY_END();
}
