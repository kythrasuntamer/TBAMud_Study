# Memory-safety audit ledger

Full, systematic review of every source file (135 files, about 93,000 lines),
carried out on 27 October 2026 from
`audit/macro-inventory-and-dg-followup-20261027`. This ledger is the coverage
record: for every file, whether a reviewer read all of it and where its
findings stand; for every finding, the verifier's verdict and its resolution.
The summary for the maintainer is `docs/audit-2026-10-report.md`.

Everything between `<!-- BEGIN GENERATED: ... -->` and
`<!-- END GENERATED: ... -->` is produced by `tools/audit_report_stats.py`
from the audit workflow's JSON results. Do not edit those parts by hand;
regenerate them:

```sh
python3 -I tools/audit_report_stats.py --results <results dir> --write
```

## Method

The audit ran as a multi-agent workflow (`tbamud-full-audit-fix`),
coordinated from this branch. Each stage wrote structured JSON to the results
directory `audit-results-20261027/`: `reviews/`, `verify/`, `fixes/` and
`deferred/`.

1. **Static analysis.** The default CMake build runs clang-tidy over every
   compiled source (`clang-analyzer-*`, `bugprone-*`, `cert-*`,
   `performance-*`, `portability-*`), and non-literal printf formats are
   compile errors. Every fix branch was finished with no clang-tidy
   warnings. A sweep of every source file with `bugprone-sizeof-expression`
   (`WarnOnSizeOfPointer`) found two copies bounded by the size of a
   pointer (see "Found outside the chunk review"), and `.clang-tidy` now
   makes that check an error.
2. **Line-by-line review.** The files were split, area by area, into chunks
   of at most 6,000 lines, keeping each file whole (A1 to I3). One reviewer
   per chunk read every line of its files against this checklist:
   - buffer bounds;
   - integer overflow and signedness;
   - reads past a terminator;
   - indexing by input-, file- or script-derived values;
   - pointer lifetime (extraction, callbacks, descriptor close);
   - NULL dereference;
   - double free and leaks on error paths;
   - format strings;
   - uninitialized reads;
   - silent failures and error locality: a guard that rejects, truncates or
     skips must report it, naming the call site, the broken invariant and
     the entity IDs;
   - constructs that blind or mislead the sanitizers and Valgrind.

   Each reviewer listed the files it read, with an honest `fully_read` flag
   and a summary of each file's purpose. Each finding records its category,
   severity, file, line, function and input controller: who controls the
   input that triggers it (`remote-unauthenticated`, `player`,
   `builder-olc`, `dg-script`, `save-or-world-file`, `immortal` or
   `internal-invariant`). Reviewers also proposed CircleMUD-style
   readability macros and listed undocumented contracts. They read the code
   at `18f80ce`.
3. **Adversarial verification.** One verifier per chunk tried to refute
   every finding: it traced callers, looked for guards, checked that the
   input is really controllable and that the cited line matches. Verdicts
   are `confirmed` (a definite code path), `plausible` (not certain) or
   `refuted` (with the guard or commit that prevents it). The verifier
   re-rated the severity and sharpened the fix. For critical and high
   findings it ran the code where it could: an ASan+UBSan build (GCC, or
   clang-21 where installed) and, as a required second check, Valgrind 3.26
   memcheck on a non-sanitizer build. The harnesses link the production
   objects and boot the stock world, and some reproductions drove the real
   server over telnet. Verifiers also reported serious bugs the reviewers
   had missed; their IDs end in `-Vnn`.
4. **Fixes.** One fix agent per chunk worked in its own git worktree and
   branch. It took the confirmed findings, the plausible critical and high
   ones and the verifier-added ones, in severity order, with one commit per
   finding. Each fix fails fast and loudly: a structured `TB_ERROR_*` or
   `TB_WARN_MSG` diagnostic with a kebab-case operation, the broken
   invariant and the entity IDs, never a silent clamp or return. Each fix
   also explains the invariant and who controls the input in a comment, and
   adds a regression test where practical that asserts on the diagnostic.
   Documentation for the reported gaps and the readability macros came
   after the fixes. Before every commit the ASan+UBSan build had to be
   free of warnings and ctest had to pass. At the end each agent ran the
   default clang-tidy build, ctest under ASan,
   `tools/check_error_locality.sh` and every C test binary under Valgrind.
   A fix agent edited only its own chunk's files.
5. **Follow-up groups.** Three kinds of work were regrouped by the files
   they touch (`deferred/<group>.json`): the part of a fix that lies in
   another chunk's files, the plausible low and medium findings, and the
   remaining error-locality findings. One agent per group is fixing them on
   an `audit-deferred-<group>` branch. Non-memory findings in those files
   are queued for a later round.
6. **Sanitizer and dynamic-analysis infrastructure** (branch
   `audit-sanitizer-infra`): stricter UBSan checks and ASan/UBSan runtime
   options shared by ctest and CI; an `ENABLE_HARDENING` build; Valgrind
   memcheck in CTest with a whole-world boot test (`circle_boot_check`); CI
   jobs for memcheck and a hardened release build; ASan poisoning in
   zmalloc and in the output-buffer pool; more error-locality rules; and a
   libFuzzer target. Its first runs found BOOT-UAF, FUZZ-1 and FUZZ-2.
7. **Live-server Valgrind harness.** The coordinator ran the real server
   under Valgrind with scripted telnet clients (character creation, login
   and the G1-05 reproduction). That found two bugs no chunk owned:
   player-index slots written to disk uninitialised, and a host string
   leaked for every new character.
8. **Fuzzing.** `fuzz_protocol` (`ENABLE_FUZZING`, Clang only) feeds
   mutated client bytes through `ProtocolInput()` and `ProtocolOutput()`
   under ASan+UBSan. Its two crashes, FUZZ-1 and FUZZ-2, are fixed, and the
   crash inputs now run as ctest regression tests.
9. **Cloud review.** A cloud code review of the audit branch independently
   found the help-search over-read that the G2 reviewer reported as G2-01.
10. **Semgrep CI.** Branch `audit/semgrep-ci` adds a `semgrep ci` job for
    pushes to `master` and `audit/**`, pull requests, a weekly schedule and
    manual runs. It uploads to the Semgrep AppSec Platform.
11. **Integration.** The fix branches were cherry-picked in a fixed
    order: chunks A1 to H1 onto `audit/full-hardening-integration`
    (`merge-plan.json`), H2 to I3 plus the infrastructure and coordinator
    branches onto `audit/full-hardening-integration-b` (`merge-plan-b.json`),
    which was merged into the first, and the follow-up rounds onto
    `-c` through `-c8` (`merge-plan-c*.json`). Conflicts were resolved by
    hand and recorded in `integrate-state*.json`. Commit subjects survive
    cherry-picks, so this ledger cites fixes by subject, not by hash.
12. **Master merge.** Phase 1 pushed the audit branch, the A+B tag and
    `audit/verify-static` to `master` as 2c180c9b; phase 2 merged the
    fan-in branches `-c`, `-m2` and `-m1` (see the report's "How the fixes
    reached master").

In numbers:

<!-- BEGIN GENERATED: ledger-numbers -->

- Scope: 135 files in 9 areas, 93,473 lines at the review base `18f80ce`, split into 21 chunks.
- Coverage: 134 of 135 files were read line by line by the reviewer of the chunk that owns them. Not read in full: `src/class.c`.
- Findings: 523 (502 from reviewers, 21 added by verifiers).
- Verdicts: 442 confirmed, 43 plausible, 17 refuted; 64 severities re-rated by the verifiers.
- Runtime evidence: 241 of 502 verdicts cite an ASan, UBSan or Valgrind run (Valgrind 231, ASan 154, UBSan 64), including 16 of 16 critical and 80 of 95 high verdicts.
- Fix commits: 603 on 21 chunk branches (353 security, 91 diag, 19 fix, 69 docs, 58 style, 13 test); 468 name finding IDs and 430 record a regression test.
- Documentation and macros: reviewers reported 249 documentation gaps and 172 macro proposals; fix agents passed 82 cross-cutting macros on to the core-utils follow-up.
- Follow-up: 68 findings in 7 follow-up groups, 14 queued for a later round, and 9 new DG findings from the documentation pass.
- Later rounds: 74 findings the chunk branches left open were fixed by the follow-up rounds; 9 of the 9 DG documentation findings are fixed.
- Integration: 10 merge plans cherry-pick 1205 distinct commits from 43 distinct branches onto the `audit/full-hardening-integration*` branches (A, B, C, C2-C8), which the M1, M2 and C fan-in branches merged into `audit/master-merge-20261028`.

<!-- END GENERATED: ledger-numbers -->

Status values:

- `reviewed`: the reviewer of the chunk that owns the file read all of it.
  When that is not so, the row says so and a note follows the table.
- `done`: nothing is left to change. The file has no findings, or every
  finding was refuted, found already safe, or fixed on the audit branch
  before the fix pass.
- `done` also covers files whose findings were fixed on chunk or
  follow-up branches: all of those branches are merged.
- `N open`: N findings against the file still need work.

## Areas and coverage

<!-- BEGIN GENERATED: ledger-summary -->

| Area | Files | Read in full | Findings | Fixed | Partly fixed | Open | Skipped | Refuted |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A: Untrusted network input | 13 | 13 | 45 | 39 | 0 | 0 | 0 | 6 |
| B: Player and world persistence | 9 | 9 | 67 | 63 | 0 | 0 | 0 | 4 |
| C: Boards, bug reports, class import, OLC save writers, trigger files, config | 22 | 21 | 63 | 60 | 0 | 0 | 0 | 3 |
| D: DG script engine core | 7 | 7 | 43 | 43 | 0 | 0 | 0 | 0 |
| E: DG script commands, triggers and trigger OLC | 7 | 7 | 41 | 39 | 0 | 0 | 1 | 1 |
| F: OLC editors | 20 | 20 | 75 | 74 | 0 | 0 | 0 | 1 |
| G: Player and immortal commands | 6 | 6 | 53 | 52 | 0 | 0 | 0 | 1 |
| H: Gameplay and world mechanics | 30 | 30 | 77 | 76 | 0 | 0 | 0 | 1 |
| I: Foundations (utilities, macros, core headers) and standalone tools | 21 | 21 | 59 | 59 | 0 | 0 | 0 | 0 |
| **Total** | 135 | 134 | 523 | 505 | 0 | 0 | 1 | 17 |

Findings are counted against the file they name, so a finding one chunk reported about another area's file counts in that file's area. "Partly fixed" means a chunk branch fixed the part in its own files and the rest is in a follow-up group or queued; "open" means no fix yet.

<!-- END GENERATED: ledger-summary -->

<!-- BEGIN GENERATED: ledger-areas -->

### Area A: Untrusted network input

Status: reviewed by A1, A2 (every file read in full); 13 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| A1 | comm.c, comm.h, protocol.c | 5,763 | 33 + 1 verifier-added | `worktree-wf_c613c2c5-f9f-28` | 35 |
| A2 | protocol.h, telnet.h, interpreter.c, interpreter.h, ban.c, ban.h, modify.c, modify.h, improved-edit.c, improved-edit.h | 4,757 | 8 + 2 verifier-added | `worktree-wf_c613c2c5-f9f-29` | 17 |

| File | Status | Findings |
| --- | --- | --- |
| `src/comm.c` | reviewed; done | 25: 22 fixed, 3 refuted |
| `src/comm.h` | reviewed; done | none |
| `src/protocol.c` | reviewed; done | 9: 8 fixed, 1 refuted |
| `src/protocol.h` | reviewed; done | none |
| `src/telnet.h` | reviewed; done | none |
| `src/interpreter.c` | reviewed; done | 3: 2 fixed, 1 refuted |
| `src/interpreter.h` | reviewed; done | none |
| `src/ban.c` | reviewed; done | 2: 2 fixed |
| `src/ban.h` | reviewed; done | none |
| `src/modify.c` | reviewed; done | 3: 3 fixed |
| `src/modify.h` | reviewed; done | none |
| `src/improved-edit.c` | reviewed; done | 3: 2 fixed, 1 refuted |
| `src/improved-edit.h` | reviewed; done | none |

### Area B: Player and world persistence

Status: reviewed by B1, B2 (every file read in full); 9 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| B1 | db.c, db.h | 4,934 | 31 + 2 verifier-added | `worktree-wf_c613c2c5-f9f-30` | 35 |
| B2 | players.c, pfdefaults.h, objsave.c, house.c, house.h, mail.c, mail.h | 4,229 | 32 | `worktree-wf_c613c2c5-f9f-31` | 39 |

| File | Status | Findings |
| --- | --- | --- |
| `src/db.c` | reviewed; done | 35: 31 fixed, 4 refuted |
| `src/db.h` | reviewed; done | none |
| `src/players.c` | reviewed; done | 14: 14 fixed |
| `src/pfdefaults.h` | reviewed; done | none |
| `src/objsave.c` | reviewed; done | 9: 9 fixed |
| `src/house.c` | reviewed; done | 5: 5 fixed |
| `src/house.h` | reviewed; done | none |
| `src/mail.c` | reviewed; done | 4: 4 fixed |
| `src/mail.h` | reviewed; done | none |

### Area C: Boards, bug reports, class import, OLC save writers, trigger files, config

Status: reviewed by C1, C2 (every file read in full except `src/class.c`); 22 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| C1 | boards.c, boards.h, ibt.c, ibt.h, class.c, class.h, genolc.c, genolc.h, genmob.c, genmob.h | 5,981 | 29 + 3 verifier-added | `worktree-wf_c613c2c5-f9f-42` | 34 |
| C2 | genobj.c, genobj.h, genwld.c, genwld.h, genzon.c, genzon.h, genshp.c, genshp.h, genqst.c, dg_db_scripts.c, config.c, config.h | 3,402 | 29 + 1 verifier-added | `worktree-wf_c613c2c5-f9f-32` | 37 |

| File | Status | Findings |
| --- | --- | --- |
| `src/boards.c` | reviewed; done | 7: 7 fixed |
| `src/boards.h` | reviewed; done | none |
| `src/ibt.c` | reviewed; done | 9: 9 fixed |
| `src/ibt.h` | reviewed; done | none |
| `src/class.c` | reviewed (not every line, see note); done | 2: 2 fixed |
| `src/class.h` | reviewed; done | none |
| `src/genolc.c` | reviewed; done | 5: 4 fixed, 1 refuted |
| `src/genolc.h` | reviewed; done | none |
| `src/genmob.c` | reviewed; done | 6: 6 fixed |
| `src/genmob.h` | reviewed; done | none |
| `src/genobj.c` | reviewed; done | 14: 14 fixed |
| `src/genobj.h` | reviewed; done | none |
| `src/genwld.c` | reviewed; done | 5: 5 fixed |
| `src/genwld.h` | reviewed; done | none |
| `src/genzon.c` | reviewed; done | 7: 7 fixed |
| `src/genzon.h` | reviewed; done | none |
| `src/genshp.c` | reviewed; done | 3: 1 fixed, 2 refuted |
| `src/genshp.h` | reviewed; done | none |
| `src/genqst.c` | reviewed; done | 3: 3 fixed |
| `src/dg_db_scripts.c` | reviewed; done | 2: 2 fixed |
| `src/config.c` | reviewed; done | none |
| `src/config.h` | reviewed; done | none |

Note: the C1 reviewer did not read every line of `src/class.c`: "Class tables and rules: parse_class, find_class_bitvector, prac_params, guild_info, the saving_throws and thaco tables, roll_real_abils, do_start, advance_level, backstab_mult, invalid_class, init_spell_levels, level_exp and titles. Lines 1-800 were read in full. In 800-2170, the pure 'case N: return M;' table rows were filtered out by a strict regex, and every other line (defaults, logs, logic) was read."

### Area D: DG script engine core

Status: reviewed by D1, D2 (every file read in full); 7 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| D1 | dg_scripts.c, dg_scripts.h, dg_variables.c, dg_misc.c | 5,974 | 29 | `worktree-wf_c613c2c5-f9f-33` | 31 |
| D2 | dg_handler.c, dg_event.c, dg_event.h | 787 | 12 + 2 verifier-added | `worktree-wf_c613c2c5-f9f-34` | 11 |

| File | Status | Findings |
| --- | --- | --- |
| `src/dg_scripts.c` | reviewed; done | 21: 21 fixed |
| `src/dg_scripts.h` | reviewed; done | none |
| `src/dg_variables.c` | reviewed; done | 9: 9 fixed |
| `src/dg_misc.c` | reviewed; done | 1: 1 fixed |
| `src/dg_handler.c` | reviewed; done | 6: 6 fixed |
| `src/dg_event.c` | reviewed; done | 6: 6 fixed |
| `src/dg_event.h` | reviewed; done | none |

### Area E: DG script commands, triggers and trigger OLC

Status: reviewed by E1, E2 (every file read in full); 7 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| E1 | dg_mobcmd.c, dg_objcmd.c, dg_wldcmd.c, dg_comm.c, dg_triggers.c | 4,726 | 22 + 2 verifier-added | `worktree-wf_c613c2c5-f9f-35` | 24 |
| E2 | dg_olc.c, dg_olc.h | 1,396 | 16 | `worktree-wf_c613c2c5-f9f-36` | 15 |

| File | Status | Findings |
| --- | --- | --- |
| `src/dg_mobcmd.c` | reviewed; done | 8: 7 fixed, 1 refuted |
| `src/dg_objcmd.c` | reviewed; done | 7: 7 fixed |
| `src/dg_wldcmd.c` | reviewed; done | 4: 4 fixed |
| `src/dg_comm.c` | reviewed; done | 1: 1 fixed |
| `src/dg_triggers.c` | reviewed; done | 5: 4 fixed, 1 skipped |
| `src/dg_olc.c` | reviewed; done | 16: 16 fixed |
| `src/dg_olc.h` | reviewed; done | none |

### Area F: OLC editors

Status: reviewed by F1, F2, F3 (every file read in full); 20 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| F1 | oedit.c, medit.c, redit.c, zedit.c, sedit.c | 5,662 | 28 + 2 verifier-added | `audit-fix-F1` | 32 |
| F2 | cedit.c, aedit.c, hedit.c, hedit.h, qedit.c, tedit.c, prefedit.c, prefedit.h, msgedit.c, msgedit.h | 5,988 | 29 | `worktree-wf_c613c2c5-f9f-38` | 34 |
| F3 | oasis.c, oasis.h, oasis_copy.c, oasis_delete.c, oasis_list.c | 2,244 | 20 | `worktree-wf_c613c2c5-f9f-39` | 29 |

| File | Status | Findings |
| --- | --- | --- |
| `src/oedit.c` | reviewed; done | 13: 13 fixed |
| `src/medit.c` | reviewed; done | 5: 5 fixed |
| `src/redit.c` | reviewed; done | 4: 4 fixed |
| `src/zedit.c` | reviewed; done | 4: 4 fixed |
| `src/sedit.c` | reviewed; done | 3: 2 fixed, 1 refuted |
| `src/cedit.c` | reviewed; done | 6: 6 fixed |
| `src/aedit.c` | reviewed; done | 4: 4 fixed |
| `src/hedit.c` | reviewed; done | 6: 6 fixed |
| `src/hedit.h` | reviewed; done | none |
| `src/qedit.c` | reviewed; done | 4: 4 fixed |
| `src/tedit.c` | reviewed; done | 2: 2 fixed |
| `src/prefedit.c` | reviewed; done | 3: 3 fixed |
| `src/prefedit.h` | reviewed; done | none |
| `src/msgedit.c` | reviewed; done | 2: 2 fixed |
| `src/msgedit.h` | reviewed; done | none |
| `src/oasis.c` | reviewed; done | 7: 7 fixed |
| `src/oasis.h` | reviewed; done | none |
| `src/oasis_copy.c` | reviewed; done | 6: 6 fixed |
| `src/oasis_delete.c` | reviewed; done | none |
| `src/oasis_list.c` | reviewed; done | 6: 6 fixed |

### Area G: Player and immortal commands

Status: reviewed by G1, G2 (every file read in full); 6 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| G1 | act.h, act.wizard.c | 5,615 | 31 + 1 verifier-added | `audit-fix-G1` | 38 |
| G2 | act.informative.c, act.comm.c, act.social.c, act.other.c | 4,713 | 21 | `g2-fix-wf40` | 24 |

| File | Status | Findings |
| --- | --- | --- |
| `src/act.h` | reviewed; done | none |
| `src/act.wizard.c` | reviewed; done | 32: 31 fixed, 1 refuted |
| `src/act.informative.c` | reviewed; done | 13: 13 fixed |
| `src/act.comm.c` | reviewed; done | 3: 3 fixed |
| `src/act.social.c` | reviewed; done | 2: 2 fixed |
| `src/act.other.c` | reviewed; done | 3: 3 fixed |

### Area H: Gameplay and world mechanics

Status: reviewed by H1, H2, H3 (every file read in full); 30 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| H1 | act.item.c, act.movement.c, act.offensive.c, fight.c, fight.h, magic.c | 5,228 | 27 | `worktree-wf_c613c2c5-f9f-43` | 31 |
| H2 | spell_parser.c, spells.c, spells.h, handler.c, handler.h, shop.c, shop.h | 5,221 | 27 + 1 verifier-added | `worktree-wf_c613c2c5-f9f-44` | 30 |
| H3 | quest.c, quest.h, castle.c, spec_assign.c, spec_procs.c, spec_procs.h, mobact.c, limits.c, weather.c, graph.c, graph.h, asciimap.c, asciimap.h, mud_event.c, mud_event.h, lists.c, lists.h | 5,467 | 22 + 1 verifier-added | `audit-fix-H3` | 30 |

| File | Status | Findings |
| --- | --- | --- |
| `src/act.item.c` | reviewed; done | 11: 11 fixed |
| `src/act.movement.c` | reviewed; done | 2: 2 fixed |
| `src/act.offensive.c` | reviewed; done | 1: 1 fixed |
| `src/fight.c` | reviewed; done | 8: 8 fixed |
| `src/fight.h` | reviewed; done | none |
| `src/magic.c` | reviewed; done | 5: 4 fixed, 1 refuted |
| `src/spell_parser.c` | reviewed; done | 9: 9 fixed |
| `src/spells.c` | reviewed; done | 2: 2 fixed |
| `src/spells.h` | reviewed; done | none |
| `src/handler.c` | reviewed; done | 8: 8 fixed |
| `src/handler.h` | reviewed; done | none |
| `src/shop.c` | reviewed; done | 9: 9 fixed |
| `src/shop.h` | reviewed; done | none |
| `src/quest.c` | reviewed; done | 7: 7 fixed |
| `src/quest.h` | reviewed; done | none |
| `src/castle.c` | reviewed; done | none |
| `src/spec_assign.c` | reviewed; done | 1: 1 fixed |
| `src/spec_procs.c` | reviewed; done | 3: 3 fixed |
| `src/spec_procs.h` | reviewed; done | none |
| `src/mobact.c` | reviewed; done | 1: 1 fixed |
| `src/limits.c` | reviewed; done | 3: 3 fixed |
| `src/weather.c` | reviewed; done | none |
| `src/graph.c` | reviewed; done | none |
| `src/graph.h` | reviewed; done | none |
| `src/asciimap.c` | reviewed; done | 2: 2 fixed |
| `src/asciimap.h` | reviewed; done | none |
| `src/mud_event.c` | reviewed; done | 2: 2 fixed |
| `src/mud_event.h` | reviewed; done | none |
| `src/lists.c` | reviewed; done | 3: 3 fixed |
| `src/lists.h` | reviewed; done | none |

### Area I: Foundations (utilities, macros, core headers) and standalone tools

Status: reviewed by I1, I2, I3 (every file read in full); 21 files done, 0 with fixes on branch; 0 findings against these files still open.

| Chunk | Files | Lines | Findings reported | Fix branch | Fix commits |
| --- | --- | --- | --- | --- | --- |
| I1 | utils.c, utils.h, structs.h | 4,776 | 15 | `worktree-wf_c613c2c5-f9f-46` | 20 |
| I2 | sysdep.h, screen.h, constants.c, constants.h, random.c, bsd-snprintf.c, bsd-snprintf.h, zmalloc.c, zmalloc.h, util/asciipasswd.c, util/autowiz.c, util/plrtoascii.c, util/rebuildAsciiIndex.c, util/rebuildMailIndex.c, util/shopconv.c, util/sign.c | 5,957 | 27 + 2 verifier-added | `audit-fix-I2` | 39 |
| I3 | util/split.c, util/wld2html.c | 653 | 14 + 1 verifier-added | `worktree-wf_c613c2c5-f9f-41` | 18 |

| File | Status | Findings |
| --- | --- | --- |
| `src/utils.c` | reviewed; done | 14: 14 fixed |
| `src/utils.h` | reviewed; done | 1: 1 fixed |
| `src/structs.h` | reviewed; done | none |
| `src/sysdep.h` | reviewed; done | 3: 3 fixed |
| `src/screen.h` | reviewed; done | none |
| `src/constants.c` | reviewed; done | 2: 2 fixed |
| `src/constants.h` | reviewed; done | none |
| `src/random.c` | reviewed; done | 1: 1 fixed |
| `src/bsd-snprintf.c` | reviewed; done | 1: 1 fixed |
| `src/bsd-snprintf.h` | reviewed; done | none |
| `src/zmalloc.c` | reviewed; done | 4: 4 fixed |
| `src/zmalloc.h` | reviewed; done | 1: 1 fixed |
| `src/util/asciipasswd.c` | reviewed; done | 1: 1 fixed |
| `src/util/autowiz.c` | reviewed; done | 4: 4 fixed |
| `src/util/plrtoascii.c` | reviewed; done | 3: 3 fixed |
| `src/util/rebuildAsciiIndex.c` | reviewed; done | 3: 3 fixed |
| `src/util/rebuildMailIndex.c` | reviewed; done | 2: 2 fixed |
| `src/util/shopconv.c` | reviewed; done | 2: 2 fixed |
| `src/util/sign.c` | reviewed; done | 2: 2 fixed |
| `src/util/split.c` | reviewed; done | 2: 2 fixed |
| `src/util/wld2html.c` | reviewed; done | 13: 13 fixed |

<!-- END GENERATED: ledger-areas -->

## Findings

Each area's table lists the findings its chunks reported, by ID. A chunk can
report a finding in another area's file; the `File:function` column names
the file. The columns:

- **Severity:** as re-rated by the verifier. "(was X)" gives the reviewer's
  rating when the verifier changed it. A refuted finding shows the severity
  the reviewer claimed.
- **Input controller:** who controls the input that triggers the bug.
- **Verdict:** the verifier's verdict. `verifier-added` findings were
  reported by the verifier itself.
- **Resolution:** `fixed` cites the fix commit's subject, which stays the
  same across cherry-picks. `fixed in a later round` cites the follow-up
  round's commits (with the chunk branch's partial fix before it, if any).
  `fixed in part` cites the commit and says where the rest is: a follow-up
  group, or the queue for a later round.
  `deferred` names the follow-up group. `skipped` and `refuted` give the
  reason.

