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

## Near term: reliability and diagnostic foundations

**Status: actively being developed and reviewed.**

1. Improve error locality across parsing, network I/O, storage, and script
   evaluation. Include enough context to locate the original failing
   operation, while avoiding the disclosure of passwords or other secrets.
2. Strengthen player-file write and recovery paths, including error handling
   for staging, writing, flushing, closing, renaming, and cleanup. Verify
   preservation of legacy player and progression fields.
3. Expand targeted regression tests and deterministic fault-injection
   coverage. Do not equate a passing compile with safe persistence.
4. Audit dangerous memory lifetimes, numeric boundaries, string formatting,
   and malformed input paths.
5. Continue source commentary and architecture documentation, including
   debugging guides and a changelog of verified improvements.

## Medium term: public-server hardening

**Status: planned and partly prototyped.**

1. Complete authentication hardening, including robust Argon2id handling,
   legacy-account migration, and authorization checks.
2. Harden connection lifecycle, protocol parsing, queues, input limits,
   account/IP throttling, and resource exhaustion handling.
3. Improve safe script execution and area-file validation so untrusted or
   malformed content cannot destabilize the server.
4. Expand compiler/sanitizer/static-analysis coverage and make the intended
   CI quality gates consistently enforceable.
5. Provide documented secure deployment patterns, monitoring, backup/restore
   exercises, and operational recovery procedures.

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
