# CircleMUD 3.0 style and modernization review

Status: initial comparative review, 2026-10-26. This is **not** a completed
line-by-line audit of the entire repository or a claim of tested behavior.

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

Security, memory safety, error locality, persistence integrity and existing
behavior are mandatory. Style consistency does not justify unsafe interfaces.
When a departure is needed, document the engineering reason in a concise
comment or PR explanation.

This guidance concerns the C style *observable in the CircleMUD source*;
it is not a claim to identify a uniquely personal style from every contributor
to that project.

## Reviewed reference points

Reference: CircleMUD 3.x source, notably
[`src/comm.c`](https://github.com/Yuffster/CircleMUD/blob/master/src/comm.c),
[`src/db.c`](https://github.com/Yuffster/CircleMUD/blob/master/src/db.c),
[`src/interpreter.c`](https://github.com/Yuffster/CircleMUD/blob/master/src/interpreter.c),
and [`src/fight.c`](https://github.com/Yuffster/CircleMUD/blob/master/src/fight.c).
This reference repository should be pinned to an exact verified 3.0 patchlevel
before using it for precise historical behavior claims.

Current fork equivalents have the same broad division of responsibilities:
communication/game loop, global world data, command interpretation and combat.
The fork also contains additional systems such as DG scripts, Oasis OLC,
libsodium password hashing and CMake/CI instrumentation. Those additions
cannot be forced into a 1990s source layout without losing necessary behavior.

### Findings and follow-up work

| Area | Observation | Required follow-up |
| --- | --- | --- |
| Server lifecycle | Global descriptor/game-loop state survives from the classic architecture | Audit disconnects, partial reads/writes, signal handling and copyover under fault injection |
| World database | Shared room/mobile/object tables retain cross-subsystem numeric references | Verify VNUM/rnum invariants during reload, OLC commit and zone reset |
| Login | Current interpreter has Argon2id/legacy crypt verification branches absent from original CircleMUD | Exercise successful/failed login, hash upgrade and resource-exhaustion cases |
| Persistence | Player ASCII-file save/load and player index are coupled to long-lived save tags | Recheck stage/flush/rename behavior, crash recovery and backward compatibility |
| Scripting | DG scripting adds mutable expression evaluation and deferred trigger lifetimes | Audit expression depth, malformed input and entity lifetime |
| Combat | Familiar combat-list/THAC0 structure persists alongside newer callbacks | Test removal during iteration, death/extraction and trigger side effects |
| OLC | Oasis editor ownership and save paths increase complexity beyond base CircleMUD | Validate editor cancel/commit cleanup, authorization and disk failure handling |
| Documentation | Many interfaces still lack ownership/error-contract descriptions | Incrementally annotate the real contracts without changing code |

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
