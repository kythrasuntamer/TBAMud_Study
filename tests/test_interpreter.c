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
#include "dg_scripts.h"
#include "db.h"

#include <sodium.h>
#include <stdarg.h>

extern FILE *logfile;

/* utils.c owns tb_error_log_at(); the link wraps it (--wrap) so the tests can
 * assert which structured diagnostic the code under test emitted. */
static int captured_errors;
static char captured_operation[64];
static char captured_detail[512];
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
{
  va_list args;

  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func;
  va_start(args, detail_fmt);
  if (vsnprintf(captured_detail, sizeof(captured_detail), detail_fmt, args) < 0)
    *captured_detail = '\0';
  va_end(args);
  captured_errors++;
  strlcpy(captured_operation, operation ? operation : "",
          sizeof(captured_operation));
}

void setUp(void)
{
  logfile = stderr;
  captured_errors = 0;
  *captured_operation = '\0';
  *captured_detail = '\0';
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
 * perform_new_char_dupe_check: booted characters are freed
 *
 * Booting a socket set its character pointer to NULL without free_char(),
 * and close_socket() frees nothing once d->character is NULL: a character
 * still in creation, a twin at the menu, and the new character on d itself
 * when both sockets were closed were all leaked. A twin in the game stays
 * in the world, linkless.
 * ========================================================= */

/* free_char() for these tests (overrides the weak stub): a character made
 * by heap_char() is really freed and forgotten, so ASan sees any later use
 * and a test can tell which characters the code released. The stack
 * characters of the fixture above are only counted. */
enum { MAX_HEAP_CHARS = 4 };
static struct char_data *heap_chars[MAX_HEAP_CHARS];
static int free_char_calls;

void free_char(struct char_data *ch)
{
  int i;

  free_char_calls++;
  for (i = 0; i < MAX_HEAP_CHARS; i++)
    if (heap_chars[i] == ch) {
      heap_chars[i] = NULL;
      free(ch->player.name);
      free(ch->player_specials);
      free(ch);
      return;
    }
}

static struct char_data *heap_char(const char *name)
{
  struct char_data *ch = calloc(1, sizeof(*ch));
  int i;

  TEST_ASSERT_NOT_NULL(ch);
  ch->player_specials = calloc(1, sizeof(*ch->player_specials));
  TEST_ASSERT_NOT_NULL(ch->player_specials);
  ch->player.name = strdup(name);
  TEST_ASSERT_NOT_NULL(ch->player.name);
  GET_IDNUM(ch) = -1;
  for (i = 0; i < MAX_HEAP_CHARS && heap_chars[i]; i++)
    ;
  TEST_ASSERT_TRUE(i < MAX_HEAP_CHARS);
  heap_chars[i] = ch;
  return ch;
}

/* Compares addresses only: a released character is never dereferenced. */
static int heap_char_live(const struct char_data *ch)
{
  int i;

  for (i = 0; i < MAX_HEAP_CHARS; i++)
    if (heap_chars[i] == ch)
      return 1;
  return 0;
}

static void free_heap_chars(void)
{
  int i;

  for (i = 0; i < MAX_HEAP_CHARS; i++)
    if (heap_chars[i])
      free_char(heap_chars[i]);
}

/* d confirms the new name "Zorblaxe"; k already has a character by that
 * name, in peer_state. */
struct heap_dupe {
  struct descriptor_data d, k;
  struct char_data *dch, *kch;
};

static void setup_heap_dupe(struct heap_dupe *f, int peer_state)
{
  memset(f, 0, sizeof(*f));
  f->dch = heap_char("Zorblaxe");
  f->kch = heap_char("Zorblaxe");
  f->dch->desc = &f->d;
  f->d.character = f->dch;
  f->kch->desc = &f->k;
  f->k.character = f->kch;
  f->d.desc_num = 2;
  f->k.desc_num = 1;
  STATE(&f->d) = CON_NAME_CNFRM;
  STATE(&f->k) = peer_state;
  f->k.next = &f->d;
  descriptor_list = &f->k;
  free_char_calls = 0;
}

/* d answers 'y' to confirm its new name. */
static void finish_heap_dupe(struct heap_dupe *f)
{
  char arg[] = "y";

  nanny(&f->d, arg);
  descriptor_list = NULL;
}

static void run_heap_dupe(struct heap_dupe *f, int peer_state)
{
  setup_heap_dupe(f, peer_state);
  finish_heap_dupe(f);
}

void test_new_char_dupe_check_frees_the_booted_creation_character(void)
{
  struct heap_dupe f;

  run_heap_dupe(&f, CON_QSEX);
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.k));
  TEST_ASSERT_NULL(f.k.character);
  TEST_ASSERT_FALSE(heap_char_live(f.kch));
  TEST_ASSERT_EQUAL_INT(CON_NEWPASSWD, STATE(&f.d));   /* the new one goes on */
  TEST_ASSERT_TRUE(heap_char_live(f.dch));
  free_heap_chars();
}

