/* Minimal utils.c replacements for standalone persistence tests that link a
 * production source file (players.c, objsave.c, house.c, mail.c) without
 * utils.c, whose tb_error_log_at() would collide with tb_error_capture.h.
 * Include from exactly one translation unit per test executable.
 */
#ifndef TB_PERSIST_SUPPORT_H
#define TB_PERSIST_SUPPORT_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void *tb_realloc_array_at(void *ptr, size_t count, size_t element_size,
                          const char *source_file, int source_line,
                          const char *source_func)
{
  (void)source_file; (void)source_line; (void)source_func;
  if (count == 0 || element_size == 0 || count > (size_t)-1 / element_size)
    return NULL;
  return realloc(ptr, count * element_size);
}

/* Copies of utils.c's bounded string builder (struct tb_buf). */
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
  size_t available, copy_len;

  if (buf == NULL || buf->data == NULL || buf->capacity == 0 || text == NULL)
    return 0;
  available = tb_buf_remaining(buf);
  copy_len = length <= available ? length : available;
  if (copy_len != 0)
    memcpy(buf->data + buf->length, text, copy_len);
  buf->length += copy_len;
  buf->data[buf->length] = '\0';
  if (copy_len != length) {
    buf->truncated = 1;
    return 0;
  }
  return 1;
}

int tb_buf_append(struct tb_buf *buf, const char *text)
{
  return text != NULL ? tb_buf_appendn(buf, text, strlen(text)) : 0;
}

int tb_buf_appendf(struct tb_buf *buf, const char *format, ...)
{
  va_list args;
  int n;
  size_t available;

  if (buf == NULL || buf->data == NULL || buf->capacity == 0 || format == NULL)
    return 0;
  available = tb_buf_remaining(buf);
  va_start(args, format);
  n = vsnprintf(buf->data + buf->length, available + 1, format, args);
  va_end(args);
  if (n < 0) {
    buf->truncated = 1;
    return 0;
  }
  if ((size_t)n > available) {
    buf->length += available;
    buf->truncated = 1;
    return 0;
  }
  buf->length += (size_t)n;
  return 1;
}

int tb_buf_truncated(const struct tb_buf *buf)
{
  return buf != NULL ? buf->truncated : 1;
}

#endif
