# Changelog

Notable changes to the public tbaMUD modernization project are recorded here.

This file distinguishes changes on the repository's default branch from **proposed**
changes in feature branches and pull requests. A PR is not released or merged simply
because it is listed here. Consult the linked PRs and their CI results before
treating any feature as production-ready.

## Unreleased — documentation branch (2026-10-26)

### Documentation
- Added maintainability comments in `src/players.c` describing ASCII player-file
  parsing fallbacks, field interpretation, and index-parser behavior.
- Added lifecycle and buffer-ownership notes in `src/comm.c`.
- Added world database, player-index, and zone-reset reference notes in `src/db.c`.
- Documented Argon2id and legacy password verification behavior, hashing
  failure handling, and login/Oasis state dispatch in `src/interpreter.c`.
- Added DG script execution and trigger-lifetime maintenance notes to
  `src/dg_scripts.c`.
- Documented active-combat list traversal, attack guards, and combat pulse
  expectations in `src/fight.c`.
- Annotated bounded input processing, output buffering, and socket-read return
  conventions in `src/comm.c`.
- Added player-file loading, staged saving, and compatibility notes to
  `src/players.c`.
- Documented Oasis OLC cleanup ownership and editor lifecycle concerns in
  `src/oasis.c`.
- Introduced this public changelog and expanded the README with the project's
  scope, contribution workflow, and verification policy.

These are documentation-only edits; no intended gameplay, persistence format, or
network behavior changes.

## Public modernization work — status snapshot (2026-10-26)

The default branch includes CMake/Unity test support, clang-tidy integration,
the required libsodium dependency for Argon2id work, and security/error-locality
guidance. The README and source are the authority for what is actually merged.
Work in the following PRs remains **proposed** and should not be described as
shipped until reviewed, tested, and merged:

- [PR #38](https://github.com/kythrasuntamer/TBAMud_Study/pull/38) —
  player save I/O failure locality; CI C/C++ regression harness failure was
  reported on commit `51f33c55a5f6acf36e8ce356cd2966c1b00415df`.
- [PR #37](https://github.com/kythrasuntamer/TBAMud_Study/pull/37) —
  diagnostic context exhaustion and truncation visibility.
- [PR #36](https://github.com/kythrasuntamer/TBAMud_Study/pull/36) —
  seven-class guild-room scaffolding; does not constitute complete playable
  class implementations.
- [PR #33](https://github.com/kythrasuntamer/TBAMud_Study/pull/33) —
  checked parsing for player-stat fields.
- [PR #32](https://github.com/kythrasuntamer/TBAMud_Study/pull/32) —
  character and progression migration contracts.
- [PR #27](https://github.com/kythrasuntamer/TBAMud_Study/pull/27) —
  level-50 Advancement Points and HP/mana purchase integration candidate.
- [PR #20](https://github.com/kythrasuntamer/TBAMud_Study/pull/20) —
  experimental TLS frontend and loopback-backend deployment.

This list is a selected overview, not a complete release history. Multiple
proposals have branch dependencies, so avoid merging them out of order.

## Change documentation policy

For subsequent changes, update this file in the same PR. Describe the
user-visible behavior, compatibility effects, affected subsystems, and tests
actually run. Document unresolved failures as unresolved; do not substitute
planned tests for completed tests. Preserve full build output and warnings
in persistent logs. No migration or deletion of existing character data is
implied by an entry here.