void test_new_char_dupe_check_closing_both_keeps_only_the_in_game_twin(void)
{
  struct heap_dupe f;

  run_heap_dupe(&f, CON_PLAYING);
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.d));
  TEST_ASSERT_NULL(f.d.character);
  TEST_ASSERT_FALSE(heap_char_live(f.dch));            /* still in creation */
  TEST_ASSERT_TRUE(heap_char_live(f.kch));             /* in the world */
  TEST_ASSERT_NULL(f.kch->desc);
  free_heap_chars();
}

void test_new_char_dupe_check_closing_both_frees_a_twin_at_the_menu(void)
{
  struct heap_dupe f;

  run_heap_dupe(&f, CON_MENU);
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.d));
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.k));
  TEST_ASSERT_FALSE(heap_char_live(f.dch));
  TEST_ASSERT_FALSE(heap_char_live(f.kch));
  TEST_ASSERT_EQUAL_INT(2, free_char_calls);
  free_heap_chars();
}

/* A twin booted at the menu's description editor: k->str points into the
 * character "boot both" frees, and string_add() still runs k's queued
 * input this pulse (k stays in descriptor_list until the close pass), so
 * it wrote into freed memory. k's editor buffers are released first, as
 * perform_dupe_check() does (fd3ae09). */
void test_new_char_dupe_check_closing_both_detaches_the_description_editor(void)
{
  struct heap_dupe f;

  setup_heap_dupe(&f, CON_PLR_DESC);
  /* As nanny's menu option 2 leaves it. */
  f.k.str = &f.kch->player.description;
  f.k.backstr = strdup("old description");
  TEST_ASSERT_NOT_NULL(f.k.backstr);
  finish_heap_dupe(&f);
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&f.k));
  TEST_ASSERT_FALSE(heap_char_live(f.kch));
  TEST_ASSERT_NULL(f.k.str);        /* no pointer into the freed twin */
  TEST_ASSERT_NULL(f.k.backstr);    /* released, as close_socket() would */
  free_heap_chars();
}

/* "Boot the older one" also takes CON_CLOSE, which an immortal's dc gives a
 * socket at the description editor while k->str still points into its
 * character: released the same way before that character is freed. */
void test_new_char_dupe_check_booting_a_closed_editor_detaches_it(void)
{
  struct heap_dupe f;

  setup_heap_dupe(&f, CON_CLOSE);
  f.k.str = &f.kch->player.description;
  f.k.backstr = strdup("old description");
  TEST_ASSERT_NOT_NULL(f.k.backstr);
  finish_heap_dupe(&f);
  TEST_ASSERT_FALSE(heap_char_live(f.kch));
  TEST_ASSERT_NULL(f.k.str);
  TEST_ASSERT_NULL(f.k.backstr);
  TEST_ASSERT_EQUAL_INT(CON_NEWPASSWD, STATE(&f.d));   /* the new one goes on */
  free_heap_chars();
}

