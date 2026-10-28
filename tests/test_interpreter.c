/**
 * @file test_interpreter.c
 * Unit tests for pure string-handling functions in src/interpreter.c:
 *   is_number, is_abbrev, delete_doubledollar, any_one_arg, one_word
 * and for the alias expansion path perform_complex_alias() (via perform_alias()).
 */

#include "unity.h"
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"

extern FILE *logfile;

/* utils.c owns tb_error_log_at(); the link wraps it (--wrap) so the tests can
 * assert which structured diagnostic the code under test emitted. */
static int captured_errors;
static char captured_operation[64];
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func; (void)detail_fmt;
  captured_errors++;
  strlcpy(captured_operation, operation ? operation : "",
          sizeof(captured_operation));
}

void setUp(void)
{
  logfile = stderr;
  captured_errors = 0;
  *captured_operation = '\0';
}
void tearDown(void) { logfile = NULL; }

/* =========================================================
 * write_to_q — real implementation for alias tests.
 * Overrides the __attribute__((weak)) stub in test_stubs.c so that
 * the queue is actually populated and we can inspect its contents.
 * ========================================================= */

int write_to_q(const char *txt, struct txt_q *queue, int aliased)
{
    struct txt_block *newt;
    CREATE(newt, struct txt_block, 1);
    newt->text    = strdup(txt);
    newt->aliased = aliased;
    if (!queue->head) {
        newt->next  = NULL;
        queue->head = queue->tail = newt;
    } else {
        queue->tail->next = newt;
        queue->tail       = newt;
        newt->next        = NULL;
    }
    return 1;
}

/* =========================================================
 * is_number
 * ========================================================= */

void test_is_number_digits_only(void)
{
    TEST_ASSERT_TRUE(is_number("42"));
}

void test_is_number_zero(void)
{
    TEST_ASSERT_TRUE(is_number("0"));
}

void test_is_number_negative(void)
{
    TEST_ASSERT_TRUE(is_number("-5"));
}

void test_is_number_empty_string(void)
{
    TEST_ASSERT_FALSE(is_number(""));
}

void test_is_number_contains_letter(void)
{
    TEST_ASSERT_FALSE(is_number("12x3"));
}

void test_is_number_minus_only(void)
{
    TEST_ASSERT_FALSE(is_number("-"));
}

void test_is_number_float(void)
{
    TEST_ASSERT_FALSE(is_number("3.14"));
}

/* =========================================================
 * is_abbrev
 * ========================================================= */

void test_is_abbrev_exact_match(void)
{
    TEST_ASSERT_TRUE(is_abbrev("north", "north"));
}

void test_is_abbrev_valid_prefix(void)
{
    TEST_ASSERT_TRUE(is_abbrev("nort", "north"));
    TEST_ASSERT_TRUE(is_abbrev("n", "north"));
}

void test_is_abbrev_non_prefix(void)
{
    TEST_ASSERT_FALSE(is_abbrev("south", "north"));
}

void test_is_abbrev_empty_arg1(void)
{
    TEST_ASSERT_FALSE(is_abbrev("", "north"));
}

void test_is_abbrev_arg1_longer_than_arg2(void)
{
    TEST_ASSERT_FALSE(is_abbrev("northward", "north"));
}

void test_is_abbrev_case_insensitive(void)
{
    TEST_ASSERT_TRUE(is_abbrev("Nor", "north"));
    TEST_ASSERT_TRUE(is_abbrev("NOR", "NORTH"));
}

/* =========================================================
 * delete_doubledollar
 * ========================================================= */

void test_delete_doubledollar_no_dollars(void)
{
    char s[] = "hello";
    delete_doubledollar(s);
    TEST_ASSERT_EQUAL_STRING("hello", s);
}

void test_delete_doubledollar_double_at_start(void)
{
    char s[] = "$$hello";
    delete_doubledollar(s);
    TEST_ASSERT_EQUAL_STRING("$hello", s);
}

void test_delete_doubledollar_double_in_middle(void)
{
    char s[] = "hello$$world";
    delete_doubledollar(s);
    TEST_ASSERT_EQUAL_STRING("hello$world", s);
}

void test_delete_doubledollar_four_dollars(void)
{
    char s[] = "$$$$";
    delete_doubledollar(s);
    TEST_ASSERT_EQUAL_STRING("$$", s);
}

void test_delete_doubledollar_single_dollar_unchanged(void)
{
    char s[] = "hello$world";
    delete_doubledollar(s);
    TEST_ASSERT_EQUAL_STRING("hello$world", s);
}

