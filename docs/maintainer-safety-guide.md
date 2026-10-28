# Maintainer guide: memory safety and diagnostics

This guide explains the safety patterns and error-reporting conventions used in
the engine, so new code follows them and existing guards are not "simplified"
away. The audit history and reasoning live in `docs/macro-audit.md`; the
per-file review status lives in `docs/memory-safety-audit.md`.

Project priorities, in order: **memory safety, error correction, error
reporting, error handling**. Two rules follow from them:

- **Fail fast, fail loudly.** A guard that rejects, truncates or skips
  something must emit a structured diagnostic. A silent clamp or silent early
  return is treated as a defect.
- **Error locality.** A diagnostic must say where the problem started (file,
  line, function of the *originating* call), which invariant broke, and which
  entities were involved (UIDs, vnums, idnums), so a log line alone is enough
  to start debugging.

## 1. Reporting errors

Use the structured reporters in `src/utils.h`. Each records `__FILE__`,
`__LINE__` and `__func__` at the call site, plus a stable error ID, a
correlation ID, the build SHA and the rolling breadcrumb trail.

| Macro | Use for |
| --- | --- |
| `TB_ERROR_MSG(subsys, op, resource, fmt, ...)` | A real error: bad input rejected, an invariant violated, data dropped. |
| `TB_ERROR_ERRNO(...)` | Same, when a system call or allocation failed; captures `errno`. |
| `TB_WARN_MSG(...)` | Unexpected but handled; the server carries on correctly (e.g. a UID rebind). |
| `TB_FATAL_MSG(...)` | Logged immediately before `abort()` (e.g. ID space exhausted). |

Conventions:

- `subsys` is the area (`"dg"`, `"act"`), `op` a short kebab-case operation
  (`"uid-remove"`, `"format-parse"`) and `resource` the thing operated on.
  Tests assert on `op`, so keep it stable.
- Identify characters with `DIAG_CHAR_FMT` / `DIAG_CHAR(ch)`, which print
  "mob vnum N" or "player idnum N". Pass a plain variable: the macro
  evaluates its argument several times.
- **Never log raw player text.** It can contain line breaks or quotes that
  split or forge log records. Log sizes and IDs instead, or an escaped excerpt
  (see `act_format_excerpt()` in `comm.c`).
- Keep builder-facing messages (`script_log`, `mudlog`) when builders need to
  see the problem in game. The structured record is for the server log and
  does not replace them.
- `TB_REPORT_*` in `sysdep.h` is a lower-level, allocation-free reporter for
  code that cannot depend on `utils.c` (headers, early boot). Prefer
  `TB_ERROR_*` everywhere else, because it carries breadcrumbs and correlation.

### Breadcrumbs: finding *which* script caused it

`script_driver()` records a breadcrumb for every trigger it runs (trigger
vnum, owner type, vnum and UID, nesting depth). Every `TB_ERROR_*` record
includes the recent breadcrumbs, so an error raised deep inside a DG callback
names the trigger that was running. Add breadcrumbs with `tb_breadcrumbf()` at
other dispatch points where "what was the server doing?" is otherwise hard to
answer.

### act() call sites

`act()` is a macro (`comm.h`) that expands to `act_at(__FILE__, __LINE__,
__func__, ...)`. Formatting diagnostics in `perform_act()` report the original
caller's location as `caller=file:line function`. Call `act()` exactly as
before. A NULL or empty message is the documented "no message" contract that
socials rely on, and is not an error.

## 2. DG script callbacks: never trust a pointer across `script_driver()`

Running a script can do almost anything: purge or teleport the owner, the
actor or other characters, detach or attach triggers, and extract objects.
Detached triggers and scripts are retired and then **freed when the outermost
script returns** (`dg_script_execution_leave()`). Characters are extracted at
the end of the pulse (`extract_pending_chars()`), so a character pointer stays
allocated until then, but it may have moved or be pending extraction.

Therefore, in any handler that keeps working after a callback:

1. **Snapshot identities, not pointers to follow.** Before dispatching, copy
   what you will iterate:
   - room occupants as UIDs: `dg_memory_targets()`;
   - triggers as (address, serial) pairs: `dg_greet_trigger_targets()`;
   - objects as (UID, address) pairs: `struct dg_cmd_object_target`.