/* =========================================================
 * command_interpreter: switched-IMPL script commands (finding A2-07)
 *
 * An NPC has a descriptor only while an immortal is switched into it, and
 * do_switch sets desc->original. The IMPL check read
 * GET_LEVEL(ch->desc->original) without testing original.
 * ========================================================= */

/* mecho is only in the mob script table, so a call means the switched-IMPL
 * path ran. Overrides the weak stub in test_stubs.c. */
static int mecho_calls;
ACMD(do_mecho)
{
  (void)ch; (void)argument; (void)cmd; (void)subcmd;
  mecho_calls++;
}

/* An NPC with a descriptor whose original is @p original types "mecho". */
static void run_switched_mecho(struct char_data *original)
{
  struct char_data mob;
  struct descriptor_data d;
  char line[] = "mecho hello";

  memset(&mob, 0, sizeof(mob));
  memset(&d, 0, sizeof(d));
  SET_BIT_AR(MOB_FLAGS(&mob), MOB_ISNPC);
  mob.nr = NOBODY;
  GET_POS(&mob) = POS_STANDING;
  mob.desc = &d;
  d.character = &mob;
  d.original = original;
  d.desc_num = 5;
  mecho_calls = 0;
  complete_cmd_info = (struct command_info *) cmd_info;
  command_interpreter(&mob, line);
  complete_cmd_info = NULL;
}

void test_command_interpreter_switched_impl_runs_script_commands(void)
{
  struct char_data imm;
  struct player_special_data specials;

  memset(&imm, 0, sizeof(imm));
  memset(&specials, 0, sizeof(specials));
  imm.player_specials = &specials;
  GET_LEVEL(&imm) = LVL_IMPL;
  run_switched_mecho(&imm);
  TEST_ASSERT_EQUAL_INT(1, mecho_calls);
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
}

void test_command_interpreter_npc_descriptor_without_original_is_reported(void)
{
  run_switched_mecho(NULL);
  TEST_ASSERT_EQUAL_INT(0, mecho_calls);
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("switch-original", captured_operation);
}

/* =========================================================
 * main
 * ========================================================= */

/* =========================================================
 * perform_dupe_check: a duplicate in the world is extracted in the mortal
 * start room, found by vnum
 *
 * It used char_to_room(ch, 1): rnum 1 is whatever room OLC shifted into
 * that slot, or past top_of_world, where char_to_room() refuses and the
 * duplicate is left in NOWHERE.
 * ========================================================= */

/* Room primitives with handler.c's contract (override the weak stubs):
 * char_from_room() aborts on a character in no valid room, char_to_room()
 * refuses an invalid room and leaves its character in NOWHERE. */
static struct room_data dupe_rooms[3];
static struct char_data *extracted_char;
static room_rnum extracted_in;

room_rnum real_room(room_vnum vnum)
{
  room_rnum i;

  for (i = 0; world && i <= top_of_world; i++)
    if (world[i].number == vnum)
      return i;
  return NOWHERE;
}
void char_from_room_at(const char *src_file, int src_line,
                       const char *src_func, struct char_data *ch)
{
  (void)src_file; (void)src_line; (void)src_func;
  TEST_ASSERT_TRUE_MESSAGE(VALID_ROOM_RNUM(IN_ROOM(ch)),
                           "char_from_room on a character in no valid room "
                           "(production aborts)");
  IN_ROOM(ch) = NOWHERE;
}
void char_to_room_at(const char *src_file, int src_line, const char *src_func,
                     struct char_data *ch, room_rnum room)
{
  (void)src_file; (void)src_line; (void)src_func;
  if (VALID_ROOM_RNUM(room))
    IN_ROOM(ch) = room;
}
void extract_char(struct char_data *ch)
{
  extracted_char = ch;
  extracted_in = IN_ROOM(ch);
}

/* d logs in as idnum 31 with the right password, typed at CON_PASSWORD. */
static const char dupe_password[] = "correct horse";
static struct char_data *password_login(struct descriptor_data *d)
{
  struct char_data *dch = heap_char("Twinnie");
  char hash[crypto_pwhash_STRBYTES];

