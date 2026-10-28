# CircleMUD 3.0 style and modernization review

Status: comparative review, 2026-10-26, mapped to the completed audit on
2026-10-28. The audit itself is in
[docs/memory-safety-audit.md](../docs/memory-safety-audit.md) (the ledger)
and [docs/audit-2026-10-report.md](../docs/audit-2026-10-report.md).

## Style baseline

Preserve the recognizable CircleMUD 3.0 C idiom associated with Jeremy
Elson's original work whenever feasible. Prefer the following ordering:

1. Keep established source-file organization, existing names, interfaces,
   indentation and straightforward C control flow.
2. Prefer clear and local logic over abstraction for abstraction's sake.
3. Add comments for invariants, ownership, external data contracts and
   non-obvious decisions; avoid narrating obvious statements.
4. Introduce checked arithmetic, bounds checks, strict parsing, explicit
   error results and diagnostics where the original idiom is unsafe or
   cannot meet the project's requirements.
5. Preserve the local style within a file rather than reformat unrelated
   code during functional changes.
6. Retain and use the familiar CircleMUD accessor and flag macros where they
   make gameplay logic shorter and more recognizable (for example GET_*,
   IS_NPC, and relevant flag helpers). Introduce a new macro only if it has
   a clear, recurring purpose and does not hide important control flow or
   failure handling. Prefer existing naming conventions over novel syntax.

### Macro safety rules

- One definition per macro, in utils.h/structs.h or the subsystem header
  that owns its meaning; CircleMUD style is preferred where it aids
  readability. See [docs/macro-audit.md](../docs/macro-audit.md).
- Each macro parameter should be evaluated once wherever practical. Avoid
  arguments with side effects in legacy macros that expand arguments more
  than once; use a static inline function, or an unsigned-cast
  single-evaluation form as in VALID_ATTACK, when reliable single
  evaluation and type checking are necessary.
- Fully parenthesize expressions and parameters in expression-like macros.
- Preserve signedness and integer width; never use shifts or casts that can
  invoke undefined behavior. Use checked helpers for bounds and arithmetic.
- Avoid macros that silently allocate, free, log, return, or change control
  flow unless that behavior is already a clear and documented codebase
  convention. Keep failures local and attributable.
- Preserve established macro contracts and expand regression tests before
  changing widely used accessors or character/stat macros.


Security, memory safety, error locality, persistence integrity and existing
behavior are mandatory. Style consistency does not justify unsafe interfaces.
When a departure is needed, document the engineering reason in a concise
comment or PR explanation.

This guidance concerns the C style *observable in the CircleMUD source*;
it is not a claim to identify a uniquely personal style from every contributor
to that project.

## Reviewed reference points

Reference: CircleMUD 3.x source, notably
[`src/comm.c`](https://github.com/Yuffster/CircleMUD/blob/12a0cedbb563465029c46de9537c96854bc11610/src/comm.c),
[`src/db.c`](https://github.com/Yuffster/CircleMUD/blob/12a0cedbb563465029c46de9537c96854bc11610/src/db.c),
[`src/interpreter.c`](https://github.com/Yuffster/CircleMUD/blob/12a0cedbb563465029c46de9537c96854bc11610/src/interpreter.c),
and [`src/fight.c`](https://github.com/Yuffster/CircleMUD/blob/12a0cedbb563465029c46de9537c96854bc11610/src/fight.c).
The links are pinned to commit 12a0ced of that repository. It is a public
CircleMUD 3.x mirror, not a verified 3.0 patchlevel, so it supports style
comparisons, not precise historical behavior claims.

Current fork equivalents have the same broad division of responsibilities:
communication/game loop, global world data, command interpretation and combat.
The fork also contains additional systems such as DG scripts, Oasis OLC,
libsodium password hashing and CMake/CI instrumentation. Those additions
cannot be forced into a 1990s source layout without losing necessary behavior.

### Findings and where the audit covers them

| Area | Observation | Audit coverage (ledger) |
| --- | --- | --- |
| Server lifecycle | Global descriptor/game-loop state survives from the classic architecture | Area A (untrusted network input) |
| World database | Shared room/mobile/object tables retain cross-subsystem numeric references | Areas B and H; rnum checks go through tb_rnum_valid (VALID_*_RNUM) |
| Login | Current interpreter has Argon2id/legacy crypt verification branches absent from original CircleMUD | Area A; legacy hashes are upgraded on login |
| Persistence | Player ASCII-file save/load and player index are coupled to long-lived save tags | Area B |
| Scripting | DG scripting adds mutable expression evaluation and deferred trigger lifetimes | Areas D and E; expression depth is bounded by line length (see eval_expr) |
| Combat | Familiar combat-list/THAC0 structure persists alongside newer callbacks | Area H |
| OLC | Oasis editor ownership and save paths increase complexity beyond base CircleMUD | Area F |


## Upstream tbaMUD comparison (initial sample)

Also compare against [upstream tbaMUD](https://github.com/tbamud/tbamud)
when evaluating changes made by this fork. Examined file headers and opening
sections of `src/comm.c`, `src/interpreter.c`, `src/fight.c`, and
`src/players.c` in the current upstream and fork. These retain the same
established subsystem layout and source-header conventions; this sample is
**not** a full-file semantic diff or a verified original fork-point comparison.

Before any broad stylistic rewrite, identify the exact inherited base commit
and use file-by-file diffs to isolate local work. Classify each changed block:
behavioral safety fix, feature extension, compatibility change, diagnostic,
test integration, or formatting-only. Preserve behavioral changes and their
provenance; adjust formatting only when safe and useful. Do not modify existing
macros merely to resemble classic code: verify argument evaluation, types,
side effects, and error reporting first.

Style changes should be narrow and separately reviewable. In particular,
avoid giant whitespace-only diffs that hide security or persistence changes.

## Documentation safeguards

Every comment must be verified against the implementation. In particular,
avoid claiming atomic/durable disk persistence without testing directory
sync and filesystem-specific behavior; staged renames are not a complete
crash-consistency proof. Avoid promising that a pointer remains valid across
script callbacks or character extraction without checking the call graph.

When updating the source, keep behavioral fixes separate from documentation
PRs. Record tests actually executed, preserve complete build/test logs, and
never merge unfinished cross-branch feature work merely to complete a style
cleanup.