/* =========================================================
 * any_one_arg
 * ========================================================= */

void test_any_one_arg_basic(void)
{
    char first[64];
    char *rest = any_one_arg("hello world", first);
    TEST_ASSERT_EQUAL_STRING("hello", first);
    TEST_ASSERT_EQUAL_STRING(" world", rest);
}

void test_any_one_arg_leading_spaces(void)
{
    char first[64];
    any_one_arg("  hello world", first);
    TEST_ASSERT_EQUAL_STRING("hello", first);
}

void test_any_one_arg_single_word(void)
{
    char first[64];
    char *rest = any_one_arg("hello", first);
    TEST_ASSERT_EQUAL_STRING("hello", first);
    TEST_ASSERT_EQUAL_STRING("", rest);
}

void test_any_one_arg_empty_string(void)
{
    char first[64];
    any_one_arg("", first);
    TEST_ASSERT_EQUAL_STRING("", first);
}

void test_any_one_arg_lowercases(void)
{
    char first[64];
    any_one_arg("HELLO", first);
    TEST_ASSERT_EQUAL_STRING("hello", first);
}

/* =========================================================
 * one_word
 * ========================================================= */

void test_one_word_unquoted_like_any_one_arg(void)
{
    char first[64];
    char *rest = one_word("hello world", first);
    TEST_ASSERT_EQUAL_STRING("hello", first);
    TEST_ASSERT_EQUAL_STRING(" world", rest);
}

void test_one_word_quoted_string(void)
{
    char first[64];
    char *rest = one_word("\"hello world\" rest", first);
    TEST_ASSERT_EQUAL_STRING("hello world", first);
    /* rest points just past the closing quote */
    TEST_ASSERT_EQUAL_STRING(" rest", rest);
}

void test_one_word_empty_quoted(void)
{
    char first[64];
    one_word("\"\" rest", first);
    TEST_ASSERT_EQUAL_STRING("", first);
}

void test_one_word_leading_spaces_skipped(void)
{
    char first[64];
    one_word("  hello", first);
    TEST_ASSERT_EQUAL_STRING("hello", first);
}

/* The splitters fill MAX_INPUT_LENGTH-byte buffers, but script-forced
 * commands can carry longer words. Exact-size heap buffers make ASan report
 * any write or read beyond the contract. */
void test_any_one_arg_truncates_overlong_word(void)
{
    size_t len = MAX_INPUT_LENGTH * 2;
    char *input = malloc(len + 1);
    char *first = malloc(MAX_INPUT_LENGTH);
    TEST_ASSERT_NOT_NULL(input);
    TEST_ASSERT_NOT_NULL(first);
    memset(input, 'w', len);
    input[len] = '\0';
    char *rest = any_one_arg(input, first);
    TEST_ASSERT_EQUAL_size_t(MAX_INPUT_LENGTH - 1, strlen(first));
    /* The whole word is consumed, not split into a second argument. */
    TEST_ASSERT_EQUAL_STRING("", rest);
    free(first);
    free(input);
}

void test_one_word_unclosed_quote_stops_at_terminator(void)
{
    char *input = strdup("\"never closed");
    char first[MAX_INPUT_LENGTH];
    TEST_ASSERT_NOT_NULL(input);
    char *rest = one_word(input, first);
    TEST_ASSERT_EQUAL_STRING("never closed", first);
    TEST_ASSERT_TRUE(rest == input + strlen(input));
    free(input);
}

/* =========================================================
 * Helpers for perform_complex_alias tests
 * ========================================================= */

/* Release every txt_block in a queue and reset head/tail to NULL. */
static void free_txt_q(struct txt_q *q)
{
    struct txt_block *b = q->head, *n;
    while (b) { n = b->next; free(b->text); free(b); b = n; }
    q->head = q->tail = NULL;
}

/* Build a heap-allocated alias_data with type ALIAS_COMPLEX. */
static struct alias_data *make_test_alias(const char *name, const char *repl)
{
    struct alias_data *a;
    CREATE(a, struct alias_data, 1);
    a->alias       = strdup(name);
    a->replacement = strdup(repl);
    a->type        = ALIAS_COMPLEX;
    a->next        = NULL;
    return a;
}

/* Free a single alias_data allocated by make_test_alias(). */
static void destroy_test_alias(struct alias_data *a)
{
    free(a->alias);
    free(a->replacement);
    free(a);
}