  TEST_ASSERT_TRUE(sodium_init() >= 0);
  /* Minimal argon2id parameters: verifying costs microseconds, not the
   * production work factor. */
  TEST_ASSERT_EQUAL_INT(0, crypto_pwhash_str_alg(hash, dupe_password,
                        strlen(dupe_password), crypto_pwhash_OPSLIMIT_MIN,
                        crypto_pwhash_MEMLIMIT_MIN, crypto_pwhash_ALG_ARGON2ID13));
  strlcpy(GET_PASSWD(dch), hash, sizeof(dch->player.passwd));
  GET_IDNUM(dch) = 31;
  memset(d, 0, sizeof(*d));
  d->character = dch;
  dch->desc = d;
  d->desc_num = 2;
  STATE(d) = CON_PASSWORD;
  strlcpy(d->host, "127.0.0.1", sizeof(d->host));
  return dch;
}

/* Two linkless copies of idnum 31 in the world, both in rnum 0: the first
 * is reconnected to, the second is the duplicate. */
static struct char_data twin1, twin2;
static struct player_special_data twin1_ps, twin2_ps;

static void dupe_world(room_vnum start)
{
  memset(dupe_rooms, 0, sizeof(dupe_rooms));
  dupe_rooms[0].number = 100;
  dupe_rooms[1].number = 101;           /* rnum 1: not the start room */
  dupe_rooms[2].number = 3001;
  world = dupe_rooms;
  top_of_world = 2;
  CONFIG_MORTAL_START = start;
  memset(&twin1, 0, sizeof(twin1));
  memset(&twin2, 0, sizeof(twin2));
  memset(&twin1_ps, 0, sizeof(twin1_ps));
  memset(&twin2_ps, 0, sizeof(twin2_ps));
  twin1.player_specials = &twin1_ps;
  twin2.player_specials = &twin2_ps;
  twin1.player.name = twin2.player.name = (char *)"Twinnie";
  GET_IDNUM(&twin1) = GET_IDNUM(&twin2) = 31;
  IN_ROOM(&twin1) = IN_ROOM(&twin2) = 0;
  twin1.next = &twin2;
  character_list = &twin1;
  extracted_char = NULL;
  extracted_in = NOWHERE;
}

static void dupe_world_free(void)
{
  free(GET_HOST(&twin1));           /* the reconnect strdup()s d->host */
  character_list = NULL;
  descriptor_list = NULL;
  world = NULL;
  top_of_world = 0;
  CONFIG_MORTAL_START = 0;
  free_heap_chars();
}

void test_dupe_check_extracts_a_duplicate_in_the_start_room(void)
{
  struct descriptor_data d;
  char arg[sizeof(dupe_password)];

  dupe_world(3001);
  password_login(&d);
  descriptor_list = &d;
  strlcpy(arg, dupe_password, sizeof(arg));
  nanny(&d, arg);

  TEST_ASSERT_EQUAL_PTR(&twin1, d.character);   /* reconnected */
  TEST_ASSERT_EQUAL_PTR(&twin2, extracted_char);
  TEST_ASSERT_EQUAL_INT(2, extracted_in);       /* rnum of vnum 3001 */
  TEST_ASSERT_EQUAL_INT(0, captured_errors);
  dupe_world_free();
}

void test_dupe_check_without_a_start_room_is_reported(void)
{
  struct descriptor_data d;
  char arg[sizeof(dupe_password)];

  dupe_world(9999);                 /* no such room */
  password_login(&d);
  descriptor_list = &d;
  strlcpy(arg, dupe_password, sizeof(arg));
  nanny(&d, arg);

  TEST_ASSERT_EQUAL_PTR(&twin2, extracted_char);
  TEST_ASSERT_EQUAL_INT(0, extracted_in);       /* where it was */
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("dupe-no-start-room", captured_operation);
  dupe_world_free();
}

