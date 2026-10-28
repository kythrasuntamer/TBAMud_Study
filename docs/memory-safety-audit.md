# Memory-safety audit ledger

Full, systematic review of every source file (135 files, about 93,000 lines),
started 27 October 2026 on `audit/macro-inventory-and-dg-followup-20261027`.
This ledger is the coverage record. A file is only marked reviewed when its
whole contents have been read against the checklist below, and every
finding is listed with its resolution.

## Method

1. **Mechanical sweep** of every `.c` file: GCC 15 `-fanalyzer` and
   clang-tidy security checks, plus a scan for risky APIs.
2. **Line-by-line review** of each area by an independent reviewer, against
   this checklist:
   - buffer bounds;
   - integer overflow and signedness;
   - reads past a terminator;
   - indexing by input-, file- or script-derived values;
   - pointer lifetime (extraction, callbacks, descriptor close);
   - NULL dereference;
   - double free and leaks on error paths;
   - format strings;
   - uninitialized reads;
   - silent failures.
   Each finding records who controls the triggering input.
3. **Verification:** every reported finding is checked against the code
   before it is fixed. Findings that don't hold up are recorded as rejected,
   with the reason.
4. **Fixes:** each confirmed finding gets a fix, a sanitizer regression
   test where practical, a structured diagnostic (see
   `docs/maintainer-safety-guide.md`), explanatory comments and its own
   commit.
5. **Fuzzing** of the telnet/protocol parser and the save-file loaders,
   once Clang/libFuzzer or AFL++ is installed.

Status values: `in review`, `reviewed`, `fixes in progress`, `done`.

## Areas and coverage

### Area A: Untrusted network input

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/comm.c` | in review | — |
| `src/comm.h` | in review | — |
| `src/protocol.c` | in review | — |
| `src/protocol.h` | in review | — |
| `src/telnet.h` | in review | — |
| `src/interpreter.c` | in review | — |
| `src/interpreter.h` | in review | — |
| `src/ban.c` | in review | — |
| `src/ban.h` | in review | — |
| `src/modify.c` | in review | — |
| `src/modify.h` | in review | — |
| `src/improved-edit.c` | in review | — |
| `src/improved-edit.h` | in review | — |

### Area B: Player and world persistence

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/db.c` | in review | — |
| `src/db.h` | in review | — |
| `src/players.c` | in review | — |
| `src/pfdefaults.h` | in review | — |
| `src/objsave.c` | in review | — |
| `src/house.c` | in review | — |
| `src/house.h` | in review | — |
| `src/mail.c` | in review | — |
| `src/mail.h` | in review | — |

### Area C: Boards, bug reports, class import, OLC save writers, trigger files, config

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/boards.c` | in review | — |
| `src/boards.h` | in review | — |
| `src/ibt.c` | in review | — |
| `src/ibt.h` | in review | — |
| `src/class.c` | in review | — |
| `src/class.h` | in review | — |
| `src/genolc.c` | in review | — |
| `src/genolc.h` | in review | — |
| `src/genmob.c` | in review | — |
| `src/genmob.h` | in review | — |
| `src/genobj.c` | in review | — |
| `src/genobj.h` | in review | — |
| `src/genwld.c` | in review | — |
| `src/genwld.h` | in review | — |
| `src/genzon.c` | in review | — |
| `src/genzon.h` | in review | — |
| `src/genshp.c` | in review | — |
| `src/genshp.h` | in review | — |
| `src/genqst.c` | in review | — |
| `src/dg_db_scripts.c` | in review | — |
| `src/config.c` | in review | — |
| `src/config.h` | in review | — |

### Area D: DG script engine core

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/dg_scripts.c` | in review | — |
| `src/dg_scripts.h` | in review | — |
| `src/dg_variables.c` | in review | — |
| `src/dg_misc.c` | in review | — |
| `src/dg_handler.c` | in review | — |
| `src/dg_event.c` | in review | — |
| `src/dg_event.h` | in review | — |

