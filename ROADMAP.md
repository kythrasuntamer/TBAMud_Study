# Public Engine Modernization Roadmap

This roadmap covers **only work intended for the public tbaMUD/CircleMUD
modernization repository**. It is not a product roadmap for any game built on
the engine. Features listed here are plans, not promises of completion or
claims that changes have already merged.

Our goal is a maintainable, secure, well-documented C MUD engine that remains
recognizably CircleMUD in style and straightforward to operate and extend.

## Engineering principles

- Preserve existing behavior, player data, world content, APIs, and backwards
  compatibility unless an intentional change is separately approved.
- Favor the familiar CircleMUD 3.x C idiom: direct control flow, meaningful
  names, useful established macros, and narrowly scoped functions. Depart from
  that idiom where necessary for correctness, portability, error locality, or
  security.
- Prefer explicit ownership, bounded operations, verified input, and checked
  arithmetic. Never hide critical failures behind best-effort continuation.
- Require tests that protect previous functionality, and capture full compiler
  warnings and test output in persistent logs.
- Keep public documentation accurate: distinguish merged features from open
  PRs, experiments, and future proposals.

## Done on master

See [CHANGELOG.md](CHANGELOG.md) and the audit report for details.

- Error locality: structured TB_ERROR_*/DIAG_* reports at the failing call,
  ratcheted by tools/check_error_locality.sh.
- Staged player, index and zone-index writes, each step reported at its
  source; deterministic failure injection (TB_TEST_FAILURE_INJECTION).
- The memory-lifetime audit of every source file
  ([docs/memory-safety-audit.md](docs/memory-safety-audit.md)), including
  OLC ownership (F3-08).
- Argon2id passwords, with legacy crypt hashes upgraded on a successful
  login.
- A connection admission throttle (comm.c tb_admit_connection()).
- CI: ASan+UBSan, Valgrind, Fil-C, Semgrep and clang-tidy; libFuzzer
  targets for the protocol, player-file and world loaders.

## Open

Taken from "What remains open" in
[docs/audit-2026-10-report.md](docs/audit-2026-10-report.md):

1. Time-boxed fuzzing in CI, and a MEMORY_DEBUG+ASan CI job.
2. Sanitizer and hardening switches for the autoconf build.
3. The Semgrep CI job needs the repository secret `SEMGREP_APP_TOKEN`.
4. `src/class.c`'s level tables (lines ~800-2170) were not read line by
   line.

## Public-server hardening (planned)

1. Harden connection lifecycle, protocol parsing, queues, input limits and
   resource exhaustion beyond the existing admission throttle.
2. Improve script execution limits and area-file validation so malformed
   content cannot destabilize the server.
3. Provide documented secure deployment patterns, monitoring, backup and
   restore exercises, and operational recovery procedures.
4. Denial-of-service defense. Application limits: bound input and output
   queues, incomplete negotiation data, login attempts, script execution
   work and per-tick processing. Test each limit and its release on
   disconnect, and keep limits fair to legitimate players behind shared IP
   addresses. Network-level and volumetric mitigation also needs
   infrastructure outside the MUD process (provider filtering, proxying,
   connection controls); application code alone cannot prevent uplink
   saturation.

## Longer term: developer and builder tooling

**Status: proposals for reusable public engine capabilities.**

1. Improve Oasis OLC validation, resource ownership, and error reporting
   without removing established commands or workflows.
2. Add isolated world-data test/preview paths and automated checks for bad
   VNUM references, exits, scripts, object references, and zone resets.
3. Investigate transactional editing, recoverable changes, and revision
   history for world content, subject to compatibility and test coverage.
4. Improve reproducible developer setup, generated API documentation,
   diagnostics demonstrations, and contributor onboarding.
5. Consider additional portable diagnostics and testing infrastructure
   when it can be maintained with modest complexity.

## Release and review policy

The roadmap has **no fixed dates**. Features move into a release only after
code review, tests, regression checks, and relevant runtime validation.
Security-critical changes should be reviewed independently of broad
formatting/style edits.

See [CHANGELOG.md](CHANGELOG.md) for a status-aware account of work, and
[the open pull requests](https://github.com/kythrasuntamer/TBAMud_Study/pulls)
for implementation details and outstanding review.

## Scope boundary

This document deliberately does not enumerate private game development,
unreleased world content, exclusive gameplay mechanics, or private operational
plans. Public engine improvements can continue independently of any
separate game that may use the engine.