/* =========================================================
 * perform_dupe_check: a booted character at the menu is freed
 *
 * Logging in over a connection of the same player that sits at the menu
 * (after quit, rent or death) or another pre-game prompt booted that
 * connection with k->character = NULL and never freed its character,
 * which is in no list: ~9 KB leaked per login, by any player (e2e soak,
 * tests/e2e/repro_login_over_menu_leaks_char.txt on audit-e2e-gameplay).
 * An in-game copy is still taken over, not freed.
 * ========================================================= */

/* k holds a copy of idnum 31 in state; d then logs in as idnum 31. */
static struct char_data *login_over(struct descriptor_data *d,
                                    struct descriptor_data *k, int state)
{
  struct char_data *kch;

  free_heap_chars();      /* left over only if an earlier test failed */
  kch = heap_char("Twinnie");
  password_login(d);
  memset(k, 0, sizeof(*k));
  GET_IDNUM(kch) = 31;
  kch->desc = k;
  k->character = kch;
  k->desc_num = 1;
  STATE(k) = state;
  k->next = d;
  descriptor_list = k;
  free_char_calls = 0;
  return kch;
}

static void login_over_run(struct descriptor_data *d)
{
  char arg[sizeof(dupe_password)];

  strlcpy(arg, dupe_password, sizeof(arg));
  nanny(d, arg);
  descriptor_list = NULL;
}

void test_dupe_check_frees_a_twin_at_the_menu(void)
{
  struct descriptor_data d, k;
  struct char_data *kch = login_over(&d, &k, CON_MENU);

  login_over_run(&d);
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&k));
  TEST_ASSERT_NULL(k.character);
  TEST_ASSERT_FALSE(heap_char_live(kch));       /* freed, not leaked */
  TEST_ASSERT_EQUAL_INT(1, free_char_calls);
  TEST_ASSERT_EQUAL_INT(CON_RMOTD, STATE(&d));  /* d logs in as usual */
  free(GET_HOST(d.character));
  free_heap_chars();
}

void test_dupe_check_detaches_the_menu_description_editor(void)
{
  struct descriptor_data d, k;
  struct char_data *kch = login_over(&d, &k, CON_PLR_DESC);

  /* As nanny's menu option 2 leaves it. */
  k.str = &kch->player.description;
  k.backstr = strdup("old description");
  TEST_ASSERT_NOT_NULL(k.backstr);
  login_over_run(&d);
  TEST_ASSERT_FALSE(heap_char_live(kch));
  TEST_ASSERT_NULL(k.str);          /* no pointer into the freed character */
  TEST_ASSERT_NULL(k.backstr);      /* released, as close_socket() would */
  free(GET_HOST(d.character));
  free_heap_chars();
}

void test_dupe_check_takes_over_an_in_game_twin(void)
{
  struct descriptor_data d, k;
  struct char_data *kch = login_over(&d, &k, CON_PLAYING);

  login_over_run(&d);
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(&k));
  TEST_ASSERT_TRUE(heap_char_live(kch));        /* in the world: kept */
  TEST_ASSERT_EQUAL_PTR(kch, d.character);      /* and taken over */
  TEST_ASSERT_EQUAL_INT(1, free_char_calls);    /* d's own copy only */
  free(GET_HOST(kch));
  kch->desc = NULL;
  free_heap_chars();
}

#ifdef NOCRYPT
/* test_interpreter_nocrypt: interpreter.c built without crypt(), and linked
 * with --wrap=sodium_init so libsodium can fail to start on demand. */
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __real_sodium_init(void);
static int fail_sodium_init;
/* NOLINTNEXTLINE(cert-dcl37-c,cert-dcl51-cpp): --wrap requires this name. */
int __wrap_sodium_init(void)
{
  return (fail_sodium_init ? -1 : __real_sodium_init());
}

/* A login whose password cannot be checked closes with a message, is
 * reported once with the idnum, and counts as neither a bad password nor a
 * rate-limit failure. */
