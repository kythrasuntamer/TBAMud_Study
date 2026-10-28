/**
* @file utils.c
* Various utility functions used within the core mud code.
*
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*
* All rights reserved.  See license for complete information.
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.
*/

#include <stdint.h>  /* SIZE_MAX */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "comm.h"
#include "modify.h"
#include "screen.h"
#include "spells.h"
#include "handler.h"
#include "interpreter.h"
#include "class.h"


/** Aportable random number function.
 * @param from The lower bounds of the random number.
 * @param to The upper bounds of the random number. */
int rand_number(int from, int to)
{
  /* error checking in case people call this incorrectly */
  if (from > to) {
    int tmp = from;
    from = to;
    to = tmp;
    log("SYSERR: rand_number() should be called with lowest, then highest. (%d, %d), not (%d, %d).", from, to, to, from);
  }

  /* This should always be of the form: ((float)(to - from + 1) * rand() /
   * (float)(RAND_MAX + from) + from); If you are using rand() due to historical
   * non-randomness of the lower bits in older implementations.  We always use
   * circle_random() though, which shouldn't have that problem. Mean and
   * standard deviation of both are identical (within the realm of statistical
   * identity) if the rand() implementation is non-broken. */
  return ((circle_random() % (to - from + 1)) + from);
}

/** Simulates a single dice roll from one to many of a certain sized die.
 * @param num The number of dice to roll.
 * @param size The number of sides each die has, and hence the number range
 * of the die. */
int dice(int num, int size)
{
  int sum = 0;

  if (size <= 0 || num <= 0)
    return (0);

  while (num-- > 0)
    sum += rand_number(1, size);

  return (sum);
}

/** Return the smaller number. Original note: Be wary of sign issues with this.
 * @param a The first number.
 * @param b The second number. */
int MIN(int a, int b)
{
  return (a < b ? a : b);
}

/** Return the larger number. Original note: Be wary of sign issues with this.
 * @param a The first number.
 * @param b The second number. */
int MAX(int a, int b)
{
  return (a > b ? a : b);
}

/** Used to capitalize a string. Will not change any mud specific color codes.
 * @param txt The string to capitalize. */
char *CAP(char *txt)
{
  char *p = txt;

  /* Skip all preceeding color codes and ANSI codes */
  while ((*p == '\t' && *(p+1)) || (*p == '\x1B' && *(p+1) == '[')) {
    if (*p == '\t') p += 2;  /* Skip \t sign and color letter/number */
    else {
      p += 2;                          /* Skip the CSI section of the ANSI code */
      while (*p && !isalpha(*p)) p++;  /* Skip until a 'letter' is found */
      if (*p) p++;                     /* Skip the letter */
    }
  }

  if (*p)
    *p = UPPER(*p);
  return (txt);
}

#if !defined(HAVE_STRLCPY)
/** A 'strlcpy' function in the same fashion as 'strdup' below. This copies up
 * to totalsize - 1 bytes from the source string, placing them and a trailing
 * NUL into the destination string. Returns the total length of the string it
 * tried to copy, not including the trailing NUL.  So a '>= totalsize' test
 * says it was truncated. (Note that you may have _expected_ truncation
 * because you only wanted a few characters from the source string.) Portable
 * function, in case your system does not have strlcpy. */
size_t strlcpy(char *dest, const char *source, size_t totalsize)
{
  size_t srclen = strlen(source);

  if (totalsize != 0) {
    size_t copylen = srclen >= totalsize ? totalsize - 1 : srclen;

    memcpy(dest, source, copylen);
    dest[copylen] = '\0';
  }

  return srclen;
}
#endif

#if !defined(HAVE_STRDUP)
/** Create a duplicate of a string function. Portable. */
char *strdup(const char *source)
{
  char *new_z;

  CREATE(new_z, char, strlen(source) + 1);
  strlcpy(new_z, source, strlen(source) + 1);
  return new_z; /* strcpy: OK */
}
#endif

/** Strips "\\r\\n" from just the end of a string. Will not remove internal
 * "\\r\\n" values to the string.
 * @post Replaces any "\\r\\n" values at the end of the string with null.
 * @param txt The writable string to prune. */
void prune_crlf(char *txt)
{
  size_t len;

  if (txt == NULL || *txt == '\0')
    return;

  len = strlen(txt);
  while (len > 0 && (txt[len - 1] == '\n' || txt[len - 1] == '\r'))
    txt[--len] = '\0';
}

#ifndef str_cmp
/** a portable, case-insensitive version of strcmp(). Returns: 0 if equal, > 0
 * if arg1 > arg2, or < 0 if arg1 < arg2. Scan until strings are found
 * different or we reach the end of both. */
int str_cmp(const char *arg1, const char *arg2)
{
  int chk, i;

  if (arg1 == NULL || arg2 == NULL) {
    log("SYSERR: str_cmp() passed a NULL pointer, %p or %p.", (void *)arg1, (void *)arg2);
    return (0);
  }

  for (i = 0; arg1[i] || arg2[i]; i++)
    if ((chk = LOWER(arg1[i]) - LOWER(arg2[i])) != 0)
      return (chk);	/* not equal */

  return (0);
}
#endif

#ifndef strn_cmp
/** a portable, case-insensitive version of strncmp(). Returns: 0 if equal, > 0
 * if arg1 > arg2, or < 0 if arg1 < arg2. Scan until strings are found
 * different, the end of both, or n is reached. */
int strn_cmp(const char *arg1, const char *arg2, int n)
{
  int chk, i;

  if (arg1 == NULL || arg2 == NULL) {
    log("SYSERR: strn_cmp() passed a NULL pointer, %p or %p.", (void *)arg1, (void *)arg2);
    return (0);
  }

  for (i = 0; (arg1[i] || arg2[i]) && (n > 0); i++, n--)
    if ((chk = LOWER(arg1[i]) - LOWER(arg2[i])) != 0)
      return (chk);	/* not equal */

  return (0);
}
#endif

/* ------------------------------------------------------------------------- */
/* Diagnostic context, provenance, allocation and bounded-string primitives.  */
/* ------------------------------------------------------------------------- */

#define TB_BREADCRUMB_COUNT 16
#define TB_BREADCRUMB_TEXT 160
#define TB_PARSE_CONTEXT_COUNT 16

struct tb_breadcrumb_entry {
  unsigned long sequence;
  unsigned long correlation;
  char kind[32];
  char detail[TB_BREADCRUMB_TEXT];
};

struct tb_parse_context_slot {
  FILE *stream;
  unsigned long line;
  long record_id;
  char resource[PATH_MAX];
  char parser[48];
  char record_type[32];
  char field[64];
};

static struct tb_breadcrumb_entry tb_breadcrumb_ring[TB_BREADCRUMB_COUNT];
static size_t tb_breadcrumb_next = 0;
static unsigned long tb_breadcrumb_sequence = 0;
static unsigned long tb_correlation_sequence = 0;
static unsigned long tb_current_correlation = 0;
static unsigned long tb_diagnostic_sequence = 0;
static struct tb_parse_context_slot tb_parse_contexts[TB_PARSE_CONTEXT_COUNT];

static const char *tb_error_id_for(const char *subsystem, const char *operation,
                                   int errnum)
{
  (void)errnum;

  if (subsystem != NULL) {
    if (!str_cmp(subsystem, "PARSER"))
      return TB_ERRID_PARSER_MALFORMED;
    if (!str_cmp(subsystem, "PERSISTENCE"))
      return (!str_cmp(operation ? operation : "", "rollback"))
               ? TB_ERRID_PERSIST_ROLLBACK : TB_ERRID_PERSIST_WRITE;
    if (!str_cmp(subsystem, "NETWORK"))
      return TB_ERRID_NETWORK_IO;
    if (!str_cmp(subsystem, "INVARIANT"))
      return TB_ERRID_INVARIANT;
    if (!str_cmp(subsystem, "SCRIPT"))
      return TB_ERRID_SCRIPT_PARSE;
    if (!str_cmp(subsystem, "BOOT"))
      return TB_ERRID_BOOT_FATAL;
  }

  return "TB-GENERAL-0001";
}

unsigned long tb_correlation_begin(const char *kind, const char *detail)
{
  unsigned long id = ++tb_correlation_sequence;

  if (id == 0)
    id = ++tb_correlation_sequence;

  tb_current_correlation = id;
  tb_breadcrumb(kind ? kind : "correlation",
                detail ? detail : "operation-begin");
  return id;
}

void tb_correlation_end(unsigned long id)
{
  if (id != 0 && tb_current_correlation == id) {
    tb_breadcrumb("correlation", "operation-end");
    tb_current_correlation = 0;
  }
}

unsigned long tb_correlation_current(void)
{
  return tb_current_correlation;
}

