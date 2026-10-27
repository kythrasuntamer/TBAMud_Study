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