static void assert_unverifiable_login(struct descriptor_data *d,
                                      struct char_data *dch, const char *why)
{
  TEST_ASSERT_EQUAL_INT(CON_CLOSE, STATE(d));
  TEST_ASSERT_EQUAL_INT(0, d->bad_pws);
  TEST_ASSERT_EQUAL_INT(0, GET_BAD_PWS(dch));
  TEST_ASSERT_EQUAL_INT(1, captured_errors);
  TEST_ASSERT_EQUAL_STRING("password-verify", captured_operation);
  TEST_ASSERT_NOT_NULL(strstr(captured_detail, "player idnum 31 name Twinnie"));
  TEST_ASSERT_NOT_NULL(strstr(captured_detail, why));
}

/* What the player was shown (write_to_output is weak in test_stubs.c). */
static char shown[2048];
size_t write_to_output(struct descriptor_data *d, const char *txt, ...)
{
  va_list args;
  size_t used = strlen(shown);
  int n;

  (void)d;
  va_start(args, txt);
  n = vsnprintf(shown + used, sizeof(shown) - used, txt, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}

/* A character called name at state, whose stored hash is a legacy DES
 * hash this crypt()-less build cannot check. */
static struct char_data *legacy_char_at(struct descriptor_data *d,
                                        const char *name, int state)
{
  struct char_data *dch = heap_char(name);

  strlcpy(GET_PASSWD(dch), "abJnggxhB/yWI", sizeof(dch->player.passwd));
  GET_IDNUM(dch) = 0;                  /* as during character creation */
  memset(d, 0, sizeof(*d));
  d->character = dch;
  dch->desc = d;
  STATE(d) = state;
  *shown = '\0';
  return dch;
}

/* Each password prompt after login treats an unverifiable password as the
 * server's problem: it says so, reports the name (idnum is 0 in creation)
 * and either closes (an unconfirmed new hash is in memory) or returns to
 * the menu.  Red before: "Passwords don't match" / "Incorrect password",
 * and CON_CNFPASSWD looped forever. */
static void check_unverifiable_prompt(const char *name, int state, int want)
{
  struct descriptor_data d;
  struct char_data *dch = legacy_char_at(&d, name, state);
  char arg[] = "whatever1";
  char who[64];

  captured_errors = 0;
  nanny(&d, arg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(want, STATE(&d), name);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(shown, "cannot be checked"), name);
  TEST_ASSERT_NULL_MESSAGE(strstr(shown, "don't match"), name);
  TEST_ASSERT_NULL_MESSAGE(strstr(shown, "Incorrect password"), name);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, captured_errors, name);
  TEST_ASSERT_EQUAL_STRING("password-verify", captured_operation);
  snprintf(who, sizeof(who), "player idnum 0 name %s", name);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(captured_detail, who), name);
  dch->desc = NULL;
  free_heap_chars();
}

void test_unverifiable_password_at_each_prompt(void)
{
  check_unverifiable_prompt("Cnfone", CON_CNFPASSWD, CON_CLOSE);
  check_unverifiable_prompt("Vrfyone", CON_CHPWD_VRFY, CON_CLOSE);
  check_unverifiable_prompt("Oldone", CON_CHPWD_GETOLD, CON_MENU);
  check_unverifiable_prompt("Delone", CON_DELCNF1, CON_MENU);
}

/* A reconnect loop against one legacy-hash account: at most
 * TB_PW_REPORT_BURST (5) records, then one "withheld" record; each attempt
 * counts against the account's login rate limit (never as a bad password),
 * so the loop is cut off with "Too many login attempts".  Red before: one
 * ERROR record per attempt, forever. */