void tb_breadcrumb(const char *kind, const char *detail)
{
  struct tb_breadcrumb_entry *entry =
      &tb_breadcrumb_ring[tb_breadcrumb_next % TB_BREADCRUMB_COUNT];

  entry->sequence = ++tb_breadcrumb_sequence;
  entry->correlation = tb_current_correlation;
  strlcpy(entry->kind, kind ? kind : "event", sizeof(entry->kind));
  strlcpy(entry->detail, detail ? detail : "(none)", sizeof(entry->detail));
  tb_breadcrumb_next = (tb_breadcrumb_next + 1) % TB_BREADCRUMB_COUNT;
}

void tb_breadcrumbf(const char *kind, const char *format, ...)
{
  char detail[TB_BREADCRUMB_TEXT];
  va_list args;
  int n;

  if (format == NULL) {
    tb_breadcrumb(kind, "(null format)");
    return;
  }

  va_start(args, format);
  n = vsnprintf(detail, sizeof(detail), format, args);
  va_end(args);

  if (n < 0)
    strlcpy(detail, "(formatting failed)", sizeof(detail));
  else if ((size_t)n >= sizeof(detail))
    strlcpy(detail + sizeof(detail) - sizeof("...[TRUNCATED]"),
            "...[TRUNCATED]", sizeof("...[TRUNCATED]"));

  tb_breadcrumb(kind, detail);
}

size_t tb_breadcrumb_snapshot(char *dst, size_t dst_size)
{
  struct tb_buf out;
  size_t count = 0;
  size_t i;
  size_t start;

  if (dst == NULL || dst_size == 0)
    return 0;

  tb_buf_init(&out, dst, dst_size);
  start = tb_breadcrumb_next;

  for (i = 0; i < TB_BREADCRUMB_COUNT; i++) {
    size_t idx = (start + i) % TB_BREADCRUMB_COUNT;
    const struct tb_breadcrumb_entry *entry = &tb_breadcrumb_ring[idx];

    if (entry->sequence == 0)
      continue;

    if (count++ != 0)
      (void)tb_buf_append(&out, " | ");

    (void)tb_buf_appendf(&out, "#%lu/corr=%lu/%s:%s",
                         entry->sequence, entry->correlation,
                         entry->kind, entry->detail);
  }

  return out.length;
}

void tb_buf_init(struct tb_buf *buf, char *storage, size_t capacity)
{
  if (buf == NULL)
    return;

  buf->data = storage;
  buf->capacity = capacity;
  buf->length = 0;
  buf->truncated = 0;

  if (storage != NULL && capacity > 0)
    storage[0] = '\0';
}

size_t tb_buf_remaining(const struct tb_buf *buf)
{
  if (buf == NULL || buf->data == NULL || buf->capacity == 0 ||
      buf->length >= buf->capacity)
    return 0;
  return buf->capacity - buf->length - 1;
}

int tb_buf_appendn(struct tb_buf *buf, const char *text, size_t length)
{
  size_t available;
  size_t copy_len;

  if (buf == NULL || buf->data == NULL || buf->capacity == 0 || text == NULL)
    return FALSE;

  available = tb_buf_remaining(buf);
  copy_len = length <= available ? length : available;

  if (copy_len != 0)
    memcpy(buf->data + buf->length, text, copy_len);
  buf->length += copy_len;
  buf->data[buf->length] = '\0';

  if (copy_len != length) {
    buf->truncated = TRUE;
    return FALSE;
  }
  return TRUE;
}

int tb_buf_append(struct tb_buf *buf, const char *text)
{
  return text != NULL ? tb_buf_appendn(buf, text, strlen(text)) : FALSE;
}

int tb_buf_appendf(struct tb_buf *buf, const char *format, ...)
{
  va_list args;
  int n;
  size_t available;

  if (buf == NULL || buf->data == NULL || buf->capacity == 0 || format == NULL)
    return FALSE;

  available = tb_buf_remaining(buf);
  va_start(args, format);
  n = vsnprintf(buf->data + buf->length, available + 1, format, args);
  va_end(args);

  if (n < 0) {
    buf->truncated = TRUE;
    return FALSE;
  }

  if ((size_t)n > available) {
    buf->length += available;
    buf->truncated = TRUE;
    return FALSE;
  }

  buf->length += (size_t)n;
  return TRUE;
}

int tb_buf_truncated(const struct tb_buf *buf)
{
  return buf != NULL ? buf->truncated : TRUE;
}

int tb_size_mul_checked(size_t count, size_t element_size, size_t *out)
{
  if (out == NULL || count == 0 || element_size == 0 ||
      count > SIZE_MAX / element_size)
    return FALSE;

  *out = count * element_size;
  return TRUE;
}

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  size_t bytes;
  void *result;

  if (!tb_size_mul_checked(count, element_size, &bytes)) {
    tb_report_domain_error_at(TB_SEV_FATAL, TB_ERR_INVARIANT,
                              "allocation-size", "calloc array overflow/zero",
                              0, source_file, source_line, source_func);
    return NULL;
  }

  (void)bytes;
  result = calloc(count, element_size);
  if (result == NULL) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_FATAL, TB_ERR_GENERAL,
                              "calloc", "array allocation", saved_errno,
                              source_file, source_line, source_func);
  }
  return result;
}

void *tb_realloc_array_at(void *ptr, size_t count, size_t element_size,
                          const char *source_file, int source_line,
                          const char *source_func)
{
  size_t bytes;
  void *result;

  if (!tb_size_mul_checked(count, element_size, &bytes)) {
    tb_report_domain_error_at(TB_SEV_FATAL, TB_ERR_INVARIANT,
                              "allocation-size", "realloc array overflow/zero",
                              0, source_file, source_line, source_func);
    return NULL;
  }

  result = realloc(ptr, bytes);
  if (result == NULL) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_FATAL, TB_ERR_GENERAL,
                              "realloc", "array allocation", saved_errno,
                              source_file, source_line, source_func);
  }
  return result;
}

static struct tb_parse_context_slot *tb_parse_context_find(FILE *stream, int create)
{
  struct tb_parse_context_slot *free_slot = NULL;
  size_t i;

  if (stream == NULL)
    return NULL;

  for (i = 0; i < TB_PARSE_CONTEXT_COUNT; i++) {
    if (tb_parse_contexts[i].stream == stream)
      return &tb_parse_contexts[i];
    if (free_slot == NULL && tb_parse_contexts[i].stream == NULL)
      free_slot = &tb_parse_contexts[i];
  }

  if (create && free_slot != NULL) {
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->stream = stream;
    free_slot->record_id = -1;
    return free_slot;
  }

  return NULL;
}

void tb_parse_context_begin(FILE *stream, const char *resource, const char *parser)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, TRUE);

  if (ctx == NULL)
    return;

  ctx->line = 0;
  ctx->record_id = -1;
  strlcpy(ctx->resource, resource ? resource : "(unknown)",
          sizeof(ctx->resource));
  strlcpy(ctx->parser, parser ? parser : "(unknown)", sizeof(ctx->parser));
  ctx->record_type[0] = '\0';
  ctx->field[0] = '\0';
}

void tb_parse_context_end(FILE *stream)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, FALSE);

  if (ctx != NULL)
    memset(ctx, 0, sizeof(*ctx));
}

void tb_parse_context_record(FILE *stream, const char *record_type, long record_id)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, FALSE);

  if (ctx == NULL)
    return;
  ctx->record_id = record_id;
  strlcpy(ctx->record_type, record_type ? record_type : "record",
          sizeof(ctx->record_type));
  ctx->field[0] = '\0';
}

void tb_parse_context_field(FILE *stream, const char *field)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, FALSE);

  if (ctx != NULL)
    strlcpy(ctx->field, field ? field : "(unknown)", sizeof(ctx->field));
}

void tb_parse_context_note_line(FILE *stream)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, FALSE);
  if (ctx != NULL)
    ctx->line++;
}

unsigned long tb_parse_context_line(FILE *stream)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, FALSE);
  return ctx != NULL ? ctx->line : 0;
}

void tb_parse_context_error_at(FILE *stream, const char *source_file,
                               int source_line, const char *source_func,
                               const char *detail_fmt, ...)
{
  struct tb_parse_context_slot *ctx = tb_parse_context_find(stream, FALSE);
  char detail[512];
  char resource[PATH_MAX + 256];
  va_list args;
  int n;

  if (detail_fmt != NULL) {
    va_start(args, detail_fmt);
    n = vsnprintf(detail, sizeof(detail), detail_fmt, args);
    va_end(args);
    if (n < 0)
      strlcpy(detail, "parser detail formatting failed", sizeof(detail));
  } else {
    strlcpy(detail, "parser error", sizeof(detail));
  }

  if (ctx != NULL) {
    (void)snprintf(resource, sizeof(resource),
                   "%s line=%lu parser=%s record=%s:%ld field=%s",
                   ctx->resource, ctx->line, ctx->parser,
                   ctx->record_type[0] ? ctx->record_type : "(none)",
                   ctx->record_id,
                   ctx->field[0] ? ctx->field : "(none)");
  } else {
    strlcpy(resource, "(parser context unavailable)", sizeof(resource));
  }

  tb_error_log_at("ERROR", "PARSER", "parse", resource, 0,
                  source_file, source_line, source_func,
                  "%s id=%s", detail, TB_ERRID_PARSER_MALFORMED);
}

