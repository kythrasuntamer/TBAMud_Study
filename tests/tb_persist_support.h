/* Minimal utils.c replacements for standalone persistence tests that link a
 * production source file (players.c, objsave.c, house.c, mail.c) without
 * utils.c, whose tb_error_log_at() would collide with tb_error_capture.h.
 * Include from exactly one translation unit per test executable.
 */
#ifndef TB_PERSIST_SUPPORT_H
#define TB_PERSIST_SUPPORT_H

#include <stdlib.h>

int MIN(int a, int b) { return a < b ? a : b; }
int MAX(int a, int b) { return a > b ? a : b; }

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  (void)source_file; (void)source_line; (void)source_func;
  if (count == 0 || element_size == 0)
    return NULL;
  return calloc(count, element_size);
}

#endif
