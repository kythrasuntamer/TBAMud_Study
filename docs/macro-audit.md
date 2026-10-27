# IronCircle macro audit — initial inventory (27 October 2026)

## Scope and preservation rules

This is an inventory and migration plan, **not** permission to mass-move macros.
Do not remove, rename, or change numerical values of serialized flags, class
identifiers, save formats, player levels, endgame AP/XP, HP/mana upgrades, or
command dispatch signatures. Check existing consumers before each relocation.
The public engine and private gameplay features remain separate.

## First-pass inspected sources on merged `master`

| Source | Approximate `#define` lines observed | Initial classification |
| --- | ---: | --- |
| `src/structs.h` | 429 | Room/zone/object/player persisted flags, indexed constants, type/config declarations |
| `src/utils.h` | 346 | Character/object accessors, logging/error-locality, checked allocation, gameplay helpers |
| `src/interpreter.c` | 7 | Login rate limits and command interpreter local constants |
| `src/dg_triggers.c` | 0 | Callback safety; not itself a macro-consolidation candidate |

Counts are *source-line tallies*, not counts of active macros; conditional
branches may introduce alternative definitions. Run the automated inventory for
a wider and repeatable repo-wide picture.

## High-priority findings

1. **Macro name collision:** `src/utils.h` defines `log` as `basic_mud_log`.
   Including some C math headers afterward has already broken the DG test
   harness under clang-tidy. This is a real namespace/preamble hazard. Any
   rename needs a complete call-site inventory and a compatibility plan;
   avoid moving `log` blindly into a global `macros.h`.
2. **Unsafe centralization risk:** `src/structs.h` includes persisted bit
   positions (e.g. `ROOM_*`, `MOB_*`) and index sentinels. Do **not**
   change their values or include ordering.
3. **Side effects and repeated evaluation:** macros that expand arguments
   multiple times need a dedicated audit. Prefer a `static inline` helper
   for type-safe, single-evaluation operations when interfaces permit it.
4. **Error locality:** retain `__FILE__`, `__LINE__`, and `__func__` at
   *call sites* for checked allocation/diagnostics. A wrapper must not erase
   the origin of a failure.
5. **Security controls:** `TB_LOGIN_*` constants in `interpreter.c`
   govern authentication throttling. Relocation is not an opportunity to
   change lockout behavior, thresholds, or attack resistance.

## Proposed organization

A future `src/macros.h` should hold only carefully reviewed, foundational,
dependency-free macros, with a unique include guard and consistent `TB_`
namespace. Domain-specific symbols should remain in the headers that own
their semantics (`structs.h` for persisted flags, `dg_scripts.h` for DG
trigger bits, `interpreter.h` for command definitions). A single
`macros.h` containing *everything* would enlarge include dependencies,
increase accidental collisions, and weaken module boundaries.

Prioritize **auditing and testing before extraction**. For each candidate:
identify definition and all consumers, verify transitive includes, classify
serialization/ABI and side-effect risks, make one mechanical change, then
run the full GCC/clang-tidy/ASan/UBSan tests. Capture all output with
`set -o pipefail` and `2>&1 | tee -a ironcircle-audit.log`.

## Reproduce the complete inventory

```bash
set -o pipefail
python3 tools/audit_macros.py --root . --report macro-audit.txt 2>&1 | tee -a ironcircle-audit.log
```

The script intentionally does not rewrite code. Repeated names are
**candidates** for review, not proof of bugs: conditional compilation,
platform-dependent definitions and include guards need human inspection.

## Next security-audit seam

Continue reviewing remaining DG callbacks where script execution may mutate
or extract the owner, actor, trigger, or object. Save/restore behavior and
post-callback validity must be tested before changing control flow. Treat
the merged DG memory-trigger PR as the baseline, not as proof that all DG
callback paths are safe.

## Second-pass concrete macro hazards (source inspected)

### Bit-array indexing and shift preconditions — high priority

`utils.h` defines `Q_FIELD(x)` by integer division by 32 and
`Q_BIT(x)` as `1U << (x % 32)`. Then `IS_SET_AR`, `SET_BIT_AR`,
`REMOVE_BIT_AR`, and `TOGGLE_BIT_AR` index their caller-supplied arrays
with `Q_FIELD(bit)`. These macros **do not check** whether `bit` is
non-negative or within the actual storage allocation. For negative
indices, the shift count can also be invalid (undefined behavior).
For large indices, out-of-bounds array access is possible.

This is a potential input-validation vulnerability **only if an untrusted
or incorrectly computed index reaches a call site**. The inspected
definitions alone do not establish reachability. Audit call sites,
especially DG variable parsing, OLC/editor commands, and network input,
before changing these foundational macros. Preserve serialized flag values
and storage layouts.