/** New variable argument log() function; logs messages to disk.
 * Works the same as the old for previously written code but is very nice
 * if new code wishes to implment printf style log messages without the need
 * to make prior sprintf calls.
 * @param format The message to log. Standard printf formatting and variable
 * arguments are allowed.
 * @param args The comma delimited, variable substitutions to make in str. */
void basic_mud_vlog(const char *format, va_list args)
{
  static unsigned long log_sequence = 0;
  time_t ct;
  struct tm *tm_info;
  char timestr[32];
  char message[MAX_STRING_LENGTH];
  char line[MAX_STRING_LENGTH + 96];
  FILE *target;
  int incoming_errno = errno;
  int message_len, line_len;
  unsigned long seq = ++log_sequence;

  if (format == NULL)
    format = "SYSERR: log() received a NULL format.";

  message_len = vsnprintf(message, sizeof(message), format, args);
  if (message_len < 0) {
    strlcpy(message, "SYSERR: log formatting failed.", sizeof(message));
  } else if ((size_t)message_len >= sizeof(message)) {
    static const char trunc[] = "...[TRUNCATED]";
    size_t tlen = sizeof(trunc) - 1;
    if (sizeof(message) > tlen + 1)
      memcpy(message + sizeof(message) - tlen - 1, trunc, tlen + 1);
  }

  ct = time(NULL);
  tm_info = localtime(&ct);
  if (tm_info == NULL ||
      strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%S%z", tm_info) == 0)
    strlcpy(timestr, "time-unavailable", sizeof(timestr));

  line_len = snprintf(line, sizeof(line), "%s seq=%lu :: %s\n",
                      timestr, seq, message);
  if (line_len < 0) {
    strlcpy(line,
            "time-unavailable seq=? :: SYSERR: log line formatting failed.\n",
            sizeof(line));
  } else if ((size_t)line_len >= sizeof(line)) {
    static const char trunc[] = "...[TRUNCATED]\n";
    size_t tlen = sizeof(trunc) - 1;
    if (sizeof(line) > tlen + 1)
      memcpy(line + sizeof(line) - tlen - 1, trunc, tlen + 1);
  }

  target = logfile ? logfile : stderr;
  if (fputs(line, target) == EOF || fflush(target) == EOF) {
    int log_errno = errno;
    if (target != stderr) {
      char fallback[4608];
      int fallback_len = snprintf(
          fallback, sizeof(fallback),
          "TBERR severity=FATAL category=BOOT operation=\"logger-write\" "
          "context=\"primary logfile write/flush failed\" "
          "errno=%d errno_name=%s error=\"%s\" seq=%lu original=\"%s\"\n",
          log_errno, tb_errno_name(log_errno), strerror(log_errno), seq, line);

      if (fallback_len < 0) {
        static const char emergency[] =
          "TBERR severity=FATAL category=BOOT operation=\"logger-write\" "
          "context=\"logger fallback formatting failed\"\n";
        (void)tb_diag_emit(emergency, sizeof(emergency) - 1);
      } else {
        size_t emit_len = (size_t)fallback_len;
        if (emit_len >= sizeof(fallback))
          emit_len = sizeof(fallback) - 1;
        (void)tb_diag_emit(fallback, emit_len);
      }
    }
  }

  errno = incoming_errno;
}

void tb_error_log_at(const char *severity, const char *subsystem,
                     const char *operation, const char *resource, int errnum,
                     const char *source_file, int source_line,
                     const char *source_func, const char *detail_fmt, ...)
{
  char detail[MAX_STRING_LENGTH / 2];
  va_list args;
  int incoming_errno = errno;

  if (detail_fmt != NULL) {
    int n;

    va_start(args, detail_fmt);
    n = vsnprintf(detail, sizeof(detail), detail_fmt, args);
    va_end(args);

    if (n < 0)
      strlcpy(detail, "detail-formatting-failed", sizeof(detail));
    else if ((size_t)n >= sizeof(detail))
      strlcpy(detail + sizeof(detail) - sizeof("...[TRUNCATED]"),
              "...[TRUNCATED]", sizeof("...[TRUNCATED]"));
  } else {
    strlcpy(detail, "(no detail)", sizeof(detail));
  }

  {
    char breadcrumbs[1024];
    const char *error_id = tb_error_id_for(subsystem, operation, errnum);
    unsigned long diag_seq = ++tb_diagnostic_sequence;
    unsigned long correlation = tb_correlation_current();

    (void)tb_breadcrumb_snapshot(breadcrumbs, sizeof(breadcrumbs));

    if (errnum != 0)
      basic_mud_log("%s id=%s diag_seq=%lu correlation=%lu subsystem=%s op=%s "
                    "resource=%s errno=%d errno_name=%s error=\"%s\" "
                    "at=%s:%d function=%s build_sha=%s compiler=\"%s\" "
                    "build_type=%s sanitizers=%s detail=\"%s\" breadcrumbs=\"%s\"",
                    severity ? severity : "ERROR", error_id, diag_seq, correlation,
                    subsystem ? subsystem : "unknown",
                    operation ? operation : "unknown",
                    resource ? resource : "(none)",
                    errnum, tb_errno_name(errnum), strerror(errnum),
                    source_file ? source_file : "(unknown)", source_line,
                    source_func ? source_func : "(unknown)",
                    TB_BUILD_SHA, TB_BUILD_COMPILER, TB_BUILD_TYPE,
                    TB_BUILD_SANITIZERS, detail, breadcrumbs);
    else
      basic_mud_log("%s id=%s diag_seq=%lu correlation=%lu subsystem=%s op=%s "
                    "resource=%s at=%s:%d function=%s build_sha=%s "
                    "compiler=\"%s\" build_type=%s sanitizers=%s "
                    "detail=\"%s\" breadcrumbs=\"%s\"",
                    severity ? severity : "ERROR", error_id, diag_seq, correlation,
                    subsystem ? subsystem : "unknown",
                    operation ? operation : "unknown",
                    resource ? resource : "(none)",
                    source_file ? source_file : "(unknown)", source_line,
                    source_func ? source_func : "(unknown)",
                    TB_BUILD_SHA, TB_BUILD_COMPILER, TB_BUILD_TYPE,
                    TB_BUILD_SANITIZERS, detail, breadcrumbs);
  }

  errno = incoming_errno;
}

/** Log messages directly to syslog on disk, no display to in game immortals.
 * Supports variable string modification arguments, a la printf. Most likely
 * any calls to plain old log() have been redirected, via macro, to this
 * function.
 * @param format The message to log. Standard printf formatting and variable
 * arguments are allowed.
 * @param ... The comma delimited, variable substitutions to make in str. */
void basic_mud_log(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  basic_mud_vlog(format, args);
  va_end(args);
}

/** Essentially the touch command. Create an empty file or update the modified
 * time of a file.
 * @param path The filepath to "touch." This filepath is relative to the /lib
 * directory relative to the root of the mud distribution. */
int touch(const char *path)
{
  FILE *fl;

  if (!(fl = fopen(path, "a"))) {
    TB_REPORT_ERRNO("fopen", path);
    return (-1);
  } else {
    tb_fclose(fl);
    return (0);
  }
}

/** Log mud messages to a file & to online imm's syslogs.
 * @param type The minimum syslog level that needs be set to see this message.
 * OFF, BRF, NRM and CMP are the values from lowest to highest. Using mudlog
 * with type = OFF should be avoided as every imm will see the message even
 * if they have syslog turned off.
 * @param level Minimum character level needed to see this message.
 * @param file TRUE log this to the syslog file, FALSE do not log this to disk.
 * @param str The message to log. Standard printf formatting and variable
 * arguments are allowed.
 * @param ... The comma delimited, variable substitutions to make in str. */
void mudlog(int type, int level, int file, const char *str, ...)
{
  char buf[MAX_STRING_LENGTH];
  struct descriptor_data *i;
  va_list args;

  if (str == NULL)
    return;	/* eh, oh well. */

  if (file) {
    va_start(args, str);
    basic_mud_vlog(str, args);
    va_end(args);
  }

  if (level < 0)
    return;

  strlcpy(buf, "[ ", sizeof(buf));	/* strcpy: OK */
  va_start(args, str);
  int formatted_length = vsnprintf(buf + 2, sizeof(buf) - 6, str, args);
  va_end(args);
  if (formatted_length < 0)
    strlcpy(buf + 2, "(log formatting failed)", sizeof(buf) - 2);
  strlcat(buf, " ]\r\n", sizeof(buf));	/* strcat: OK */

  for (i = descriptor_list; i; i = i->next) {
    if (STATE(i) != CON_PLAYING || IS_NPC(i->character)) /* switch */
      continue;
    if (GET_LEVEL(i->character) < level)
      continue;
    if (PLR_FLAGGED(i->character, PLR_WRITING))
      continue;
    if (type > (PRF_FLAGGED(i->character, PRF_LOG1) ? 1 : 0) + (PRF_FLAGGED(i->character, PRF_LOG2) ? 2 : 0))
      continue;

    send_to_char(i->character, "%s%s%s", CCGRN(i->character, C_NRM), buf, CCNRM(i->character, C_NRM));
  }
}



/** Take a bitvector and return a human readable
 * description of which bits are set in it.
 * @pre The final element in the names array must contain a one character
 * string consisting of a single newline character "\\n". Caller of function is
 * responsible for creating the memory buffer for the result string.
 * @param[in] bitvector The bitvector to test for set bits.
 * @param[in] names An array of human readable strings describing each possible
 * bit. The final element in this array must be a string made of a single
 * newline character (eg "\\n").
 * If you don't have a 'const' array for the names param, cast it as such.
 * @param[out] result Holds the names of the set bits in bitvector. The bit
 * names will be delimited by a single space.
 * Caller of sprintbit is responsible for creating the buffer for result.
 * Will be set to "NOBITS" if no bits are set in bitvector (ie bitvector = 0).
 * @param[in] reslen The length of the available memory in the result buffer.
 * Ideally, results will be large enough to hold the description of every bit
 * that could possibly be set in bitvector. */
size_t sprintbit(bitvector_t bitvector, const char *names[], char *result, size_t reslen)
{
  size_t len = 0;
  int nlen;
  long nr;

  *result = '\0';

  for (nr = 0; bitvector && len < reslen; bitvector >>= 1) {
    if (IS_SET(bitvector, 1)) {
      nlen = snprintf(result + len, reslen - len, "%s ", *names[nr] != '\n' ? names[nr] : "UNDEFINED");
      if (len + nlen >= reslen || nlen < 0)
        break;
      len += nlen;
    }

    if (*names[nr] != '\n')
      nr++;
  }

  if (!*result)
    len = strlcpy(result, "NOBITS ", reslen);

  return (len);
}

/** Return the human readable name of a defined type.
 * @pre The final element in the names array must contain a one character
 * string consisting of a single newline character "\\n". Caller of function is
 * responsible for creating the memory buffer for the result string.
 * @param[in] type The type number to be translated.
 * @param[in] names An array of human readable strings describing each possible
 * bit. The final element in this array must be a string made of a single
 * newline character (eg "\\n").
 * @param[out] result Holds the translated name of the type.
 * Caller of sprintbit is responsible for creating the buffer for result.
 * Will be set to "UNDEFINED" if the type is greater than the number of names
 * available.
 * @param[in] reslen The length of the available memory in the result buffer. */
size_t sprinttype(int type, const char *names[], char *result, size_t reslen)
{
  int nr = 0;

  while (type && *names[nr] != '\n') {
    type--;
    nr++;
  }

  return strlcpy(result, *names[nr] != '\n' ? names[nr] : "UNDEFINED", reslen);
}

/** Take a bitarray and return a human readable description of which bits are
 * set in it.
 * @pre The final element in the names array must contain a one character
 * string consisting of a single newline character "\\n". Caller of function is
 * responsible for creating the memory buffer for the result string large enough
 * to hold all possible bit translations. There is no error checking for
 * possible array overflow for result.
 * @param[in] bitvector The bitarray in which to test for set bits.
 * @param[in] names An array of human readable strings describing each possible
 * bit. The final element in this array must be a string made of a single
 * newline character (eg "\\n").
 * If you don't have a 'const' array for the names param, cast it as such.
 * @param[in] maxar The number of 'bytes' in the bitarray. This number will
 * usually be pre-defined for the particular bitarray you are using.
 * @param[out] result Holds the names of the set bits in bitarray. The bit
 * names are delimited by a single space. Ideally, results will be large enough
 * to hold the description of every bit that could possibly be set in bitvector.
 * Will be set to "NOBITS" if no bits are set in bitarray (ie all bits in the
 * bitarray are equal to 0).
 */
void sprintbitarray(int bitvector[], const char *names[], int maxar, char *result, size_t reslen)
{
  struct tb_buf out;
  int nr, teller, found = FALSE;

  if (result == NULL || reslen == 0)
    return;

  tb_buf_init(&out, result, reslen);

  for (teller = 0; teller < maxar && !found; teller++) {
    for (nr = 0; nr < 32 && !found; nr++) {
      int bit = (teller * 32) + nr;
      const char *name = names[bit];

      if (IS_SET_AR(bitvector, bit)) {
        if (*name == '\n')
          (void)tb_buf_append(&out, "UNDEFINED ");
        else if (*name != '\0') {
          (void)tb_buf_append(&out, name);
          (void)tb_buf_append(&out, " ");
        }
      }

      if (*name == '\n')
        found = TRUE;
    }
  }

  if (out.length == 0)
    (void)tb_buf_append(&out, "NOBITS ");
}

/** Calculate the REAL time passed between two time invervals.
 * @todo Recommend making this function foresightedly useful by calculating
 * real months and years, too.
 * @param t2 The later time.
 * @param t1 The earlier time. */
struct time_info_data *real_time_passed(time_t t2, time_t t1)
{
  long secs;
  static struct time_info_data now;

  secs = t2 - t1;

  now.hours = (secs / SECS_PER_REAL_HOUR) % 24;	/* 0..23 hours */
  secs -= SECS_PER_REAL_HOUR * now.hours;

  now.day = (secs / SECS_PER_REAL_DAY);	/* 0..34 days  */
  /* secs -= SECS_PER_REAL_DAY * now.day; - Not used. */

  now.month = -1;
  now.year = -1;

  return (&now);
}

/** Calculate the MUD time passed between two time invervals.
 * @param t2 The later time.
 * @param t1 The earlier time. */
struct time_info_data *mud_time_passed(time_t t2, time_t t1)
{
  long secs;
  static struct time_info_data now;

  secs = t2 - t1;

  now.hours = (secs / SECS_PER_MUD_HOUR) % 24;	/* 0..23 hours */
  secs -= SECS_PER_MUD_HOUR * now.hours;

  now.day = (secs / SECS_PER_MUD_DAY) % 35;	/* 0..34 days  */
  secs -= SECS_PER_MUD_DAY * now.day;

  now.month = (secs / SECS_PER_MUD_MONTH) % 17;	/* 0..16 months */
  secs -= SECS_PER_MUD_MONTH * now.month;

  now.year = (secs / SECS_PER_MUD_YEAR);	/* 0..XX? years */

  return (&now);
}

/** Translate the current mud time to real seconds (in type time_t).
 * @param now The current mud time to translate into a real time unit. */
time_t mud_time_to_secs(struct time_info_data *now)
{
  time_t when = 0;

  when += now->year  * SECS_PER_MUD_YEAR;
  when += now->month * SECS_PER_MUD_MONTH;
  when += now->day   * SECS_PER_MUD_DAY;
  when += now->hours * SECS_PER_MUD_HOUR;
  return (time(NULL) - when);
}

/** Calculate a player's MUD age.
 * @todo The minimum starting age of 17 is hardcoded in this function. Recommend
 * changing the minimum age to a property (variable) external to this function.
 * @param ch A valid player character. */
struct time_info_data *age(struct char_data *ch)
{
  static struct time_info_data player_age;

  player_age = *mud_time_passed(time(0), ch->player.time.birth);

  player_age.year += 17;	/* All players start at 17 */

  return (&player_age);
}

/** Check if making ch follow victim will create an illegal follow loop. In
 * essence, this prevents someone from following a character in a group that
 * is already being lead by the character.
 * @param ch The character trying to follow.
 * @param victim The character being followed. */
bool circle_follow(struct char_data *ch, struct char_data *victim)
{
  struct char_data *k;

  for (k = victim; k; k = k->master) {
    if (k == ch)
      return (TRUE);
  }

  return (FALSE);
}

/** Call on a character (NPC or PC) to stop them from following someone and
 * to break any charm affect.
 * @todo Make the messages returned from the broken charm affect more
 * understandable.
 * @pre ch MUST be following someone, else core dump.
 * @post The charm affect (AFF_CHARM) will be removed from the character and
 * the character will stop following the "master" they were following.
 * @param ch The character (NPC or PC) to stop from following.
 * */
void stop_follower(struct char_data *ch)
{
  struct follow_type *j, *k;

  /* Makes sure this function is not called when it shouldn't be called. */
  if (ch->master == NULL) {
    core_dump();
    return;
  }

  if (AFF_FLAGGED(ch, AFF_CHARM)) {
    act("You realize that $N is a jerk!", FALSE, ch, 0, ch->master, TO_CHAR);
    act("$n realizes that $N is a jerk!", FALSE, ch, 0, ch->master, TO_NOTVICT);
    act("$n hates your guts!", FALSE, ch, 0, ch->master, TO_VICT);
    if (affected_by_spell(ch, SPELL_CHARM))
      affect_from_char(ch, SPELL_CHARM);
  } else {
    act("You stop following $N.", FALSE, ch, 0, ch->master, TO_CHAR);
    act("$n stops following $N.", TRUE, ch, 0, ch->master, TO_NOTVICT);
    if (CAN_SEE(ch->master, ch))
      act("$n stops following you.", TRUE, ch, 0, ch->master, TO_VICT);
  }

  if (ch->master->followers->follower == ch) {	/* Head of follower-list? */
    k = ch->master->followers;
    ch->master->followers = k->next;
    free(k);
  } else {			/* locate follower who is not head of list */
    for (k = ch->master->followers; k->next->follower != ch; k = k->next);

    j = k->next;
    k->next = j->next;
    free(j);
  }

  ch->master = NULL;
  REMOVE_BIT_AR(AFF_FLAGS(ch), AFF_CHARM);
}

/** Finds the number of follows that are following, and charmed by, the
 * character (PC or NPC).
 * @param ch The character to check for charmed followers.
 */
int num_followers_charmed(struct char_data *ch)
{
  struct follow_type *lackey;
  int total = 0;

  for (lackey = ch->followers; lackey; lackey = lackey->next)
    if (AFF_FLAGGED(lackey->follower, AFF_CHARM) && lackey->follower->master == ch)
      total++;

  return (total);
}

/** Called when a character that follows/is followed dies. If the character
 * is the leader of a group, it stops everyone in the group from following
 * them. Despite the title, this function does not actually perform the kill on
 * the character passed in as the argument.
 * @param ch The character (NPC or PC) to stop from following.
 * */
void die_follower(struct char_data *ch)
{
  struct follow_type *j, *k;

  if (ch->master)
    stop_follower(ch);

  for (k = ch->followers; k; k = j) {
    j = k->next;
    stop_follower(k->follower);
  }
}

/** Adds a new follower to a group.
 * @todo Maybe make circle_follow an inherent part of this function?
 * @pre Make sure to call circle_follow first. ch may also not already
 * be following anyone, otherwise core dump.
 * @param ch The character to follow.
 * @param leader The character to be followed. */
void add_follower(struct char_data *ch, struct char_data *leader)
{
  struct follow_type *k;

  if (ch->master) {
    core_dump();
    return;
  }

  ch->master = leader;

  CREATE(k, struct follow_type, 1);

  k->follower = ch;
  k->next = leader->followers;
  leader->followers = k;

  act("You now follow $N.", FALSE, ch, 0, leader, TO_CHAR);
  if (CAN_SEE(leader, ch))
    act("$n starts following you.", TRUE, ch, 0, leader, TO_VICT);
  act("$n starts to follow $N.", TRUE, ch, 0, leader, TO_NOTVICT);
}

/** Reads the next non-blank line off of the input stream. Empty lines are
 * skipped. Lines which begin with '*' are considered to be comments and are
 * skipped.
 * @pre Caller must allocate memory for buf.
 * @post If a there is a line to be read, the newline character is removed from
 * the file line ending and the string is returned. Else a null string is
 * returned in buf.
 * @param[in] fl The file to be read from.
 * @param[out] buf The next non-blank line read from the file. Buffer given must
 * be at least READ_SIZE (256) characters large. */
int get_line(FILE *fl, char *buf)
{
  char temp[READ_SIZE];
  int lines = 0;
  int sl;

  do {
    if (!fgets(temp, READ_SIZE, fl))
      return (0);
    lines++;
    tb_parse_context_note_line(fl);

    sl = strlen(temp);
    if (sl == READ_SIZE - 1 && temp[sl - 1] != '\n') {
      /* Longer than the buffer.  The rest must not come back as the next
       * line: in a player file it would be parsed as a tag of its own. */
      size_t dropped = 0;
      int c;

      while ((c = getc(fl)) != EOF && c != '\n')
        if (c != '\r')
          dropped++;
      if (dropped > 0 && *temp != '*')
        TB_PARSE_CONTEXT_ERROR(fl, "line longer than %d bytes; discarded "
                               "its last %zu bytes", READ_SIZE - 1, dropped);
    }
  } while (*temp == '*' || *temp == '\n' || *temp == '\r');

  /* Last line of file doesn't always have a \n, but it should. */
  sl = strlen(temp);
  while (sl > 0 && (temp[sl - 1] == '\n' || temp[sl - 1] == '\r'))
    temp[--sl] = '\0';

  /* buf is a pointer here: sizeof(buf) is only pointer size, not capacity.
   * The API requires callers to supply at least READ_SIZE bytes. */
  strlcpy(buf, temp, READ_SIZE);
  return (lines);
}

/** Create the full path, relative to the library path, of the player type
 * file to open.
 * @todo Make the return type bool.
 * @pre Caller is responsible for allocating memory buffer for the created
 * file name.
 * @post The potential file path to open is created. This function does not
 * actually open any file descriptors.
 * @param[out] filename Buffer to store the full path of the file to open.
 * @param[in] fbufsize The maximum size of filename, and the maximum size
 * of the path that can be written to it.
 * @param[in] mode What type of files can be created. Currently, recognized
 * modes are CRASH_FILE, ETEXT_FILE, SCRIPT_VARS_FILE and PLR_FILE.
 * @param[in] orig_name The player name to create the filepath (of type mode)
 * for. */
int get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name)
{
  const char *prefix, *middle, *suffix;
  char name[PATH_MAX], *ptr;

  if (orig_name == NULL || *orig_name == '\0' || filename == NULL) {
    log("SYSERR: NULL pointer or empty string passed to get_filename(), %p or %p.",
		(const void *)orig_name, (void *)filename);
    return (0);
  }

  switch (mode) {
  case CRASH_FILE:
    prefix = LIB_PLROBJS;
    suffix = SUF_OBJS;
    break;
  case ETEXT_FILE:
    prefix = LIB_PLRTEXT;
    suffix = SUF_TEXT;
    break;
  case SCRIPT_VARS_FILE:
    prefix = LIB_PLRVARS;
    suffix = SUF_MEM;
    break;
  case PLR_FILE:
    prefix = LIB_PLRFILES;
    suffix = SUF_PLR;
    break;
  default:
    return (0);
  }

  strlcpy(name, orig_name, sizeof(name));
  for (ptr = name; *ptr; ptr++)
    *ptr = LOWER(*ptr);

  switch (LOWER(*name)) {
  case 'a':  case 'b':  case 'c':  case 'd':  case 'e':
    middle = "A-E";
    break;
  case 'f':  case 'g':  case 'h':  case 'i':  case 'j':
    middle = "F-J";
    break;
  case 'k':  case 'l':  case 'm':  case 'n':  case 'o':
    middle = "K-O";
    break;
  case 'p':  case 'q':  case 'r':  case 's':  case 't':
    middle = "P-T";
    break;
  case 'u':  case 'v':  case 'w':  case 'x':  case 'y':  case 'z':
    middle = "U-Z";
    break;
  default:
    middle = "ZZZ";
    break;
  }

  tb_snprintf(filename, fbufsize, "%s%s"SLASH"%s.%s", prefix, middle, name, suffix);
  return (1);
}