<!-- BEGIN GENERATED: ledger-findings -->

### Area A: Untrusted network input

44 findings from chunks A1, A2: 38 fixed; 1 skipped, no change needed; 5 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| A1-01 | high | buffer-bounds | `src/comm.c:process_output` | player | confirmed | fixed: `style: DIAG_DESC descriptor identity and TB_WARN_EVERY rate-limited warning`<br>`security: process_output never leaves bufptr negative after a partial prompt write`<br>`style: one helper releases the large output buffer` |
| A1-02 | high | double-free-or-leak | `src/comm.c:vwrite_to_output` | player | confirmed | fixed: `security: overflowing an active large output buffer no longer leaks it` |
| A1-03 | - (claimed high) | lifetime | `src/comm.c:process_input` | remote-unauthenticated | refuted | refuted: already fixed by `security: "--" flushes only queued input, not the live output buffer` (35a2832) |
| A1-04 | medium (was high) | lifetime | `src/protocol.c:ProtocolOutput` | player | confirmed | fixed: `security: unicode substitute text outlives its escape block in ProtocolOutput` |
| A1-05 | medium | integer | `src/protocol.c:ProtocolOutput` | player | confirmed | fixed: `security: bound the unicode code point parsed from '\t[u...]' escapes`<br>`style: ISDIGIT_C / ISALPHA_C / TOLOWER_C classify protocol bytes safely` |
| A1-06 | low (was medium) | read-past-terminator | `src/protocol.c:ProtocolOutput` | builder-olc | confirmed | fixed: `security: ProtocolOutput stops at the terminator after an unfinished escape`<br>also fixed directly by `security: protocol output never reads past a trailing tab; MSSP callbacks typed` (already on this branch) |
| A1-07 | high | undefined-behavior | `src/comm.c:msdp_update` | builder-olc | confirmed | fixed: `security: MSDP opponent health no longer divides by a zero max_hit` |
| A1-08 | medium (was high) | buffer-bounds | `src/comm.c:new_descriptor` | immortal | confirmed | fixed: `security: refuse connections whose fd select() cannot watch`<br>`security: copyover recovery validates each fd and reports lost players` |
| A1-09 | medium | sanitizer-hazard | `src/comm.c:make_prompt` | player | confirmed | fixed: `security: auto prompt compares quarter thresholds without shifting negatives` |
| A1-10 | low (was medium) | undefined-behavior | `src/comm.c:vwrite_to_output` | internal-invariant | confirmed | fixed: `security: vwrite_to_output passes ProtocolOutput a real int length`<br>also fixed directly by `security: protocol output length is written through a real int` (already on this branch) |
| A1-11 | medium | silent-failure | `src/protocol.c:ProtocolOutput` | player | confirmed | fixed: `security: oversized protocol output is cut with a marker, not dropped whole` |
| A1-12 | medium | lifetime | `src/comm.c:act_body` | dg-script | confirmed | fixed: `security: act() snapshots room recipients before running act triggers` |
| A1-13 | medium | silent-failure | `src/comm.c:act_body` | dg-script | confirmed | fixed: `security: nested act() no longer changes the outer act's trigger setting` |
| A1-14 | medium | silent-failure | `src/comm.c:game_loop` | internal-invariant | plausible | fixed in a later round: `security: a failed socket poll no longer passes for a normal shutdown` |
| A1-15 | low | read-past-terminator | `src/protocol.c:ProtocolInput` | remote-unauthenticated | confirmed | fixed: `security: ProtocolInput drops NUL bytes and returns the bytes it appended` |
| A1-16 | low | silent-failure | `src/comm.c:process_input` | player | confirmed | fixed: `diag: report input lines cut to the command buffer` |
| A1-17 | low | silent-failure | `src/comm.c:write_to_q` | player | confirmed | fixed: `diag: report commands dropped by the pending-input bound` |
| A1-18 | low | silent-failure | `src/comm.c:new_descriptor` | remote-unauthenticated | confirmed | fixed: `diag: report connections refused by the admission rate limit` |
| A1-19 | low | error-locality | `src/comm.c:act_body` | internal-invariant | confirmed | fixed: `diag: act() with no placed target names its caller and entities` |
| A1-20 | low | error-locality | `src/protocol.c:ReportBug` | builder-olc | confirmed | fixed: `diag: protocol reports name the descriptor and escape the offending text` |
| A1-21 | - (claimed low) | untrusted-index | `src/comm.c:send_to_room` | builder-olc | refuted | refuted: no caller can pass an invalid rnum (act.movement.c checks to_room; mud_event.c indexes world[] first); a guard would be defence in depth only |
| A1-22 | low | silent-failure | `src/comm.c:send_to_range` | internal-invariant | confirmed | fixed: `security: send_to_range reaches the highest room and reports bad ranges` |
| A1-23 | - (claimed low) | null-deref | `src/comm.c:act_body` | dg-script | refuted | refuted: both TO_GMOTE callers pass a non-NULL ch, and a playing character is never in NOWHERE between handler calls |
| A1-24 | low | undefined-behavior | `src/comm.c:hupsig` | internal-invariant | confirmed | fixed: `security: signal handlers only set flags and preserve errno` |
| A1-25 | low | undefined-behavior | `src/comm.c:open_logfile` | immortal | confirmed | fixed: `security: a failed log-file open no longer closes stderr`<br>`test: clang-tidy clean the A1 tests and the open_logfile dup2 path` |
| A1-26 | low | error-locality | `src/comm.c:copyover_recover` | save-or-world-file | confirmed | fixed: `security: copyover recovery validates each fd and reports lost players` |
| A1-27 | low | double-free-or-leak | `src/comm.c:game_loop` | immortal | confirmed | fixed: `security: SIGUSR2 emergency unban frees the lists it clears` |
| A1-28 | low | null-deref | `src/protocol.c:ProtocolCreate` | internal-invariant | confirmed | fixed: `security: protocol allocations abort on failure instead of writing through NULL` |
| A1-29 | low | other | `src/protocol.c:ProtocolInput` | remote-unauthenticated | confirmed | fixed: `security: telnet subnegotiations survive being split across reads` |
| A1-30 | low | format-string | `src/comm.c:process_output` | player | confirmed | fixed: `security: snoopers see only the bytes actually sent` |
| A1-31 | - (claimed low) | untrusted-index | `src/protocol.c:MSDPSetString` | internal-invariant | refuted | refuted: every caller passes an eMSDP_* constant; client MSDP input is resolved by name through validated loops |
| A1-32 | low | silent-failure | `src/comm.c:game_loop` | remote-unauthenticated | confirmed | fixed: `security: close descriptors whose prompt write fails` |
| A1-33 | low | integer | `src/comm.c:main` | immortal | confirmed | fixed: `style: DIAG_DESC descriptor identity and TB_WARN_EVERY rate-limited warning`<br>`security: range-check the port and copyover socket before narrowing` |
| A1-V01 | high | undefined-behavior | `src/dg_triggers.c:hitprcnt_mtrigger` | builder-olc | verifier-added | skipped: already safe: hitprcnt_mtrigger() tests GET_MAX_HIT(ch) before dividing, and that guard predates the audit (the verifier missed it) |
| A2-01 | critical | null-deref | `src/interpreter.c:perform_new_char_dupe_check` | remote-unauthenticated | confirmed | fixed: `security: new-character dupe check skips sockets with no name yet` |
| A2-02 | medium | read-past-terminator | `src/modify.c:next_page` | player | confirmed | fixed: `security: pager colour-code scans stop at the string terminator` |
| A2-03 | low | read-past-terminator | `src/improved-edit.c:improved_editor_execute` | player | confirmed | fixed: `security: editor rejects a bare "/" before reading its arguments` |
| A2-04 | - (claimed medium) | buffer-bounds | `src/improved-edit.c:format_text` | player | refuted | refuted: no reachable string is long enough for strncpy() to leave the buffer unterminated (editor input is capped far below MAX_STRING_LENGTH) |
| A2-05 | medium | silent-failure | `src/ban.c:load_banned` | save-or-world-file | confirmed | fixed: `security: ban file loader reports every record it rejects or truncates` |
| A2-06 | low | silent-failure | `src/ban.c:do_ban` | immortal | confirmed | fixed: `security: ban refuses a site longer than it can store` |
| A2-07 | low | null-deref | `src/interpreter.c:command_interpreter` | internal-invariant | plausible | fixed in a later round: `security: command_interpreter checks a switched NPC's original before use` |
| A2-08 | low | silent-failure | `src/modify.c:show_string` | player | confirmed | fixed: `diag: report a pager page clamped to show_string's buffer` |
| A2-V01 | medium | read-past-terminator | `src/modify.c:next_page` | save-or-world-file | verifier-added | fixed: `security: pager colour-code scans stop at the string terminator` |
| A2-V02 | medium | read-past-terminator | `src/improved-edit.c:format_text` | save-or-world-file | verifier-added | fixed: `security: editor /f word scan stays inside the edit buffer` |