`TOGGLE_BIT_AR(var, bit)` expands `var` and `bit` more than once.
Expressions with side effects therefore must never be passed to it.
A replacement should use a single-evaluation, checked API where practical,
with appropriate array length and explicit error reporting.

### Logging and math header collision — confirmed

`utils.h` globally defines `log` as `basic_mud_log`. Because
`<math.h>` declares a `log` function, later system-header includes can
break preprocessing (observed under clang-tidy in the earlier callback
test). Separate namespaced `TB_LOG` APIs and incremental call-site
conversion should be evaluated; no legacy alias was removed here.

### Checked memory allocation — preserve

`CREATE` and `RECREATE` in `utils.h` already cache their count argument
and call checked allocation helpers with `__FILE__`, `__LINE__`, and
`__func__`. The fatal failure behavior is currently `abort()` and
must **not** be changed incidentally during macro relocation. Any
modernization of allocation behavior requires separate design and tests.

### DG trigger-variable formatting — review

`ADD_UID_VAR` in `dg_scripts.h` uses `tb_snprintf(buf, sizeof(buf), ...)`.
This relies on `buf` being an **actual array** at the call site.
Passing a `char *` would make `sizeof(buf)` pointer-sized and truncate
the output. The same macro uses `trig` and `id` only once; this is not
a general-purpose string builder. Consider an explicit buffer-length
helper after validating all call sites. Avoid changing UID format or
trigger variable semantics.

### Visibility predicates — expensive and state-dependent

`CAN_SEE` expands through `IMM_CAN_SEE`, `MORT_CAN_SEE`, `LIGHT_OK`,
and flag tests; it evaluates some arguments more than once and can
access rooms/player specials. Callers must verify character lifetime
and location after DG callbacks before evaluating it. A macro relocation
alone will not solve lifetime safety.

These are audit findings, **not yet security fixes**. No code-path
exploitability claim is made, and no production source is changed in
this draft.

## Third-pass call-site tracing (27 October 2026)

### Confirmed *unprotected traversal pattern* in ordinary greeting triggers

`src/dg_triggers.c:greet_mtrigger()` (near lines 286–318 on audited
`master`) still traverses room occupants with `ch = ch->next_in_room`
and script triggers with a cached `next_trig = t->next`. It invokes
`script_driver(&ch, t, MOB_TRIGGER, TRIG_NEW)` inside both traversals,
then continues without re-resolving `ch`, the actor, or the cached
trigger node. A callback that extracts/moves the NPC, actor, or triggers
can invalidate the pointers. This is the *same class* of dangerous
iterator/lifetime pattern addressed for memory triggers in PR #43,
but it is a **separate function not fixed by that PR**.

The source pattern is confirmed. Actual exploitable extraction paths
through this callback still require adversarial runtime testing. Treat
as high-priority security follow-up; do not automatically copy the
memory-trigger patch because ordinary greeting triggers have different
return-value and dispatch semantics. Preserve trigger order and
`final = FALSE` behavior.

`leave_mtrigger()` (near line 715) also iterates `next_in_room`
and invokes script callbacks. Audit it with the same lifetime matrix.
More DG handlers contain `ADD_UID_VAR` and callback dispatch; each
requires separate verification.

### Reachability refinement: bit-array lookup

`src/dg_scripts.c:check_flags_by_name_ar()` matches a string against
`namelist[0..numflags)`, assigns only a matched nonnegative loop
index to `item`, and returns early if no match. This **specific**
call site does *not* appear to permit a negative index from an
unrecognized string. However, safety still depends on callers
supplying consistent `numflags`, actual `namelist` capacity, and
bit-array storage size (at least enough for `numflags` bits).

`src/utils.c:sprintbitarray()` computes nonnegative `bit` as
`teller * 32 + nr` under loop bounds, but also reads
`names[bit]`; its safety depends on valid `maxar` and on the
names/bitvector allocations being large enough. Multiplication could
overflow a signed int for extreme invalid `maxar` values. These
are preconditions to validate at the callers, not demonstrated
remotely exploitable bugs.

### Audit actions before production fixes

1. Add sanitizer-backed tests to ordinary greet and leave trigger
   paths for actor/owner removal, room relocation, trigger extraction,
   and nested callbacks.
2. Inventory caller contracts for `check_flags_by_name_ar` and
   `sprintbitarray`, including lengths of both bit arrays and
   name tables; add negative and excessive-length cases.