/** Calculate the number of player characters (PCs) in the room. Any NPC (mob)
 * is not returned in the count.
 * @param room The room to check for PCs. */
int num_pc_in_room(struct room_data *room)
{
  int i = 0;
  struct char_data *ch;

  for (ch = room->people; ch != NULL; ch = ch->next_in_room)
    if (!IS_NPC(ch))
      i++;

  return (i);
}


/** This function (derived from basic fork() abort() idea by Erwin S Andreasen)
 * causes your MUD to dump core (assuming you can) but continue running. The
 * core dump will allow post-mortem debugging that is less severe than assert();
 * Don't call this directly as core_dump_unix() but as simply 'core_dump()' so
 * that it will be excluded from systems not supporting them. You still want to
 * call abort() or exit(1) for non-recoverable errors, of course. Wonder if
 * flushing streams includes sockets?
 * @param who The file in which this call was made.
 * @param line The line at which this call was made. */
void core_dump_real(const char *who, int line)
{
  log("SYSERR: Assertion failed at %s:%d!", who, line);

#if 1	/* By default, let's not litter. */
#if defined(CIRCLE_UNIX)
  /* These would be duplicated otherwise...make very sure. */
  tb_fflush(stdout);
  tb_fflush(stderr);
  tb_fflush(logfile);
  /* Everything, just in case, for the systems that support it. */
  tb_fflush(NULL);

  /* Kill the child so the debugger or script doesn't think the MUD crashed.
   * The 'autorun' script would otherwise run it again. */
  if (fork() == 0)
    abort();
#endif
#endif
}

