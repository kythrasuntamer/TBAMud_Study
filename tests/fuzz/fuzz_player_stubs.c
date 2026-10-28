/* What fuzz_player links instead of the rest of the server.
 *
 * The loaders and the object/character lifecycle they use are production
 * code (see fuzz_player.c). This file supplies:
 *   - globals of files that are not linked (comm.c, config.c, ...);
 *   - output: send_to_char(), write_to_output(), act() format into a sink,
 *     so a bad format or argument from a loader is still caught;
 *   - stand-ins for game systems the loaders reference but never reach on
 *     these paths (triggers, events, fighting, histories). Those abort with
 *     "fuzz_player: unexpected call" if they are ever reached, so a stub can
 *     never silently change what a loader does.
 */
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "handler.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include "config.h"
#include "act.h"
#include "lists.h"
#include "prefedit.h"

#include <stdarg.h>

#include "fuzz_player.h"

#define UNEXPECTED() do { \
  fprintf(stderr, "==fuzz_player== ERROR: unexpected call to %s(): a stub " \
          "was reached; link the real function\n", __func__); \
  abort(); \
} while (0)

/* The same for a stub of an *_at function (the target of a call-site macro
 * such as simple_list() -> simple_list_at()): also names the caller. */
#define UNEXPECTED_AT(file, line, func) do { \
  fprintf(stderr, "==fuzz_player== ERROR: unexpected call to %s() from " \
          "%s:%d (%s): a stub was reached; link the real function\n", \
          __func__, (file) ? (file) : "?", (line), (func) ? (func) : "?"); \
  abort(); \
} while (0)

/* ---------------------------------------------------------------- */
/* Sinks                                                            */
/* ---------------------------------------------------------------- */
static volatile size_t sink_total;

void fuzz_sink_string(const char *str)
{
  sink_total += strlen(str);
}

static void sink_vformat(const char *fmt, va_list args)
  __attribute__ ((format (printf, 1, 0)));
static void sink_vformat(const char *fmt, va_list args)
{
  static char sink[MAX_STRING_LENGTH];

  if (fmt == NULL)
    return;
  if (vsnprintf(sink, sizeof(sink), fmt, args) < 0)
    sink[0] = '\0';
  sink_total += strlen(sink);
}

static char *log_buffer;
static size_t log_size;

void fuzz_log_open(void)
{
  if ((logfile = open_memstream(&log_buffer, &log_size)) == NULL)
    abort();
}

void fuzz_log_rewind(void)
{
  if (logfile != NULL && fseek(logfile, 0L, SEEK_SET) != 0)
    abort();
}

/* ---------------------------------------------------------------- */
/* comm.c                                                           */
/* ---------------------------------------------------------------- */
FILE *logfile = NULL;
struct descriptor_data *descriptor_list = NULL;
int circle_shutdown = 0;
int circle_reboot = 0;
int no_specials = 0;
int scheck = 0;
unsigned long pulse = 0;
ush_int port = 0;
socket_t mother_desc = 0;

size_t send_to_char(struct char_data *ch, const char *messg, ...)
{
  va_list args;

  (void) ch;
  va_start(args, messg);
  sink_vformat(messg, args);
  va_end(args);
  return 0;
}

size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{
  va_list args;

  (void) t;
  va_start(args, txt);
  sink_vformat(txt, args);
  va_end(args);
  return 0;
}

size_t vwrite_to_output(struct descriptor_data *t, const char *format, va_list args)
{
  va_list copy;

  (void) t;
  va_copy(copy, args);
  sink_vformat(format, copy);
  va_end(copy);
  return 0;
}

/* act() is a format of its own ($n, $p ...); read the template and the
 * strings it would substitute. */
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type)
{
  (void) src_file; (void) src_line; (void) src_func; (void) hide_invisible;
  (void) vict_obj; (void) type;
  if (str)
    fuzz_sink_string(str);
  if (ch && GET_NAME(ch))
    fuzz_sink_string(GET_NAME(ch));
  if (obj && obj->short_description)
    fuzz_sink_string(obj->short_description);
  return NULL;
}

/* ---------------------------------------------------------------- */
/* Game systems the loaders reference but never reach here          */
/* ---------------------------------------------------------------- */
/* fight.c: char_to_room()/char_from_room() stop a fight; nobody fights. */
void stop_fighting(struct char_data *ch) { (void) ch; UNEXPECTED(); }

/* act.informative.c: free_char() frees communication histories; a loaded
 * character has none. */
void free_history(struct char_data *ch, int type) { (void) ch; (void) type; UNEXPECTED(); }

/* prefedit.c: free_char() makes every preference editor still pointing at a
 * departing player let go of it, by walking descriptor_list. The harness
 * never puts a descriptor on that list (fuzz_player.c's player_desc is only
 * reached through ch->desc), so the real function finds nothing to do; this
 * does the same, and stops if the list is ever non-empty, where it could not
 * stand in for the real walk (and its cleanup_olc()). */
void prefedit_forget_char(struct char_data *ch)
{
  (void) ch;
  if (descriptor_list != NULL)
    UNEXPECTED();
}

/* lists.c / dg_event.c: extract_obj() and free_char() cancel pending
 * events; nothing here schedules any (ch->events and obj->events stay NULL).
 * simple_list(), free_list() and event_cancel() are call-site macros over
 * these *_at functions (lists.h, dg_event.h). */
void *simple_list_at(struct list_data *pList, const char *file, int line,
                     const char *func)
{
  (void) pList;
  UNEXPECTED_AT(file, line, func);
}

void free_list_at(struct list_data *pList, const char *file, int line,
                  const char *func)
{
  (void) pList;
  UNEXPECTED_AT(file, line, func);
}

void event_cancel_at(struct event *event, const char *file, int line,
                     const char *func)
{
  (void) event;
  UNEXPECTED_AT(file, line, func);
}