### Area B: Player and world persistence

65 findings from chunks B1, B2: 61 fixed; 4 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| B1-01 | high | buffer-bounds | `src/db.c:file_to_string` | internal-invariant | confirmed | fixed: `security: file_to_string bounds by the caller's real buffer size`<br>also fixed directly by `security: text files load whole again, not as their first 7 bytes` (already on this branch) |
| B1-02 | critical | lifetime | `src/db.c:reset_zone` | dg-script | confirmed | fixed: `security: reset_zone never reuses an object its load or wear trigger purged` |
| B1-03 | high | untrusted-index | `src/db.c:setup_dir` | save-or-world-file | confirmed | fixed: `security: reject world exit directions outside dir_option[]` |
| B1-04 | high | read-past-terminator | `src/db.c:fread_clean_string` | player | confirmed | fixed: `security: fread_clean_string stops its CR/LF scan at the buffer start` |
| B1-05 | high | buffer-bounds | `src/db.c:fread_string` | save-or-world-file | confirmed | fixed: `security: fread_string keeps a long line inside its chunk buffer` |
| B1-06 | medium (was high) | untrusted-index | `src/db.c:parse_room` | save-or-world-file | confirmed | fixed: `security: room sectors outside 0..NUM_ROOM_SECTORS-1 are reported` |
| B1-07 | medium (was high) | integer | `src/db.c:reset_zone` | save-or-world-file | confirmed | fixed: `security: zone top may not be NOWHERE, and the trigger walk cannot wrap` |
| B1-08 | medium (was high) | buffer-bounds | `src/db.c:load_config` | immortal | confirmed | fixed: `security: load_config reads into a full READ_SIZE line buffer` |
| B1-09 | medium | null-deref | `src/db.c:load_config` | save-or-world-file | confirmed | fixed: `security: an empty config menu or start message falls back to the default` |
| B1-10 | medium | untrusted-index | `src/db.c:reset_zone` | builder-olc | confirmed | fixed: `security: reset_zone names the 'E' object from arg1 and disables a bad slot` |
| B1-11 | medium | untrusted-index | `src/db.c:check_object` | save-or-world-file | confirmed | fixed: `security: check_object repairs a liquid type outside drinks[]` |
| B1-12 | medium | untrusted-index | `src/db.c:parse_simple_mob` | save-or-world-file | confirmed | fixed: `security: reject mob positions and sex outside their tables` |
| B1-13 | medium | buffer-bounds | `src/db.c:boot_social_messages` | save-or-world-file | confirmed | fixed: `security: boot_social_messages stops at the slots it allocated` |
| B1-14 | medium | buffer-bounds | `src/db.c:parse_object` | save-or-world-file | confirmed | fixed: `security: discrete_load never fills a prototype slot past the allocation` |
| B1-15 | medium | integer | `src/db.c:discrete_load` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: world record numbers must fit a vnum; disorder is reported`<br>fixed in a later round: `security: index_boot sorts a table whose record numbers did not ascend`<br>`security: world record numbers must fit a vnum; disorder is reported` |
| B1-16 | medium | integer | `src/db.c:renum_zone_table` | builder-olc | confirmed | fixed: `security: zone command vnums are range-checked before they are narrowed` |
| B1-17 | - (claimed medium) | read-past-terminator | `src/db.c:load_help` | builder-olc | refuted | refuted: already fixed by `security: bound the argument splitters to their MAX_INPUT_LENGTH contract` (05425e1) |
| B1-18 | medium | buffer-bounds | `src/db.c:parse_mobile` | builder-olc | confirmed | fixed in part on the chunk branch: `security: world loading tests for a leading article without fname()`<br>fixed in a later round: `security: world loading tests for a leading article without fname()` |
| B1-19 | - (claimed low) | buffer-bounds | `src/db.c:check_object` | save-or-world-file | refuted | refuted: already fixed by `security: bound the argument splitters to their MAX_INPUT_LENGTH contract` (05425e1) |
| B1-20 | - (claimed low) | buffer-bounds | `src/db.c:fread_flags` | save-or-world-file | refuted | refuted: already fixed by `security: bound the argument splitters to their MAX_INPUT_LENGTH contract` (05425e1); the silent tb_parse_int_or_zero() it mentions is a separate low issue |
| B1-21 | medium | buffer-bounds | `src/db.c:get_one_line` | save-or-world-file | confirmed | fixed: `security: get_one_line strips only line endings and reports odd lines` |
| B1-22 | low | buffer-bounds | `src/db.c:index_boot` | save-or-world-file | confirmed | fixed: `security: index_boot reads index lines with a bounded width` |
| B1-23 | medium | silent-failure | `src/db.c:setup_dir` | save-or-world-file | confirmed | fixed: `security: a disabled diagonal exit is skipped whole, and reported` |
| B1-24 | low | silent-failure | `src/db.c:parse_room` | save-or-world-file | confirmed | fixed: `diag: flag bits that do not fit a 32-bit slot are reported` |
| B1-25 | low | double-free-or-leak | `src/db.c:reset_zone` | builder-olc | confirmed | fixed: `diag: reset_zone 'P' finds the container before creating the object` |
| B1-26 | - (claimed medium) | silent-failure | `src/db.c:init_char` | internal-invariant | refuted | refuted: create_entry() runs just before init_char() and cannot fail; the else branch also logs a SYSERR |
| B1-27 | low | silent-failure | `src/db.c:interpret_espec` | save-or-world-file | confirmed | fixed: `diag: mob E-spec values that are clamped or malformed are reported` |
| B1-28 | low | error-locality | `src/db.c:renum_zone_table` | save-or-world-file | confirmed | fixed: `diag: renum_zone_table names the missing room of a 'V' command` |
| B1-29 | low | sanitizer-hazard | `src/db.c:destroy_db` | save-or-world-file | confirmed | fixed: `security: destroy_db frees the strings of a disabled 'V' command` |
| B1-30 | low | silent-failure | `src/db.c:file_to_string_alloc` | immortal | confirmed | fixed: `diag: text files reload while a pager is reading the old copy` |
| B1-31 | low | undefined-behavior | `src/db.c:fread_number` | save-or-world-file | confirmed | fixed: `security: fread_number checks int range instead of overflowing` |
| B1-V01 | low | read-past-terminator | `src/db.c:fread_string` | save-or-world-file | verifier-added | fixed: `security: report a NUL byte at the start of a '~' string line` |
| B1-V02 | low | silent-failure | `src/db.c:reset_zone` | builder-olc | verifier-added | fixed: `security: zone 'V' on a mob or object uses arg2 as the variable context` |
| B2-01 | critical | null-deref | `src/objsave.c:handle_obj` | player | confirmed | fixed: `security: fill a refused autoeq container where it actually is` |
| B2-02 | high | buffer-bounds | `src/players.c:load_char` | save-or-world-file | confirmed | fixed: `security: bound pfile Act/Aff/Pref flag words before copying them` |
| B2-03 | high | read-past-terminator | `src/players.c:tag_argument` | save-or-world-file | confirmed | fixed: `security: tag_argument stops at the terminator of a short tag line` |
| B2-04 | high | buffer-bounds | `src/objsave.c:Crash_listrent` | player | confirmed | fixed: `security: bound rent and house listings with tb_buf and report truncation` |
| B2-05 | high | silent-failure | `src/mail.c:read_mail_record / read_delete` | save-or-world-file | confirmed | fixed: `security: a damaged mail record aborts the rewrite instead of truncating` |
| B2-06 | high | other | `src/mail.c:postmaster_receive_mail` | player | confirmed | fixed: `security: stop the mail receive loop when a message cannot be removed` |
| B2-07 | high | error-locality | `src/mail.c:read_mail_record` | save-or-world-file | confirmed | fixed: `security: truncated pfile descriptions and mail bodies no longer exit(1)` |
| B2-08 | high | untrusted-index | `src/house.c:House_boot` | save-or-world-file | confirmed | fixed: `security: reject hcontrol records whose guest count overruns guests[]` |
| B2-09 | high | silent-failure | `src/house.c:House_boot` | save-or-world-file | confirmed | fixed: `security: House_boot reports every skipped record and keeps hcontrol` |
| B2-10 | high | silent-failure | `src/players.c:build_player_index` | save-or-world-file | confirmed | fixed: `security: a malformed player index record no longer truncates the index` |
| B2-11 | high | error-locality | `src/players.c:save_player_index` | internal-invariant | confirmed | fixed: `security: stage player index saves and keep the live index on failure` |
| B2-12 | high | silent-failure | `src/objsave.c:Crash_rentsave / Crash_crashsave / Crash_idlesave / Crash_cryosave` | internal-invariant | confirmed | fixed: `security: stage rent and house saves; the receptionist aborts on failure` |
| B2-13 | medium | error-locality | `src/objsave.c:Crash_idlesave / Crash_rentsave / Crash_cryosave` | internal-invariant | confirmed | fixed: `security: extract rented objects only after the rent file commits` |
| B2-14 | medium | silent-failure | `src/objsave.c:Crash_save` | internal-invariant | confirmed | fixed: `security: propagate failed records from inside saved containers` |
| B2-15 | medium | untrusted-index | `src/objsave.c:objsave_parse_objects` | save-or-world-file | confirmed | fixed: `security: a skipped rent record no longer rewrites the previous object` |
| B2-16 | medium | untrusted-index | `src/players.c:load_char` | save-or-world-file | confirmed | fixed: `security: range-check pfile class, level, sex and skill values` |
| B2-17 | medium | uninitialized | `src/players.c:load_char` | save-or-world-file | confirmed | fixed: `security: legacy one-word Pref line converts its own value` |
| B2-18 | medium | uninitialized | `src/players.c:read_aliases_ascii` | save-or-world-file | confirmed | fixed: `security: check every alias line read from a player file` |
| B2-19 | medium | silent-failure | `src/players.c:save_char` | player | confirmed | fixed: `security: save_char keeps affects it cannot store` |
| B2-20 | medium | silent-failure | `src/players.c:save_char` | player | confirmed | fixed: `security: a re-created deleted character is indexed, and save_char reports a missing index entry` |
| B2-21 | low (was medium) | lifetime | `src/players.c:save_char` | dg-script | plausible | fixed in a later round: `security: save_char runs no wear/remove triggers mid-save` |
| B2-22 | medium | integer | `src/objsave.c:Crash_load_objs` | save-or-world-file | confirmed | fixed: `security: rent charges use signed 64-bit days and never wrap` |
| B2-23 | medium | sanitizer-hazard | `src/house.c:hcontrol_build_house` | immortal | confirmed | fixed in part on the chunk branch: `security: zero a newly built house record before it is written to hcontrol`<br>fixed in a later round: `security: hcontrol carries a format header; other layouts fail loudly`<br>`security: zero a newly built house record before it is written to hcontrol` |
| B2-24 | low (was medium) | untrusted-index | `src/house.c:do_house` | player | confirmed | fixed: `security: deleting the last house guest no longer reads past guests[]` |
| B2-25 | medium | untrusted-index | `src/mail.c:postmaster_receive_mail` | internal-invariant | confirmed | fixed: `security: mail notes carry no prototype rnum` |
| B2-26 | low | uninitialized | `src/players.c:remove_player` | save-or-world-file | confirmed | fixed in part on the chunk branch: `diag: remove_player reports an unrepresentable last-login time`<br>fixed in a later round: `security: tb_strftime leaves an empty string when the time is unrepresentable`<br>`diag: remove_player reports an unrepresentable last-login time` |
| B2-27 | low | double-free-or-leak | `src/house.c:ascii_convert_house` | immortal | confirmed | fixed: `security: house asciiconvert releases its objects and skips missing prototypes` |
| B2-28 | low | other | `src/players.c:clean_pfiles` | internal-invariant | confirmed | fixed: `security: clean_pfiles re-examines the slot a removed player vacates` |
| B2-29 | low | null-deref | `src/players.c:free_player_index` | internal-invariant | plausible | fixed in a later round: `security: an empty player index is -1, and remove_player checks its slot` |
| B2-30 | medium | error-locality | `src/players.c:parse_pfile_int_or_zero / load_affects / load_skills / load_quests / load_HMVS` | save-or-world-file | confirmed | fixed: `diag: pfile integer and unknown-tag diagnostics name the player and tag` |
| B2-31 | medium | error-locality | `src/objsave.c:objsave_parse_objects` | save-or-world-file | confirmed | fixed: `diag: rent and house object-record diagnostics name the file, owner, line and vnum` |
| B2-32 | low | format-string | `src/objsave.c:Crash_report_unrentables / Crash_report_rent` | builder-olc | plausible | fixed in a later round: `security: rent offers pass the object to act() as $p, not as format text` |

### Area C: Boards, bug reports, class import, OLC save writers, trigger files, config

62 findings from chunks C1, C2: 60 fixed; 2 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| C1-01 | critical | lifetime | `src/ibt.c:clean_ibt_list` | player | confirmed | fixed: `security: aborting an IBT frees only the aborting writer's node` |
| C1-02 | critical | buffer-bounds | `src/boards.c:board_write_message` | player | confirmed | fixed: `security: board headlines never write into the caller's command buffer` |
| C1-03 | high | double-free-or-leak | `src/boards.c:board_remove_msg` | player | confirmed | fixed: `security: removing a board message no longer leaves a stale index entry` |
| C1-04 | high | untrusted-index | `src/boards.c:board_load_board` | save-or-world-file | confirmed | fixed: `security: board loader never trusts pointers, slots or lengths from disk` |
| C1-05 | medium | read-past-terminator | `src/boards.c:board_load_board` | save-or-world-file | confirmed | fixed: `security: board loader requires NUL-terminated headings and bodies` |
| C1-06 | medium | double-free-or-leak | `src/boards.c:board_write_message` | player | confirmed | fixed: `security: board writes validate the headline before reserving a slot` |
| C1-07 | high | error-locality | `src/boards.c:init_boards` | builder-olc | confirmed | fixed in part on the chunk branch: `security: boot the bulletin boards in boot_db instead of on first use`<br>fixed in a later round: `security: boot the bulletin boards in boot_db instead of on first use` |
| C1-08 | low | error-locality | `src/boards.c:gen_board` | internal-invariant | confirmed | fixed: `diag: board diagnostics name the board vnum, message, slot and character` |
| C1-09 | medium | buffer-bounds | `src/db.c:fread_clean_string` | player | confirmed | fixed: `security: fread_clean_string stops its line-ending scan at the buffer start` |
| C1-10 | low (was medium) | uninitialized | `src/ibt.c:read_ibt` | save-or-world-file | confirmed | fixed: `security: IBT loader drops malformed records instead of exiting or crashing` |
| C1-11 | medium | null-deref | `src/ibt.c:read_ibt` | save-or-world-file | confirmed | fixed: `security: IBT loader drops malformed records instead of exiting or crashing` |
| C1-12 | low | double-free-or-leak | `src/ibt.c:free_ibt` | immortal | confirmed | fixed: `security: removing an IBT frees its admin notes` |
| C1-13 | high (was medium) | lifetime | `src/ibt.c:do_ibt` | immortal | confirmed | fixed in part on the chunk branch: `security: IBT remove and ibtedit track records, not list positions`<br>fixed in a later round: `diag: ibtedit shows and logs a record's current number`<br>`security: IBT remove and ibtedit track records, not list positions` |
| C1-14 | low (was medium) | null-deref | `src/ibt.c:ibtedit_parse` | internal-invariant | confirmed | fixed: `security: ibtedit_parse returns after ending a session in a bad mode` |
| C1-15 | medium | silent-failure | `src/ibt.c:save_ibt_file` | internal-invariant | confirmed | fixed: `security: IBT lists are saved through a temporary file and checked` |
| C1-16 | low | undefined-behavior | `src/ibt.c:ibtedit_disp_main_menu` | save-or-world-file | confirmed | fixed: `security: ibtedit working copy always has a name and header text` |
| C1-17 | low | sanitizer-hazard | `src/ibt.c:do_ibt` | save-or-world-file | plausible | fixed in a later round: `security: IBT dates are converted as time_t and checked before display` |
| C1-18 | medium (was high) | other | `src/class.c:saving_throws` | player | confirmed | fixed: `security: saving throws past the end of a class table use its last row` |
| C1-19 | low | error-locality | `src/class.c:level_exp` | save-or-world-file | confirmed | fixed in part on the chunk branch: `diag: class-table misses report the table, class, save type and level`<br>fixed in a later round: `diag: saving throws and level thresholds name the character on a table miss`<br>`diag: class-table misses name the character whose data is out of range`<br>`diag: class-table misses report the table, class, save type and level` |
| C1-20 | high | silent-failure | `src/genolc.c:save_all` | builder-olc | confirmed | fixed: `security: save_all tries each queued save once instead of spinning` |
| C1-21 | high | buffer-bounds | `src/genmob.c:delete_mobile` | builder-olc | confirmed | fixed: `security: delete_mobile revisits the slot a deleted zone command vacates` |
| C1-22 | medium | silent-failure | `src/genmob.c:delete_mobile` | builder-olc | confirmed | fixed: `security: delete_mobile never renumbers NOBODY or hands a shop to another mob` |
| C1-23 | medium | lifetime | `src/genmob.c:delete_mobile` | builder-olc | confirmed | fixed: `security: delete_mobile frees the prototype and detaches doomed instances` |
| C1-24 | medium | untrusted-index | `src/genolc.c:export_save_shops` | builder-olc | confirmed | fixed: `security: zone export handles a shop without a keeper` |
| C1-25 | medium (was low) | read-past-terminator | `src/genolc.c:export_save_objects` | save-or-world-file | confirmed | fixed: `security: zone export copies strings with a terminating, reporting copy` |
| C1-26 | low | silent-failure | `src/genolc.c:do_export_zone` | immortal | confirmed | fixed: `diag: zone export writers report incomplete files; info file counts exits` |
| C1-27 | - (claimed low) | other | `src/genolc.c:do_export_zone` | immortal | refuted | refuted: process_input() keeps only printable ASCII and splits lines, so the argument cannot forge a log record; the command is LVL_IMPL only |
| C1-28 | low | untrusted-index | `src/genmob.c:add_mobile` | builder-olc | plausible | fixed in a later round: `security: add_mobile refuses a vnum that belongs to no zone` |
| C1-29 | low | silent-failure | `src/genmob.c:save_mobiles` | internal-invariant | plausible | fixed in a later round: `security: save_mobiles keeps the old file when a write failed earlier` |
| C1-V01 | high | untrusted-index | `src/genmob.c:delete_mobile` | builder-olc | verifier-added | fixed: `security: delete_mobile never renumbers NOBODY or hands a shop to another mob` |
| C1-V02 | medium | silent-failure | `src/db.c:fread_clean_string` | save-or-world-file | verifier-added | fixed: `security: IBT loader drops malformed records instead of exiting or crashing` |
| C1-V03 | high | buffer-bounds | `src/genobj.c:delete_object` | builder-olc | verifier-added | fixed: `security: delete_object revisits the slot a deleted zone command vacates` |
| C2-01 | critical | buffer-bounds | `src/genobj.c:delete_object` | builder-olc | confirmed | fixed in part on the chunk branch: `security: delete_object re-examines the zone command that slides into a drop`<br>fixed in a later round: `security: delete_mobile steps past a zone command it could not remove`<br>`security: save_zone range-checks stored rnums before converting to vnums`<br>`security: delete_object re-examines the zone command that slides into a drop` |
| C2-02 | critical | integer | `src/genobj.c:delete_object` | builder-olc | confirmed | fixed: `security: delete_object leaves prototype-less objects at NOTHING` |
| C2-03 | high (was critical) | lifetime | `src/genwld.c:add_room / delete_room` | builder-olc | confirmed | fixed: `security: re-point room mud events after add_room/delete_room move world[]` |
| C2-04 | high | lifetime | `src/genwld.c:add_room` | builder-olc | confirmed | fixed: `security: a newly added room never inherits the caller's live lists` |
| C2-05 | critical | untrusted-index | `src/genqst.c:delete_quest` | builder-olc | confirmed | fixed: `security: delete_quest checks its rnum first and restores the spec by mob rnum` |
| C2-06 | - (claimed high) | format-string | `src/genshp.c:modify_shop_string` | builder-olc | refuted | refuted: already fixed by `security: expand shop messages without printf; type-check shop file reads` (02400df) |
| C2-07 | high | silent-failure | `src/genobj.c:delete_object` | builder-olc | confirmed | fixed: `security: delete_object rehomes contents by the container's real location` |
| C2-08 | high | untrusted-index | `src/genobj.c:delete_object` | builder-olc | confirmed | fixed: `security: drop shop products of a deleted prototype; refuse board objects` |
| C2-09 | high | untrusted-index | `src/genshp.c:save_shops` | builder-olc | confirmed | fixed: `security: save_shops validates the keeper rnum before indexing mob_index` |
| C2-10 | medium (was high) | double-free-or-leak | `src/genobj.c:update_all_objects` | builder-olc | confirmed | fixed: `security: free live instances' private state before a prototype update` |
| C2-11 | medium | double-free-or-leak | `src/genobj.c:copy_object_main` | builder-olc | confirmed | fixed: `security: copy_object/copy_room own triggers and keep the live events list` |
| C2-12 | high | buffer-bounds | `src/genzon.c:create_new_zone` | immortal | confirmed | fixed: `security: zedit new rejects the last zone's number and never truncates files` |
| C2-13 | high | null-deref | `src/genwld.c:save_rooms` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: save_rooms writes a NULL extra-description field as empty`<br>fixed in a later round: `security: redit copies a world-file extra description with no body`<br>`security: save_rooms writes a NULL extra-description field as empty` |
| C2-14 | medium (was high) | null-deref | `src/dg_db_scripts.c:parse_trigger` | save-or-world-file | confirmed | fixed: `security: parse_trigger loads an empty command body as '* No script'` |
| C2-15 | medium (was high) | silent-failure | `src/genqst.c:save_quests` | builder-olc | confirmed | fixed: `diag: save_quests abandons the zone save when a quest record does not fit` |
| C2-16 | medium | silent-failure | `src/genobj.c:save_objects` | internal-invariant | plausible | fixed in a later round: `security: save_rooms keeps the old file when a write failed earlier`<br>`security: save_mobiles keeps the old file when a write failed earlier`<br>`security: OLC writers keep the old file when a write failed earlier` |
| C2-17 | medium | untrusted-index | `src/genzon.c:save_zone` | internal-invariant | confirmed | fixed: `security: save_zone range-checks stored rnums before converting to vnums` |
| C2-18 | medium | silent-failure | `src/genzon.c:save_zone` | builder-olc | confirmed | fixed: `diag: report each reset line save_zone drops from disk` |
| C2-19 | low (was medium) | uninitialized | `src/genzon.c:new_command` | builder-olc | confirmed | fixed: `security: new_command inserts a fully zeroed blank zone command` |
| C2-20 | medium | silent-failure | `src/genzon.c:create_world_index` | save-or-world-file | confirmed | fixed: `diag: create_world_index reports failure and refuses an unterminated index` |
| C2-21 | medium | untrusted-index | `src/genwld.c:add_room` | builder-olc | confirmed | fixed: `security: add_room renumbers every zone's commands and GET_WAS_IN` |
| C2-22 | medium | integer | `src/genobj.c:oset_apply` | dg-script | confirmed | fixed: `security: oset apply parses strictly and rejects modifiers outside sbyte` |
| C2-23 | low (was medium) | double-free-or-leak | `src/genobj.c:delete_object` | builder-olc | confirmed | fixed: `security: delete_object frees the prototype and checks instances without assert` |
| C2-24 | low | double-free-or-leak | `src/genobj.c:oset_alias` | dg-script | confirmed | fixed: `security: oset string setters free what a prototype-less object owns` |
| C2-25 | low | error-locality | `src/genobj.c:save_objects` | builder-olc | confirmed | fixed: `diag: OLC writers name the object, zone, path and errno when they fail`<br>`test: clear clang-tidy findings in the genolc safety test and save_shops` |
| C2-26 | medium | silent-failure | `src/dg_db_scripts.c:dg_read_trigger` | save-or-world-file | confirmed | fixed: `diag: keep a missing trigger vnum in the prototype's trigger list` |
| C2-27 | low | double-free-or-leak | `src/genzon.c:remove_cmd_from_list` | builder-olc | confirmed | fixed in part on the chunk branch: `diag: remove_cmd_from_list reports a refused removal instead of returning`<br>fixed in a later round: `security: delete_room frees the 'V' strings of the commands it cancels`<br>`security: a zone command list owns its 'V' strings and frees them on removal`<br>`diag: remove_cmd_from_list reports a refused removal instead of returning` |
| C2-28 | low | untrusted-index | `src/genobj.c:add_object` | internal-invariant | plausible | fixed in a later round: `security: add_object refuses a vnum that belongs to no zone` |
| C2-29 | low | silent-failure | `src/genzon.c:create_new_zone` | immortal | confirmed | fixed: `diag: zedit new checks every stub write and removes files of a failed create` |
| C2-V01 | high | untrusted-index | `src/genwld.c:delete_room` | builder-olc | verifier-added | fixed: `security: delete_room moves start-room rnums down with world[]` |

