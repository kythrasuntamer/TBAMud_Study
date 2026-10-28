/* libFuzzer target for the telnet/MUD protocol layer (protocol.c).
 *
 * Every byte a client sends passes through ProtocolInput() before the
 * command interpreter sees it: telnet IAC negotiation, subnegotiation
 * (TTYPE, NAWS, CHARSET, MSDP, GMCP, MSSP, ATCP), and in-band MXP. Text the
 * server sends back is colour-expanded by ProtocolOutput(). Both run on
 * untrusted input, so both are fuzzed here with ASan + UBSan.
 *
 * Build: cmake -DENABLE_FUZZING=ON -DCMAKE_C_COMPILER=clang ...
 * Run:   bin/fuzz_protocol -max_total_time=300 tests/fuzz/corpus/protocol
 * The input mirrors comm.c's process_input(): at most MAX_PROTOCOL_BUFFER-1
 * bytes, NUL-terminated, written into a MAX_RAW_INPUT_LENGTH output buffer.
 */
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "db.h"
#include "protocol.h"

#include <stdint.h>

/* Symbols protocol.c reaches outside itself. */
struct player_special_data dummy_mob;

/* db.c: DIAG_DESC (comm.h) names a descriptor's character by mob vnum or
 * player idnum. desc.character stays NULL below, so no character is ever
 * looked up; the tables exist only so the reference links. */
struct index_data *mob_index = NULL;
mob_rnum top_of_mobt = 0;

/* utils.c: the structured diagnostics behind protocol.c's PROTOCOL_WARN
 * (TB_WARN_EVERY), TB_ERROR_MSG and TB_FATAL_MSG reports. Format the detail
 * exactly as the server would, into a sink, so a bad format or argument in
 * a report is still caught. A FATAL report is followed by abort() at its
 * call site, as in the server. */
void tb_error_log_at(const char *severity, const char *subsystem,
                     const char *operation, const char *resource, int errnum,
                     const char *source_file, int source_line,
                     const char *source_func, const char *detail_fmt, ...)
{
  static char sink[MAX_STRING_LENGTH / 2];
  va_list args;

  (void)severity; (void)subsystem; (void)operation; (void)resource;
  (void)errnum; (void)source_file; (void)source_line; (void)source_func;
  if (detail_fmt == NULL)
    return;
  va_start(args, detail_fmt);
  if (vsnprintf(sink, sizeof(sink), detail_fmt, args) < 0)
    sink[0] = '\0';
  va_end(args);
}

/* utils.c: CREATE()'s allocator. The server reports a zero or overflowing
 * count, or a failed calloc(), as FATAL and CREATE() then aborts, so any of
 * them reached from client bytes is a finding: stop loudly, naming the
 * CREATE() call site. */
void *tb_calloc_array_at(size_t count, size_t element_size,
                         const char *source_file, int source_line,
                         const char *source_func)
{
  void *result = NULL;

  if (count != 0 && element_size != 0 && count <= SIZE_MAX / element_size)
    result = calloc(count, element_size);
  if (result == NULL) {
    fprintf(stderr, "==fuzz_protocol== ERROR: CREATE(%zu x %zu bytes) failed "
            "at %s:%d (%s)\n", count, element_size,
            source_file ? source_file : "?", source_line,
            source_func ? source_func : "?");
    abort();
  }
  return result;
}

/* utils.c's CHECK_PLAYER_SPECIAL reporter (an NPC read a player-only field
 * through PRF_FLAGGED). desc.character stays NULL below, so protocol.c never
 * reaches it; if it ever does, the harness no longer models what it claims,
 * so stop loudly instead of carrying on. */
void tb_player_special_on_mob_at(const struct char_data *ch, const char *field,
                                 const char *source_file, int source_line,
                                 const char *source_func)
{
  (void)ch; (void)field; (void)source_file; (void)source_line;
  (void)source_func;
  abort();
}

void basic_mud_log(const char *format, ...)
{
  (void)format;
}

/* Format exactly as the server would, into a sink, so a bad format or
 * argument from protocol.c is still caught. */
size_t write_to_output(struct descriptor_data *t, const char *txt, ...)
{
  static char sink[MAX_STRING_LENGTH];
  va_list args;

  (void)t;
  va_start(args, txt);
  if (vsnprintf(sink, sizeof(sink), txt, args) < 0)
    sink[0] = '\0';
  va_end(args);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  struct descriptor_data desc;
  char small_output[SMALL_BUFSIZE];
  char *input, *output;
  const char *colourised;
  int length;

  if (size == 0 || size >= MAX_PROTOCOL_BUFFER)
    return 0;

  memset(&desc, 0, sizeof(desc));
  small_output[0] = '\0';
  desc.output = small_output;
  desc.pProtocol = ProtocolCreate();
  if (desc.pProtocol == NULL)
    abort();

  /* Exact-size heap copies, so ASan sees a read one byte past the end. */
  input = malloc(size + 1);
  output = malloc(MAX_RAW_INPUT_LENGTH);
  if (input == NULL || output == NULL)
    abort();
  memcpy(input, data, size);
  input[size] = '\0';

  (void)ProtocolInput(&desc, input, (int)size, output, MAX_RAW_INPUT_LENGTH);

  /* The same bytes as server-side text through the colour expander. */
  length = (int)size;
  colourised = ProtocolOutput(&desc, input, &length);
  if (colourised == NULL || length < 0)
    abort();

  free(output);
  free(input);
  ProtocolDestroy(desc.pProtocol);
  return 0;
}
