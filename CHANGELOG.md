# Changelog (IronCircle fork)

Fork-level changes. The historic tbaMUD log is `changelog` (frozen 2013).
Per-finding detail lives in [docs/memory-safety-audit.md](docs/memory-safety-audit.md)
(the ledger) and [docs/audit-2026-10-report.md](docs/audit-2026-10-report.md)
(the summary); this file does not repeat it.

## 2026-10-28: pull requests merged after the audit

Each was merged only after the full gate: gcc ASan+UBSan ctest, Valgrind,
the clang fuzz targets, clang-tidy with no findings,
tools/check_error_locality.sh, the end-to-end soaks with an ASan server
where the change touches gameplay or OLC, and Fil-C. (#49 was merged on the
owner's instruction before its last gate steps finished; they all passed
afterwards, see the PR.)

- #49 zedit: a zone header with no builders string saves safely; a header
  with no name is refused before any live change, and every refusal is
  reported with the builder, zone and room.
- #38 save_char: each step of the staged player save (write, flush, close,
  rename, cleanup) is reported where it fails and names the player; a
  failed save never replaces the previous pfile; native fault-injection
  tests (test_players_save_faults).
- #37 diagnostics: parser-context exhaustion is reported at the caller and
  rate-limited; index_boot() records the file, line and parser of each
  load; one truncation marker, `...[TRUNCATED]`.
- #41 movement: do_simple_move() checks the room and zone indices before
  any world[]/zone_table[] access and again after every special and leave
  trigger; a bad index refuses the move and is reported at the stage that
  found it, with the character, direction and both rooms (vnum and rnum).
- #50 tools/rebalance_world_50.py: stages a level-50 rebalance of the
  mobile files with a per-mobile report and never edits the live world;
  levels 11 and up are stretched monotonically onto 11-50, and immortal
  levels are rejected with file:line.
- #51 leftover ledger fixes: editor range errors end with a real line
  break; ban sites are lowercased on load so uppercase sites match; the
  rent-header contract checks every Crash_* reader and runs in ctest;
  tools/audit_report_stats.py renders the same report on every clone.
- #36 seven classes: Druid, Berserker, Sorcerer, Paladin, Ranger, Bard and
  Necromancer join Wizard, Cleric, Thief, Warrior and Monk (12 classes;
  saved IDs 0-4 unchanged). Each new class uses its archetype's THAC0,
  saving throw and experience tables and its anti-class item and shop
  rules; Monk keeps its exemption from those item and shop rules.
  Paladin, Ranger and Bard gain half-caster mana; new classes have titles
  ("the Druid"). Eight new Midgaard training halls, reached through guarded
  passages that admit only the matching classes. The guild_guard spec proc
  never blocked anyone (its direction loop overran and it read the wrong
  command table); it is fixed, but no stock mob carried it: the four
  original guilds have always been guarded by DG triggers 3000-3003, and
  nothing changes there. A Wizard's mana discount stops at the spell's
  minimum cost; `show thaco` and `show exp` name which newer classes use
  each column. Every TBERR record now ends with `truncated_fields=` (empty
  unless a field was cut): update external log parsers. Back up the player
  files before deploying: a binary from before #36 loads class 5-11
  characters as the default class.
<!-- PLACEHOLDER-MERGED: add an entry for each PR merged after this one
     (#40 password verify / persistence style, #18 area imports, and the
     string_add fix if it merges), then delete this comment. -->

## 2026-10-28: memory-safety and error-locality audit merged

- Phase 1 (up to 2c180c9b) and phase 2 (3d3a06ef) merged the full audit: every
  finding fixed or recorded in the ledger; no finding open.
- Structured diagnostics (TB_ERROR_*/TB_WARN_*/DIAG_*) with call-site and
  entity IDs; tools/check_error_locality.sh ratchets legacy SYSERR logging.
- Build/test: ASan+UBSan options shared with CI, ENABLE_HARDENING,
  Valgrind memcheck, Fil-C CI, Semgrep and clang-tidy (sizeof-pointer as
  error), compile-time failure injection (TB_TEST_FAILURE_INJECTION),
  libFuzzer targets fuzz_protocol, fuzz_player and fuzz_world with
  regression corpora in ctest.
- Persistence: staged player/index/zone-index writes; untrusted pfile
  fields range-checked or clamped and reported; endgame ranks clamped.
- Login: Argon2id (libsodium), with legacy crypt hashes upgraded on login.
- Follow-ups merged: #43 DG memory unlink before callbacks, #44 genolc
  sprintascii unsigned shift, #45 DG wait seconds suffix, #46 crash-save
  header validation, #48 Claude GitHub Actions workflows; the read-only
  source macro inventory tool was merged directly (319caeb).

## Open proposals (not merged)
<!-- PLACEHOLDER-OPEN: list the PRs still open when this branch merges, or
     write "None." -->
See the [open pull requests](https://github.com/kythrasuntamer/TBAMud_Study/pulls)
for their status.