### Area D: DG script engine core

43 findings from chunks D1, D2: 43 fixed.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| D1-01 | critical | buffer-bounds | `src/dg_variables.c:text_processed` | player | confirmed | fixed: `security: bound %var.car% to the replacement buffer` |
| D1-02 | high (was critical) | integer | `src/dg_scripts.c:process_wait` | dg-script | confirmed | fixed: `security: bound DG wait lengths before they reach the event queue` |
| D1-03 | high | other | `src/dg_scripts.c:eval_expr` | player | confirmed | fixed on the audit branch before the fix pass: `security: player text never becomes DG variable syntax on a second pass` (a195541) |
| D1-04 | high | read-past-terminator | `src/dg_variables.c:var_subst` | player | confirmed | fixed: `security: a lone '%' ending a DG line is kept and reported` |
| D1-05 | medium | silent-failure | `src/dg_variables.c:var_subst` | dg-script | confirmed | fixed: `security: reset the var_subst subfield for every variable` |
| D1-06 | critical (was high) | lifetime | `src/dg_scripts.c:script_trigger_check` | dg-script | confirmed | fixed: `security: random and time object dispatch never follows a freed obj->next` |
| D1-07 | high | lifetime | `src/dg_scripts.c:script_driver` | dg-script | confirmed | fixed: `security: script_driver revalidates its object owner after every command`<br>integration skipped this commit: superseded by audit-branch 545dd30 (per-frame owner check, no global dg_owner_purged) |
| D1-08 | high | null-deref | `src/dg_scripts.c:find_case` | builder-olc | confirmed | fixed: `security: while/switch/break without 'done' end the trigger loudly` |
| D1-09 | medium (was high) | untrusted-index | `src/dg_scripts.c:find_room` | dg-script | confirmed | fixed: `security: room UIDs stay in their namespace` |
| D1-10 | medium | null-deref | `src/dg_scripts.c:do_detach` | immortal | confirmed | fixed: `security: detach with no trigger argument stops after its usage message` |
| D1-11 | medium | double-free-or-leak | `src/dg_scripts.c:do_detach` | immortal | confirmed | fixed: `security: detach no longer leaks a trigger instance on every call` |
| D1-12 | medium | silent-failure | `src/dg_scripts.c:script_driver` | dg-script | confirmed | fixed: `security: a rejected DG wait ends the trigger instead of wedging it` |
| D1-13 | medium | double-free-or-leak | `src/dg_scripts.c:process_attach` | dg-script | confirmed | fixed: `security: attach rejects a player target before instantiating the trigger` |
| D1-14 | low (was medium) | double-free-or-leak | `src/dg_scripts.c:script_driver` | dg-script | confirmed | fixed: `security: a self-detaching script still frees its variables` |
| D1-15 | medium | silent-failure | `src/dg_scripts.c:extract_value` | dg-script | confirmed | fixed: `security: extract stops at the last word instead of spinning num times` |
| D1-16 | medium | buffer-bounds | `src/dg_scripts.c:do_stat_trigger` | save-or-world-file | confirmed | fixed: `security: tstat builds its listing with a bounded buffer` |
| D1-17 | medium | buffer-bounds | `src/dg_scripts.c:read_saved_vars` | save-or-world-file | confirmed | fixed: `security: legacy saved DG variables parse into argument-sized buffers` |
| D1-18 | medium | buffer-bounds | `src/dg_variables.c:find_replacement` | dg-script | confirmed | fixed: `security: %obj.extra% lists every flag instead of seven bytes` |
| D1-19 | low (was medium) | untrusted-index | `src/dg_variables.c:find_replacement` | internal-invariant | plausible | fixed in a later round: `security: DG room lookups check that the script owner is in a room` |
| D1-20 | medium | integer | `src/dg_variables.c:find_replacement` | dg-script | confirmed | fixed: `security: DG field setters saturate instead of wrapping narrow fields` |
| D1-21 | medium | integer | `src/dg_misc.c:do_dg_affect` | dg-script | confirmed | fixed: `security: dg_affect refuses values its affect fields cannot hold` |
| D1-22 | low | other | `src/dg_variables.c:find_replacement` | dg-script | confirmed | fixed: `security: is_killer/is_thief setters no longer flip mob flags` |
| D1-23 | low | silent-failure | `src/dg_scripts.c:trig_wait_event` | internal-invariant | confirmed | fixed: `security: a DG wait in the last room resumes instead of wedging` |
| D1-24 | low | silent-failure | `src/dg_scripts.c:eval_op` | dg-script | confirmed | fixed: `diag: DG arithmetic reports saturation and division by zero` |
| D1-25 | low | null-deref | `src/dg_scripts.c:find_case` | internal-invariant | confirmed | fixed: `security: switch case comparison uses a stack buffer`<br>integration skipped this commit: superseded by audit-branch stack buffers + prototype-safe copy |
| D1-26 | low | sanitizer-hazard | `src/dg_variables.c:text_processed` | dg-script | confirmed | fixed: `security: %var.trim% trims by index, never before the buffer` |
| D1-27 | low | silent-failure | `src/dg_scripts.c:process_rdelete` | dg-script | confirmed | fixed in part on the chunk branch: `diag: lost DG remote writes and detach misses are reported`<br>fixed in a later round: `diag: an rdelete with an unknown UID says rdelete, not remote`<br>`diag: lost DG remote writes and detach misses are reported` |
| D1-28 | low | error-locality | `src/dg_scripts.c:find_char_by_uid_in_lookup_table` | internal-invariant | confirmed | fixed: `diag: DG lookup misses and engine log lines name their trigger` |
| D1-29 | medium (was low) | silent-failure | `src/dg_variables.c:var_subst` | dg-script | confirmed | fixed: `security: field chains no longer rename the variable inside var_subst's copy` |
| D2-01 | high (was critical) | lifetime | `src/dg_event.c:event_cancel / event_process` | dg-script | confirmed | fixed: `security: defer freeing an event cancelled from its own callback` |
| D2-02 | high (was critical) | integer | `src/dg_event.c:event_create / queue_enq / queue_deq` | dg-script | confirmed | fixed in part on the chunk branch: `security: reject event keys that overflow into negative buckets`<br>fixed in a later round: `security: a DG wait whose event key would overflow is rejected by its trigger`<br>`security: reject event keys that overflow into negative buckets` |
| D2-03 | medium | lifetime | `src/dg_handler.c:update_wait_events` | builder-olc | confirmed | fixed in a later round: `security: a room wait finds its room by vnum after world[] moves` |
| D2-04 | medium | buffer-bounds | `src/dg_scripts.c:trig_wait_event` | save-or-world-file | confirmed | fixed in a later round: `security: a room wait finds its room by vnum after world[] moves` |
| D2-05 | low | silent-failure | `src/dg_handler.c:extract_script / free_proto_script` | internal-invariant | confirmed | fixed: `diag: make DG script ownership checks cover the last room and fail loudly` |
| D2-06 | low | silent-failure | `src/dg_handler.c:dg_script_execution_leave` | internal-invariant | plausible | fixed in a later round: `diag: an unbalanced DG execution leave is fatal and named` |
| D2-07 | low | double-free-or-leak | `src/dg_handler.c:extract_trigger` | internal-invariant | plausible | fixed in a later round: `security: extract_trigger refuses a trigger it has already extracted` |
| D2-08 | low | silent-failure | `src/dg_handler.c:free_proto_script / copy_proto_script` | builder-olc | plausible | fixed in a later round: `diag: proto-script helpers report a bad owner and a leaked list` |
| D2-09 | low | error-locality | `src/dg_event.c:event_cancel / event_process / extract_script` | internal-invariant | confirmed | fixed: `diag: report DG event cancel and extract_script misuse with call sites` |
| D2-10 | low | silent-failure | `src/dg_event.c:event_create` | dg-script | confirmed | fixed: `diag: warn when event_create clamps a non-positive delay` |
| D2-11 | low | error-locality | `src/dg_handler.c:delete_variables` | player | plausible | fixed in a later round: `diag: a script-variable file that survives deletion is reported with its player` |
| D2-12 | low | null-deref | `src/dg_event.c:event_time / event_is_queued` | internal-invariant | plausible | fixed in a later round: `security: event_time and event_is_queued refuse an event that has no slot` |
| D2-V01 | medium | integer | `src/dg_scripts.c:1852` | dg-script | verifier-added | fixed in a later round: `security: bound DG wait lengths before they reach the event queue` |
| D2-V02 | low | lifetime | `src/dg_event.c:155` | internal-invariant | verifier-added | fixed: `security: clear event_q after event_free_all and guard its users` |

### Area E: DG script commands, triggers and trigger OLC

