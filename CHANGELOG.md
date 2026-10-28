# Changelog (IronCircle fork)

Fork-level changes. The historic tbaMUD log is `changelog` (frozen 2013).
Per-finding detail lives in [docs/memory-safety-audit.md](docs/memory-safety-audit.md)
(the ledger) and [docs/audit-2026-10-report.md](docs/audit-2026-10-report.md)
(the summary); this file does not repeat it.

## 2026-10-28: pull requests merged after the audit

Each was merged only after the full gate: gcc ASan+UBSan ctest, Valgrind,
the clang fuzz targets, clang-tidy with no findings,
tools/check_error_locality.sh, the end-to-end soaks with an ASan server
where the change touches gameplay or OLC, and Fil-C.

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
<!-- PLACEHOLDER-MERGED: add one entry per PR merged after this file was
     written (#41 movement index checks, #40 password verify / persistence
     style, #50 level-50 rebalance tool, #36 seven-class guilds, #18 area
     imports, and any fix/leftover-* branches), then delete this comment. -->

## 2026-10-28: memory-safety and error-locality audit merged

- Phase 1 (2c180c9b) and phase 2 (3d3a06ef) merged the full audit: every
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
- Follow-ups merged: #43 to #48 (DG memory unlink, OLC flag shift, DG wait
  suffix, crash-save headers, macro inventory).

## Open proposals (not merged)
<!-- PLACEHOLDER-OPEN: list the PRs still open when this branch merges, or
     write "None." -->
See the [open pull requests](https://github.com/kythrasuntamer/TBAMud_Study/pulls)
for their status.