### Area E: DG script commands, triggers and trigger OLC

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/dg_mobcmd.c` | in review | — |
| `src/dg_objcmd.c` | in review | — |
| `src/dg_wldcmd.c` | in review | — |
| `src/dg_comm.c` | in review | — |
| `src/dg_triggers.c` | in review | — |
| `src/dg_olc.c` | in review | — |
| `src/dg_olc.h` | in review | — |

### Area F: OLC editors

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/oedit.c` | in review | — |
| `src/medit.c` | in review | — |
| `src/redit.c` | in review | — |
| `src/zedit.c` | in review | — |
| `src/sedit.c` | in review | — |
| `src/cedit.c` | in review | — |
| `src/aedit.c` | in review | — |
| `src/hedit.c` | in review | — |
| `src/hedit.h` | in review | — |
| `src/qedit.c` | in review | — |
| `src/tedit.c` | in review | — |
| `src/prefedit.c` | in review | — |
| `src/prefedit.h` | in review | — |
| `src/msgedit.c` | in review | — |
| `src/msgedit.h` | in review | — |
| `src/oasis.c` | in review | — |
| `src/oasis.h` | in review | — |
| `src/oasis_copy.c` | in review | — |
| `src/oasis_delete.c` | in review | — |
| `src/oasis_list.c` | in review | — |

### Area G: Player and immortal commands

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/act.h` | in review | — |
| `src/act.wizard.c` | in review | — |
| `src/act.informative.c` | in review | — |
| `src/act.comm.c` | in review | — |
| `src/act.social.c` | in review | — |
| `src/act.other.c` | in review | — |

### Area H: Gameplay and world mechanics

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/act.item.c` | in review | — |
| `src/act.movement.c` | in review | — |
| `src/act.offensive.c` | in review | — |
| `src/fight.c` | in review | — |
| `src/fight.h` | in review | — |
| `src/magic.c` | in review | — |
| `src/spell_parser.c` | in review | — |
| `src/spells.c` | in review | — |
| `src/spells.h` | in review | — |
| `src/handler.c` | in review | — |
| `src/handler.h` | in review | — |
| `src/shop.c` | in review | — |
| `src/shop.h` | in review | — |
| `src/quest.c` | in review | — |
| `src/quest.h` | in review | — |
| `src/castle.c` | in review | — |
| `src/spec_assign.c` | in review | — |
| `src/spec_procs.c` | in review | — |
| `src/spec_procs.h` | in review | — |
| `src/mobact.c` | in review | — |
| `src/limits.c` | in review | — |
| `src/weather.c` | in review | — |
| `src/graph.c` | in review | — |
| `src/graph.h` | in review | — |
| `src/asciimap.c` | in review | — |
| `src/asciimap.h` | in review | — |
| `src/mud_event.c` | in review | — |
| `src/mud_event.h` | in review | — |
| `src/lists.c` | in review | — |
| `src/lists.h` | in review | — |

### Area I: Foundations (utilities, macros, core headers) and standalone tools

Status: in review.

| File | Status | Findings |
| --- | --- | --- |
| `src/utils.c` | in review | — |
| `src/utils.h` | in review | — |
| `src/structs.h` | in review | — |
| `src/sysdep.h` | in review | — |
| `src/screen.h` | in review | — |
| `src/constants.c` | in review | — |
| `src/constants.h` | in review | — |
| `src/random.c` | in review | — |
| `src/bsd-snprintf.c` | in review | — |
| `src/bsd-snprintf.h` | in review | — |
| `src/zmalloc.c` | in review | — |
| `src/zmalloc.h` | in review | — |
| `src/util/asciipasswd.c` | in review | — |
| `src/util/autowiz.c` | in review | — |
| `src/util/plrtoascii.c` | in review | — |
| `src/util/rebuildAsciiIndex.c` | in review | — |
| `src/util/rebuildMailIndex.c` | in review | — |
| `src/util/shopconv.c` | in review | — |
| `src/util/sign.c` | in review | — |
| `src/util/split.c` | in review | — |
| `src/util/wld2html.c` | in review | — |

## Findings

Findings are added per area as they are verified (ID, severity, file:line,
input controller, resolution and commit).