/** Count the number bytes taken up by color codes in a string that will be
 * empty space once the color codes are converted and made non-printable.
 * @param string The string in which to check for color codes. */
int count_color_chars(char *string)
{
  int i, len;
  int num = 0;

	if (!string || !*string)
		return 0;

	len = strlen(string);
  for (i = 0; i < len; i++) {
    while (string[i] == '\t') {
      if (string[i + 1] == '\t')
        num++;
      else
        num += 2;
      i += 2;
    }
  }
  return num;
}

/* Not the prettiest thing I've ever written but it does the task which
 * is counting all characters in a string which are not part of the
 * protocol system. This is with the exception of detailed MXP codes. */
int count_non_protocol_chars(char * str)
{
  int count = 0;
  char *string = str;
  
  while (*string) {
    if (*string == '\r' || *string == '\n') {
      string++;
      continue;
    }
    if (*string == '@' || *string == '\t') {
      string++;
      if (*string != '[' && *string != '<' && *string != '>' && *string != '(' && *string != ')')
        string++;
      else if (*string == '[') {
        while (*string && *string != ']')
          string++;
        string++;
      } else
        string++;
      continue;
    }
    count++;
    string++;
  }
  
  return count;
}

/** Tests to see if a room is dark. Rules (unless overridden by ROOM_DARK):
 * Inside and City rooms are always lit. Outside rooms are dark at sunset and
 * night.
 * @todo Make the return value a bool.
 * @param room The real room to test for. */