/* Zero-initialise all three objects and wire them together so that
 * IS_NPC() returns false and GET_ALIASES() returns a. */
static void alias_env_init(struct descriptor_data     *d,
                           struct char_data            *ch,
                           struct player_special_data  *psd,
                           struct alias_data           *a)
{
    memset(d,   0, sizeof(*d));
    memset(ch,  0, sizeof(*ch));
    memset(psd, 0, sizeof(*psd));
    ch->player_specials = psd;
    psd->aliases        = a;
    d->character        = ch;
}

/* =========================================================
 * perform_complex_alias — tested via the public perform_alias()
 * ========================================================= */

/* Literal replacement with no variable tokens. */
void test_pca_literal(void)
{
    struct alias_data         *a = make_test_alias("lit", "hello world");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "lit";

    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("hello world", d.input.head->text);
    TEST_ASSERT_NULL(d.input.head->next);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* $* glob expands to the full argument string after the alias trigger. */
void test_pca_glob_expansion(void)
{
    struct alias_data         *a = make_test_alias("say", "say $*");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "say hello there";

    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("say hello there", d.input.head->text);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* $1 expands to the first whitespace-delimited token of the arguments. */
void test_pca_token1_expansion(void)
{
    struct alias_data         *a = make_test_alias("s1", "say $1");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "s1 hello world";

    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("say hello", d.input.head->text);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* Semicolon separator (;) produces two independent queue entries. */
void test_pca_separator(void)
{
    struct alias_data         *a = make_test_alias("seq", "go north;go east");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "seq";

    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("go north", d.input.head->text);
    TEST_ASSERT_NOT_NULL(d.input.head->next);
    TEST_ASSERT_EQUAL_STRING("go east", d.input.head->next->text);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* $$ in the replacement is preserved as $$ in the output (act-safety doubling). */
void test_pca_dollar_dollar(void)
{
    struct alias_data         *a = make_test_alias("dol", "cost $$5");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "dol";

    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("cost $$5", d.input.head->text);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* A replacement whose last character is a bare '$' leaves the scan on the
 * terminator, and the walk used to step over it and carry on through
 * whatever followed in memory.  These two build the replacement with known
 * bytes after its terminator, so the over-read has a definite result rather
 * than an accidental one: a ';' out there opens a second command, and the
 * queue is where it shows up. */
static void set_replacement_with_trailing_bytes(struct alias_data *a,
                                                const char *beyond)
{
    size_t len = strlen(a->replacement), tail = strlen(beyond);
    char *buf;

    CREATE(buf, char, len + 1 + tail + 1);
    memcpy(buf, a->replacement, len + 1);
    memcpy(buf + len + 1, beyond, tail + 1);
    free(a->replacement);
    a->replacement = buf;
}

void test_pca_trailing_dollar_stops_at_the_terminator(void)
{
    struct alias_data         *a = make_test_alias("f", "say hi$");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "f";

    set_replacement_with_trailing_bytes(a, ";shout boo");
    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("say hi", d.input.head->text);
    TEST_ASSERT_NULL(d.input.head->next);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

void test_pca_trailing_dollar_keeps_earlier_segments(void)
{
    struct alias_data         *a = make_test_alias("seq", "go north;go east$");
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    char orig[] = "seq";

    set_replacement_with_trailing_bytes(a, ";shout boo");
    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    TEST_ASSERT_NOT_NULL(d.input.head);
    TEST_ASSERT_EQUAL_STRING("go north", d.input.head->text);
    TEST_ASSERT_NOT_NULL(d.input.head->next);
    TEST_ASSERT_EQUAL_STRING("go east", d.input.head->next->text);
    TEST_ASSERT_NULL(d.input.head->next->next);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* Overflow via $*: 255 "$*" tokens × 50-char argument exceeds
 * MAX_RAW_INPUT_LENGTH.  The queue must be empty after the call. */
void test_pca_overflow_glob(void)
{
    /* 255 × "$*" = 510 bytes + NUL */
    char repl[511];
    char orig[64];
    struct alias_data         *a;
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    int i;

    for (i = 0; i < 255; i++) { repl[i * 2] = '$'; repl[i * 2 + 1] = '*'; }
    repl[510] = '\0';

    /* "al " + 50 'x' chars */
    memcpy(orig, "al ", 3);
    memset(orig + 3, 'x', 50);
    orig[53] = '\0';

    a = make_test_alias("al", repl);
    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    /* No partial results must leak into the queue on overflow. */
    TEST_ASSERT_NULL(d.input.head);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* Overflow via $1: 255 "$1" tokens × 50-char first token exceeds
 * MAX_RAW_INPUT_LENGTH.  The queue must be empty after the call. */
void test_pca_overflow_token(void)
{
    /* 255 × "$1" = 510 bytes + NUL */
    char repl[511];
    char orig[64];
    struct alias_data         *a;
    struct descriptor_data     d;
    struct char_data           ch;
    struct player_special_data psd;
    int i;

    for (i = 0; i < 255; i++) { repl[i * 2] = '$'; repl[i * 2 + 1] = '1'; }
    repl[510] = '\0';

    /* "al " + 50 'y' chars */
    memcpy(orig, "al ", 3);
    memset(orig + 3, 'y', 50);
    orig[53] = '\0';

    a = make_test_alias("al", repl);
    alias_env_init(&d, &ch, &psd, a);
    perform_alias(&d, orig, sizeof(orig));

    /* No partial results must leak into the queue on overflow. */
    TEST_ASSERT_NULL(d.input.head);

    free_txt_q(&d.input);
    destroy_test_alias(a);
}

/* =========================================================
 * nanny CON_NAME_CNFRM -> perform_new_char_dupe_check (finding A2-01)
 *
 * Any unauthenticated client can sit at the name prompt with a character
 * whose player.name is still NULL. Confirming a new name on another socket
 * must not strcmp() that NULL name.
 * ========================================================= */

struct dupe_fixture {
  struct descriptor_data d, k;
  struct char_data dch, kch;
  struct player_special_data dps, kps;
};

static void dupe_fixture_init(struct dupe_fixture *f, int peer_state)
{
  memset(f, 0, sizeof(*f));
  f->dch.player_specials = &f->dps;
  f->kch.player_specials = &f->kps;
  GET_IDNUM(&f->dch) = -1;
  GET_IDNUM(&f->kch) = -1;
  f->dch.player.name = strdup("Zorblaxe");
  f->dch.desc = &f->d;
  f->kch.desc = &f->k;          /* peer: name not entered yet (NULL) */
  f->d.character = &f->dch;
  f->k.character = &f->kch;
  f->d.desc_num = 2;
  f->k.desc_num = 1;
  STATE(&f->d) = CON_NAME_CNFRM;
  STATE(&f->k) = peer_state;
  f->k.next = &f->d;
  descriptor_list = &f->k;
}

static void dupe_fixture_free(struct dupe_fixture *f)
{
  descriptor_list = NULL;
  free(f->dch.player.name);
}

void test_new_char_dupe_check_skips_peer_still_at_name_prompt(void)
{
  struct dupe_fixture f;
  char arg[] = "y";

  dupe_fixture_init(&f, CON_GET_NAME);
  nanny(&f.d, arg);

  TEST_ASSERT_EQUAL_INT(CON_NEWPASSWD, STATE(&f.d));
  TEST_ASSERT_EQUAL_INT(CON_GET_NAME, STATE(&f.k));
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
  dupe_fixture_free(&f);
}

void test_new_char_dupe_check_reports_unnamed_peer_past_confirmation(void)
{
  struct dupe_fixture f;
  char arg[] = "y";

  dupe_fixture_init(&f, CON_NEWPASSWD);
  nanny(&f.d, arg);

  TEST_ASSERT_EQUAL_INT(CON_NEWPASSWD, STATE(&f.d));
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("new-char-dupe-check", captured_operation);
  dupe_fixture_free(&f);
}

void test_new_char_dupe_check_ignores_name_case(void)
{
  struct dupe_fixture f;
  char arg[] = "y";

  dupe_fixture_init(&f, CON_QSEX);
  f.kch.player.name = strdup("ZORBLAXE");
  nanny(&f.d, arg);

  /* The older in-creation socket is booted; the new one proceeds. */
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.k));
  TEST_ASSERT_EQUAL_INT(CON_NEWPASSWD, STATE(&f.d));
  free(f.kch.player.name);
  dupe_fixture_free(&f);
}

void test_new_char_dupe_check_in_game_twin_closes_without_null_deref(void)
{
  struct dupe_fixture f;
  char arg[] = "y";
  char *dname;

  dupe_fixture_init(&f, CON_PLAYING);
  f.kch.player.name = strdup("Zorblaxe");
  dname = f.dch.player.name;
  nanny(&f.d, arg);

  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.k));
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.d));
  TEST_ASSERT_NULL(f.d.character);
  free(f.kch.player.name);
  f.dch.player.name = dname;
  dupe_fixture_free(&f);
}

/* =========================================================
 * sort_commands
 * ========================================================= */

/* F2-17: sort_commands runs at boot and again after every aedit save; a
 * second call must replace cmd_sort_info, not leak the first array
 * (LeakSanitizer / Valgrind report the leak if it does). */
static void test_sort_commands_twice_does_not_leak(void)
{
    struct command_info cmds[] = {
        { "RESERVED", "", 0, 0, 0, 0 },
        { "zap", "zap", 0, 0, 0, 0 },
        { "apple", "apple", 0, 0, 0, 0 },
        { "\n", "zzzzzzz", 0, 0, 0, 0 }
    };
    struct command_info *saved = complete_cmd_info;

    complete_cmd_info = cmds;
    sort_commands();
    sort_commands();
    TEST_ASSERT_NOT_NULL(cmd_sort_info);
    TEST_ASSERT_EQUAL_INT(0, cmd_sort_info[0]);
    TEST_ASSERT_EQUAL_INT(2, cmd_sort_info[1]);
    TEST_ASSERT_EQUAL_INT(1, cmd_sort_info[2]);
    TEST_ASSERT_EQUAL_INT(3, cmd_sort_info[3]);
    free(cmd_sort_info);
    cmd_sort_info = NULL;
    complete_cmd_info = saved;
}

/* =========================================================
 * main
 * ========================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* is_number */
    RUN_TEST(test_is_number_digits_only);
    RUN_TEST(test_is_number_zero);
    RUN_TEST(test_is_number_negative);
    RUN_TEST(test_is_number_empty_string);
    RUN_TEST(test_is_number_contains_letter);
    RUN_TEST(test_is_number_minus_only);
    RUN_TEST(test_is_number_float);

    /* is_abbrev */
    RUN_TEST(test_is_abbrev_exact_match);
    RUN_TEST(test_is_abbrev_valid_prefix);
    RUN_TEST(test_is_abbrev_non_prefix);
    RUN_TEST(test_is_abbrev_empty_arg1);
    RUN_TEST(test_is_abbrev_arg1_longer_than_arg2);
    RUN_TEST(test_is_abbrev_case_insensitive);

    /* delete_doubledollar */
    RUN_TEST(test_delete_doubledollar_no_dollars);
    RUN_TEST(test_delete_doubledollar_double_at_start);
    RUN_TEST(test_delete_doubledollar_double_in_middle);
    RUN_TEST(test_delete_doubledollar_four_dollars);
    RUN_TEST(test_delete_doubledollar_single_dollar_unchanged);

    /* any_one_arg */
    RUN_TEST(test_any_one_arg_basic);
    RUN_TEST(test_any_one_arg_leading_spaces);
    RUN_TEST(test_any_one_arg_single_word);
    RUN_TEST(test_any_one_arg_empty_string);
    RUN_TEST(test_any_one_arg_lowercases);

    /* one_word */
    RUN_TEST(test_one_word_unquoted_like_any_one_arg);
    RUN_TEST(test_one_word_quoted_string);
    RUN_TEST(test_one_word_empty_quoted);
    RUN_TEST(test_one_word_leading_spaces_skipped);
    RUN_TEST(test_any_one_arg_truncates_overlong_word);
    RUN_TEST(test_one_word_unclosed_quote_stops_at_terminator);

    /* perform_complex_alias */
    RUN_TEST(test_pca_literal);
    RUN_TEST(test_pca_glob_expansion);
    RUN_TEST(test_pca_token1_expansion);
    RUN_TEST(test_pca_separator);
    RUN_TEST(test_pca_dollar_dollar);
    RUN_TEST(test_pca_trailing_dollar_stops_at_the_terminator);
    RUN_TEST(test_pca_trailing_dollar_keeps_earlier_segments);
    RUN_TEST(test_pca_overflow_glob);
    RUN_TEST(test_pca_overflow_token);

    /* nanny / perform_new_char_dupe_check */
    RUN_TEST(test_new_char_dupe_check_skips_peer_still_at_name_prompt);
    RUN_TEST(test_new_char_dupe_check_reports_unnamed_peer_past_confirmation);
    RUN_TEST(test_new_char_dupe_check_ignores_name_case);
    RUN_TEST(test_new_char_dupe_check_in_game_twin_closes_without_null_deref);

    RUN_TEST(test_sort_commands_twice_does_not_leak);

    return UNITY_END();
}