40 findings from chunks E1, E2: 39 fixed; 1 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| E1-01 | - (claimed critical) | format-string | `src/dg_mobcmd.c:mob_log / do_mlog (also obj_log/do_olog, wld_log/do_wlog)` | player | refuted | refuted: already fixed by `security: DG log commands never treat script or name text as a format` (6e3be0c) |
| E1-02 | high | lifetime | `src/dg_objcmd.c:do_oat` | dg-script | confirmed | fixed: `security: oat destroys its clone by UID, never after it is freed` |
| E1-03 | high | lifetime | `src/dg_objcmd.c:do_opurge / do_oforce (script_driver owner contract)` | dg-script | confirmed | fixed: `security: stop an object script whose command freed its own owner` |
| E1-04 | high | null-deref | `src/dg_mobcmd.c:do_mload, do_mpurge, do_mforce, do_mhunt, do_mremember, do_mforget` | player | confirmed | fixed: `security: privileged m-commands no longer read GET_LEVEL(NULL)` |
| E1-05 | high | untrusted-index | `src/dg_objcmd.c:do_osetval` | dg-script | confirmed | fixed: `security: osetval rejects liquid and attack-type values out of range` |
| E1-06 | high | lifetime | `src/dg_mobcmd.c:do_mtransform` | dg-script | confirmed | fixed: `security: mtransform keeps furniture, memory; frees replaced strings` |
| E1-07 | high | lifetime | `src/dg_objcmd.c:do_otransform` | dg-script | confirmed | fixed: `security: otransform keeps sitters, events, weights and index counts` |
| E1-08 | medium (was high) | silent-failure | `src/dg_objcmd.c:do_omove` | dg-script | confirmed | fixed: `security: omove stops when its target room does not resolve` |
| E1-09 | medium | untrusted-index | `src/dg_objcmd.c:do_oteleport` | dg-script | confirmed | fixed: `security: oteleport all checks the object is in a room` |
| E1-10 | medium | lifetime | `src/dg_wldcmd.c:do_wteleport/do_wforce (also do_mteleport, do_oteleport, do_oforce)` | dg-script | confirmed | fixed: `security: DG "all" loops walk a UID snapshot, not next_in_room` |
| E1-11 | medium | uninitialized | `src/dg_mobcmd.c:do_mjunk` | dg-script | confirmed | fixed: `security: mjunk dispatches on find_all_dots(), junks only matches` |
| E1-12 | medium | silent-failure | `src/dg_mobcmd.c:do_mload` | dg-script | confirmed | fixed: `security: mload never loads an object into itself` |
| E1-13 | medium | silent-failure | `src/dg_wldcmd.c:do_wteleport / do_wmove (and other numeric DG args)` | dg-script | confirmed | fixed: `security: DG numeric arguments are parsed strictly, vnums range-checked` |
| E1-14 | low (was medium) | other | `src/dg_mobcmd.c:do_mdoor (also do_odoor, do_wdoor)` | dg-script | confirmed | fixed: `security: DG doors are edited by one helper; new exits lead nowhere` |
| E1-15 | low | untrusted-index | `src/dg_triggers.c:command_otrigger` | internal-invariant | plausible | fixed in a later round: `security: command_otrigger checks the actor's room before reading it` |
| E1-16 | low | silent-failure | `src/dg_triggers.c:load_mtrigger / load_otrigger` | dg-script | confirmed | fixed: `diag: report load triggers that disable their prototype's scripts` |
| E1-17 | low | buffer-bounds | `src/dg_triggers.c:one_phrase / word_check` | builder-olc | confirmed | fixed: `security: one_phrase honours its buffer size; long word lists reported` |
| E1-18 | low | silent-failure | `src/dg_triggers.c:greet_memory_mtrigger` | internal-invariant | confirmed | fixed: `diag: report a failed memory-command copy in greet_memory_mtrigger` |
| E1-19 | low | other | `src/dg_wldcmd.c:do_wload` | dg-script | confirmed | fixed: `security: wload sets %lastloaded% for every placement` |
| E1-20 | low | untrusted-index | `src/dg_mobcmd.c:do_masound / do_mpurge / do_mecho` | internal-invariant | plausible | fixed in a later round: `security: a mob script runs no command while its mob is in no room` |
| E1-21 | low | error-locality | `src/dg_comm.c:sub_write` | internal-invariant | confirmed | fixed: `diag: sub_write names a misplaced speaker and rejects any non-room` |
| E1-22 | low | error-locality | `src/dg_mobcmd.c:mob_log / obj_log / wld_log callers` | dg-script | confirmed | fixed: `diag: DG command rejections and charm refusals reach the error log` |
| E1-V01 | medium | integer | `src/dg_wldcmd.c:do_wteleport / do_wmove` | dg-script | verifier-added | fixed: `security: DG numeric arguments are parsed strictly, vnums range-checked` |
| E1-V02 | medium | integer | `src/dg_objcmd.c:do_otransform` | dg-script | verifier-added | fixed: `security: otransform keeps sitters, events, weights and index counts` |
| E2-01 | high (was critical) | buffer-bounds | `src/dg_olc.c:format_script` | builder-olc | confirmed | fixed: `security: format_script refuses to nest past its block stack` |
| E2-02 | critical | lifetime | `src/dg_olc.c:trigedit_save` | builder-olc | confirmed | fixed: `security: inserting a trigger renumbers the displaced prototype's instances` |
| E2-03 | high | lifetime | `src/dg_olc.c:trigedit_save` | builder-olc | confirmed | fixed: `security: saving a copied trigger keeps the prototype's own rnum` |
| E2-04 | medium | double-free-or-leak | `src/dg_olc.c:trigedit_parse` | builder-olc | confirmed | fixed: `security: copying a trigger releases the old scratch trigger and buffer` |
| E2-05 | low (was medium) | double-free-or-leak | `src/dg_olc.c:trigedit_parse` | builder-olc | confirmed | fixed: `security: trigedit frees the old argument before replacing it` |
| E2-06 | medium | silent-failure | `src/dg_olc.c:format_script` | builder-olc | confirmed | fixed: `security: format_script writes whole lines or refuses loudly` |
| E2-07 | medium | silent-failure | `src/dg_olc.c:trigedit_save` | builder-olc | confirmed | fixed: `diag: trigedit_save checks the zone first and reports every failure` |
| E2-08 | low (was medium) | silent-failure | `src/dg_olc.c:trigedit_create_index` | save-or-world-file | confirmed | fixed: `diag: trigger index rewrite keeps a truncated index and reports it` |
| E2-09 | low (was medium) | double-free-or-leak | `src/dg_olc.c:do_oasis_trigedit` | internal-invariant | plausible | fixed in a later round: `diag: trigedit releases a stale OLC session with cleanup_olc and reports it` |
| E2-10 | low (was medium) | silent-failure | `src/dg_olc.c:trigedit_parse` | builder-olc | confirmed | fixed: `security: trigger editor rejects non-numeric answers instead of using 0` |
| E2-11 | low | untrusted-index | `src/dg_olc.c:trigedit_disp_types` | builder-olc | confirmed | fixed: `security: trigger type menu is bounded by each attach type's table` |
| E2-12 | low | untrusted-index | `src/dg_olc.c:dg_script_menu` | internal-invariant | plausible | fixed in a later round: `security: the trigger attachment menu checks each trigger vnum it lists` |
| E2-13 | low | silent-failure | `src/dg_olc.c:script_syntax_highlighting` | builder-olc | confirmed | fixed: `diag: trigger listing says when it was cut and reports failed allocations` |
| E2-14 | low | null-deref | `src/dg_olc.c:format_script` | internal-invariant | confirmed | fixed: `security: trigger editor never stores a NULL from a failed strdup` |
| E2-15 | low | error-locality | `src/dg_olc.c:script_save_to_disk` | internal-invariant | plausible | fixed in a later round: `diag: script_save_to_disk names the saver and returns a status on bad input` |
| E2-16 | low | lifetime | `src/dg_olc.c:trigedit_save` | builder-olc | plausible | fixed in a later round: `security: the trigedit scratch trigger never points at a prototype's script` |

### Area F: OLC editors