int room_is_dark(room_rnum room)
{
  if (!VALID_ROOM_RNUM(room)) {
    log("room_is_dark: Invalid room rnum %d. (0-%d)", room, top_of_world);
    return (FALSE);
  }

  if (world[room].light)
    return (FALSE);

  if (ROOM_FLAGGED(room, ROOM_DARK))
    return (TRUE);

  if (SECT(room) == SECT_INSIDE || SECT(room) == SECT_CITY)
    return (FALSE);

  if (weather_info.sunlight == SUN_SET || weather_info.sunlight == SUN_DARK)
    return (TRUE);

  return (FALSE);
}

/** Calculates the Levenshtein distance between two strings. Currently used
 * by the mud to make suggestions to the player when commands are mistyped.
 * This function is most useful when an index of possible choices are available
 * and the results of this function are constrained and used to help narrow
 * down the possible choices. For more information about Levenshtein distance,
 * recommend doing an internet or wikipedia search.
 * @param s1 The input string.
 * @param s2 The string to be compared to. */
/* NOLINTBEGIN(clang-analyzer-security.ArrayBound): matrix dimensions are derived directly from strlen() + 1. */
int levenshtein_distance(const char *s1, const char *s2)
{
  int **d, result;
  size_t i, j, s1_len, s2_len;

  if (!s1 || !s2)
    return INT_MAX;

  s1_len = strlen(s1);
  s2_len = strlen(s2);

  /* The public result type is int, so lengths beyond INT_MAX cannot be
   * represented correctly and would also overflow the matrix cell values. */
  if (s1_len > INT_MAX || s2_len > INT_MAX)
    return INT_MAX;

  CREATE(d, int *, s1_len + 1);

  for (i = 0; i <= s1_len; i++) {
    CREATE(d[i], int, s2_len + 1); /* NOLINT(clang-analyzer-security.ArrayBound): d has s1_len + 1 rows. */
    d[i][0] = (int)i;
  }

  for (j = 0; j <= s2_len; j++)
    d[0][j] = (int)j;
  for (i = 1; i <= s1_len; i++)
    for (j = 1; j <= s2_len; j++)
      d[i][j] = MIN(d[i - 1][j] + 1, MIN(d[i][j - 1] + 1,
      d[i - 1][j - 1] + ((s1[i - 1] == s2[j - 1]) ? 0 : 1)));

  result = d[s1_len][s2_len];

  for (i = 0; i <= s1_len; i++)
    free(d[i]);
  free(d);

  return result;
}
/* NOLINTEND(clang-analyzer-security.ArrayBound) */

/** Removes a character from a piece of furniture. Unlike some of the other
 * _from_ functions, this does not place the character into NOWHERE.
 * @post ch is unattached from the furniture object.
 * @param ch The character to remove from the furniture object.
 */
void char_from_furniture(struct char_data *ch)
{
  struct obj_data *furniture;
  struct char_data *tempch;

  if (!SITTING(ch))
    return;

  if (!(furniture = SITTING(ch))){
    log("SYSERR: No furniture for char in char_from_furniture.");
    SITTING(ch) = NULL;
    NEXT_SITTING(ch) = NULL;
    return;
  }

  if (!(tempch = OBJ_SAT_IN_BY(furniture))){
    log("SYSERR: Char from furniture, but no furniture!");
    SITTING(ch) = NULL;
    NEXT_SITTING(ch) = NULL;
    GET_OBJ_VAL(furniture, 1) = 0;
    return;
  }

  if (tempch == ch){
    if (!NEXT_SITTING(ch)) {
      OBJ_SAT_IN_BY(furniture) = NULL;
    } else {
      OBJ_SAT_IN_BY(furniture) = NEXT_SITTING(ch);
    }
  } else {
    for (tempch = OBJ_SAT_IN_BY(furniture); tempch; tempch = NEXT_SITTING(tempch)) {
      if (NEXT_SITTING(tempch) == ch) {
        NEXT_SITTING(tempch) = NEXT_SITTING(ch);
      }
    }
  }
  GET_OBJ_VAL(furniture, 1) -= 1;
  SITTING(ch) = NULL;
  NEXT_SITTING(ch) = NULL;


  if (GET_OBJ_VAL(furniture, 1) < 1){
    OBJ_SAT_IN_BY(furniture) = NULL;
    GET_OBJ_VAL(furniture, 1) = 0;
  }

 return;
}


/* column_list
   The list is output in a fixed format, and only the number of columns can be adjusted
   This function will output the list to the player
   Vars:
     ch          - the player
     num_cols    - the desired number of columns
     list        - a pointer to a list of strings
     list_length - So we can work with lists that don't end with /n
     show_nums   - when set to TRUE, it will show a number before the list entry.

   Invariant: every write into buf is bounded by struct tb_buf, so output can
   never pass the end of the stack buffer. The list entries (social names,
   command names) and the viewer's screen width decide the size: social names
   come from builders through aedit and screen width from the player (40..200),
   so a long list on a wide screen can need more than MAX_STRING_LENGTH. When
   that happens the list is cut at the last entry that fits, the viewer sees
   the OVERFLOW marker, and a column-list-truncated warning names the viewer.
*/
#define COLUMN_LIST_OVERFLOW "\r\n*** OVERFLOW ***\r\n"

void column_list(struct char_data *ch, int num_cols, const char **list, int list_length, bool show_nums)
{
   size_t max_len = 0;
   int num_per_col, col_width, r, c, i, offset = 0;
   char buf[MAX_STRING_LENGTH];
   struct tb_buf out;

   /* Reserve room for the overflow marker so it always fits after a cut. */
   tb_buf_init(&out, buf, sizeof(buf) - (sizeof(COLUMN_LIST_OVERFLOW) - 1));
   /* Work out the longest list item */
   for (i=0; i<list_length; i++)
     if (max_len < strlen(list[i]))
       max_len = strlen(list[i]);

   /* auto columns case */
   if (num_cols == 0) {
	   num_cols = (IS_NPC(ch) ? 80 : GET_SCREEN_WIDTH(ch)) / (max_len + (show_nums ? 5 : 1));
   }

   /* Ensure that the number of columns is in the range 1-10 */
   num_cols = MIN(MAX(num_cols,1), 10);

   /* Calculate the width of each column */
   if (IS_NPC(ch))   col_width = 80 / num_cols;
   else              col_width = (GET_SCREEN_WIDTH(ch)) / num_cols;

   if (show_nums) col_width-=4;

   if (col_width < 0 || (size_t)col_width < max_len)
     TB_WARN_MSG("utils", "column-list-narrow", "column_list",
                 "col_width=%d max_len=%zu num_cols=%d for " DIAG_CHAR_FMT,
                 col_width, max_len, num_cols, DIAG_CHAR(ch));

   /* Calculate how many list items there should be per column */
   num_per_col = (list_length / num_cols) + ((list_length % num_cols) ? 1 : 0);

   /* Fill 'buf' with the columnised list; stop at the first cut. */
   for (r=0; r<num_per_col && !tb_buf_truncated(&out); r++)
   {
     for (c=0; c<num_cols && !tb_buf_truncated(&out); c++)
     {
       offset = (c*num_per_col)+r;
       if (offset < list_length)
       {
         if (show_nums)
           (void)tb_buf_appendf(&out, "%2d) %-*s", offset+1, col_width, list[(offset)]);
         else
           (void)tb_buf_appendf(&out, "%-*s", col_width, list[(offset)]);
       }
     }
     (void)tb_buf_append(&out, "\r\n");
   }

   if (tb_buf_truncated(&out)) {
     /* out.length <= its capacity - 1, so the reserved tail holds the marker. */
     strlcpy(buf + out.length, COLUMN_LIST_OVERFLOW, sizeof(buf) - out.length);
     TB_WARN_MSG("utils", "column-list-truncated", "column_list",
                 "list_length=%d num_cols=%d col_width=%d max_len=%zu "
                 "kept=%zu of capacity=%zu for " DIAG_CHAR_FMT,
                 list_length, num_cols, col_width, max_len, out.length,
                 sizeof(buf), DIAG_CHAR(ch));
   }

   /* Send the list to the player */
   page_string(ch->desc, buf, TRUE);
}
#undef COLUMN_LIST_OVERFLOW