3. Prefer named, checked helpers for *untrusted* bit-index operations
   rather than blindly inserting bounds checks into serialized
   bit-flag macros everywhere.

## Fourth-pass DG callback audit — object and command paths

Source reviewed: `src/dg_triggers.c` on PR #47, after ordinary greeting hardening.

| Function | Observed post-callback pattern | Assessment |
| --- | --- | --- |
| `leave_otrigger` | Caches `obj_next = obj->next_content`, then runs a script and resumes both trigger and object iteration | High-priority candidate: extracted next object or trigger could invalidate cached pointers |
| `timer_otrigger` | Dispatches `script_driver(&obj,...)` then continues `t = t->next` | High-priority candidate: owner/trigger extraction during callback |
| `cmd_otrig` | On a script return of zero, continues `t = t->next` | High-priority candidate; actual return contract must be preserved |
| `command_otrigger` | Iterates `actor->carrying` and room `contents` with `obj = obj->next_content` around `cmd_otrig` | High-priority candidate even if inner `cmd_otrig` returns safely |
| `load_otrigger` | After callback, if `SCRIPT_ERROR_CODE`, reads `GET_OBJ_RNUM(obj)` | Potential stale-object read if the script extracts its own object |
| `speech_mtrigger` and `command_mtrigger` | Cache room-occupant `next_in_room` before script dispatch | Candidate: next occupant extracted by callback; inspect actor and trigger liveness |

**Important distinction:** `leave_mtrigger` for mobile NPCs returns directly
from `script_driver`, so the same *post-callback* iterator hazard is not
present in that specific handler. `leave_otrigger` is different: it keeps
iterating after callbacks.

No exploitability is claimed yet. Each candidate needs an isolated,
sanitizer-backed extraction test that exercises the production handler.
For object cases, use stable object UIDs or an equivalent live-object
lookup: never dereference a pointer merely to test whether it was freed.
Snapshotting `next_content` alone is insufficient because a callback can
remove the next object. Avoid treating pointer equality as proof of identity
when allocator address reuse is possible. Preserve multi-trigger ordering,
return-value aggregation, and existing player-save/character progression.

Next engineering patch should prioritize `leave_otrigger` and
`timer_otrigger`, followed by command and load paths. Keep the test
harness and macro inventory on the draft PR until all relevant regressions
and sanitizers pass.

## Fourth-pass DG callback lifetime inventory (source review)

Inspected `src/dg_triggers.c` on the PR #47 branch following ordinary
`greet_mtrigger` hardening. The following are source-backed risk patterns,
not runtime-confirmed vulnerabilities:

| Handler | Callback behavior | Preliminary lifetime assessment |
| --- | --- | --- |
| `leave_mtrigger` / `door_mtrigger` | Return `script_driver` result directly | No post-callback iterator dereference within these functions. Validate callers' actor/object assumptions separately. |
| `timer_otrigger` | `script_driver(&obj,...)` followed by `t = t->next` | **High:** callback can invalidate current trigger and owner; test extracted trigger and object under ASan. |
| `leave_otrigger` | Saves `obj_next = obj->next_content` and keeps traversing; also continues `t = t->next` | **High:** saved next object/trigger pointers can become stale after extraction. Preserve cumulative `final = 0` cancellation behavior. |
| `cmd_otrig` | Continues trigger loop when callback returns false | **High:** current trigger/object may be extracted before next iteration. |
| `command_mtrigger` | Saves `ch_next`, continues if callback returns false | **High:** cached next occupant and trigger links can become stale. |
| `load_otrigger` | After callback, checks `GET_OBJ_RNUM(obj)` on recursive script error | **High:** object may be extracted; requires live UID check before dereference and preserving recursive-error handling. |
| `receive_mtrigger` | Checks `DEAD(actor)` and `DEAD(ch)` after callback | **Review:** check semantics against deferred vs immediate extraction before assuming pointers are safe. |
| `get_otrigger`, `wear_otrigger`, `remove_otrigger`, `drop_otrigger`, `give_otrigger` | Test `!obj` after callback | **Review:** local pointer does not automatically become NULL when object is freed. Existing checks are insufficient as a general object-lifetime proof. |
| `leave_wtrigger` / `door_wtrigger` | Direct callback return | No iterator use afterward within the handler. Caller validity still needs review. |

### Test-first repair plan

Add focused executable ASan/UBSan cases for `timer_otrigger`,
`leave_otrigger`, `cmd_otrig`, and `command_mtrigger` that extract the
*current* and *next* target and modify trigger chains. Also verify
`load_otrigger` after extraction on recursive-error return.