void test_unverifiable_login_flood_is_rate_limited(void)
{
  struct descriptor_data d;
  struct char_data *dch;
  char arg[] = "whatever1";
  int i, reports = 0, withheld = 0, cut_off = 0;

  for (i = 0; i < 20 && !cut_off; i++) {
    dch = legacy_char_at(&d, "Floodie", CON_PASSWORD);
    captured_errors = 0;
    *captured_operation = '\0';
    nanny(&d, arg);
    if (strstr(shown, "Too many login attempts"))
      cut_off = 1;
    else
      TEST_ASSERT_NOT_NULL(strstr(shown, "cannot be checked"));
    if (!strcmp(captured_operation, "password-verify"))
      reports += captured_errors;
    else if (!strcmp(captured_operation, "password-verify-suppressed"))
      withheld += captured_errors;
    TEST_ASSERT_EQUAL_INT(0, d.bad_pws);
    TEST_ASSERT_EQUAL_INT(0, GET_BAD_PWS(dch));
    dch->desc = NULL;
    free_heap_chars();
  }
  TEST_ASSERT_EQUAL_INT(5, reports);
  TEST_ASSERT_EQUAL_INT(1, withheld);
  TEST_ASSERT_TRUE(cut_off);
  TEST_ASSERT_TRUE(i <= 9);            /* TB_LOGIN_MAX_FAILURES (8) + 1 */
}

/* Red before: the legacy hash was silently a wrong password (3 strikes). */
void test_legacy_hash_without_crypt_is_reported_not_counted(void)
{
  struct descriptor_data d;
  struct char_data *dch;
  char arg[sizeof(dupe_password)];

  dupe_world(3001);
  dch = password_login(&d);
  strlcpy(GET_PASSWD(dch), "abJnggxhB/yWI", sizeof(dch->player.passwd)); /* DES */
  descriptor_list = &d;
  strlcpy(arg, dupe_password, sizeof(arg));
  nanny(&d, arg);

  assert_unverifiable_login(&d, dch, "no crypt()");
  dupe_world_free();
}

/* Red before: a libsodium start-up failure was silently a wrong password. */
void test_sodium_init_failure_is_reported_not_counted(void)
{
  struct descriptor_data d;
  struct char_data *dch;
  char arg[sizeof(dupe_password)];

  dupe_world(3001);
  dch = password_login(&d);
  descriptor_list = &d;
  strlcpy(arg, dupe_password, sizeof(arg));
  fail_sodium_init = 1;
  nanny(&d, arg);
  fail_sodium_init = 0;

  assert_unverifiable_login(&d, dch, "sodium_init failed");
  dupe_world_free();
}
#endif /* NOCRYPT */

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

    /* perform_new_char_dupe_check frees booted characters */
    RUN_TEST(test_new_char_dupe_check_frees_the_booted_creation_character);
    RUN_TEST(test_new_char_dupe_check_closing_both_keeps_only_the_in_game_twin);
    RUN_TEST(test_new_char_dupe_check_closing_both_frees_a_twin_at_the_menu);
    RUN_TEST(test_new_char_dupe_check_closing_both_detaches_the_description_editor);
    RUN_TEST(test_new_char_dupe_check_booting_a_closed_editor_detaches_it);

    /* command_interpreter switched-IMPL check */
    RUN_TEST(test_command_interpreter_switched_impl_runs_script_commands);
    RUN_TEST(test_command_interpreter_npc_descriptor_without_original_is_reported);

    /* perform_dupe_check: duplicate extraction room */
    RUN_TEST(test_dupe_check_extracts_a_duplicate_in_the_start_room);
    RUN_TEST(test_dupe_check_without_a_start_room_is_reported);

    /* perform_dupe_check: booted characters */
    RUN_TEST(test_dupe_check_frees_a_twin_at_the_menu);
    RUN_TEST(test_dupe_check_detaches_the_menu_description_editor);
    RUN_TEST(test_dupe_check_takes_over_an_in_game_twin);
#ifdef NOCRYPT
    RUN_TEST(test_legacy_hash_without_crypt_is_reported_not_counted);
    RUN_TEST(test_sodium_init_failure_is_reported_not_counted);
    RUN_TEST(test_unverifiable_password_at_each_prompt);
    RUN_TEST(test_unverifiable_login_flood_is_rate_limited);
#endif

    return UNITY_END();
}