/**
 * Search through a string array of flags for a particular flag.
 * @param flag_list An array of flag name strings. The final element must
 * be a string made up of a single newline.
 * @param flag_name The name to search in flag_list.
 */
int get_flag_by_name(const char *flag_list[], char *flag_name)
{
  int i=0;
  for (;flag_list[i] && *flag_list[i] && strcmp(flag_list[i], "\n") != 0; i++)
    if (!strcmp(flag_list[i], flag_name))
      return (i);
  return (NOFLAG);
}

/**
 * Reads a certain number of lines from the begining of a file, like performing
 * a 'head'.
 * @pre Expects an already open file and the user to supply enough memory
 * in the output buffer to hold the lines read from the file. Assumes the
 * file is a text file. Expects buf to be nulled out if the entire buf is
 * to be used, otherwise, appends file information beyond the first null
 * character. lines_to_read is assumed to be a positive number.
 * @post Rewinds the file pointer to the beginning of the file. If buf is
 * too small to handle the requested output, **OVERFLOW** is appended to the
 * buffer.
 * @param[in] file A pointer to an already successfully opened file.
 * @param[out] buf Buffer to hold the data read from the file. Will not
 * overwrite preexisting information in a non-null string.
 * @param[in] bufsize The total size of the buffer.
 * @param[in] lines_to_read The number of lines to be read from the front of
 * the file.
 */
int file_head(FILE *file, char *buf, size_t bufsize, int lines_to_read)
{
  int lines_read = 0;
  char line[READ_SIZE];
  size_t buflen, remaining;
  int readstatus = 1;
  int n;
  bool overflowed = FALSE;
  const char *overflow = "\r\n**OVERFLOW**\r\n";

  if (!file || !buf || bufsize == 0 || lines_to_read <= 0)
    return lines_to_read <= 0 ? lines_to_read : 0;

  buflen = strlen(buf);
  if (buflen >= bufsize)
    return 0;

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    return 0;

  while (lines_read < lines_to_read && readstatus > 0 && buflen < bufsize - 1) {
    readstatus = get_line(file, line);
    if (readstatus <= 0)
      break;

    remaining = bufsize - buflen;
    n = snprintf(buf + buflen, remaining, "%s\r\n", line);
    if (n < 0)
      break;
    if ((size_t)n >= remaining) {
      overflowed = TRUE;
      buflen = bufsize - 1;
      break;
    }

    buflen += (size_t)n;
    lines_read++;
  }

  if (overflowed) {
    size_t olen = strlen(overflow);

    if (olen + 1 >= bufsize) {
      core_dump();
      tb_snprintf(buf, bufsize, "%s", overflow);
    } else {
      size_t start = (buflen > olen) ? buflen - olen : 0;
      tb_snprintf(buf + start, bufsize - start, "%s", overflow);
    }
  }

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    TB_REPORT_ERRNO("fseek", "restore file position in line reader");
  return lines_read;
}

/**
 * Reads a certain number of lines from the end of the file, like performing
 * a 'tail'.
 * @pre Expects an already open file and the user to supply enough memory
 * in the output buffer to hold the lines read from the file. Assumes the
 * file is a text file. Expects buf to be nulled out if the entire buf is
 * to be used, otherwise, appends file information beyond the first null
 * character in buf. lines_to_read is assumed to be a positive number.
 * @post Rewinds the file pointer to the beginning of the file. If buf is
 * too small to handle the requested output, **OVERFLOW** is appended to the
 * buffer.
 * @param[in] file A pointer to an already successfully opened file.
 * @param[out] buf Buffer to hold the data read from the file. Will not
 * overwrite preexisting information in a non-null string.
 * @param[in] bufsize The total size of the buffer.
 * @param[in] lines_to_read The number of lines to be read from the back of
 * the file.
 */
int file_tail(FILE *file, char *buf, size_t bufsize, int lines_to_read)
{
  int lines_read = 0;
  int total_lines;
  int c;
  char line[READ_SIZE];
  size_t buflen, remaining;
  int readstatus = 1;
  int n;
  bool overflowed = FALSE;
  const char *overflow = "\r\n**OVERFLOW**\r\n";

  if (!file || !buf || bufsize == 0 || lines_to_read <= 0)
    return lines_to_read <= 0 ? lines_to_read : 0;

  buflen = strlen(buf);
  if (buflen >= bufsize)
    return 0;

  total_lines = file_numlines(file);

  while ((lines_to_read + lines_read) < total_lines) {
    do {
      c = fgetc(file);
    } while (c != '\n' && c != EOF);

    if (c == EOF)
      break;
    lines_read++;
  }

  lines_read = 0;

  while (lines_read < lines_to_read && readstatus > 0 && buflen < bufsize - 1) {
    readstatus = get_line(file, line);
    if (readstatus <= 0)
      break;

    remaining = bufsize - buflen;
    n = snprintf(buf + buflen, remaining, "%s\r\n", line);
    if (n < 0)
      break;
    if ((size_t)n >= remaining) {
      overflowed = TRUE;
      buflen = bufsize - 1;
      break;
    }

    buflen += (size_t)n;
    lines_read++;
  }

  if (overflowed) {
    size_t olen = strlen(overflow);

    if (olen + 1 >= bufsize) {
      core_dump();
      tb_snprintf(buf, bufsize, "%s", overflow);
    } else {
      size_t start = (buflen > olen) ? buflen - olen : 0;
      tb_snprintf(buf + start, bufsize - start, "%s", overflow);
    }
  }

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    TB_REPORT_ERRNO("fseek", "restore file position in line reader");
  return lines_read;
}

/** Returns the byte size of a file. We assume size_t to be a large enough type
 * to handle all of the file sizes in the mud, and so do not make SIZE_MAX
 * checks.
 * @pre file parameter must already be opened.
 * @post file will be rewound.
 * @param file The file to determine the size of.
 */
size_t file_sizeof(FILE *file)
{
  size_t numbytes = 0;
  int c;

  if (!file)
    return 0;

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    return 0;

  while ((c = fgetc(file)) != EOF)
    numbytes++;

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    TB_REPORT_ERRNO("fseek", "restore file position after size scan");
  return numbytes;
}

/** Returns the number of newlines "\\n" in a file, which we equate to number of
 * lines. We assume the int type more than adequate to count the number of lines
 * and do not make checks for overrunning INT_MAX.
 * @pre file parameter must already be opened.
 * @post file will be rewound.
 * @param file The file to determine the size of.
 */
int file_numlines(FILE *file)
{
  int numlines = 0;
  int c;

  if (!file)
    return 0;

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    return 0;

  while ((c = fgetc(file)) != EOF)
    if (c == '\n' && numlines < INT_MAX)
      numlines++;

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0)
    TB_REPORT_ERRNO("fseek", "restore file position after line count");
  return numlines;
}


/** A string converter designed to deal with the compile sensitive IDXTYPE.
 * Relies on the friendlier strtol function.
 * @pre Assumes that NOWHERE, NOTHING, NOBODY, NOFLAG, etc are all equal.
 * @param str_to_conv A string of characters to attempt to convert to an
 * IDXTYPE number.
 */
IDXTYPE atoidx( const char *str_to_conv )
{
  long int result;

  /* Check for errors */
  errno = 0;

  result = strtol(str_to_conv, NULL, 10);

  if ( errno || (result > IDXTYPE_MAX) || (result < 0) )
    return NOWHERE; /* All of the NO* settings should be the same */
  else
    return (IDXTYPE) result;
}

#define isspace_ignoretabs(c) ((c)!='\t' && isspace(c))

/*
   strfrmt (String Format) function
   Used by automap/map system
   Re-formats a string to fit within a particular size box.
   Recognises @ color codes, and if a line ends in one color, the
   next line will start with the same color.
   Ends every line with \tn to prevent color bleeds.
*/
static char *strfrmt_append(char *dst, const char *end, const char *src)
{
  while (*src && dst < end)
    *dst++ = *src++;
  *dst = '\0';
  return dst;
}

