# Foundations: types, macros, diagnostics and tooling

This document covers the code everything else is built on: the core types
in `structs.h`, the macro families in `utils.h`, the portability layer and
safe wrappers in `sysdep.h`, the structured diagnostics in `utils.c`, the
small support files (`random.c`, `constants.c`, `bsd-snprintf.c`,
`screen.h`, `zmalloc.c`), the stand-alone tools in `src/util/`, and the
build, test and sanitizer tooling.

The reporting rules themselves (when to use which reporter, how to write a
diagnostic, the DG callback lifetime rules) are in
`docs/maintainer-safety-guide.md`; this document explains the machinery
behind them. The macro audit history is in `docs/macro-audit.md`.

## Contents

1. [Header layering](#header-layering)
2. [Core types: structs.h](#core-types-structsh)
3. [Macro families: utils.h](#macro-families-utilsh)
4. [Utility functions: utils.c](#utility-functions-utilsc)
5. [Portability and safe wrappers: sysdep.h](#portability-and-safe-wrappers-sysdeph)
6. [Structured diagnostics](#structured-diagnostics)
7. [Small support files](#small-support-files)
8. [Memory debugging, sanitizers and fuzzing](#memory-debugging-sanitizers-and-fuzzing)
9. [Tests](#tests)
10. [Stand-alone tools: src/util](#stand-alone-tools-srcutil)

## Header layering

Every `.c` file in `src/` starts with the same include order:

```c
#include "conf.h"      /* generated: feature tests, TB_BUILD_* provenance */
#include "sysdep.h"    /* system headers, portability, tb_* wrappers */
#include "structs.h"   /* core types (pulls in protocol.h and lists.h) */
#include "utils.h"     /* macros and utility prototypes */
/* then the subsystem headers: comm.h, db.h, handler.h, interpreter.h ... */
```

`conf.h` comes from `src/conf.h.cmake.in` (CMake) or `src/conf.h.in`
(autoconf); the platform variants `conf.h.win`, `conf.h.os2` and so on are
copied by hand on those systems. `sysdep.h` must come before anything that
uses a system type. `structs.h` includes `zmalloc.h` at its end when
`MEMORY_DEBUG` is defined, so every file that includes `structs.h` gets the
debugging allocator.

Only the CMake template defines `TB_BUILD_SHA`, `TB_BUILD_COMPILER`,
`TB_BUILD_TYPE` and `TB_BUILD_SANITIZERS`, which `utils.c` uses in every
diagnostic. The legacy `configure` script also fails its first compiler
test on current GCC (its probe `main(){return(0);}` is rejected under the
C99 implicit-int rules), so use the CMake build.

## Core types: structs.h

`structs.h` defines the data model described in
[overview.md](overview.md#the-data-model) and the constants that are stored
in files.

### Index and number types

| Type | Definition | Notes |
| --- | --- | --- |
| `IDXTYPE` | `unsigned short` (`CIRCLE_UNSIGNED_INDEX` is 1) | all vnums and rnums |
| `room_vnum`, `obj_vnum`, `mob_vnum`, `zone_vnum`, `shop_vnum`, `trig_vnum`, `qst_vnum` | `IDXTYPE` | builder numbers |
| `room_rnum`, `obj_rnum`, ... | `IDXTYPE` | array indexes |
| `NOWHERE`, `NOTHING`, `NOBODY`, `NOFLAG` | `(IDXTYPE)~0` = 65535 | "none" |
| `sbyte`, `ubyte`, `sh_int`, `ush_int`, `byte` | 8 and 16 bit integers | many character fields are this narrow |
| `bool` | `char` | `TRUE`/`FALSE` from `utils.h` |
| `bitvector_t` | `unsigned long` | legacy single-word flags |

Because the index type is 16 bits, a vnum above 65534 cannot be stored, and
arithmetic on rnums wraps. Many character fields are narrow on purpose
(`sh_int` hit points, `byte` light count and carried item count, `sbyte`
ability scores); values written into them must be range-checked first.

### Flags

Room, player/mobile, preference, affect, object extra, wear and zone flags
are bit arrays of 4 x 32 bits (`RF_ARRAY_MAX`, `PM_ARRAY_MAX`, ...), stored
in `int[4]` and tested with the `*_AR` macros. Exit flags (`EX_*`) and
container flags (`CONT_*`) are single words.

**Never renumber a flag, type, position, sex, class, liquid, apply or rent
constant.** Their numeric values are stored in world files, player files
and rent files, and `constants.c` holds name tables indexed by them. Add
new values at the end, keep the `NUM_*` counts and the tables in step, and
mark bits that must not appear in world files `(R)` as the existing ones
are.

### Sizes and limits

| Constant | Value | Used for |
| --- | --- | --- |
| `MAX_INPUT_LENGTH` | 512 | one command line; output buffers of the argument helpers |
| `MAX_RAW_INPUT_LENGTH` | 12288 | a descriptor's raw input buffer |
| `MAX_STRING_LENGTH` | 49152 | general text buffers, longest world string |
| `SMALL_BUFSIZE`, `LARGE_BUFSIZE` | 1024, 24448 | output buffers |
| `MAX_SOCK_BUF`, `MAX_PROMPT_LENGTH` | 24576, 96 | one socket write, the prompt |
| `READ_SIZE` (`utils.h`) | 256 | `get_line()` buffers |
| `MAX_NAME_LENGTH`, `MAX_PWD_LENGTH`, `MAX_TITLE_LENGTH` | 20, 30, 80 | player strings |
| `HOST_LENGTH` | 40 | host names |
| `MAX_SKILLS`, `MAX_AFFECT`, `MAX_OBJ_AFFECT` | 200, 32, 6 | per-character and per-object arrays |
| `LVL_IMPL`, `LVL_GRGOD`, `LVL_GOD`, `LVL_IMMORT` | 60, 58, 55, 51 | immortal levels; `LVL_BUILDER` is `LVL_IMMORT` |
| `OPT_USEC`, `PASSES_PER_SEC` | 100000, 10 | the pulse; `n RL_SEC` = `n * 10` pulses |

The `PULSE_*` constants are listed in
[overview.md](overview.md#pulse-timing).

### Function pointer types

`SPECIAL(name)` declares a special procedure:
`int name(struct char_data *ch, void *me, int cmd, char *argument)`. It
returns non-zero if it handled the command. `cmd` is 0 when
`mobile_activity()` calls a mobile's procedure on its 10-second timer
rather than for a typed command. `ACMD(name)`
(`utils.h`) declares a command handler, and `EVENTFUNC(name)`
(`dg_event.h`) an event callback.

## Macro families: utils.h

The macros follow the CircleMUD 3.0 style: short, upper-case names that make
call sites read like the game rules. Most are plain field accessors and can
be assigned to. Read the warnings in the table: several evaluate an
argument more than once or have preconditions.

### Allocation and lists

| Macro | Meaning |
| --- | --- |
| `CREATE(p, type, n)` | `p = calloc(n, sizeof(type))` through `tb_calloc_array_at()`; checks the multiplication; **calls `abort()` if the allocation fails or `n` is 0** |
| `RECREATE(p, type, n)` | `realloc` with the same checks; `p` is unchanged until it succeeds; the new tail is not zeroed; `n` = 0 aborts |
| `TB_CALLOC_ARRAY`, `TB_REALLOC_ARRAY` | the checked helpers without the abort, for callers that can recover |
| `REMOVE_FROM_LIST(item, head, next)` | unlink `item` from a singly linked list; **needs a local variable named `temp`** of the list's type; does nothing if `item` is not in the list |
| `LINK`, `UNLINK` | doubly linked list append and remove; `UNLINK` does not free |
| `DISPOSE(p)` | free and set to `NULL`; logs a `SYSERR` if `p` was already `NULL` |
| `STRALLOC(s)`, `STRFREE(p)` | `strdup()` and `DISPOSE()` |

### Bits and flags

| Macro | Meaning |
| --- | --- |
| `IS_SET_AR`, `SET_BIT_AR`, `REMOVE_BIT_AR`, `TOGGLE_BIT_AR` | bit `n` of an `int[4]` array: word `n / 32`, bit `n % 32` (`Q_FIELD`, `Q_BIT`) |
| `IS_SET`, `SET_BIT`, `REMOVE_BIT`, `TOGGLE_BIT` | single-word masks |
| `PLR_FLAGS`, `MOB_FLAGS`, `PRF_FLAGS`, `AFF_FLAGS`, `ROOM_FLAGS`, `ZONE_FLAGS` | the flag arrays |
| `PLR_FLAGGED(ch, f)`, `MOB_FLAGGED(ch, f)` | test, and check `IS_NPC` first (players and mobiles share the `act` array) |
| `AFF_FLAGGED`, `PRF_FLAGGED`, `ROOM_FLAGGED`, `ZONE_FLAGGED`, `EXIT_FLAGGED`, `OBJ_FLAGGED`, `OBJWEAR_FLAGGED`, `OBJAFF_FLAGGED`, `OBJVAL_FLAGGED` | tests |
| `PLR_TOG_CHK`, `PRF_TOG_CHK` | toggle and return the new state |

`ROOM_FLAGS(loc)` and `ROOM_FLAGGED(loc, f)` index `world[loc]` without
checking the rnum.

### Characters

- `IS_NPC(ch)` tests `MOB_ISNPC`. `IS_MOB(ch)` also requires a valid
  prototype rnum.
- Names and descriptions: `GET_NAME(ch)` (the short description for NPCs),
  `GET_PC_NAME`, `GET_TITLE`, `GET_PASSWD`.
- Statistics: `GET_LEVEL`, `GET_REAL_LEVEL` (the immortal's level while
  switched), `GET_CLASS`, `GET_SEX`, `GET_STR` ... `GET_CHA`, `GET_HIT`,
  `GET_MAX_HIT`, `GET_MANA`, `GET_MOVE`, `GET_GOLD`, `GET_BANK_GOLD`,
  `GET_EXP`, `GET_AC`, `GET_HITROLL`, `GET_DAMROLL`, `GET_ALIGNMENT`,
  `GET_POS`, `GET_IDNUM`, `GET_SAVE(ch, i)`.
- Locations: `IN_ROOM(ch)` (an rnum), `GET_WAS_IN`, `GET_EQ(ch, slot)`,
  `FIGHTING`, `HUNTING`, `SITTING`, `GROUP`.
- Player-only data goes through `CHECK_PLAYER_SPECIAL`: `GET_COND`,
  `GET_LOADROOM`, `GET_PRACTICES`, `GET_INVIS_LEV`, `GET_WIMP_LEV`,
  `GET_FREEZE_LEV`, `GET_BAD_PWS`, `POOFIN`, `POOFOUT`, `GET_ALIASES`,
  `GET_LAST_TELL`, `GET_HOST`, `GET_SKILL`/`SET_SKILL`, `GET_PAGE_LENGTH`,
  `GET_SCREEN_WIDTH`, the quest and endgame fields and others. Used on an
  NPC they read or write the shared `dummy_mob` and log
  `SYSERR: Mob using '...'`. Check `IS_NPC()` before using them.
- Mobiles: `GET_MOB_RNUM`, `GET_MOB_VNUM` (`NOBODY` for a non-prototyped
  NPC), `GET_MOB_SPEC`, `GET_DEFAULT_POS`, `MEMORY`.
- State: `AWAKE`, `DEAD` (pending extraction), `GET_WAIT_STATE`,
  `WAIT_STATE(ch, n)`, `IS_GOOD`/`IS_EVIL`/`IS_NEUTRAL`, `CAN_CARRY_W`,
  `CAN_CARRY_N`, `IS_CARRYING_W`, `IS_CARRYING_N`.
- Pronouns: `HSHR` (his/her/its), `HSSH` (he/she/it), `HMHR` (him/her/it).
- Diagnostics: `DIAG_CHAR_FMT` and `DIAG_CHAR(ch)`, see
  [below](#structured-diagnostics).

### Objects

`GET_OBJ_TYPE`, `GET_OBJ_VAL(obj, n)`, `GET_OBJ_WEIGHT`, `GET_OBJ_COST`,
`GET_OBJ_RENT`, `GET_OBJ_TIMER`, `GET_OBJ_LEVEL`, `GET_OBJ_EXTRA`,
`GET_OBJ_WEAR`, `GET_OBJ_AFFECT`, `GET_OBJ_RNUM`, `GET_OBJ_VNUM`
(`NOTHING` without a prototype), `GET_OBJ_SPEC`, `GET_OBJ_SHORT`,
`IS_CORPSE`, `CAN_WEAR`, `OBJ_SAT_IN_BY`. `VALID_OBJ_RNUM(obj)` checks the
object's rnum.

### Rooms and exits

`VALID_ROOM_RNUM(r)`, `GET_ROOM_VNUM(r)`, `GET_ROOM_SPEC(r)`, `SECT(r)` and
`GET_ROOM_ZONE(r)` check the rnum and return a sentinel when it is invalid.
`EXIT(ch, dir)` is `world[IN_ROOM(ch)].dir_option[dir]`, `W_EXIT(room, dir)`
and `R_EXIT(roomptr, dir)` are the rnum and pointer forms, `CAN_GO(ch, dir)`
requires an open exit with a destination, and `DIR_COUNT` is 6 or 10
depending on `CONFIG_DIAGONAL_DIRS`. None of the exit macros check the
direction or the room.

### Visibility

`CAN_SEE(sub, obj)` combines `LIGHT_OK`, `INVIS_OK`, holy light, and the
invisibility level; `CAN_SEE_OBJ(sub, obj)` adds the carrier's visibility.
`PERS(ch, vict)` gives `ch`'s name as `vict` sees it ("someone", "an
immortal"), and `OBJS`/`OBJN` do the same for objects. These macros are long
and evaluate their arguments many times: pass plain variables.

### Strings and small helpers

`LOWER`, `UPPER` (inline functions), `ISNEWL`, `AN(s)` ("a"/"an"), `ANA`,
`SANA`, `TANA`, `YESNO`, `ONOFF`, `URANGE(lo, x, hi)`. `MIN`, `MAX` and
`CAP` are functions in `utils.c`, not macros (`CAP` capitalises the first
letter after any colour codes, in place).

### Descriptors and configuration

`STATE(d)`, `IS_IN_OLC(d)`, `IS_PLAYING(d)` (see
[networking.md](networking.md#descriptor-states-and-lifecycle)).
`SENDOK(ch)` decides whether `act()` delivers to `ch`; it uses a local
variable `to_sleeping` from `act_body()`. Every `config_info` field has a
`CONFIG_*` accessor, for example `CONFIG_MAX_PLAYING`, `CONFIG_PK_SETTING`,
`CONFIG_MORTAL_START`, `CONFIG_AUTO_SAVE`.

### Multiple evaluation and hidden dependencies

- `DIAG_CHAR`, `CAN_SEE`, `PERS`, `GET_REAL_LEVEL`, `STRENGTH_APPLY_INDEX`,
  `URANGE` and most compound macros evaluate arguments more than once.
- `REMOVE_FROM_LIST` needs `temp`; `SENDOK` needs `to_sleeping`; the
  `screen.h` `Q*` macros use a variable named `ch`.
- `GET_QUEST_TYPE` calls `real_quest()` twice.

Prefer adding a function when a macro would need a temporary.

## Utility functions: utils.c

| Function | Contract |
| --- | --- |
| `rand_number(from, to)` | uniform-ish integer in `[from, to]` from `circle_random()`; swaps reversed bounds with a `SYSERR` |
| `dice(n, size)` | sum of `n` rolls of `1..size`; 0 if either is not positive |
| `str_cmp`, `strn_cmp` | case-insensitive compare. Where the C library has `strcasecmp`/`strncasecmp` (Linux, BSD, macOS), `sysdep.h` defines these names as those functions and a `NULL` argument is undefined behaviour; only the `utils.c` fallback logs a `NULL` and returns 0 ("equal") |
| `strlcpy`, `strdup` | fallbacks when the C library lacks them |
| `sprintbit`, `sprinttype`, `sprintbitarray` | names of set bits or of a type value from a `"\n"`-terminated table into a bounded buffer; an out-of-table value prints `UNDEFINED` |
| `get_flag_by_name` | index of a name in a `"\n"`-terminated table |
| `get_line(fl, buf)` | next non-blank, non-`*` line into a `READ_SIZE` buffer; reports and discards the tail of a longer line; returns 0 at end of file without touching `buf` |
| `get_filename(buf, size, mode, name)` | player file paths: `CRASH_FILE`, `ETEXT_FILE`, `SCRIPT_VARS_FILE`, `PLR_FILE` |
| `mudlog(type, level, file, fmt, ...)` | write to the log (if `file`) and show `[ ... ]` to playing immortals of at least `level` whose syslog setting (`OFF`, `BRF`, `NRM`, `CMP`) is at least `type` |
| `basic_mud_log` (the `log` macro) | timestamped line to `logfile` (or standard error) |
| `core_dump()` | log `SYSERR: Assertion failed`, flush, and `fork()` a child that aborts, so a core file is written while the server keeps running |
| `touch(path)` | create the file or update its time |
| `room_is_dark`, `num_pc_in_room`, `age`, `real_time_passed`, `mud_time_passed`, `mud_time_to_secs` | world and time helpers |
| `add_follower`, `stop_follower`, `die_follower`, `circle_follow`, `num_followers_charmed` | follower lists |
| `char_from_furniture` | unseat a character |
| `count_color_chars`, `count_non_protocol_chars`, `strfrmt`, `strpaste`, `column_list`, `right_trim_whitespace`, `remove_from_string`, `levenshtein_distance` | text helpers |
| `file_head`, `file_tail`, `file_sizeof`, `file_numlines` | file viewers for immortal commands |

## Portability and safe wrappers: sysdep.h

`sysdep.h` includes the system headers that `conf.h` says exist, defines
`socket_t` and `CLOSE_SOCKET`, maps `str_cmp`/`strn_cmp` onto
`strcasecmp`/`strncasecmp` where available, prototypes library functions
the system headers miss (`NEED_*_PROTO`), and includes `bsd-snprintf.h`
when the C library has no `snprintf`/`vsnprintf`. It also holds the
allocation-free building blocks the rest of the error handling uses.

### Number parsers

| Function | On bad input |
| --- | --- |
| `tb_parse_long_or_zero`, `tb_parse_int_or_zero` | return 0 for text that does not start with a number or is out of range, **without any report**; trailing text is ignored. These replaced `atoi()`/`atol()` and keep their behaviour. |
| `tb_parse_long_checked(text, &out)`, `tb_parse_int_checked` | require the whole string (apart from white space) to be a number in range; return 1 on success, 0 otherwise |
| `tb_parse_int_prefix_checked(text, &out, &end)` | parse a leading number and return where it ended |
| `tb_parse_float_checked` | strict float parse |

Use the checked forms for new code and report the failure at the call site.

### The allocation-free reporter

`TB_REPORT_ERRNO(op, context)`, `TB_REPORT_ERROR`, `TB_REPORT_WARN`,
`TB_REPORT_FATAL`, `TB_REPORT_DOMAIN_ERROR` and `TB_REPORT_DOMAIN_ERRNO`
format one line into a stack buffer and write it to file descriptor 2 with
`write()`:

```
TBERR timestamp=2026-10-27T05:00:00Z pid=1234 severity=ERROR category=PERSISTENCE
      operation="fopen" context="etc/badsites" errno=13 errno_name=EACCES
      error="Permission denied" source="ban.c:124" function="write_ban_list"
```

(one line in the log). The category comes from the operation name
(`fopen`, `rename` ... are `PERSISTENCE`; `socket`, `accept` ... are
`NETWORK`; `parse`, `strtol` ... are `PARSER`). Quotes, backslashes and
control characters are escaped so a record is always one line, and `errno`
is preserved. `TB_INVARIANT(expr, context)` evaluates to `expr` and reports
when it is false. Use these where `utils.c` is not available (headers,
`src/util` tools, very early boot); everywhere else use `TB_ERROR_*`.

### stdio wrappers

`tb_fprintf`, `tb_fputs`, `tb_fputc`, `tb_fwrite`, `tb_fflush` and
`tb_fclose` call the stdio function and report a failure (including a
`NULL` stream) with the caller's file, line and function. They return
nothing. The `_checked` forms (`tb_fprintf_checked(stream, context, ...)`,
`tb_fwrite_checked`, `tb_fflush_checked`, `tb_fclose_checked`) return 1 or 0
so transactional code can roll back.

`tb_snprintf(dst, size, ...)` reports a formatting failure and empties
`dst`, but does not report truncation. `tb_strftime(dst, size, fmt, tm)`
reports a zero-length result and empties `dst`; with a `NULL` `tm` it
reports and returns **without writing `dst`**, so initialise the buffer.

### Atomic replace

`tb_replace_file_checked(temp, dest)` commits a fully written temporary
file: it tries `rename()`, and where that cannot replace an existing file it
renames `dest` to `dest.bak`, renames `temp` to `dest`, and restores the
backup if that fails. It never deliberately loses the last good `dest`.
Builds with `TB_TEST_FAILURE_INJECTION` can force each step to fail through
`tb_test_set_failpoints()`; production builds compile the failpoints to a
constant false. `tests/test_error_handling.c` uses them.

## Structured diagnostics

`utils.h` declares the reporters that every subsystem should use:

```c
TB_ERROR_MSG(subsys, op, resource, fmt, ...)    /* an error, handled */
TB_ERROR_ERRNO(subsys, op, resource, fmt, ...)  /* same, with errno */
TB_WARN_MSG(subsys, op, resource, fmt, ...)     /* unexpected, handled correctly */
TB_FATAL_MSG(subsys, op, resource, fmt, ...)    /* logged just before abort() */
```

They expand to `utils.c:tb_error_log_at` with `__FILE__`, `__LINE__` and
`__func__`, so the record names the code that detected the problem. The
record is one log line through `basic_mud_log()`:

```
ERROR id=TB-GENERAL-0001 diag_seq=17 correlation=0 subsystem=act
      op=format-parse resource=perform_act at=comm.c:2802
      function=perform_act build_sha=aeeb58d1a2b3 compiler="GNU 14.2.0"
      build_type=Debug sanitizers=address,undefined
      detail="..." breadcrumbs="#41/corr=0/dg:trigger 1200 ..."
```

- `id` is a stable error identifier (`TB-PARSER-0001`, `TB-PERSIST-0001`,
  ...); it is derived from the subsystem name, and most subsystems use
  `TB-GENERAL-0001`.
- `diag_seq` counts diagnostics since boot.
- `op` is a short kebab-case operation. Tests assert on it
  (`tests/tb_error_capture.h`), so keep it stable.
- `detail` is the formatted message, at most 24 KB; it is not escaped, so
  never put raw player text in it.
- `build_*` and `sanitizers` identify the binary.
- `breadcrumbs` is the recent history (see below).

`DIAG_CHAR_FMT` / `DIAG_CHAR(ch)` print a character as `mob vnum N` or
`player idnum N`:

```c
TB_ERROR_MSG("dg", "uid-remove", "lookup-table",
             "entity " DIAG_CHAR_FMT " was not registered", DIAG_CHAR(ch));
```

`DIAG_CHAR` evaluates `ch` several times.

### Breadcrumbs and correlation

`tb_breadcrumb(kind, detail)` and `tb_breadcrumbf(kind, fmt, ...)` record an
event in a fixed ring of 16 entries of 160 bytes. No allocation is
involved, so they are safe on error paths. `script_driver()` records one for
every trigger it runs, so an error deep inside a script names the trigger.
Every `TB_ERROR_*` record includes the ring, oldest first.

`tb_correlation_begin(kind, detail)` starts a logical operation and returns
an id that later records carry until `tb_correlation_end(id)`. There is one
current correlation, not a stack.

### Parse context

See [persistence.md](persistence.md#parse-context). Up to 16 streams can
have a context at once.

### Which output to use

| Need | Use |
| --- | --- |
| A rejected, repaired or failed operation | `TB_ERROR_MSG` / `TB_ERROR_ERRNO` |
| Something unexpected that was handled correctly | `TB_WARN_MSG` |
| The process is about to abort | `TB_FATAL_MSG`, then `abort()` |
| A problem builders must see in the game | `mudlog()` or `script_log()` **as well as** the structured record |
| Plain operational messages | `log()` |
| No `utils.c` (headers, tools, early boot) | `TB_REPORT_*` |

`tools/check_error_locality.sh` fails the build check on raw reporting
patterns (`perror()`, `strerror(errno)`, `fprintf(stderr, ...)`, printed
`SYSERR` lines).

### The bounded string builder

`struct tb_buf` wraps a caller-owned buffer. `tb_buf_init(&b, storage,
size)` empties it; `tb_buf_append`, `tb_buf_appendn` and `tb_buf_appendf`
add text, never write past the end, always keep the buffer terminated, and
return 0 (and set `truncated`) when something did not fit;
`tb_buf_remaining` and `tb_buf_truncated` query it. Use it instead of the
`len += snprintf(buf + len, sizeof(buf) - len, ...)` pattern, which writes
out of bounds as soon as one call truncates.

## Small support files

### random.c

The Park-Miller "minimal standard" generator (multiplier 16807, modulus
2^31 - 1, Schrage's method) behind `circle_srandom()` and
`circle_random()`, with one file-static seed. `init_game()` seeds it with
`time(0)`. Syntax-check mode never seeds it, and an unseeded generator
returns 2147483647 on every call. `rand_number()` reduces the result with
`%`, so it is slightly biased for large ranges; it is not suitable for
anything security related.

### constants.c and constants.h

The name tables for every enumerated value (`dirs`, `room_bits`,
`sector_types`, `position_types`, `connected_types`, `item_types`,
`drinks`, `apply_types`, `trig_types`, `month_name`, ...) and the stat
tables (`str_app`, `dex_app_skill`, `con_app`, `wis_app`, `int_app`,
`movement_loss`, `rev_dir`, `drink_aff`). Name tables end with a `"\n"`
entry, which `sprinttype()`, `sprintbit()`, `search_block()` and
`get_flag_by_name()` need. Five tables export a `*_count`
(`room_bits_count`, `action_bits_count`, `affected_bits_count`,
`extra_bits_count`, `wear_bits_count`). `constants.h` declares the tables
with incomplete array types, so callers cannot see their length; many code
paths index them with a game value without a range check. When you add a
value to an enumeration in `structs.h`, add its name here in the same
position.

### bsd-snprintf.c

A portable `snprintf`/`vsnprintf` (from OpenSSH) compiled in only when
`conf.h` says the C library lacks them. Modern systems do not use it.

### screen.h

The legacy ANSI colour strings (`KNRM`, `KRED`, ..., `KNUL` = `""`) and the
per-character colour macros. A player's colour level comes from
`PRF_COLOR_1`/`PRF_COLOR_2` (`C_OFF`, `C_SPR`, `C_NRM`, `C_CMP`);
`CCRED(ch, C_NRM)` returns the escape sequence or an empty string. New text
should use the `\t` codes that `ProtocolOutput()` expands
([networking.md](networking.md#colour-and-mxp-codes)).

### config.c

The compiled-in defaults for the configuration; see
[persistence.md](persistence.md#configuration).

## Memory debugging, sanitizers and fuzzing

### zmalloc (`MEMORY_DEBUG`)

`zmalloc.c` is Eric Murray's allocation monitor. Configure with
`-DMEMORY_DEBUG=1`; `zmalloc.h` then redefines `malloc`, `calloc`,
`realloc`, `free` and `strdup` in every file that includes `structs.h`.
Each block gets 4-byte `0xdeadDEAD` guard pads and is tracked in a
256-bucket hash table by address. `zfree()` checks the pads and marks the
block freed but **never releases it**, so double frees are detected and
memory use only grows. `main()` calls `zmalloc_init()` first and
`zmalloc_check()` last, which writes leaks, double frees and pad damage to
`zmalloc.log`. Sizes are `int`, and the `calloc(n, x)` macro expands to
`zmalloc((n*x), ...)` without parentheses around its arguments and without
an overflow check. Because blocks are never freed, `MEMORY_DEBUG` hides
use-after-free from every other tool; branch `audit-sanitizer-infra`
(commit `997a9d2`) poisons freed blocks and pads under ASan.

### CMake options

| Option | Base branch | Effect |
| --- | --- | --- |
| `TBAMUD_BIN_DIR` | yes | where `circle`, the tools and the tests are written; default `<build>/bin`. A `.tbamud-build-owner` file records the build tree that owns the directory, and configuring a second tree into it prints a warning. `autorun` runs `<source>/bin/circle`, so configure one tree with `-DTBAMUD_BIN_DIR=<source>/bin` to feed it. |
| `STATIC_ANALYSIS` | yes, `ON` | runs clang-tidy on every file when it is installed |
| `ENABLE_SANITIZERS` | yes | `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all` (GCC or Clang) |
| `MEMORY_DEBUG` | yes | zmalloc |
| `ENABLE_HARDENING` | `audit-sanitizer-infra` | `_FORTIFY_SOURCE=3` (non-Debug), `-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection=full`, `-ftrivial-auto-var-init=pattern`, `-Wl,-z,relro,-z,now`; refused together with sanitizers |
| `ENABLE_FUZZING` | `audit-sanitizer-infra` | Clang only; builds `tests/fuzz/fuzz_protocol` (libFuzzer + ASan + UBSan over `protocol.c`) and replays its corpus as tests |
| `SANITIZER_STRICT_INTEGER` | `audit-sanitizer-infra` | adds `integer` and `implicit-conversion` checks for audits |
| `TBAMUD_ASAN_OPTIONS`, `TBAMUD_UBSAN_OPTIONS` | `audit-sanitizer-infra` | runtime options applied to every test in a sanitizer tree, so local `ctest` matches CI |

Every build also compiles with `-Wformat -Werror=format-security
-Werror=format-nonliteral` (`-Wformat=2` on `audit-sanitizer-infra`) and
requires libsodium for password hashing.

`audit-sanitizer-infra` (and `audit-fix-infra-findings`, which builds on
it) also adds extra UBSan checks (`float-cast-overflow`,
`float-divide-by-zero`, `local-bounds`, `nullability` where supported),
ASan poisoning of pooled output buffers (`6f3a4d7`), a Valgrind
configuration and a CI job for it (`d350cf1`, `c230ab7`), and a stricter
error-locality check (`9943804`).

### Running the checks

```sh
# sanitizers
cmake -S . -B build-asan -DENABLE_SANITIZERS=ON -DBUILD_TESTING=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
  ctest --test-dir build-asan --output-on-failure

# Valgrind: on audit-sanitizer-infra the memcheck options and the 'native'
# label are set by CMakeLists.txt; on the base branch drop '-L native' and
# pass your own --error-exitcode through MEMORYCHECK_COMMAND_OPTIONS
cmake -S . -B build-vg -DCMAKE_BUILD_TYPE=Debug
cmake --build build-vg
ctest --test-dir build-vg -T memcheck -L native

# fuzzing (audit-sanitizer-infra, Clang only)
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DENABLE_FUZZING=ON
cmake --build build-fuzz
build-fuzz/bin/fuzz_protocol -max_total_time=300 tests/fuzz/corpus/protocol
```

On the infra branches the Valgrind options make every definite or indirect
leak and every invalid access fail the test (`--error-exitcode=99`), and
tests are labelled `native`, `unit`, `boot`, `contract` and `fuzz` so that
Valgrind runs only native code. Do not run Valgrind on a hardened or
sanitized tree.

## Tests

`tests/` uses the vendored Unity framework. Each `add_mud_test(name
sources...)` in `tests/CMakeLists.txt` links the **production** source
files under test with `test_stubs.c`, which provides weak stand-ins for the
rest of the server. Tests that link a large file (`comm.c`, `dg_scripts.c`,
`dg_triggers.c`, ...) compile it with `-ffunction-sections
-fdata-sections` and link with `--gc-sections` so only the code under test
needs its dependencies; `test_act_format` renames `comm.c`'s `main`.
`tests/tb_error_capture.h` captures `TB_ERROR_*` records so a test can
assert that a guard reported what it rejected.

Other tests:

- `test_world_boot` runs `circle -c -d lib` (the infra branches call it
  `circle_boot_check`), which loads and frees the whole shipped world under
  whatever sanitizer the tree uses.
- `test_login_greeting` boots the real server and requires the login
  prompt.
- The `test_*_contract.py` scripts check source-level contracts (for
  example that a function re-validates an object after a trigger). They
  need Python 3 and are skipped without it.

`doc/testing.md` describes the older `tests/Makefile.in` flow.

## Stand-alone tools: src/util

These are built by `src/util/CMakeLists.txt` (target `utils`) with
`-DCIRCLE_UTIL`. They do not link the server and keep their own copies of
the helpers they need.

| Tool | Purpose |
| --- | --- |
| `asciipasswd` | print a `Pass:` line with a `crypt()` hash for a player file; the server upgrades it to Argon2id at the next login |
| `autowiz` | build the wizlist and immlist files from `lib/plrfiles/index`; started by `limits.c:run_autowiz()`, signals the server with `SIGUSR1` to reload them |
| `plrtoascii` | convert a CircleMUD 3.0 binary player file into ASCII player files and an index; reads raw structs, so the source ABI matters |
| `rebuildIndex` (`rebuildAsciiIndex.c`) | rebuild `plrfiles/index` from the `.plr` files under the current directory |
| `rebuildMailIndex` | rebuild a mail index from `.ml` files (an older mail format) |
| `shopconv` | convert CircleMUD 2.20 shop files to the 3.0 format, keeping a `.bak` |
| `sign` | a tiny TCP server that sends a fixed text to every connection (for example a "we have moved" notice) |
| `split` | split standard input into files named by `=name` lines and write an `index` |
| `wld2html` | turn `.wld` files into one linked HTML page per room |

`src/util/scheck` is a note: the syntax checker is now `circle -c`.
