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
- #40 login and player-file style: tb_password_verify() tells a match, a
  mismatch and a password this build cannot check apart (a legacy crypt
  hash on a build without crypt(), or libsodium failing to start). An
  uncheckable password is reported as `password-verify` with the
  character's name and idnum, at most 5 records per account and 20 overall
  per minute, followed by one `password-verify-suppressed` record; it
  counts against the account's login rate limit but never as a bad
  password. Every prompt says the password cannot be checked: the new
  character and change-password confirmations close the connection (an
  unconfirmed new hash is held in memory and must not be saved), and the
  old-password and delete-confirmation prompts return to the menu. On a
  build without crypt(), a legacy hash is checked against a dummy Argon2id
  hash so the response time does not reveal which accounts still hold
  legacy hashes. players.c's checked parsers use the classic CircleMUD
  `return (x);` style.
- #18 area imports: tools to convert, validate and install areas from other
  MUD code bases (validate_import_refs, audit_world_vnums, convert_crimson2,
  and fetch_mud_assets, which requires a pinned commit and refuses an
  entry with no sha256 unless `--allow-unhashed` is given). The installer
  builds every file first and replaces them atomically; a failed replace
  is reported with what was already installed and leaves no temporary
  files. From Crimson 2: Moria34's new content is merged into the stock
  Mines of Moria, zone 41, and rescaled to it (Melcrin, a level-16 caster
  boss; the phantom, level 11; the Blade of the Moors Trolls, a level-15
  3d5 weapon). Shatterstone is zone 58, with a level-20 guard, its template
  mob dropped and its torch made a normal light, reached from the Dwarven
  Kingdom (6542 down to 5830 up). Four stock door exits that named
  nonexistent keys are fixed, and room 25650 gets the missing return side
  of 25628's trap door. The Crimson 2 authors' redistribution rights are
  not verified; the owner chose to include the areas.
<!-- PLACEHOLDER-MERGED: add an entry for each PR merged after #18, in
     merge order (#52 string_add aborts; the script_damage overkill fix;
     the zone 103 Kamehameha cap; the delete_room orphan resets / zone 27
     doll quest fix; the coordinator's fix/leftover-* branches), then
     delete this comment. -->

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
<!-- PLACEHOLDER-OPEN: list the PRs still open when this branch merges
     (currently #52 is open; more may be opened), or write "None." -->
See the [open pull requests](https://github.com/kythrasuntamer/TBAMud_Study/pulls)
for their status.