char *strfrmt(char *str, int w, int h, int justify, int hpad, int vpad)
{
  static char ret[MAX_STRING_LENGTH];

  (void)justify;
  char line[MAX_INPUT_LENGTH];
  char *sp = str;
  char *lp = line;
  char *rp = ret;
  char *wp;
  /* Where a word, a space or a pad may be written to.  line stops five
   * bytes short of its end to keep room for the longest sequence that
   * closes a line -- "\tn\r\n" and the terminator -- so the writes that
   * close one need no bound of their own.  That holds because every other
   * write through lp below is guarded by this, bar the colour restore,
   * which runs on a line just reset to empty.  Without the reservation a
   * full line would lose its newline to the same guards that stop the
   * overflow, and two lines of the box would run together. */
  const char *lend = line + sizeof(line) - 5;
  const char *rend = ret + sizeof(ret) - 1;
  int wlen = 0, llen = 0, lcount = 0;
  char last_color='n';
  bool new_line_started = FALSE;

  /* w is the width of the box, and it drives the padding fills below.  It
   * arrives as the player's screen width less the width of the map beside
   * it, and str_and_map() does not clamp CONFIG_MINIMAP_SIZE, so a large
   * minimap on a forty-column screen makes it negative -- which the
   * memset() at the foot of this function turned into a size_t of about
   * eighteen quintillion.  Wider than a line can hold is meaningless too,
   * and only spins the fills. */
  if (w < 0)
    w = 0;
  if (w > (int)sizeof(line) - 5)
    w = (int)sizeof(line) - 5;

  memset(line, '\0', MAX_INPUT_LENGTH);
  /* Nomalize spaces and newlines */
  /* Split into lines, including convert \\ into \r\n */
  while(*sp) {
    /* eat leading space */
    while(*sp && isspace_ignoretabs(*sp)) sp++;
    /* word begins */
    wp = sp;
    wlen = 0;
    while(*sp) { /* Find the end of the word */
      if(isspace_ignoretabs(*sp)) break;
      if(*sp=='\\' && sp[1] && sp[1]=='\\') {
        if(sp!=wp)
          break; /* Finish dealing with the current word */
        sp += 2; /* Eat the marker and any trailing space */
        while(*sp && isspace_ignoretabs(*sp)) sp++;
        wp = sp;
        /* Start a new line */
        if(hpad)
          for(; llen < w; llen++)
            if (lp < lend) *lp++ = ' ';
        *lp++ = '\r';
        *lp++ = '\n';
        *lp = '\0';
        rp = strfrmt_append(rp, rend, line);
        llen = 0;
        lcount++;
        lp = line;
      } else if (*sp=='`'||*sp=='$'||*sp=='#') {
        if (sp[1] && (sp[1]==*sp))
          wlen++; /* One printable char here */
        /* Eat the whole code, but not the terminator.  A description whose
         * last character is one of these has nothing after it, and stepping
         * two would carry the scan below past the end of the string.  redit
         * takes any text, so that is a builder's typo away. */
        sp += sp[1] ? 2 : 1;
      } else if (*sp=='\t'&&sp[1]) {
        char MXPcode = sp[1]=='[' ? ']' : sp[1]=='<' ? '>' : '\0';
	
  if (!MXPcode)
	   last_color = sp[1];
 
        sp += 2; /* Eat the code */
        if (MXPcode)
        {
           while (*sp!='\0'&&*sp!=MXPcode)
             ++sp; /* Eat the rest of the code */
        } 
      } else {
        wlen++;
        sp++;
      }
    }
    if(llen + wlen + (lp==line ? 0 : 1) > w) {
      /* Start a new line */
      if(hpad)
        for(; llen < w; llen++)
          if (lp < lend) *lp++ = ' ';
      *lp++ = '\t';  /* 'normal' color */
      *lp++ = 'n';
      *lp++ = '\r'; /* New line */
      *lp++ = '\n';
      *lp = '\0';
      rp = strfrmt_append(rp, rend, line);
      llen = 0;
      lcount++;
      lp = line;
      if (last_color != 'n') {
        *lp++ = '\t';  /* restore previous color */
        *lp++ = last_color;
        new_line_started = TRUE;
      }
    }
    /* add word to line */
    if (lp!=line && new_line_started!=TRUE) {
      if (lp < lend) *lp++ = ' ';
      llen++;
    }
    new_line_started = FALSE;
    llen += wlen ;
    /* wlen counts printable width and this copies bytes, so the two part
     * company over colour codes -- and a word with no space in it is not
     * broken at all, whatever its length.  Neither of those is a reason
     * to write past the end of line. */
    for( ; wp!=sp && lp<lend ; *lp++ = *wp++);
  }
  /* Copy over the last line */
  if(lp!=line) {
    if(hpad)
      for(; llen < w; llen++)
        if (lp < lend) *lp++ = ' ';
    *lp++ = '\r';
    *lp++ = '\n';
    *lp = '\0';
    rp = strfrmt_append(rp, rend, line);
    lcount++;
  }
  if(vpad) {
    while(lcount < h) {
      if(hpad) {
        int pad = w < rend - rp ? w : (int)(rend - rp);

        if (pad > 0) {
          memset(rp, ' ', pad);
          rp += pad;
        }
      }
      if (rp < rend) *rp++ = '\r';
      if (rp < rend) *rp++ = '\n';
      lcount++;
    }
    *rp = '\0';
  }
  return ret;
}

/**
   Takes two long strings (multiple lines) and joins them side-by-side.
   Used by the automap/map system
   @param str1 The string to be displayed on the left.
   @param str2 The string to be displayed on the right.
   @param joiner ???.
*/
char *strpaste(char *str1, char *str2, char *joiner)
{
  static char ret[MAX_STRING_LENGTH + 1];
  const char *sp1 = str1 ? str1 : "";
  const char *sp2 = str2 ? str2 : "";
  const char *safe_joiner = joiner ? joiner : "";
  size_t used = 0;
  size_t jlen = strlen(safe_joiner);

  while (used < MAX_STRING_LENGTH && (*sp1 || *sp2)) {
    while (used < MAX_STRING_LENGTH && *sp1 && !ISNEWL(*sp1))
      ret[used++] = *sp1++;

    if (*sp1) {
      if (sp1[1] && sp1[1] != sp1[0] && ISNEWL(sp1[1]))
        sp1++;
      sp1++;
    }

    if (jlen > MAX_STRING_LENGTH - used)
      break;
    if (jlen != 0) {
      memcpy(ret + used, safe_joiner, jlen);
      used += jlen;
    }

    while (used < MAX_STRING_LENGTH && *sp2 && !ISNEWL(*sp2))
      ret[used++] = *sp2++;

    if (*sp2) {
      if (sp2[1] && sp2[1] != sp2[0] && ISNEWL(sp2[1]))
        sp2++;
      sp2++;
    }

    if (used > MAX_STRING_LENGTH - 2)
      break;
    ret[used++] = '\r';
    ret[used++] = '\n';
  }

  ret[used] = '\0';
  return ret;
}

/* Create a blank affect struct */
void new_affect(struct affected_type *af)
{
  int i;
  af->spell     = 0;
  af->duration  = 0;
  af->modifier  = 0;
  af->location  = APPLY_NONE;
  for (i=0; i<AF_ARRAY_MAX; i++) af->bitvector[i]=0;
}

/* Handy function to get class ID number by name (abbreviations allowed) */
int get_class_by_name(char *classname)
{
    int i;
    for (i=0; i<NUM_CLASSES; i++)
      if (is_abbrev(classname, pc_class_types[i])) return(i);

    return (-1);
}

char * convert_from_tabs(char * string)
{
  static char buf[MAX_STRING_LENGTH * 8];
  
  strlcpy(buf, string, sizeof(buf));
  parse_tab(buf);
  return(buf);
}

/* This removes all trailing whitespace from the end of a string */
char *right_trim_whitespace(const char *string)
{
  char *r = strdup(string);

  if (r != NULL) {
    size_t len = strlen(r);

    while (len > 0 &&
           (isspace((unsigned char) r[len - 1]) ||
            !isprint((unsigned char) r[len - 1])))
      len--;

    r[len] = '\0';
  }

  return r;
}

/**
 * Remove all occurrences of a given word in string.
 */
void remove_from_string(char *string, const char *to_remove)
{
    int i, j, string_len, to_remove_len;
    int found;

    string_len   = strlen(string);      // Length of string
    to_remove_len = strlen(to_remove); // Length of word to remove


    for(i=0; i <= string_len - to_remove_len; i++)
    {
        /* Match word with string */
        found = 1;
        for(j=0; j<to_remove_len; j++)
        {
            if(string[i + j] != to_remove[j])
            {
                found = 0;
                break;
            }
        }

        /* If it is not a word */
        if(string[i + j] != ' ' && string[i + j] != '\t' && string[i + j] != '\n' && string[i + j] != '\0') 
        {
            found = 0;
        }

        /*
         * If word is found then shift all characters to left
         * and decrement the string length
         */
        if(found == 1)
        {
            for(j=i; j<=string_len - to_remove_len; j++)
            {
                string[j] = string[j + to_remove_len];
            }

            string_len = string_len - to_remove_len;

            // We will match next occurrence of word from current index.
            i--;
        }
    }
    
}
