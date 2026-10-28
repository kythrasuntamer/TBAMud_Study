/* Capturing replacement for utils.c's tb_error_log_at(), for standalone test
 * executables that do not link utils.c. Include from exactly one translation
 * unit per test executable. Tests assert that guards report their failures
 * (fail loudly), not merely that they avoid crashing.
 */
#ifndef TB_ERROR_CAPTURE_H
#define TB_ERROR_CAPTURE_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int tb_captured_errors;
static char tb_captured_severity[16];
static char tb_captured_operation[64];
static char tb_captured_detail[512];
static int tb_captured_line;
/* Every operation and detail since the last reset, one per line, for tests
 * where a guard reports more than one record (the fields above hold only
 * the last one). Truncated silently: this is test scaffolding. */
static char tb_captured_op_log[1024];
static char tb_captured_detail_log[4096];

void tb_error_log_at(const char *severity, const char *subsystem,
                     const char *operation, const char *resource, int errnum,
                     const char *source_file, int source_line,
                     const char *source_func, const char *detail_fmt, ...)
{
  va_list args;
  (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_func;
  tb_captured_errors++;
  tb_captured_line = source_line;
  if (snprintf(tb_captured_severity, sizeof(tb_captured_severity), "%s",
               severity ? severity : "") < 0)
    tb_captured_severity[0] = '\0';
  if (snprintf(tb_captured_operation, sizeof(tb_captured_operation), "%s",
               operation ? operation : "") < 0)
    tb_captured_operation[0] = '\0';
  va_start(args, detail_fmt);
  if (vsnprintf(tb_captured_detail, sizeof(tb_captured_detail),
                detail_fmt, args) < 0)
    tb_captured_detail[0] = '\0';
  va_end(args);
  {
    size_t used = strlen(tb_captured_op_log);
    if (snprintf(tb_captured_op_log + used, sizeof(tb_captured_op_log) - used,
                 "%s\n", tb_captured_operation) < 0)
      tb_captured_op_log[used] = '\0';
    used = strlen(tb_captured_detail_log);
    if (snprintf(tb_captured_detail_log + used,
                 sizeof(tb_captured_detail_log) - used, "%s\n",
                 tb_captured_detail) < 0)
      tb_captured_detail_log[used] = '\0';
  }
}

static void tb_error_capture_reset(void)
{
  tb_captured_errors = 0;
  tb_captured_line = 0;
  tb_captured_severity[0] = tb_captured_operation[0] = '\0';
  tb_captured_detail[0] = '\0';
  tb_captured_op_log[0] = tb_captured_detail_log[0] = '\0';
}

#endif