79 findings from chunks F1, F2, F3: 77 fixed; 2 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| F1-01 | high | lifetime | `src/oedit.c:iedit_parse / iedit_save_internally / iedit_object_still_exists` | player | confirmed | fixed: `security: iedit re-validates the live object by UID before saving` |
| F1-02 | high | lifetime | `src/redit.c:redit_setup_existing` | builder-olc | confirmed | fixed: `security: redit room copies no longer alias the source room's contents` |
| F1-03 | high | untrusted-index | `src/oedit.c:oedit_parse (OEDIT_DELETE) / medit_parse (MEDIT_DELETE) / redit_parse (REDIT_DELETE)` | builder-olc | confirmed | fixed: `security: renumber open OLC sessions on delete; report stale rnums` |
| F1-04 | high | untrusted-index | `src/oedit.c:oedit_parse (OEDIT_COPY/OEDIT_DELETE), medit_parse (MEDIT_COPY/MEDIT_DELETE), redit_parse (REDIT_COPY/REDIT_DELETE)` | builder-olc | confirmed | fixed: `security: OLC delete acts on the edited vnum and checks zone permission` |
| F1-05 | high | untrusted-index | `src/oedit.c:oedit_save_internally` | builder-olc | confirmed | fixed: `security: object inserts renumber other oedit and iedit sessions` |
| F1-06 | - (claimed high) | format-string | `src/sedit.c:sedit_parse (SEDIT_NOITEM1..SEDIT_SELL) via modify_shop_string` | builder-olc | refuted | refuted: already fixed by `security: expand shop messages without printf; type-check shop file reads` (02400df) |
| F1-07 | medium (was high) | double-free-or-leak | `src/oedit.c:oedit_save_internally` | builder-olc | confirmed | fixed: `fix: oedit save tears down live instances' scripts before add_object` |
| F1-08 | low (was medium) | double-free-or-leak | `src/medit.c:medit_save_internally / oedit_save_internally / redit_save_internally` | builder-olc | confirmed | fixed: `fix: OLC saves free the prototype's previous trigger list` |
| F1-09 | low (was medium) | double-free-or-leak | `src/oedit.c:oedit_parse (OEDIT_MAIN_MENU 'q' with OLC_VAL==0) / medit_parse / redit_parse / cleanup_olc` | builder-olc | confirmed | fixed: `fix: OLC trigger-list copies have one owner and are freed on every exit` |
| F1-10 | low (was medium) | double-free-or-leak | `src/medit.c:medit_parse (MEDIT_COPY) and peers` | builder-olc | confirmed | fixed: `fix: OLC 'W' copy releases the scratch copy it replaces` |
| F1-11 | medium | other | `src/medit.c:medit_parse (MEDIT_COPY) -> medit_save_internally -> add_mobile` | builder-olc | confirmed | fixed: `security: medit save checks the scratch mobile's rnum against OLC_NUM` |
| F1-12 | low (was medium) | null-deref | `src/medit.c:medit_parse (MEDIT_D_DESC / default) and sedit_parse (default)` | internal-invariant | plausible | fixed: `style: OLC_ABORT and OLC_DIAG for "should never get here" paths`<br>`style: clear the clang-tidy findings the F1 fixes introduced`<br>(follow-up `gen-olc` asked to confirm the fix) |
| F1-13 | medium | integer | `src/redit.c:redit_save_internally` | builder-olc | confirmed | fixed: `security: room inserts no longer wrap NOWHERE exits or move door directions` |
| F1-14 | medium | silent-failure | `src/redit.c:redit_save_internally` | builder-olc | confirmed | fixed: `security: redit decides insert-or-update from OLC_NUM, not the scratch vnum` |
| F1-15 | medium | untrusted-index | `src/zedit.c:zedit_parse (ZEDIT_CHANGE_ENTRY 'A')` | builder-olc | confirmed | fixed: `security: zedit abort no longer indexes past a shrunken command list` |
| F1-16 | low (was medium) | untrusted-index | `src/zedit.c:zedit_disp_menu / medit_disp_menu / sedit_namelist_menu` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: OLC menus bound name-table lookups by the table size`<br>fixed in a later round: `security: read_type_list drops a buy type that does not fit an int`<br>`security: load_zones disables an 'E' or 'D' whose slot or direction is out of range`<br>`security: OLC menus bound name-table lookups by the table size` |
| F1-17 | medium | other | `src/oedit.c:iedit_save_internally / iedit_replace_values` | builder-olc | confirmed | fixed: `fix: iedit re-inserts a held object so its weight and affects are re-accounted` |
| F1-18 | medium | other | `src/sedit.c:sedit_parse (SEDIT_KEEPER)` | builder-olc | confirmed | fixed: `security: sedit installs a shopkeeper's spec proc at save, with zone permission` |
| F1-19 | low | buffer-bounds | `src/zedit.c:zedit_parse (ZEDIT_ARG2 'D')` | builder-olc | confirmed | fixed: `fix: zedit door direction accepts only the game's exits` |
| F1-20 | low | uninitialized | `src/zedit.c:zedit_parse (ZEDIT_NEW_ENTRY / 'n') via genzon.c new_command; ZEDIT_SARG1/SARG2` | builder-olc | confirmed | fixed: `fix: zedit initialises new commands and owns its 'V' strings` |
| F1-21 | low | double-free-or-leak | `src/oedit.c:do_oasis_oedit (and do_oasis_medit/redit/zedit/sedit)` | internal-invariant | plausible | fixed: `diag: OLC session start reports and fully releases a stale OLC structure`<br>(follow-up `gen-olc` asked to confirm the fix) |
| F1-22 | low | error-locality | `src/oedit.c:oedit_save_internally / medit_save_internally / redit_save_internally / zedit_save_internally / iedit_save_internally` | internal-invariant | confirmed | fixed: `diag: OLC save failures are reported with IDs and never announced as saved` |
| F1-23 | low | silent-failure | `src/oedit.c:oedit_parse / medit_parse / sedit_parse / zedit_parse numeric fields` | builder-olc | confirmed | fixed: `diag: OLC clamps of builder input tell the builder and log olc-clamp`<br>`style: clear the clang-tidy findings the F1 fixes introduced` |
| F1-24 | low | other | `src/oedit.c:oedit_parse (OEDIT_VALUE_1 furniture; OEDIT_APPLY)` | builder-olc | confirmed | fixed: `fix: oedit re-prompts a bad seat count and checks duplicate applies correctly` |
| F1-25 | low | lifetime | `src/redit.c:redit_parse (REDIT_EXTRADESC_MENU case 0)` | builder-olc | plausible | fixed: `fix: redit clears OLC_DESC after freeing an incomplete extra description`<br>(follow-up `gen-olc` asked to confirm the fix) |
| F1-26 | low | silent-failure | `src/sedit.c:sedit_rooms_menu / sedit_parse (SEDIT_NEW_PRODUCT)` | builder-olc | confirmed | fixed: `fix: sedit room list no longer rewrites deleted rooms to the Void` |
| F1-27 | low | buffer-bounds | `src/medit.c:medit_parse (MEDIT_L_DESC) / redit_parse (REDIT_NAME)` | builder-olc | confirmed | fixed: `fix: medit long descriptions keep their line ending; redit refuses over-long names`<br>`style: clear the clang-tidy findings the F1 fixes introduced` |
| F1-28 | low | sanitizer-hazard | `src/oedit.c:oedit_disp_perm_menu / oedit_disp_menu / medit_disp_mob_flags / medit_disp_menu` | internal-invariant | plausible | fixed: `style: SPRINTBITARRAY_FIELD ties flag printing to the array's own size`<br>(follow-up `gen-olc` asked to confirm the fix) |
| F1-V01 | medium | other | `src/oedit.c:do_oasis_iedit` | builder-olc | verifier-added | fixed: `security: iedit requires edit permission for the object's zone` |
| F1-V02 | low | double-free-or-leak | `src/genobj.c:delete_object` | builder-olc | verifier-added | fixed: `fix: deleting an object prototype frees its strings and trigger list`<br>integration skipped this commit: identical fix already applied from C2 (single free verified) |
| F2-01 | critical | buffer-bounds | `src/hedit.c:hedit_parse / hedit_save_internally` | builder-olc | confirmed | fixed: `security: hedit 'n' walk can no longer write past help_table` |
| F2-02 | high | untrusted-index | `src/hedit.c:do_oasis_hedit` | builder-olc | confirmed | fixed: `security: hedit tests a help search miss before indexing help_table` |
| F2-03 | high | double-free-or-leak | `src/hedit.c:hedit_parse` | builder-olc | confirmed | fixed: `security: hedit never hands the editor a string literal to free` |
| F2-04 | medium (was high) | null-deref | `src/tedit.c:do_tedit / tedit_string_cleanup` | builder-olc | confirmed | fixed: `security: tedit edits a private copy and never publishes NULL text` |
| F2-05 | - (claimed high) | format-string | `src/interpreter.c:perform_complete_protocol-negotiation path (greeting write)` | builder-olc | refuted | refuted: already fixed by `security: send client-reported protocol banner as data, not a format` (91bad16) |
| F2-06 | high | lifetime | `src/prefedit.c:prefedit_setup / prefedit_disp_main_menu / prefedit_save_to_char` | immortal | confirmed | fixed: `security: prefedit tracks its target by idnum, not a raw pointer` |
| F2-07 | medium (was high) | untrusted-index | `src/qedit.c:qedit_disp_menu` | builder-olc | confirmed | fixed: `security: qedit names quest types through a range-checked lookup` |
| F2-08 | high | untrusted-index | `src/qedit.c:qedit_parse (QEDIT_CONFIRM_DELETE) -> delete_quest` | builder-olc | confirmed | fixed: `security: qedit refuses to delete a quest that was never saved` |
| F2-09 | high | untrusted-index | `src/aedit.c:aedit_parse (AEDIT_MIN_CHAR_POS/AEDIT_MIN_VICT_POS)` | builder-olc | confirmed | fixed: `security: aedit range-checks social positions and level` |
| F2-10 | medium | uninitialized | `src/prefedit.c:do_oasis_prefedit` | player | confirmed | fixed: `security: prefedit no longer sends an uninitialised buffer through act()` |
| F2-11 | medium | null-deref | `src/prefedit.c:do_oasis_prefedit (also do_oasis_qedit qedit.c:126, do_msgedit msgedit.c:352)` | dg-script | confirmed | fixed: `security: prefedit, qedit and msgedit refuse callers with no descriptor` |
| F2-12 | low (was medium) | double-free-or-leak | `src/oasis.c:cleanup_olc (called from cedit_parse cedit.c:839/849/1764 and close_socket)` | builder-olc | confirmed | fixed: `security: always release the cedit config copy in cleanup_olc`<br>(the same defect as F3-07; fixed by that chunk's commit) |
| F2-13 | low (was medium) | double-free-or-leak | `src/qedit.c:qedit_parse (QEDIT_CONFIRM_SAVESTRING)` | builder-olc | confirmed | fixed: `security: qedit frees its quest strings after saving` |
| F2-14 | medium | error-locality | `src/msgedit.c:save_messages_to_disk` | builder-olc | confirmed | fixed: `diag: msgedit save never exits the server and reports failures` |
| F2-15 | medium | silent-failure | `src/tedit.c:tedit_string_cleanup` | builder-olc | confirmed | fixed: `diag: tedit saves through a verified temp file and reports failures` |
| F2-16 | low (was medium) | other | `src/msgedit.c:msgedit_parse` | builder-olc | confirmed | fixed: `security: msgedit keeps each editor's quit intent in its own OLC mode` |
| F2-17 | low | double-free-or-leak | `src/aedit.c:aedit_save_internally -> sort_commands` | builder-olc | confirmed | fixed: `security: sort_commands releases the previous order before rebuilding it` |
| F2-18 | low | double-free-or-leak | `src/cedit.c:cedit_parse (CEDIT_DFLT_IP/CEDIT_DFLT_DIR/CEDIT_LOGNAME)` | builder-olc | confirmed | fixed: `security: cedit frees the old default IP, directory and log name it replaces` |
| F2-19 | low | double-free-or-leak | `src/hedit.c:hedit_save_internally` | builder-olc | confirmed | fixed: `security: hedit frees the replaced help row and never saves into a duplicate` |
| F2-20 | low | buffer-bounds | `src/cedit.c:reassign_rooms` | builder-olc | confirmed | fixed: `security: cedit's dump reassignment covers the last room and only clears dumps` |
| F2-21 | low | silent-failure | `src/cedit.c:cedit_parse / cedit_save_to_disk (and aedit.c:74,355; hedit.c:71,240; qedit.c:395,420; msgedit.c:564)` | builder-olc | confirmed | fixed: `diag: OLC save helpers return their result and never claim a failed save` |
| F2-22 | low | silent-failure | `src/cedit.c:cedit_parse / qedit_parse / prefedit_parse / msgedit_parse / do_msgedit / aedit_parse` | builder-olc | confirmed | fixed: `diag: cedit, qedit and aedit re-prompt bad numbers instead of clamping` |
| F2-23 | low | integer | `src/cedit.c:cedit_parse (room numbers) / qedit_parse (qedit.c:600,642,679,690,713)` | builder-olc | confirmed | fixed: `security: cedit range-checks room vnums before real_room and storage` |
| F2-24 | low | error-locality | `src/aedit.c:do_oasis_aedit (also hedit.c:77, qedit.c:127, prefedit.c:997, msgedit.c:353, tedit.c:133; default cases cedit.c:1765, qedit.c:723, hedit.c:354/369, prefedit.c:775, aedit.c:752)` | internal-invariant | confirmed | fixed: `diag: OLC editors release a stale workspace with cleanup_olc and report it` |
| F2-25 | low | silent-failure | `src/aedit.c:aedit_parse (message fields) / msgedit_parse (msgedit.c:588-698)` | builder-olc | confirmed | fixed: `diag: aedit refuses social messages that start with the reserved '#'` |
| F2-26 | low | other | `src/cedit.c:cedit_parse (CEDIT_CONFIRM_SAVESTRING)` | builder-olc | confirmed | fixed: `security: cedit autosaves on the OLC autosave toggle, not the crash-save one` |
| F2-27 | low | read-past-terminator | `src/hedit.c:hedit_save_to_disk` | save-or-world-file | plausible | fixed in a later round: `security: hedit refuses to save a help entry longer than its buffer` |
| F2-28 | low | other | `src/qedit.c:qedit_parse (QEDIT_FLAGS)` | builder-olc | plausible | fixed in a later round: `fix: qedit's flag menu toggles the bit of the flag the builder chose` |
| F2-29 | low | error-locality | `src/hedit.c:hedit_parse` | internal-invariant | confirmed | fixed: `diag: hedit logs saves with mudlog's real argument order and reports bad modes` |
| F3-01 | critical | buffer-bounds | `src/oasis_list.c:perform_obj_aff_list` | builder-olc | confirmed | fixed: `security: bound olist affect output and report truncation` |
| F3-02 | high | buffer-bounds | `src/oasis_list.c:perform_obj_aff_list` | builder-olc | confirmed | fixed: `security: index olist affect rows by their rnum, not the stale scan index` |
| F3-03 | high | untrusted-index | `src/oasis_list.c:perform_obj_type_list` | builder-olc | confirmed | fixed: `security: range-check olist type before indexing item_types[]` |
| F3-04 | medium | read-past-terminator | `src/utils.c:count_color_chars` | builder-olc | confirmed | fixed: `security: stop count_color_chars at a trailing lone tab escape` |
| F3-05 | high | lifetime | `src/oasis_copy.c:do_oasis_copy` | immortal | confirmed | fixed: `security: never insert a copied room that shares the source's object list` |
| F3-06 | medium | lifetime | `src/oasis_copy.c:do_oasis_copy` | immortal | confirmed | fixed: `security: rcopy saves as a new room and rejects out-of-range vnums` |
| F3-07 | medium (was high) | double-free-or-leak | `src/oasis.c:cleanup_olc` | immortal | confirmed | fixed: `security: always release the cedit config copy in cleanup_olc` |
| F3-08 | medium | double-free-or-leak | `src/oasis.c:cleanup_olc` | remote-unauthenticated | confirmed | fixed: `security: give OLC_SCRIPT a single owner and free it in cleanup_olc` |
| F3-09 | low (was medium) | double-free-or-leak | `src/oasis_copy.c:buildwalk` | builder-olc | confirmed | fixed: `security: free buildwalk's template strings after the room is added` |
| F3-10 | medium | double-free-or-leak | `src/oasis_copy.c:do_dig` | immortal | confirmed | fixed: `security: refuse dig/buildwalk/copy instead of freeing an open editor` |
| F3-11 | medium | double-free-or-leak | `src/comm.c:close_socket` | remote-unauthenticated | confirmed | fixed: `security: release any OLC workspace when a socket closes` |
| F3-12 | low | double-free-or-leak | `src/oasis.c:cleanup_olc` | builder-olc | confirmed | fixed: `security: zedit's command copy owns its sarg strings` |
| F3-13 | low | error-locality | `src/oasis.c:cleanup_olc` | internal-invariant | confirmed | fixed: `diag: report bad cleanup types and keep internal OLC users quiet` |
| F3-14 | low | silent-failure | `src/oasis_copy.c:do_oasis_copy` | internal-invariant | confirmed | fixed: `diag: copy commands explain refusals and never report a failed save as done` |
| F3-15 | low | silent-failure | `src/oasis_list.c:perform_mob_flag_list` | builder-olc | confirmed | fixed: `diag: every OLC list reports truncation instead of stopping silently` |
| F3-16 | low | silent-failure | `src/oasis_list.c:list_mobiles` | builder-olc | confirmed | fixed: `security: mlist, olist and zlist list a one-record table` |
| F3-17 | low | other | `src/oasis_list.c:perform_mob_flag_list` | builder-olc | confirmed | fixed: `security: mlist level/flags read prototypes instead of spawning mobs` |
| F3-18 | low | integer | `src/oasis_copy.c:do_dig` | builder-olc | confirmed | fixed: `security: dig range-checks its target vnum` |
| F3-19 | low | buffer-bounds | `src/oasis.c:split_argument` | save-or-world-file | plausible | fixed in a later round: `security: split_argument bounds the config key by its buffer` |
| F3-20 | low | other | `src/oasis.c:can_edit_zone` | internal-invariant | plausible | fixed in a later round: `security: aedit checks the socials-editor permission with can_use_editor`<br>`security: can_edit_zone bounds its zone index and keeps editor sentinels out` |

### Area G: Player and immortal commands

53 findings from chunks G1, G2: 52 fixed; 1 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| G1-01 | critical | lifetime | `src/act.wizard.c:perform_set / do_set` | immortal | confirmed | fixed: `security: "set file <name> room" no longer leaves a freed char in a room`<br>`test: test_act_wizard can run a single test (TB_TEST_ONLY)` |
| G1-02 | - (claimed high) | buffer-bounds | `src/act.wizard.c:do_plist` | immortal | refuted | refuted: already fixed by `security: bound the argument splitters to their MAX_INPUT_LENGTH contract` (05425e1) |
| G1-03 | medium | buffer-bounds | `src/act.wizard.c:do_zcheck` | builder-olc | confirmed | fixed: `security: zcheck builds each report in a tb_buf and resets it per room` |
| G1-04 | high | other | `src/act.wizard.c:do_stat` | immortal | confirmed | fixed: `security: "stat file" never saves the stale copy or deletes crash files` |
| G1-05 | high | null-deref | `src/act.wizard.c:do_return / do_switch` | dg-script | confirmed | fixed: `security: return and switch refuse a caller with no descriptor`<br>`test: test_act_wizard can run a single test (TB_TEST_ONLY)` |
| G1-06 | medium | lifetime | `src/act.wizard.c:do_copyover` | immortal | confirmed | fixed: `fix: copyover records and saves a switched immortal's own body` |
| G1-07 | medium | lifetime | `src/act.wizard.c:do_force` | immortal | plausible | fixed in a later round: `security: 'force room' forces a UID snapshot, never a stale next_in_room` |
| G1-08 | medium | untrusted-index | `src/act.wizard.c:do_stat_object` | save-or-world-file | confirmed | fixed: `security: stat and zcheck bound attack types before indexing attack_hit_text`<br>`style: clang-tidy clean for the G1 changes and test_act_wizard.c` |
| G1-09 | medium | untrusted-index | `src/act.wizard.c:do_stat_character` | save-or-world-file | confirmed | fixed: `security: stat bounds an affect's apply location before naming it` |
| G1-10 | medium | untrusted-index | `src/act.wizard.c:do_zcheck` | save-or-world-file | confirmed | fixed: `security: zcheck bounds object apply locations before indexing zaffs`<br>`style: clang-tidy clean for the G1 changes and test_act_wizard.c` |
| G1-11 | medium | untrusted-index | `src/act.wizard.c:list_zone_commands_room` | save-or-world-file | confirmed | fixed: `security: stat room bounds zone command positions and directions` |
| G1-12 | medium | untrusted-index | `src/act.wizard.c:do_show (player) / do_last` | save-or-world-file | confirmed | fixed: `security: show player and last bound the pfile's sex and class` |
| G1-13 | medium | uninitialized | `src/act.wizard.c:do_changelog` | immortal | confirmed | fixed: `security: changelog never writes uninitialised text and restores on failure`<br>`style: clang-tidy clean for the G1 changes and test_act_wizard.c` |
| G1-14 | medium | integer | `src/act.wizard.c:do_load / do_vstat / do_stat / find_target_room / perform_set` | immortal | confirmed | fixed: `security: immortal commands reject vnums outside the IDXTYPE range` |
| G1-15 | medium | silent-failure | `src/act.wizard.c:change_player_name` | immortal | confirmed | fixed: `security: set name moves the player's files instead of orphaning them` |
| G1-16 | low | double-free-or-leak | `src/act.wizard.c:change_player_name` | immortal | confirmed | fixed: `security: set name checks the new name in the index without leaking a probe` |
| G1-17 | low | buffer-bounds | `src/act.wizard.c:change_player_name` | internal-invariant | plausible | fixed: `security: set name tests the index bound instead of reading past it`<br>(follow-up `commands` asked to confirm the fix) |
| G1-18 | medium | silent-failure | `src/act.wizard.c:mod_llog_entry / find_llog_entry / do_last` | save-or-world-file | confirmed | fixed: `security: last-login file is read and written by record index` |
| G1-19 | medium | uninitialized | `src/act.wizard.c:list_llog_entries / do_last / do_stat_character / do_show` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: immortal time displays never print an unwritten buffer`<br>fixed in a later round: `security: tb_strftime leaves an empty string when the time is unrepresentable`<br>`security: immortal time displays never print an unwritten buffer` |
| G1-20 | medium (was low) | null-deref | `src/act.wizard.c:do_zcheck` | save-or-world-file | confirmed | fixed: `security: zcheck treats NULL room, extra and mob descriptions as findings` |
| G1-21 | low | silent-failure | `src/act.wizard.c:perform_set` | immortal | confirmed | fixed: `fix: set poofin/poofout on another player below IMPL is refused, not "Okay."` |
| G1-22 | low | lifetime | `src/act.wizard.c:do_at` | immortal | confirmed | fixed: `fix: at re-resolves both rooms by vnum after the command runs` |
| G1-23 | low | integer | `src/act.wizard.c:print_zone_to_buf` | internal-invariant | plausible | fixed: `security: print_zone_to_buf clamps truncated writes instead of wrapping`<br>(follow-up `commands` asked to confirm the fix) |
| G1-24 | low | silent-failure | `src/act.wizard.c:do_show` | save-or-world-file | confirmed | fixed: `diag: show listings mark and report truncated output` |
| G1-25 | low | other | `src/act.wizard.c:do_zcheck / print_zone_to_buf / trg_checkload` | internal-invariant | confirmed | fixed: `fix: zcheck, show zone and checkload include the highest rnum` |
| G1-26 | low | silent-failure | `src/act.wizard.c:do_cheat / do_return` | player | confirmed | fixed: `diag: the idnum 1 level restore on 'return' is logged` |
| G1-27 | low | other | `src/act.wizard.c:do_zpurge` | builder-olc | confirmed | fixed: `security: zpurge * requires LVL_GOD instead of edit rights on zone 0` |
| G1-28 | low | other | `src/act.wizard.c:do_advance / perform_set` | immortal | confirmed | fixed: `security: demotion by advance or set level drops snoops it no longer allows` |
| G1-29 | low | silent-failure | `src/act.wizard.c:do_copyover` | immortal | confirmed | fixed: `diag: copyover checks its state file and does not exec on a short write` |
| G1-30 | low | error-locality | `src/act.wizard.c:do_wizutil` | internal-invariant | confirmed | fixed: `diag: an unknown wizutil subcmd is reported with both identities, no save` |
| G1-31 | low | other | `src/act.wizard.c:do_show` | immortal | confirmed | fixed: `fix: show player prints the last logon's minutes` |
| G1-V01 | medium | silent-failure | `src/act.wizard.c:do_stat` | immortal | verifier-added | fixed: `security: "stat file" never saves the stale copy or deletes crash files` |
| G2-01 | high | buffer-bounds | `src/act.informative.c:search_help` | player | confirmed | fixed: `security: search_help stays inside the help table`<br>also fixed directly by `security: help on a topic past the last keyword no longer over-reads` (already on this branch) |
| G2-02 | high | read-past-terminator | `src/act.informative.c:do_score` | builder-olc | confirmed | fixed in part on the chunk branch: `security: score checks the current quest before reading aquest_table`<br>fixed in a later round: `security: delete_quest takes every loaded player off the deleted quest`<br>`security: load_char clears a current quest that is no longer loaded`<br>`security: score checks the current quest before reading aquest_table` |
| G2-03 | high | null-deref | `src/act.informative.c:show_obj_to_char / list_obj_to_char / list_one_char / find_exdesc` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: room and object display never dereference NULL world strings`<br>fixed in a later round: `security: parse_mobile stores "undefined" for an empty mob alias list`<br>`security: world loaders store "undefined" for empty display strings`<br>`security: room and object display never dereference NULL world strings` |
| G2-04 | medium | untrusted-index | `src/act.informative.c:list_one_char` | save-or-world-file | confirmed | fixed: `security: room display range-checks a mob's position` |
| G2-05 | medium | untrusted-index | `src/act.informative.c:look_at_room` | save-or-world-file | confirmed | fixed: `security: showvnums room header range-checks the sector type` |
| G2-06 | medium | buffer-bounds | `src/act.informative.c:do_areas` | builder-olc | confirmed | fixed: `security: 'areas' builds its listing with a bounded tb_buf` |
| G2-07 | medium | undefined-behavior | `src/act.comm.c:perform_tell / do_spec_comm / do_page / do_gen_comm / do_qcomm` | dg-script | confirmed | fixed: `security: communication text can no longer inject act() $-codes` |
| G2-08 | medium | undefined-behavior | `src/act.social.c:do_action` | builder-olc | confirmed | fixed: `security: body-part socials refuse messages that read the part as an object` |
| G2-09 | medium | read-past-terminator | `src/act.comm.c:legal_communication` | player | confirmed | fixed: `security: legal_communication stops at a trailing '@'` |
| G2-10 | medium | integer | `src/act.other.c:do_steal` | player | confirmed | fixed: `security: steal computes the gold share without int overflow` |
| G2-11 | medium | silent-failure | `src/act.other.c:do_split` | player | confirmed | fixed in part on the chunk branch: `security: split refuses a share that would pass MAX_GOLD`<br>fixed in a later round: `security: split refuses a share that would pass MAX_GOLD` |
| G2-12 | medium | untrusted-index | `src/act.informative.c:do_who / do_users` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: who and users range-check a player's class`<br>fixed in a later round: `security: CLASS_ABBR range-checks a pfile class before indexing class_abbrevs`<br>`security: who and users range-check a player's class` |
| G2-13 | low | null-deref | `src/act.social.c:do_gmote / do_action` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: socials report a missing message instead of printing NULL`<br>fixed in a later round: `diag: aedit reports a saved social that lacks a message it needs`<br>`diag: boot reports socials missing a message their command needs`<br>`security: socials report a missing message instead of printing NULL` |
| G2-14 | low | silent-failure | `src/act.comm.c:do_say / perform_tell / do_gen_comm` | dg-script | confirmed | fixed: `security: history stores only what act() delivered to each listener` |
| G2-15 | low | silent-failure | `src/act.informative.c:perform_immort_where` | immortal | confirmed | fixed: `diag: immortal 'where' shows and reports a truncated listing` |
| G2-16 | low | null-deref | `src/act.informative.c:show_obj_to_char / print_object_location` | internal-invariant | plausible | fixed in a later round: `diag: a trigger-less object script is reported, not silently skipped` |
| G2-17 | low | untrusted-index | `src/act.informative.c:look_at_room` | internal-invariant | plausible | fixed in a later round: `security: look_at_room reports an unplaced viewer at its caller` |
| G2-18 | low | silent-failure | `src/act.informative.c:look_in_obj` | save-or-world-file | confirmed | fixed in part on the chunk branch: `diag: report a drink container holding more than its capacity`<br>fixed in a later round: `diag: report a drink container holding more than its capacity` |
| G2-19 | low | error-locality | `src/act.informative.c:show_obj_to_char / do_gen_ps / do_use / do_gen_tog / is_tell_ok` | internal-invariant | confirmed | fixed: `diag: player command guards report call site and entity IDs` |
| G2-20 | low | other | `src/act.informative.c:do_help` | player | plausible | fixed in a later round: `security: do_help logs an escaped excerpt of a missed topic, not raw text` |
| G2-21 | low | silent-failure | `src/act.other.c:do_endgame` | save-or-world-file | confirmed | fixed in part on the chunk branch: `diag: endgame reports a corrupt rank instead of calling it the limit`<br>fixed in a later round: `diag: endgame reports a corrupt rank instead of calling it the limit` |

### Area H: Gameplay and world mechanics

78 findings from chunks H1, H2, H3: 77 fixed; 1 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| H1-01 | high | lifetime | `src/act.item.c:get_from_room / get_from_container / do_put / do_drop / do_give / do_wear` | dg-script | confirmed | fixed: `security: item loops resume through a UID mark after each trigger` |
| H1-02 | high | lifetime | `src/act.item.c:perform_put / get_from_container` | dg-script | confirmed | fixed: `security: put and get re-validate the container after their triggers` |
| H1-03 | high | null-deref | `src/act.movement.c:do_doorcmd` | dg-script | confirmed | fixed: `security: door commands re-validate the exit or container after triggers` |
| H1-04 | high | lifetime | `src/fight.c:hit` | dg-script | confirmed | fixed: `security: hit() reads the weapon after the fight trigger` |
| H1-05 | high | lifetime | `src/fight.c:make_corpse` | dg-script | confirmed | fixed: `security: make_corpse moves the inventory before running remove triggers` |
| H1-06 | critical (was high) | lifetime | `src/fight.c:set_fighting / stop_fighting (with do_sit in act.movement.c:800)` | player | confirmed | fixed: `security: unseat characters when a fight stands them up` |
| H1-07 | high | untrusted-index | `src/act.item.c:do_drink / do_pour / name_from_drinkcon / name_to_drinkcon` | dg-script | confirmed | fixed in part on the chunk branch: `security: range-check the liquid type before the drink tables`<br>fixed in a later round: `security: create water refuses a container whose liquid has no table entry`<br>`security: range-check the liquid type before the drink tables` |
| H1-08 | high | untrusted-index | `src/fight.c:hit / dam_message / replace_string` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: validate weapon attack types before attack_hit_text`<br>fixed in a later round: `security: rent and house records get check_object's value checks`<br>`security: check_object repairs a weapon attack type outside attack_hit_text[]`<br>`security: validate weapon attack types before attack_hit_text` |
| H1-09 | high (was medium) | lifetime | `src/magic.c:mag_summons` | dg-script | confirmed | fixed: `security: animate dead re-validates the corpse after the load trigger` |
| H1-10 | high (was medium) | lifetime | `src/magic.c:mag_rooms` | builder-olc | confirmed | fixed: `security: key the darkness expiry event by room vnum` |
| H1-11 | - (claimed medium) | silent-failure | `src/magic.c:mag_rooms` | player | refuted | refuted: call_magic() returns before mag_rooms() in a NOMAGIC room, so the darkness bit is never set there; the check is dead code |
| H1-12 | medium | double-free-or-leak | `src/act.item.c:do_sac` | player | confirmed | fixed: `security: sacrificing a carried container spills it into the room` |
| H1-13 | medium | untrusted-index | `src/act.item.c:name_from_drinkcon` | save-or-world-file | confirmed | fixed: `security: drink-container names check the prototype rnum` |
| H1-14 | medium | untrusted-index | `src/act.movement.c:do_simple_move` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: range-check the room sector before movement_loss[]`<br>fixed in a later round: `security: range-check the room sector before movement_loss[]` |
| H1-15 | medium | other | `src/fight.c:damage` | player | confirmed | fixed: `security: autoloot and autosac act on the corpse just made` |
| H1-16 | low (was medium) | silent-failure | `src/fight.c:damage` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: peaceful rooms stop players even without the DG proxy mob`<br>fixed in a later round: `diag: boot reports a world without the DG caster proxy mob`<br>`security: peaceful rooms stop players even without the DG proxy mob` |
| H1-17 | medium | silent-failure | `src/act.item.c:perform_drop_gold` | dg-script | confirmed | fixed: `security: gold drops run the room drop trigger with the coins in hand` |
| H1-18 | medium | integer | `src/fight.c:hit` | dg-script | confirmed | fixed in part on the chunk branch: `security: clamp and report oversized damage dice in hit()`<br>fixed in a later round: `security: rent and house records get check_object's value checks`<br>`security: parse_simple_mob rejects numbers its byte and sh_int fields cannot hold`<br>`security: check_object clamps weapon damage dice to the oedit limit`<br>`security: clamp and report oversized damage dice in hit()` |
| H1-19 | low | lifetime | `src/magic.c:mag_areas` | dg-script | plausible | fixed in a later round: `security: area spells re-resolve each occupant instead of following next_in_room` |
| H1-20 | low | integer | `src/act.item.c:get_from_container / get_from_room / do_give / do_put` | player | confirmed | fixed: `security: get, put and give refuse counts that are not positive` |
| H1-21 | low | integer | `src/act.item.c:do_drop` | player | confirmed | fixed: `security: the junk reward goes through increase_gold()` |
| H1-22 | low | other | `src/magic.c:mag_summons` | internal-invariant | confirmed | fixed: `fix: clone and animate dead print their own summon messages` |
| H1-23 | low | other | `src/act.offensive.c:do_bandage` | player | confirmed | fixed: `fix: bandage succeeds when the roll is under the skill` |
| H1-24 | low | error-locality | `src/fight.c:set_fighting` | internal-invariant | confirmed | fixed: `diag: set_fighting reports a double start at its caller` |
| H1-25 | low | lifetime | `src/act.item.c:perform_remove` | dg-script | confirmed | fixed: `security: remove re-checks the equipment slot after its trigger` |
| H1-26 | low | error-locality | `src/act.item.c:weight_change_object / perform_drop / perform_remove / do_pour` | internal-invariant | plausible | fixed in a later round: `diag: mag_unaffects and mag_creations report the spell and the characters`<br>`diag: item and corpse guards name the object, character and room` |
| H1-27 | low | silent-failure | `src/act.item.c:do_drop` | immortal | confirmed | fixed in part on the chunk branch: `diag: donate picks only donation rooms that exist and reports bad ones`<br>fixed in a later round: `diag: boot checks the configured donation rooms against the world`<br>`diag: donate picks only donation rooms that exist and reports bad ones` |
| H2-01 | high | lifetime | `src/spell_parser.c:call_magic` | dg-script | confirmed | fixed in part on the chunk branch: `security: call_magic stops when a cast trigger purges a participant`<br>fixed in a later round: `security: call_magic stops when a cast trigger purges a participant` |
| H2-02 | high | lifetime | `src/spell_parser.c:mag_objectmagic` | dg-script | confirmed | fixed: `security: scrolls and potions are not extracted twice after a cast purge` |
| H2-03 | high | untrusted-index | `src/spell_parser.c:mag_objectmagic` | dg-script | confirmed | fixed: `security: wands and staves check value 3 before indexing spell_info` |
| H2-04 | high | buffer-bounds | `src/handler.c:fname` | builder-olc | confirmed | fixed: `security: fname bounds its copy to the static keyword buffer` |
| H2-05 | high | buffer-bounds | `src/shop.c:push` | builder-olc | confirmed | fixed: `security: shop buyword evaluation rejects expressions deeper than its stack` |
| H2-06 | high | lifetime | `src/handler.c:obj_from_room` | player | confirmed | fixed in part on the chunk branch: `security: obj_from_room clears the furniture's sitter back-pointer`<br>fixed in a later round: `security: scavenger mobs leave occupied furniture where it is`<br>`security: obj_from_room clears the furniture's sitter back-pointer` |
| H2-07 | medium | untrusted-index | `src/shop.c:list_object` | dg-script | confirmed | fixed: `security: shop list checks a drink container's liquid against drinks[]` |
| H2-08 | medium | untrusted-index | `src/shop.c:list_detailed_shop` | save-or-world-file | confirmed | fixed: `security: shop buy types outside item_types[] are dropped at load` |
| H2-09 | medium | untrusted-index | `src/spell_parser.c:do_cast` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: do_cast refuses a player class outside min_level[]`<br>fixed in a later round: `security: do_cast refuses a player class outside min_level[]` |
| H2-10 | low (was medium) | lifetime | `src/spell_parser.c:mag_objectmagic` | dg-script | plausible | fixed in a later round: `security: area spells re-resolve each occupant instead of following next_in_room`<br>`security: a staff re-resolves each occupant instead of following next_in_room` |
| H2-11 | medium | integer | `src/handler.c:affect_join` | player | confirmed | fixed: `security: affect_join clamps stacked modifiers and durations` |
| H2-12 | medium | silent-failure | `src/handler.c:equip_char` | save-or-world-file | confirmed | fixed: `security: equip_char rejections name the caller and never orphan the object` |
| H2-13 | medium | error-locality | `src/handler.c:char_from_room` | internal-invariant | confirmed | fixed: `diag: room and object primitives report the entity and the caller` |
| H2-14 | low | null-deref | `src/handler.c:obj_from_room` | internal-invariant | confirmed | fixed: `diag: obj_from_room reports a NULL object without dereferencing it` |
| H2-15 | low | read-past-terminator | `src/spells.c:spell_locate_object` | save-or-world-file | confirmed | fixed: `security: locate object handles a missing short description` |
| H2-16 | low | silent-failure | `src/spell_parser.c:do_cast` | player | confirmed | fixed: `security: locate object never searches with a stale cast keyword` |
| H2-17 | low | uninitialized | `src/shop.c:read_line` | save-or-world-file | confirmed | fixed: `security: shop read_line never logs an uninitialised buffer` |
| H2-18 | low | buffer-bounds | `src/shop.c:read_type_list` | save-or-world-file | confirmed | fixed: `security: shop type-list reader never writes before its line buffer` |
| H2-19 | medium | silent-failure | `src/shop.c:add_to_shop_list` | save-or-world-file | confirmed | fixed: `diag: shops report missing produced items, missing keepers and full lists` |
| H2-20 | medium | format-string | `src/shop.c:shopping_buy` | builder-olc | confirmed | fixed: `security: shop buy/sell room messages escape '$' in spliced text` |
| H2-21 | low | error-locality | `src/shop.c:evaluate_expression` | builder-olc | confirmed | fixed: `diag: malformed shop buywords name the shop and refuse the sale` |
| H2-22 | low | silent-failure | `src/shop.c:get_purchase_obj` | builder-olc | confirmed | fixed: `diag: report shop items destroyed for having no cost` |
| H2-23 | low | silent-failure | `src/spell_parser.c:call_magic` | save-or-world-file | confirmed | fixed: `diag: call_magic reports out-of-range spell numbers` |
| H2-24 | low | other | `src/spell_parser.c:mag_assign_spells` | internal-invariant | confirmed | fixed: `security: identify is registered once, with its mana cost` |
| H2-25 | low | sanitizer-hazard | `src/handler.c:update_object` | player | plausible | fixed in a later round: `security: object timer and owner walks loop over siblings instead of recursing`<br>`fix: object timers count down once per tick, carried and worn ones included` |
| H2-26 | low | lifetime | `src/handler.c:free_group` | internal-invariant | plausible | fixed in a later round: `security: free_group refuses a group that still has members` |
| H2-27 | low | other | `src/spells.c:spell_teleport` | builder-olc | confirmed | fixed: `security: teleport gives up when no room is eligible` |
| H2-V01 | high | lifetime | `src/spell_parser.c:mag_objectmagic` | dg-script | verifier-added | fixed: `security: a staff purged by a cast trigger is not read again` |
| H3-01 | critical | untrusted-index | `src/quest.c:autoquest_trigger_check / generic_complete_quest` | player | confirmed | fixed: `security: Find-mob quests complete once per quest, not once per matching mob` |
| H3-02 | high | other | `src/quest.c:generic_complete_quest` | builder-olc | confirmed | fixed: `security: quest rewards cannot re-enter quest completion` |
| H3-03 | high | untrusted-index | `src/quest.c:quest_list / quest_stat` | builder-olc | confirmed | fixed: `security: dangling quest prev/next links print a marker instead of reading aquest_table[NOTHING]` |
| H3-04 | medium | untrusted-index | `src/quest.c:quest_stat` | save-or-world-file | confirmed | fixed: `security: reject out-of-range quest types at load and never index quest_types[] with one` |
| H3-05 | medium | error-locality | `src/quest.c:parse_quest` | save-or-world-file | confirmed | fixed: `diag: quest file errors name the quest vnum, the failing line and the file offset`<br>`test: the quest parse test checks fclose so the default build has no clang-tidy warning` |
| H3-06 | medium | silent-failure | `src/quest.c:questmaster / assign_the_quests` | builder-olc | confirmed | fixed: `security: a questmaster keeps its original spec proc when quests are added or deleted` |
| H3-07 | low | silent-failure | `src/quest.c:generic_complete_quest` | builder-olc | confirmed | fixed: `diag: missing quest reward objects and corrupt completed-quest lists are reported` |
| H3-08 | high | lifetime | `src/limits.c:point_update` | dg-script | confirmed | fixed: `security: point_update never follows an object pointer across a timer or decay script` |
| H3-09 | medium | integer | `src/limits.c:increase_gold / increase_bank` | player | confirmed | fixed: `security: coin arithmetic cannot overflow, clamps are reported, and the bank refuses lossy transfers` |
| H3-10 | low | silent-failure | `src/limits.c:check_idling` | builder-olc | confirmed | fixed: `diag: idle players go to rooms named by vnum, and orphaned corpse contents are reported` |
| H3-11 | high | buffer-bounds | `src/asciimap.c:str_and_map / MapArea` | immortal | confirmed | fixed: `security: the automap stays inside map[][] for every minimap size` |
| H3-12 | medium | untrusted-index | `src/asciimap.c:MapArea / StringMap / WorldMap / CompactStringMap` | save-or-world-file | confirmed | fixed: `security: out-of-range room sectors never index the map glyph tables` |
| H3-13 | high | lifetime | `src/mud_event.c:event_countdown / attach_mud_event / free_mud_event` | builder-olc | confirmed | fixed: `security: room mud events name their room by vnum, not by a pointer into world[]` |
| H3-14 | medium (was high) | double-free-or-leak | `src/lists.c:find_in_list / simple_list / free_list` | player | confirmed | fixed: `security: list removal no longer ends simple_list loops; free_list frees every item` |
| H3-15 | high (was medium) | null-deref | `src/mud_event.c:free_mud_event (EVENT_ROOM/EVENT_CHAR) with free_list` | builder-olc | confirmed | fixed: `security: cancelling an owner's last mud event no longer frees the owner's list` |
| H3-16 | low (was medium) | lifetime | `src/lists.c:simple_list / char_has_mud_event` | internal-invariant | plausible | fixed: `security: list removal no longer ends simple_list loops; free_list frees every item`<br>(follow-up `core-utils` asked to confirm the fix) |
| H3-17 | medium | lifetime | `src/mobact.c:mobile_activity` | dg-script | confirmed | fixed: `security: a mob purged by a script mid-turn stops acting for the rest of the pulse` |
| H3-18 | medium | untrusted-index | `src/spec_procs.c:list_skills / guild (also limits.c set_title, gain_exp)` | save-or-world-file | confirmed | fixed: `security: practice refuses a pfile class that cannot index the practice tables` |
| H3-19 | low | format-string | `src/spec_assign.c:get_spec_func_name` | immortal | confirmed | fixed: `diag: get_spec_func_name never returns NULL and names the King's Castle procs` |
| H3-20 | low | silent-failure | `src/spec_procs.c:list_skills` | internal-invariant | confirmed | fixed: `diag: a truncated practice list is marked and reported` |
| H3-21 | low | silent-failure | `src/spec_procs.c:pet_shops` | builder-olc | confirmed | fixed: `security: pet shops find their storage room by vnum and refuse a missing one` |
| H3-22 | low | error-locality | `src/lists.c:remove_from_list / merge_iterator / next_in_list / simple_list / free_list` | internal-invariant | confirmed | fixed: `diag: list invariant violations name the calling file, line and function` |
| H3-V01 | high | untrusted-index | `src/genqst.c:169` | builder-olc | verifier-added | fixed: `security: delete_quest restores the questmaster spec proc by mob rnum, not vnum` |

### Area I: Foundations (utilities, macros, core headers) and standalone tools

59 findings from chunks I1, I2, I3: 58 fixed; 1 refuted.

| ID | Severity | Category | File:function | Input controller | Verdict | Resolution |
| --- | --- | --- | --- | --- | --- | --- |
| I1-01 | high | buffer-bounds | `src/utils.c:column_list` | builder-olc | confirmed | fixed in part on the chunk branch: `security: column_list stays inside its output buffer`<br>fixed in a later round: `security: aedit refuses social names longer than 30 characters`<br>`security: column_list stays inside its output buffer` |
| I1-02 | high | lifetime | `src/utils.c:stop_follower / die_follower` | dg-script | confirmed | fixed: `security: unlink a follower before act() can run scripts` |
| I1-03 | - (claimed high) | lifetime | `src/genshp.c:remove_shop_from_type_list` | builder-olc | refuted | refuted: the duplicate free/assign pair is not in this tree (the reviewer read another checkout); only a silent out-of-range return remains, listed in the gen-olc follow-up |
| I1-04 | medium | silent-failure | `src/utils.c:get_line` | save-or-world-file | confirmed | fixed in part on the chunk branch: `security: get_line reports and drops the tail of an overlong line`<br>fixed in a later round: `security: get_line reports and drops the tail of an overlong line` |
| I1-05 | low (was medium) | read-past-terminator | `src/utils.c:count_non_protocol_chars` | builder-olc | confirmed | fixed: `security: count_non_protocol_chars stops at the end of the string` |
| I1-06 | low | read-past-terminator | `src/utils.c:count_color_chars` | save-or-world-file | confirmed | fixed: `security: count_color_chars stops at a lone trailing tab` |
| I1-07 | medium (was low) | uninitialized | `src/utils.c:strfrmt` | builder-olc | confirmed | fixed in part on the chunk branch: `security: strfrmt formats NULL text as empty and never returns stale output`<br>fixed in a later round: `diag: the automap names a room whose description is NULL`<br>`security: strfrmt formats NULL text as empty and never returns stale output` |
| I1-08 | low | error-locality | `src/utils.h:CHECK_PLAYER_SPECIAL` | internal-invariant | confirmed | fixed: `diag: CHECK_PLAYER_SPECIAL names the mob and the calling function` |
| I1-09 | low | error-locality | `src/utils.c:room_is_dark` | internal-invariant | confirmed | fixed: `diag: room_is_dark reports an invalid rnum with its caller` |
| I1-10 | low | error-locality | `src/utils.c:rand_number` | builder-olc | confirmed | fixed in part on the chunk branch: `diag: rand_number reports inverted bounds at the calling line`<br>fixed in a later round: `diag: rand_number reports inverted bounds at the calling line` |
| I1-11 | low | null-deref | `src/utils.c:stop_follower` | internal-invariant | confirmed | fixed: `diag: name both characters when the follower list is inconsistent` |
| I1-12 | low | silent-failure | `src/utils.c:mudlog / get_filename` | internal-invariant | confirmed | fixed: `diag: mudlog and get_filename report what they drop` |
| I1-13 | low | untrusted-index | `src/utils.c:get_filename` | immortal | confirmed | fixed: `security: get_filename rejects names that are not plain path components` |
| I1-14 | low | silent-failure | `src/utils.c:sprintbitarray / strpaste / strfrmt_append / tb_buf_*` | builder-olc | plausible | fixed in a later round: `diag: the bounded string builders report what they cut` |
| I1-15 | low | undefined-behavior | `src/utils.c:CAP / strfrmt (isspace_ignoretabs)` | builder-olc | confirmed | fixed: `security: pass unsigned char values to ctype in CAP and strfrmt` |
| I2-01 | high | buffer-bounds | `src/util/autowiz.c:get_line` | internal-invariant | confirmed | fixed: `security: autowiz reads whole index lines and reports rejected records`<br>also fixed directly by `security: autowiz reads whole index lines, not their first 7 bytes` (already on this branch) |
| I2-02 | high | buffer-bounds | `src/util/plrtoascii.c:plr_filename` | save-or-world-file | confirmed | fixed: `security: plrtoascii validates the raw name field before using it as a path`<br>`style: FIXED_STR_ARGS for plrtoascii's fixed-width text fields` |
| I2-03 | high | uninitialized | `src/util/plrtoascii.c:convert` | save-or-world-file | confirmed | fixed: `security: plrtoascii writes index records in the server's format` |
| I2-04 | medium (was high) | buffer-bounds | `src/util/rebuildMailIndex.c:parse_mail_flags` | save-or-world-file | confirmed | fixed: `security: rebuildMailIndex bounds the Flag line words and reports bad ones`<br>`style: TB_SCAN_STR ties scanf field widths to named buffer lengths` |
| I2-05 | high | silent-failure | `src/util/rebuildAsciiIndex.c:walkdir` | save-or-world-file | confirmed | fixed: `security: rebuildIndex writes the 5-field index the server parses, unclamped` |
| I2-06 | medium | untrusted-index | `src/util/rebuildAsciiIndex.c:findLine` | player | confirmed | fixed: `security: rebuildIndex reads pfile tags only at record level` |
| I2-07 | medium | null-deref | `src/util/autowiz.c:add_name` | save-or-world-file | confirmed | fixed: `security: autowiz range-checks index levels before filing names` |
| I2-08 | low (was medium) | undefined-behavior | `src/util/autowiz.c:asciiflag_conv` | save-or-world-file | confirmed | fixed: `security: ascii flag helpers in the util tools use unsigned, width-checked shifts`<br>`security: rebuildMailIndex closes each directory and does not follow symlinks` |
| I2-09 | low | buffer-bounds | `src/util/autowiz.c:get_line` | save-or-world-file | confirmed | fixed: `security: autowiz strips only real line endings and reports over-long lines` |
| I2-10 | medium | error-locality | `src/util/shopconv.c:do_int` | save-or-world-file | confirmed | fixed: `security: shopconv restores the original file when a shop field is malformed` |
| I2-11 | low | silent-failure | `src/util/shopconv.c:do_float` | save-or-world-file | confirmed | fixed: `security: shopconv rejects non-finite or non-positive ratios, no silent truncation` |
| I2-12 | low (was medium) | untrusted-index | `src/constants.c:connected_types[]` | internal-invariant | confirmed | fixed: `security: connected_types names CON_IEDIT; table length checked at compile time`<br>`style: every constants.c name table with a structs.h count is length-checked at build time` |
| I2-13 | low | silent-failure | `src/constants.c:preference_bits[]` | internal-invariant | confirmed | fixed: `diag: preference_bits names PRF_VERBOSE; length checked at compile time`<br>`style: every constants.c name table with a structs.h count is length-checked at build time` |
| I2-14 | medium | error-locality | `src/sysdep.h:tb_replace_file_checked_at` | internal-invariant | confirmed | fixed: `security: tb_replace_file_checked never deletes <dest>.bak and names every path` |
| I2-15 | low (was medium) | integer | `src/zmalloc.h:calloc/malloc macros` | internal-invariant | confirmed | fixed: `security: zmalloc sizes are size_t; calloc's multiplication is checked` |
| I2-16 | medium | sanitizer-hazard | `src/zmalloc.c:zfree` | internal-invariant | confirmed | fixed: `security: zmalloc and the sanitizers no longer silently blind each other` |
| I2-17 | medium | silent-failure | `src/zmalloc.c:zlogf` | internal-invariant | confirmed | fixed: `diag: zmalloc reports heap misuse on stderr and aborts`<br>`diag: a failed zrealloc is reported, not only written to zmalloc.log` |
| I2-18 | low | silent-failure | `src/zmalloc.c:zrealloc` | internal-invariant | confirmed | fixed: `diag: zrealloc of an untracked pointer is fatal instead of a fresh block` |
| I2-19 | low | silent-failure | `src/bsd-snprintf.c:vsnprintf` | internal-invariant | confirmed | fixed: `security: refuse to build with the non-C99 bsd-snprintf fallback` |
| I2-20 | low | integer | `src/random.c:circle_srandom` | internal-invariant | confirmed | fixed: `diag: circle_srandom reduces the seed into the generator's domain` |
| I2-21 | low | silent-failure | `src/util/asciipasswd.c:main` | immortal | confirmed | fixed: `diag: asciipasswd validates the name and checks crypt's result` |
| I2-22 | low | integer | `src/util/sign.c:main` | immortal | confirmed | fixed: `diag: sign requires a whole port number in 1024..65535` |
| I2-23 | low | other | `src/util/sign.c:reap` | remote-unauthenticated | confirmed | fixed: `diag: sign's SIGCHLD handler preserves errno` |
| I2-24 | medium | silent-failure | `src/util/plrtoascii.c:convert` | save-or-world-file | confirmed | fixed: `security: plrtoascii refuses a playerfile that is not whole records`<br>`security: plrtoascii writes index records in the server's format` |
| I2-25 | low | double-free-or-leak | `src/util/rebuildMailIndex.c:walkdir` | save-or-world-file | confirmed | fixed: `security: rebuildMailIndex closes each directory and does not follow symlinks` |
| I2-26 | low | silent-failure | `src/sysdep.h:tb_parse_long_or_zero` | dg-script | confirmed | fixed in part on the chunk branch: `diag: atoi-style parsers report overflow; autowiz arguments are checked`<br>fixed in a later round: `diag: atoi-style parsers report overflow; autowiz arguments are checked` |
| I2-27 | low | other | `src/sysdep.h:struct in_addr fallback` | internal-invariant | confirmed | fixed: `security: sysdep.h struct in_addr fallback parses and is 32 bits` |
| I2-V01 | medium | sanitizer-hazard | `src/zmalloc.c:zmalloc` | internal-invariant | verifier-added | fixed: `security: zmalloc blocks keep malloc's alignment` |
| I2-V02 | high | silent-failure | `src/util/rebuildAsciiIndex.c:walkdir` | save-or-world-file | verifier-added | fixed: `security: rebuildIndex writes the 5-field index the server parses, unclamped` |
| I3-01 | high | buffer-bounds | `src/util/wld2html.c:461` | save-or-world-file | confirmed | fixed: `security: wld2html rejects door directions outside dir_option[]` |
| I3-02 | high | buffer-bounds | `src/util/wld2html.c:545` | save-or-world-file | confirmed | fixed: `security: wld2html fread_string reads whole lines, no tmp[] overrun`<br>`style: wld2html token parsers use table-free character tests` |
| I3-03 | high | buffer-bounds | `src/util/wld2html.c:423` | save-or-world-file | confirmed | fixed: `security: wld2html bounds parse_room's record cursor by rec_count`<br>`style: wld2html token parsers use table-free character tests` |
| I3-04 | high | sanitizer-hazard | `src/util/wld2html.c:566` | save-or-world-file | confirmed | fixed: `security: wld2html fread_string copies exactly its allocation size` |
| I3-05 | medium (was high) | null-deref | `src/util/split.c:36` | save-or-world-file | confirmed | fixed: `security: split checks the '=' marker line for its newline` |
| I3-06 | medium | silent-failure | `src/util/wld2html.c:379` | save-or-world-file | confirmed | fixed: `diag: wld2html converts vnums >= 100000 and reports the #99999 sentinel` |
| I3-07 | medium | silent-failure | `src/util/wld2html.c:587` | save-or-world-file | confirmed | fixed: `security: wld2html get_line keeps whole lines and the final line` |
| I3-08 | medium | undefined-behavior | `src/util/wld2html.c:401` | save-or-world-file | confirmed | fixed: `security: wld2html room flag letters cannot shift past 32 bits` |
| I3-09 | low | double-free-or-leak | `src/util/wld2html.c:492` | save-or-world-file | confirmed | fixed: `security: wld2html rejects a direction defined twice in one room` |
| I3-10 | medium | error-locality | `src/util/wld2html.c:272` | save-or-world-file | confirmed | fixed: `security: wld2html never prints a NULL room name or description` |
| I3-11 | medium | silent-failure | `src/util/split.c:43` | save-or-world-file | confirmed | fixed: `diag: split exits non-zero on output failure and reports orphan lines` |
| I3-12 | low | error-locality | `src/util/wld2html.c:361` | save-or-world-file | confirmed | fixed: `diag: wld2html parse errors name the input file, line and room vnum` |
| I3-13 | low | other | `src/util/wld2html.c:47` | internal-invariant | confirmed | fixed: `security: wld2html builds as C23; drop dead Diku typedefs and structs` |
| I3-14 | low | other | `src/util/wld2html.c:269` | builder-olc | confirmed | fixed: `security: wld2html HTML-escapes builder text in generated pages` |
| I3-V01 | high | buffer-bounds | `src/util/wld2html.c:566` | save-or-world-file | verifier-added | fixed: `security: wld2html fread_string reads whole lines, no tmp[] overrun`<br>`security: wld2html fread_string copies exactly its allocation size` |

<!-- END GENERATED: ledger-findings -->

<!-- BEGIN GENERATED: ledger-outside -->

### Found outside the chunk review

Fixed directly by the coordinator. A related chunk finding means a chunk reviewer reported the same bug and its fix agent fixed it on the chunk branch too; integration keeps one of the two.

| Bug | Found by | Who or what triggers it | Fix (commit subject) | Where the fix is | Chunk finding | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Text files loaded as their first 7 bytes: no login prompt for new players | new players could not log in; the B1 reviewer had also reported it (B1-01) | every new connection | `security: text files load whole again, not as their first 7 bytes` | this branch | B1-01 | adds the WarnOnSizeOfPointer clang-tidy error and tests/test_login_greeting.py; backported as `security: new players can log in again (login hotfix backport)` to 1 local branches (master: unknown) |
| autowiz read player-index lines as their first 7 bytes | the sizeof-of-pointer sweep; the I2 reviewer had also reported it (I2-01) | every autowiz run (empty wizlist and immlist) | `security: autowiz reads whole index lines, not their first 7 bytes` | this branch | I2-01 | - |
| New player-index slots written to disk uninitialised; a reused slot leaked its name | Valgrind on a live server | every new character | `security: new player-index slots start zeroed, not as realloc garbage` | this branch | - | - |
| Host string leaked once per new character | Valgrind on a live server | every new character | `security: creating a character no longer leaks its host string` | this branch | - | - |
| search_help() read one entry past the help table ('help ~~~~') | the cloud review; the G2 reviewer had also reported it (G2-01) | any player | `security: help on a topic past the last keyword no longer over-reads` | this branch | G2-01 | adds tests/test_help_search.py |
| BOOT-UAF: world teardown left world[] dangling for the DG debugging scans | circle_boot_check (circle -c) under ASan and Valgrind | every clean shutdown and every circle -c | `security: world teardown leaves no dangling world pointer (BOOT-UAF)`<br>`fix: shutdown no longer reads the freed world (regression from 4f33b6c)` | audit-fix-infra-findings; this branch has `security: world teardown leaves no dangling world pointer (BOOT-UAF)` | D2-05 | re-enables circle_boot_check |
| FUZZ-1: ProtocolOutput() read one byte past text ending in a tab | fuzz_protocol (libFuzzer); the same loop as A1-06 | any output that ends in a tab | `security: protocol output never reads past a trailing tab; MSSP callbacks typed` | this branch | A1-06 | - |
| FUZZ-2: MSSP callbacks called through a mismatched function type | fuzz_protocol (libFuzzer) | any MSSP request, no login needed | `security: protocol output never reads past a trailing tab; MSSP callbacks typed` | this branch | - | - |
| vwrite_to_output() wrote a size_t length through an int pointer | the sanitizer-infrastructure review (also A1-10) | every line of output | `security: protocol output length is written through a real int` | this branch | A1-10 | - |
| Parallel builds overwrote one another's bin/circle | the coordinator, before the fix agents started | tooling | `build: give each CMake build tree its own bin directory` | this branch | - | - |
| No SAST in CI | the coordinator | tooling | `ci: run Semgrep SAST and upload to the Semgrep AppSec Platform` | this branch | - | - |

### New DG findings from the documentation pass

Reported by the dg-scripts documentation agent after the fix pass and fixed by the dg and gen-olc follow-up rounds. They have no chunk ID; the DGDOC labels are this ledger's.

| Label | Finding (as reported) | Severity | Fix owner | Status |
| --- | --- | --- | --- | --- |
| DGDOC-1 | dg_variables.c:find_replacement(), `%x.level(N)%` setter: no target check — any builder script can lower an immortal's level (privilege change by builder input). | high | dg follow-up | fixed: `security: a DG level setter never changes an immortal's level` |
| DGDOC-2 | dg_scripts.c:do_attach(): for mobs/objects checks the zone of the builder's own room, not the target's zone (detach checks the target). Builder authority bypass. | not rated | dg follow-up | fixed: `security: attach and detach check the zone of the trigger's target` |
| DGDOC-3 | dg_scripts.c:do_vdelete(): any builder can delete any player's saved script variables by UID; no zone/level check. | not rated | dg follow-up | fixed: `security: vdelete checks the builder's zone and rank before deleting` |
| DGDOC-4 | script_driver() owner check: misses a player owner being extracted; a script can run for a mob already pending extraction (wait expiring in the same pulse the mob died). Lifetime. | not rated | dg follow-up | fixed: `security: a mob script never runs for a character pending extraction` |
| DGDOC-5 | dg_triggers.c:damage_mtrigger(): the script's return value becomes the damage and the default is 1, so a damage trigger without `return %damage%` reduces every hit to 1. | not rated | dg follow-up | fixed: `fix: a mob damage trigger without 'return' keeps the damage` |
| DGDOC-6 | script_driver(): control keywords matched by prefix (`else` matches `elsewhere`, `end` matches `endure`); `while` counters live in the command list shared by all instances of a trigger. | not rated | dg follow-up | fixed: `fix: DG control keywords match whole words, the same way in every scanner`<br>`fix: the DG while-loop pause counts each trigger instance's own passes` |
| DGDOC-7 | medit_save_internally()/oedit_save_internally() (owner: gen-olc agent): every save rebuilds the scripts of all live copies, wiping their variables and pending waits even when the trigger list did not change. | not rated | gen-olc agent | fixed: `fix: oedit and medit keep live scripts when the trigger list is unchanged` |
| DGDOC-8 | dg_comm.c:sub_write(): visibility-dependent object lookup is inverted (dead code today; owner: errloc or later round). | not rated | errloc or later round | fixed: `fix: sub_write's visible-object lookup tries room, equipment, inventory` |
| DGDOC-9 | Lookup table: an object ID equal to the bucket sentinel (1,000,000,000) could never be found (theoretical). | not rated | dg follow-up | fixed: `fix: an object UID equal to the lookup sentinel is stored where lookups look` |

Merge note from the same file: cfa23ca/eed9ae3 (C2) keep missing-trigger T lines; dg_olc.c:dg_script_menu() then needs -28's checked lookup merged too, or it reads past the trigger index.

### Follow-up groups

Findings whose fix needs files outside the reporting chunk, the plausible low and medium findings no fix agent was asked to fix, and the error-locality findings, regrouped by the files they touch. A finding can be in two groups when each owns half of the fix.

| Group | Branch | Based on | Files owned | Findings | Still partly open | Not started | Fixed | Refuted |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `commands` | - | `audit-fix-G1` | 5 | 8 | 0 | 0 | 8 | 0 |
| `core-utils` | - | `audit-fix-I2` | 11 | 13 | 0 | 0 | 13 | 0 |
| `db-loaders` | - | `worktree-wf_c613c2c5-f9f-30` | 2 | 8 | 0 | 0 | 8 | 0 |
| `dg` | - | `worktree-wf_c613c2c5-f9f-33` | 10 | 12 | 0 | 0 | 12 | 0 |
| `errloc` | - | `worktree-wf_c613c2c5-f9f-28` | 7 | 9 | 0 | 0 | 9 | 0 |
| `gen-olc` | - | `worktree-wf_c613c2c5-f9f-32` | 11 | 15 | 0 | 0 | 14 | 1 |
| `player-load` | - | `worktree-wf_c613c2c5-f9f-31` | 3 | 7 | 0 | 0 | 7 | 0 |

<!-- END GENERATED: ledger-outside -->
