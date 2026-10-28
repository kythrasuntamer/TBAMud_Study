/**
 * @file test_utils.c
 * Unit tests for pure / near-pure functions in src/utils.c
 */

#include "unity.h"

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"

/* Redirect mud log output so basic_mud_vlog() doesn't print the
 * "SYSERR: Using log() before stream was initialized!" warning. */
extern FILE *logfile;

/* Regression: get_line must copy the entire zone header, not pointer width. */
void test_get_line_preserves_zone_header(void)
{
    FILE *fp = tmpfile();
    char buf[READ_SIZE];
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("0 99 30 2 d 0 0 0 -1 -1\n", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0L, SEEK_SET));
    TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
    TEST_ASSERT_EQUAL_STRING("0 99 30 2 d 0 0 0 -1 -1", buf);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* A line longer than the buffer used to come back in pieces, so a player
 * file value could end in a tag ("Levl: 34") read as a line of its own. */
void test_get_line_discards_rest_of_overlong_line(void)
{
    FILE *fp = tmpfile();
    char buf[READ_SIZE], line[READ_SIZE + 64];
    TEST_ASSERT_NOT_NULL(fp);
    memset(line, 'x', READ_SIZE - 1);
    strlcpy(line + READ_SIZE - 1, "Levl: 34\n", sizeof(line) - (READ_SIZE - 1));
    TEST_ASSERT_TRUE(fputs(line, fp) >= 0);
    TEST_ASSERT_TRUE(fputs("Next\n", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0L, SEEK_SET));
    TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
    TEST_ASSERT_EQUAL_size_t(READ_SIZE - 1, strlen(buf));
    TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
    TEST_ASSERT_EQUAL_STRING("Next", buf);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* A line that exactly fills the buffer is whole; the next line is intact. */
void test_get_line_reads_full_buffer_line_whole(void)
{
    FILE *fp = tmpfile();
    char buf[READ_SIZE], line[READ_SIZE + 2];
    TEST_ASSERT_NOT_NULL(fp);
    memset(line, 'y', READ_SIZE - 1);
    strlcpy(line + READ_SIZE - 1, "\r\n", sizeof(line) - (READ_SIZE - 1));
    TEST_ASSERT_TRUE(fputs(line, fp) >= 0);
    TEST_ASSERT_TRUE(fputs("Next\n", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0L, SEEK_SET));
    TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
    TEST_ASSERT_EQUAL_size_t(READ_SIZE - 1, strlen(buf));
    TEST_ASSERT_EQUAL_INT(1, get_line(fp, buf));
    TEST_ASSERT_EQUAL_STRING("Next", buf);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

void setUp(void)    { logfile = stderr; }
void tearDown(void) { logfile = NULL; }

/* =========================================================
 * prune_crlf
 * ========================================================= */

void test_prune_crlf_strips_crlf(void)
{
    char s[] = "hello\r\n";
    prune_crlf(s);
    TEST_ASSERT_EQUAL_STRING("hello", s);
}

void test_prune_crlf_strips_lf_only(void)
{
    char s[] = "hello\n";
    prune_crlf(s);
    TEST_ASSERT_EQUAL_STRING("hello", s);
}

void test_prune_crlf_no_op_on_clean(void)
{
    char s[] = "hello";
    prune_crlf(s);
    TEST_ASSERT_EQUAL_STRING("hello", s);
}

void test_prune_crlf_multiple_trailing(void)
{
    char s[] = "hi\r\n\r\n";
    prune_crlf(s);
    TEST_ASSERT_EQUAL_STRING("hi", s);
}

/* =========================================================
 * str_cmp  (may be an alias for strcasecmp on this platform)
 * ========================================================= */

void test_str_cmp_equal_strings(void)
{
    TEST_ASSERT_EQUAL_INT(0, str_cmp("hello", "hello"));
}

void test_str_cmp_case_insensitive(void)
{
    TEST_ASSERT_EQUAL_INT(0, str_cmp("Hello", "hello"));
    TEST_ASSERT_EQUAL_INT(0, str_cmp("HELLO", "hello"));
}

void test_str_cmp_ordering_less(void)
{
    TEST_ASSERT_LESS_THAN(0, str_cmp("a", "b"));
}

void test_str_cmp_ordering_greater(void)
{
    TEST_ASSERT_GREATER_THAN(0, str_cmp("b", "a"));
}

void test_str_cmp_empty_equal(void)
{
    TEST_ASSERT_EQUAL_INT(0, str_cmp("", ""));
}

/* =========================================================
 * strn_cmp  (may be an alias for strncasecmp on this platform)
 * ========================================================= */

void test_strn_cmp_equal_prefix(void)
{
    TEST_ASSERT_EQUAL_INT(0, strn_cmp("hello", "hello world", 5));
}

void test_strn_cmp_differ_past_n(void)
{
    /* First 5 chars same, so strn_cmp("hello!", "hellox", 5) == 0 */
    TEST_ASSERT_EQUAL_INT(0, strn_cmp("hello!", "hellox", 5));
}

void test_strn_cmp_differ_within_n(void)
{
    TEST_ASSERT_NOT_EQUAL(0, strn_cmp("abc", "xyz", 3));
}

/* =========================================================
 * sprintbit
 * ========================================================= */

void test_sprintbit_no_bits_set(void)
{
    static const char *names[] = { "FLAG_A", "FLAG_B", "\n" };
    char result[256];
    sprintbit(0, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("NOBITS ", result);
}

void test_sprintbit_single_bit(void)
{
    static const char *names[] = { "FLAG_A", "FLAG_B", "\n" };
    char result[256];
    sprintbit(1, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("FLAG_A ", result);
}

void test_sprintbit_multiple_bits(void)
{
    static const char *names[] = { "FLAG_A", "FLAG_B", "\n" };
    char result[256];
    sprintbit(3, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("FLAG_A FLAG_B ", result);
}

void test_sprintbit_undefined_bit(void)
{
    /* Bit 2 is beyond the named array – should produce "UNDEFINED" */
    static const char *names[] = { "FLAG_A", "FLAG_B", "\n" };
    char result[256];
    sprintbit(4, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("UNDEFINED ", result);
}

/* =========================================================
 * sprinttype
 * ========================================================= */

void test_sprinttype_valid_index_zero(void)
{
    static const char *names[] = { "ZERO", "ONE", "\n" };
    char result[64];
    sprinttype(0, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("ZERO", result);
}

void test_sprinttype_valid_index_one(void)
{
    static const char *names[] = { "ZERO", "ONE", "\n" };
    char result[64];
    sprinttype(1, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("ONE", result);
}

void test_sprinttype_out_of_range(void)
{
    static const char *names[] = { "ZERO", "ONE", "\n" };
    char result[64];
    sprinttype(5, names, result, sizeof(result));
    TEST_ASSERT_EQUAL_STRING("UNDEFINED", result);
}

/* =========================================================
 * levenshtein_distance
 * ========================================================= */

void test_levenshtein_identical_strings(void)
{
    TEST_ASSERT_EQUAL_INT(0, levenshtein_distance("hello", "hello"));
}

void test_levenshtein_empty_and_nonempty(void)
{
    TEST_ASSERT_EQUAL_INT(5, levenshtein_distance("", "hello"));
    TEST_ASSERT_EQUAL_INT(5, levenshtein_distance("hello", ""));
}

void test_levenshtein_single_insertion(void)
{
    TEST_ASSERT_EQUAL_INT(1, levenshtein_distance("abc", "abcd"));
}

void test_levenshtein_single_deletion(void)
{
    TEST_ASSERT_EQUAL_INT(1, levenshtein_distance("abcd", "abc"));
}

void test_levenshtein_single_substitution(void)
{
    TEST_ASSERT_EQUAL_INT(1, levenshtein_distance("abc", "axc"));
}

void test_levenshtein_both_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, levenshtein_distance("", ""));
}

void test_levenshtein_null_input_is_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(INT_MAX, levenshtein_distance(NULL, "hello"));
}

/* =========================================================
 * file helpers
 * ========================================================= */

void test_file_sizeof_counts_exact_bytes(void)
{
    FILE *fp = tmpfile();

    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("abc", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fflush(fp));

    TEST_ASSERT_EQUAL_size_t(3, file_sizeof(fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

void test_file_numlines_counts_newlines_only(void)
{
    FILE *fp = tmpfile();

    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("one\ntwo\nthree", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fflush(fp));

    TEST_ASSERT_EQUAL_INT(2, file_numlines(fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* =========================================================
 * count_color_chars
 * ========================================================= */

void test_count_color_chars_no_codes(void)
{
    TEST_ASSERT_EQUAL_INT(0, count_color_chars("hello"));
}

void test_count_color_chars_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, count_color_chars(""));
}

void test_count_color_chars_single_color_code(void)
{
    /* "\tR" is a two-char color escape; both are skipped (counted as overhead) */
    TEST_ASSERT_EQUAL_INT(2, count_color_chars("\tR"));
}

void test_count_color_chars_double_tab(void)
{
    /* "\t\t" is an escaped literal tab; costs 1 overhead char */
    TEST_ASSERT_EQUAL_INT(1, count_color_chars("\t\t"));
}

void test_count_color_chars_mixed(void)
{
    /* "\tRhello" → 2 overhead + 0 for "hello" */
    TEST_ASSERT_EQUAL_INT(2, count_color_chars("\tRhello"));
}

void test_count_color_chars_trailing_lone_tab_stays_in_bounds(void)
{
    /* F3-04: a lone trailing tab used to step i past the terminator and read
     * string[len + 1].  strdup() gives ASan an exact-size heap block, so the
     * old overread is a heap-buffer-overflow.  The tab counts as one
     * overhead byte and the malformed escape is logged (op=color-escape). */
    char *s = strdup("a sword\t");
    FILE *cap = tmpfile();
    char line[4096];
    int seen = 0;

    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(cap);
    logfile = cap;
    TEST_ASSERT_EQUAL_INT(1, count_color_chars(s));
    fflush(cap);
    rewind(cap);
    while (fgets(line, sizeof(line), cap))
        if (strstr(line, "op=color-escape") && !strstr(line, "a sword"))
            seen = 1;
    logfile = stderr;
    fclose(cap);
    free(s);
    TEST_ASSERT_TRUE(seen);
}

/* =========================================================
 * count_non_protocol_chars
 * ========================================================= */

void test_count_non_protocol_chars_plain(void)
{
    TEST_ASSERT_EQUAL_INT(5, count_non_protocol_chars("hello"));
}

void test_count_non_protocol_chars_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, count_non_protocol_chars(""));
}

void test_count_non_protocol_chars_newlines_skipped(void)
{
    TEST_ASSERT_EQUAL_INT(5, count_non_protocol_chars("\r\nhello"));
}

void test_count_non_protocol_chars_bracket_tag(void)
{
    /* "@[bold]hi" – "@[bold]" is a protocol tag; only "hi" counted */
    TEST_ASSERT_EQUAL_INT(2, count_non_protocol_chars("@[bold]hi"));
}

/* =========================================================
 * atoidx
 * ========================================================= */

void test_atoidx_valid_number(void)
{
    TEST_ASSERT_EQUAL_INT(42, (int)atoidx("42"));
}

void test_atoidx_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, (int)atoidx("0"));
}

void test_atoidx_negative_returns_nowhere(void)
{
    TEST_ASSERT_EQUAL_INT((int)NOWHERE, (int)atoidx("-1"));
}

void test_atoidx_overflow_returns_nowhere(void)
{
    /* IDXTYPE_MAX is 65535 on this build; a larger value overflows */
    TEST_ASSERT_EQUAL_INT((int)NOWHERE, (int)atoidx("99999999"));
}

/* =========================================================
 * right_trim_whitespace
 * ========================================================= */

void test_right_trim_whitespace_trailing_spaces(void)
{
    char *r = right_trim_whitespace("hello   ");
    TEST_ASSERT_EQUAL_STRING("hello", r);
    free(r);
}

void test_right_trim_whitespace_no_trailing(void)
{
    char *r = right_trim_whitespace("hello");
    TEST_ASSERT_EQUAL_STRING("hello", r);
    free(r);
}

void test_right_trim_whitespace_all_whitespace(void)
{
    char *r = right_trim_whitespace("   ");
    TEST_ASSERT_EQUAL_STRING("", r);
    free(r);
}

void test_right_trim_whitespace_empty(void)
{
    char *r = right_trim_whitespace("");
    TEST_ASSERT_EQUAL_STRING("", r);
    free(r);
}

/* =========================================================
 * remove_from_string
 * ========================================================= */

void test_remove_from_string_word_present(void)
{
    char s[] = "hello world";
    remove_from_string(s, "world");
    /* "world" and the trailing NUL shift left; "hello " remains */
    TEST_ASSERT_EQUAL_STRING("hello ", s);
}

void test_remove_from_string_word_absent(void)
{
    char s[] = "hello world";
    remove_from_string(s, "nope");
    TEST_ASSERT_EQUAL_STRING("hello world", s);
}

void test_remove_from_string_word_at_start(void)
{
    char s[] = "hello world";
    remove_from_string(s, "hello");
    /* "hello" removed; " world" remains */
    TEST_ASSERT_EQUAL_STRING(" world", s);
}

/* =========================================================
 * real_time_passed
 * ========================================================= */

void test_real_time_passed_hours(void)
{
    time_t base = 1000000;
    struct time_info_data *t = real_time_passed(base + 3 * SECS_PER_REAL_HOUR, base);
    TEST_ASSERT_EQUAL_INT(3, t->hours);
    TEST_ASSERT_EQUAL_INT(0, t->day);
}

void test_real_time_passed_days(void)
{
    time_t base = 1000000;
    struct time_info_data *t = real_time_passed(base + 2 * SECS_PER_REAL_DAY + SECS_PER_REAL_HOUR, base);
    TEST_ASSERT_EQUAL_INT(1, t->hours);
    TEST_ASSERT_EQUAL_INT(2, t->day);
}

/* =========================================================
 * mud_time_passed
 * ========================================================= */

void test_mud_time_passed_hours(void)
{
    time_t base = 1000000;
    struct time_info_data *t = mud_time_passed(base + 2 * SECS_PER_MUD_HOUR, base);
    TEST_ASSERT_EQUAL_INT(2, t->hours);
    TEST_ASSERT_EQUAL_INT(0, t->day);
    TEST_ASSERT_EQUAL_INT(0, t->month);
    TEST_ASSERT_EQUAL_INT(0, t->year);
}

void test_mud_time_passed_days(void)
{
    time_t base = 1000000;
    struct time_info_data *t = mud_time_passed(base + SECS_PER_MUD_DAY, base);
    TEST_ASSERT_EQUAL_INT(0, t->hours);
    TEST_ASSERT_EQUAL_INT(1, t->day);
    TEST_ASSERT_EQUAL_INT(0, t->month);
    TEST_ASSERT_EQUAL_INT(0, t->year);
}

void test_mud_time_passed_months(void)
{
    time_t base = 1000000;
    struct time_info_data *t = mud_time_passed(base + SECS_PER_MUD_MONTH, base);
    TEST_ASSERT_EQUAL_INT(0, t->hours);
    TEST_ASSERT_EQUAL_INT(0, t->day);
    TEST_ASSERT_EQUAL_INT(1, t->month);
    TEST_ASSERT_EQUAL_INT(0, t->year);
}

/* =========================================================
 * main
 * ========================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* prune_crlf */
    RUN_TEST(test_get_line_preserves_zone_header);
    RUN_TEST(test_get_line_discards_rest_of_overlong_line);
    RUN_TEST(test_get_line_reads_full_buffer_line_whole);
    RUN_TEST(test_prune_crlf_strips_crlf);
    RUN_TEST(test_prune_crlf_strips_lf_only);
    RUN_TEST(test_prune_crlf_no_op_on_clean);
    RUN_TEST(test_prune_crlf_multiple_trailing);

    /* str_cmp */
    RUN_TEST(test_str_cmp_equal_strings);
    RUN_TEST(test_str_cmp_case_insensitive);
    RUN_TEST(test_str_cmp_ordering_less);
    RUN_TEST(test_str_cmp_ordering_greater);
    RUN_TEST(test_str_cmp_empty_equal);

    /* strn_cmp */
    RUN_TEST(test_strn_cmp_equal_prefix);
    RUN_TEST(test_strn_cmp_differ_past_n);
    RUN_TEST(test_strn_cmp_differ_within_n);

    /* sprintbit */
    RUN_TEST(test_sprintbit_no_bits_set);
    RUN_TEST(test_sprintbit_single_bit);
    RUN_TEST(test_sprintbit_multiple_bits);
    RUN_TEST(test_sprintbit_undefined_bit);

    /* sprinttype */
    RUN_TEST(test_sprinttype_valid_index_zero);
    RUN_TEST(test_sprinttype_valid_index_one);
    RUN_TEST(test_sprinttype_out_of_range);

    /* levenshtein_distance */
    RUN_TEST(test_levenshtein_identical_strings);
    RUN_TEST(test_levenshtein_empty_and_nonempty);
    RUN_TEST(test_levenshtein_single_insertion);
    RUN_TEST(test_levenshtein_single_deletion);
    RUN_TEST(test_levenshtein_single_substitution);
    RUN_TEST(test_levenshtein_both_empty);
    RUN_TEST(test_levenshtein_null_input_is_rejected);

    /* file helpers */
    RUN_TEST(test_file_sizeof_counts_exact_bytes);
    RUN_TEST(test_file_numlines_counts_newlines_only);

    /* count_color_chars */
    RUN_TEST(test_count_color_chars_no_codes);
    RUN_TEST(test_count_color_chars_empty);
    RUN_TEST(test_count_color_chars_single_color_code);
    RUN_TEST(test_count_color_chars_double_tab);
    RUN_TEST(test_count_color_chars_mixed);
    RUN_TEST(test_count_color_chars_trailing_lone_tab_stays_in_bounds);

    /* count_non_protocol_chars */
    RUN_TEST(test_count_non_protocol_chars_plain);
    RUN_TEST(test_count_non_protocol_chars_empty);
    RUN_TEST(test_count_non_protocol_chars_newlines_skipped);
    RUN_TEST(test_count_non_protocol_chars_bracket_tag);

    /* atoidx */
    RUN_TEST(test_atoidx_valid_number);
    RUN_TEST(test_atoidx_zero);
    RUN_TEST(test_atoidx_negative_returns_nowhere);
    RUN_TEST(test_atoidx_overflow_returns_nowhere);

    /* right_trim_whitespace */
    RUN_TEST(test_right_trim_whitespace_trailing_spaces);
    RUN_TEST(test_right_trim_whitespace_no_trailing);
    RUN_TEST(test_right_trim_whitespace_all_whitespace);
    RUN_TEST(test_right_trim_whitespace_empty);

    /* remove_from_string */
    RUN_TEST(test_remove_from_string_word_present);
    RUN_TEST(test_remove_from_string_word_absent);
    RUN_TEST(test_remove_from_string_word_at_start);

    /* real_time_passed */
    RUN_TEST(test_real_time_passed_hours);
    RUN_TEST(test_real_time_passed_days);

    /* mud_time_passed */
    RUN_TEST(test_mud_time_passed_hours);
    RUN_TEST(test_mud_time_passed_days);
    RUN_TEST(test_mud_time_passed_months);

    return UNITY_END();
}
