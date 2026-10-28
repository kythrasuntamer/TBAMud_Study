/* Shared fixture for the gameplay regression tests (act.item.c,
 * act.movement.c, act.offensive.c, fight.c, magic.c).
 *
 * The tests link the production gameplay source together with the real
 * handler.c, utils.c and dg_scripts.c UID registry, and replace only what
 * would need a booted world: DG trigger entry points (routed to a
 * per-test hook so a test can play the part of a hostile script), object
 * and mobile prototypes, and output. Objects are heap-allocated and really
 * freed by extract_obj(), so a stale pointer is a use-after-free that ASan
 * and Valgrind report.
 */
#ifndef TB_GAMEPLAY_FIXTURE_H
#define TB_GAMEPLAY_FIXTURE_H

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"

/* DG entry points the fixture intercepts. */
enum fx_trigger {
  FX_GET_OTRIG, FX_DROP_OTRIG, FX_DROP_WTRIG, FX_GIVE_OTRIG,
  FX_RECEIVE_MTRIG, FX_WEAR_OTRIG, FX_REMOVE_OTRIG, FX_CONSUME_OTRIG,
  FX_DOOR_MTRIG, FX_DOOR_WTRIG, FX_FIGHT_MTRIG, FX_LOAD_MTRIG,
  FX_DEATH_MTRIG, FX_HITPRCNT_MTRIG,
  FX_TRIGGER_COUNT
};

/* A test's stand-in for a DG script. obj is the trigger's object (or NULL),
 * actor the acting character. The return value is what the trigger entry
 * point returns (1 = allow); for void entry points it is ignored. */
typedef int (*fx_trigger_fn)(enum fx_trigger kind, struct char_data *actor,
                             struct obj_data *obj, int arg);

extern fx_trigger_fn fx_hook;
extern int fx_trigger_calls[FX_TRIGGER_COUNT];

/* rand_number() as seen by the code under test: returns fx_rand_value when
 * it is >= 0 (clamped into [from, to]), otherwise from. */
extern int fx_rand_value;
/* Counts of core_dump() and log() calls made by the code under test. */
extern int fx_core_dumps;
extern int fx_log_lines;
/* Last text sent to a character with send_to_char(), any recipient. */
extern char fx_last_send[512];
/* Arguments of the last new_mud_event() call (no event is scheduled). */
extern int fx_last_event_id;
extern void *fx_last_event_struct;
extern char fx_last_event_vars[64];

/* The tests link utils.c, so tb_error_capture.h's capturing reporter is
 * compiled under this name and the linker (--wrap=tb_error_log_at) routes
 * the code under test to it. Include the capture header as:
 *   #define tb_error_log_at __wrap_tb_error_log_at
 *   #include "tb_error_capture.h"
 *   #undef tb_error_log_at
 */
void __wrap_tb_error_log_at(const char *severity, const char *subsystem,
                            const char *operation, const char *resource,
                            int errnum, const char *source_file,
                            int source_line, const char *source_func,
                            const char *detail_fmt, ...)
                            __attribute__ ((format (printf, 9, 10)));

void fx_setup(int rooms);
void fx_teardown(void);

struct char_data *fx_new_char(const char *name, int npc, room_rnum room);
/* Extract ch and its inventory/equipment immediately (memory really freed). */
void fx_free_char(struct char_data *ch);
struct obj_data *fx_new_obj(const char *name, int type);

#endif