Do not apply pointer-based snapshots as proof of identity across allocator
reuse: a freed allocation may be reused at the same address. Prefer stable
object/trigger IDs with validation against live registries when available;
otherwise define a bounded, non-dereferencing snapshot strategy supported
by tests. Preserve script order, multi-trigger semantics, return values,
and persistent game data. Avoid changing these handlers in bulk before
individual regressions pass.

## Fifth pass: macro and security audit

### Linked-list macro: REMOVE_FROM_LIST

`src/utils.h` defines `REMOVE_FROM_LIST(item, head, next)` using
an implicitly required caller variable `temp`. It does not report
whether the requested item was found; if absent, it silently performs
no removal. The macro evaluates `item` more than once and assumes
the supplied `item` and linked-list nodes remain live. In a
callback-driven DG path, it must not be applied to cached pointers
whose lifetime may have ended.

Risk: ambiguous error reporting and a potentially unsafe removal
operation when callers have inconsistent list ownership. Follow-up:
inventory actual callers and their ownership conditions before changing
the macro signature. A future typed helper should return explicit
success/failure, preserve pointer identity, and avoid repeated
evaluation.

### CHECK_PLAYER_SPECIAL is intentionally conditional

The two `CHECK_PLAYER_SPECIAL` definitions in `utils.h` are
mutually exclusive `#if 1` / `#else` alternatives, **not** a
double-active macro collision. The enabled version logs when player
specials are accessed on the shared `dummy_mob` storage. Removing
that check for a tidier macro layout would weaken diagnostics and
could conceal cross-NPC state corruption. Retain it and its
call-site file/line context until an equivalent checked accessor
has tests.

### Tool-local CREATE should not be blindly centralized

`src/util/wld2html.c` provides a standalone `CREATE` macro
distinct from the production checked allocator in `src/utils.h`.
The tool also embeds its own direction/exit constants. These
definitions may intentionally avoid game-engine header dependencies.
Unifying them without inspecting standalone builds could break the
utility or change allocation failure behavior. Compare their
preconditions and tests first.

### Security review: DG timer repair pending verification

The current branch includes a `timer_otrigger()` change
(`903e69a`) and three adversarial tests (`d3526d6`), but the
local ASan/UBSan result for those commits has **not** been supplied.
Do not mark them validated or merge the draft PR.

The timer fix uses the object UID lookup as a liveness test and
compares captured trigger pointers against currently attached
triggers. Follow-up must assess ABA/address-reuse risk: a newly
allocated trigger at the same address as a freed trigger could pass
pointer comparison. A stable trigger identity or generation check
may be required to guarantee the original dispatch set.

### Additional callback priorities

`leave_otrigger`, `command_mtrigger`, and `cmd_otrig` still
continue across callbacks using potentially invalidated list links.
Priority is sanitizer-backed tests for deletion/replacement of the
current and next node, actor extraction, reentrant dispatch, and
preservation of the original return-value contracts.

## Sixth pass: DG callback continuation and integrity (27 October 2026)

Further source inspection establishes the following independent risks:

- **`command_otrigger()`** traverses both equipped inventory and
  room-contained objects, using `obj = obj->next_content` after
  `cmd_otrig()`. That helper may invoke `script_driver()` and return
  false, allowing iteration to continue even if the current object,
  next object, actor, or the room contents have changed. Hardening only
  `cmd_otrig()` would not repair the caller's object iteration.
- **`load_mtrigger()`** calls `script_driver()`, then on
  `SCRIPT_ERROR_CODE` reads `GET_MOB_RNUM(ch)`. A script which
  extracts `ch` before returning the recursion sentinel may leave
  this dereference unsafe. Like `load_otrigger()`, it needs an identity
  validation strategy before post-callback dereference, plus explicit
  regression coverage for recursion error behavior.
- **`give_otrigger()`** evaluates `obj->carried_by` after callback,
  guarded only by `!obj`. A pointer freed by the callback stays
  non-NULL locally: this is insufficient as a lifetime check.
  The same form appears in the object give/drop family. Before fixing,
  inventory whether extraction is immediate or deferred in each
  script-driver path and whether object UID registry removal occurs
  before deallocation.
- **`REMOVE_FROM_LIST`** in `utils.h` dereferences `(item)->next`
  and relies on an outer `temp` symbol; callers must guarantee item
  lifetime, list membership and type. It does not return whether
  removal succeeded, making error-locality and invariant checking
  harder. Changing it requires caller-by-caller review.

### Repair ordering

1. Verify outstanding timer-trigger patch under local sanitizers, then
   separately test trigger pointer reuse/ABA.
2. Secure `command_otrigger()` *and* `cmd_otrig()` together, preserving
   object scan order and command interception behavior.