2. **Re-acquire and re-validate after every callback.**
   - Characters: `find_char(uid)` and `dg_memory_character_live()`, which
     check identity, pending extraction and room.
   - Objects: `obj_uid_matches(uid, obj)`, which compares the registered
     address without dereferencing `obj`.
   - Triggers: `dg_find_live_greet_trigger()` / `dg_trigger_target_is()`,
     which search the live list and compare address **and** serial.
3. **Never follow `t->next`, `obj->next_content` or `ch->next_in_room`
   saved from before a callback.**

Why the serial: a freed trigger's address can be reused by a new trigger
(ABA), and address equality alone would accept the impostor.
`trig_data_init()` gives every trigger instance a never-reused `serial`, and a
detected reuse is reported as a `trigger-identity` warning.

Why the UID table checks ownership: when a player logs in while an earlier
character with the same idnum still awaits extraction, the UID is rebound to
the new character. `remove_from_lookup_table(uid, entity)` therefore removes
the mapping only if it still belongs to the entity being freed.

`script_driver()` sets the caller's *object* pointer to NULL when the object
purges itself. Capture anything you need afterwards (e.g. the prototype rnum
in `load_otrigger()`) **before** dispatching.

**Player text never becomes script syntax.** `eval`, `makeuid`, `attach`,
`detach` and the `%%name%%` idiom substitute a line twice, so a `%` typed by a
player would be evaluated as `%actor.level(34)%`. Store player-authored text
(command arguments, speech, act messages, player-set fields such as `title`)
with `add_player_var()` or `dg_neutralize_percent()`, which keep `%` as the
inert byte `DG_INERT_PERCENT`. `dg_restore_percent()` turns it back where text
leaves the engine: dispatched commands, `eval_op()` comparisons and stat
displays (`dg_display_value()`). Don't restore it anywhere a value can be
substituted again.

Handlers validate the actor's placement with `dg_trigger_actor_placed()`
before indexing `world[IN_ROOM(actor)]`. An actor outside a valid room is
reported as a `trigger-dispatch` error and nothing is dispatched.

## 3. Buffers and strings

- Bound every write by the destination's real size. `tb_snprintf(dst, size,
  ...)` keeps call-site locality on formatting failures. Note that it does
  not yet report truncation (an open item in `docs/macro-audit.md`).
- `sizeof(buf)` is only correct for an array. `ADD_UID_VAR` formats into its
  own buffer for this reason; don't reintroduce `sizeof` on a parameter.
- Parsers that walk escapes or quotes (`\`, `$`, `"`) must check for the
  terminator before stepping past the escape character. `perform_act()` and
  `sub_write()` both had this bug.
- When output must be truncated, truncate safely *and* report it with the
  sizes involved.

## 4. Tests

- Executable tests link the **production** source file and supply only the
  external symbols it needs, discarding unrelated code with
  `-ffunction-sections -fdata-sections -Wl,--gc-sections` (see
  `tests/CMakeLists.txt`). Examples: `test_dg_memory_runtime`,
  `test_dg_lookup_table`, `test_act_format` (links `comm.c` with `main`
  renamed), `test_dg_sub_write`.
- A memory-safety regression test should fail under ASan on the unpatched
  code. Use heap copies of inputs (`strdup`) so ASan sees over-reads.
- Include `tests/tb_error_capture.h` and assert that the expected diagnostic
  fired (`tb_captured_errors`, `tb_captured_operation`, `tb_captured_detail`).
  A guard that stops crashing but reports nothing is not done.
- Build and run under sanitizers:

  ```bash
  cmake -S . -B build-asan -DENABLE_SANITIZERS=ON -DBUILD_TESTING=ON
  cmake --build build-asan
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
    ctest --test-dir build-asan --output-on-failure
  ```

  CI (`.github/workflows/sanitizers.yml`) runs the same under Clang for
  `master` and every push to `audit/**`.

## 5. Macros

Small, well-named macros in the style of CircleMUD 3.0 (`GET_NAME`, `IS_NPC`,
`CREATE`) are encouraged where they make call sites clearer. When adding one:

- put it in the header that owns its domain;
- keep `__FILE__`/`__LINE__`/`__func__` from the call site in any diagnostic
  wrapper;
- comment any macro that evaluates an argument more than once, and never pass
  such a macro an argument with side effects;
- never change the value of a serialized flag or constant.
