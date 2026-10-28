/* Executable tests of the production DG script memory handlers
 * (src/dg_handler.c).  Rooms, scripts and prototype lists are disposable
 * fixtures; the game globals dg_handler.c needs are test-local.
 * Fatal invariant checks abort(), so those cases run in a forked child and
 * the logger turns the expected diagnostic op into a distinctive exit code.
 */
#include "unity.h"
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_scripts.h"
#include "dg_event.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct obj_data *object_list;
struct trig_data *trigger_list;
struct index_data **trig_index;

/* ---- diagnostic capture ------------------------------------------------ */
#define EXIT_EXPECTED_OP 42
static const char *fatal_op_to_exit;   /* in a child: _exit() on this op */
static char last_op[64];
static int n_captured;

void tb_error_log_at(const char *severity, const char *subsystem,
                     const char *operation, const char *resource, int errnum,
                     const char *source_file, int source_line,
                     const char *source_func, const char *detail_fmt, ...)
{
  (void)severity; (void)subsystem; (void)resource; (void)errnum;
  (void)source_file; (void)source_line; (void)source_func; (void)detail_fmt;
  if (fatal_op_to_exit && !strcmp(operation, fatal_op_to_exit))
    _exit(EXIT_EXPECTED_OP);
  (void)snprintf(last_op, sizeof(last_op), "%s", operation);
  n_captured++;
}

void basic_mud_log(const char *format, ...) { (void)format; }

void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *file, int line, const char *func)
{
  void *allocation;
  (void)file; (void)line; (void)func;
  if (element_size == 0 || count == 0 || count > SIZE_MAX / element_size)
    abort();
  allocation = calloc(count, element_size);
  if (!allocation)
    abort();
  return allocation;
}

int str_cmp(const char *a, const char *b) { return strcasecmp(a, b); }
int get_filename(char *filename, size_t fbufsize, int mode, const char *orig_name)
{ (void)filename; (void)fbufsize; (void)mode; (void)orig_name; return 0; }

/* No fixture trigger has a pending wait; reaching the queue is a bug. */
void event_cancel_at(struct event *event, const char *file, int line,
                     const char *func)
{ (void)event; (void)file; (void)line; (void)func; abort(); }

/* ---- fixtures ---------------------------------------------------------- */
static struct room_data rooms[3];

void setUp(void)
{
  memset(rooms, 0, sizeof(rooms));
  world = rooms;
  top_of_world = 2;
  last_op[0] = '\0';
  n_captured = 0;
  fatal_op_to_exit = NULL;
}

void tearDown(void) { world = NULL; top_of_world = 0; }

/* Run fn in a child and return its exit status (or 128 + signal). */
static int run_in_child(void (*fn)(void), const char *expected_op)
{
  int status;
  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    fatal_op_to_exit = expected_op;
    fn();
    _exit(0);
  }
  TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return WEXITSTATUS(status);
}

static void free_shared_proto_with_last_room(void)
{
  struct trig_proto_list *p = calloc(1, sizeof(*p));
  struct room_data other;

  memset(&other, 0, sizeof(other));
  other.proto_script = p;
  rooms[2].proto_script = p;      /* world[top_of_world] shares the list */
  free_proto_script(&other, WLD_TRIGGER);
}

/* D2-05: the ownership scan skipped world[top_of_world] and used assert(),
 * so a list shared with the last room was freed silently. */
static void test_proto_shared_with_last_room_is_fatal(void)
{
  TEST_ASSERT_EQUAL_INT(EXIT_EXPECTED_OP,
      run_in_child(free_shared_proto_with_last_room,
                   "proto-script-shared-owner"));
}

static void extract_shared_script_with_last_room(void)
{
  struct script_data *sc = calloc(1, sizeof(*sc));
  struct room_data other;

  memset(&other, 0, sizeof(other));
  other.script = sc;
  rooms[2].script = sc;
  extract_script(&other, WLD_TRIGGER);
}

static void test_script_shared_with_last_room_is_fatal(void)
{
  TEST_ASSERT_EQUAL_INT(EXIT_EXPECTED_OP,
      run_in_child(extract_shared_script_with_last_room,
                   "script-shared-owner"));
}

/* An unshared script and proto list are freed without diagnostics. */
static void test_unshared_script_frees_quietly(void)
{
  rooms[2].script = calloc(1, sizeof(struct script_data));
  rooms[2].proto_script = calloc(1, sizeof(struct trig_proto_list));
  extract_script(&rooms[2], WLD_TRIGGER);
  free_proto_script(&rooms[2], WLD_TRIGGER);
  TEST_ASSERT_NULL(rooms[2].script);
  TEST_ASSERT_NULL(rooms[2].proto_script);
  TEST_ASSERT_EQUAL_INT(0, n_captured);
}

/* D2-09: bad extract_script() arguments report a structured op. */
static void test_extract_script_bad_args_are_reported(void)
{
  extract_script(NULL, WLD_TRIGGER);
  TEST_ASSERT_EQUAL_STRING("extract-script-null", last_op);
  extract_script(&rooms[0], 99);
  TEST_ASSERT_EQUAL_STRING("extract-script-type", last_op);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_extract_script_bad_args_are_reported);
  RUN_TEST(test_proto_shared_with_last_room_is_fatal);
  RUN_TEST(test_script_shared_with_last_room_is_fatal);
  RUN_TEST(test_unshared_script_frees_quietly);
  return UNITY_END();
}