3. Secure `leave_otrigger()` room traversal with stable object IDs
   and bounded snapshot semantics, retaining cumulative cancellation.
4. Cover load-trigger recursion errors for both mob and object
   extraction. Refactor object give/drop/wear/remove callback checks
   only with verified object lifecycle contracts.

No exploitability, complete coverage, or successful local test result is
claimed for these additional cases.

## Seventh pass: combined macro/security audit and status

### Command-trigger traversal: nested invalidation confirmed in source

`command_otrigger()` has three distinct scan regions: equipped objects,
carried objects, and room objects. Each calls `cmd_otrig()`, which can
execute a DG callback and return false. When that happens, the outer
loops continue using actor inventory pointers, `obj->next_content`,
or `world[IN_ROOM(actor)].contents`. The callback can change all of
these. Merely fixing `cmd_otrig()`'s trigger iteration does not protect
the outer object/actor traversal.

A correct repair must revalidate the actor's stable identity, location
and inventory ownership between callbacks, and use stable object IDs
to avoid stale pointers. It must preserve the existing scan order:
equipment first, inventory second, room contents third. Account for
callbacks which create or delete objects during dispatch and prevent
unbounded repeated processing.

### Load trigger errors: both object and mobile cases

`load_mtrigger()` and `load_otrigger()` directly dereference
`ch`/`obj` following a callback *only when* it returned
`SCRIPT_ERROR_CODE`. This narrows the dangerous path but does not
remove it. Snapshot the original prototype index while its owner is
live and validate before any post-callback dereference; separately
consider whether disabling prototype scripts after extraction remains
required by existing recursion-error semantics.

### Macro repair safety notes

`REMOVE_FROM_LIST`, `LINK`, and `UNLINK` are structural macros
whose expansion depends on caller-provided field names and local
variables. Treat replacements as interface changes, not whitespace
cleanup. For these linked-list macros, expand argument side effects
and confirm head/tail invariants in unit tests before refactoring.
Review use within callback-exposed paths especially carefully.

### Measured vs estimated audit coverage

The read-only macro inventory has scanned 143 C/header files and
counted 2,648 definitions; this is inventory coverage, not verification
of each macro's safety. Approximately 25% **overall** completion is
a planning estimate only. No fixed list of review units or known
vulnerability denominator has been established, so no exact security
audit percentage is defensible. Track milestones individually:
inventory, caller review, runtime exploitability, remediation,
sanitizer tests, and CI.

### Outstanding validation

The three newly authored timer-object runtime tests are not yet
confirmed by a local GCC ASan/UBSan run. Do not merge the draft
branch solely on a passing earlier native-sanitizer workflow.

## Standing error-locality standard and next-phase gate

All macro and security fixes must preserve or strengthen error locality:
report the origin of a failure (`__FILE__`, `__LINE__`, `__func__`),
operation, invariant, and safe stable entity/trigger identifiers; retain
errno or a typed cause where relevant; avoid overwriting the original
failure while propagating errors; and make diagnostics bounded and
safe even when objects have been freed.

Use explicit checked return statuses for operations that can fail,
rather than silent no-ops. Preserve `TB_ERROR_LOG`, correlation IDs,
and breadcrumbs. Avoid logging with pointers that have become stale
after DG callbacks. Record recoverable input errors without crashing
the network service; use prominent diagnostics and fail-closed
behavior for critical violated internal invariants.

**Sequence:** finish the security and macro audits and verify their
repairs first. If an error-locality gap remains anywhere in the
public engine, a dedicated, repository-wide error-locality hardening
initiative is the *next* priority before feature expansion. Build
tests proving that errors identify their origin and cause, including
nested DG callbacks, malformed input, failed allocation and persistence
rollback paths.

### Additional linked-list macro observations

`LINK` and `UNLINK` perform unchecked writes through caller-supplied
nodes and expect head/tail/neighbor invariants. Neither detects duplicate
linking or removal of a node not owned by the list. `UNLINK` also leaves
the removed node's `next`/`prev` members unchanged, despite changing
its neighbors: later accidental reuse may operate on stale links.
These are source-level contract hazards, not demonstrated exploits.
Audit ownership at callers before adding checked helpers; never change
the old macros in bulk without compatibility and regression coverage.

`DISPOSE(point)` logs null-pointer frees but silently accepts calls
where `point` is an invalid non-null pointer; it assigns NULL after
free, which protects only that lvalue and does not eliminate aliases.
It already records file/line on null, but not the calling function.
Any replacement must preserve local diagnostics and not claim to
detect arbitrary invalid frees.
