# tbaMUD modernization study fork

This repository modernizes the CircleMUD/tbaMUD C engine with an emphasis on
**maintainability, actionable diagnostics, security, reliable persistence,
reproducible builds, and regression protection**. The public repository is a
place to develop and share reusable engine improvements.

We are working toward an engine suitable for an independently operated,
public-facing hack-and-slash MUD. **That is a development goal, not a claim
that this fork is already production-hardened or ready for public deployment.**
Experimental gameplay systems and future world designs are not part of the
current stable public engine contract.

## What's being improved

- **Error locality:** preserve the operation and source context of failures,
  including malformed player/world data, network handling, and I/O errors.
- **Persistence reliability:** protect existing player records and advancement
  state, and report distinct save/flush/rename failures at their source.
- **Security:** strengthen password handling, input validation, resource bounds,
  and defenses appropriate to a publicly reachable C server.
- **Developer experience:** build with modern CMake, retain compiler warnings,
  run Unity tests, and use static analysis and memory-safety workflows.
- **Maintainability:** explain ownership, invariants, error behavior, and
  compatibility constraints alongside the code.

**Implementation status matters:** Some capabilities are already present on
the default branch; other improvements remain in draft PRs or prototype
branches. See [CHANGELOG.md](CHANGELOG.md) for a public, status-aware overview.
Do not infer that a proposed PR is merged, fully tested, or production-ready.

## Documentation and contribution policy

When changing a subsystem, document non-obvious control flow, invariants,
ownership and lifetime, persistence compatibility, failure behavior, and
security-sensitive assumptions. Comments must describe what the code *does*,
not guarantees we hope to implement later. Prefer meaningful explanations to
comments that merely paraphrase a line of C.

Preserve character saves, levels, classes, Advancement Points, world content,
and existing features unless a change is explicitly authorized. Review
branch dependencies before integrating work; add regression tests for bug
fixes and avoid merging draft branches before required validation.

## Build and test

From the repository root, with a C compiler and CMake installed:

```sh
set -o pipefail
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug 2>&1 | tee configure.log
cmake --build build --parallel 2>&1 | tee build.log
ctest --test-dir build --output-on-failure 2>&1 | tee test.log
```

The C tests live under `tests/` and use the vendored Unity framework. The
C/C++ CI workflow additionally runs clang-tidy; separate CI workflows build
with Fil-C and Clang sanitizers. See [doc/testing.md](doc/testing.md).

## Security and reliability

Untrusted player and network text must always be passed as *data*, never as a
printf-style format string. For example, use `write_to_output(d, "%s", text)`
when `text` was already constructed. Check truncation, allocation, parsing,
and persistence results at the call site; emitting a diagnostic alone does not
make a failed operation safe to continue. See `tools/check_error_locality.sh`
for the existing error-reporting checks.

## Denial-of-service defense roadmap

Application limits: bound input and output queues, incomplete negotiation data,
login attempts, script execution work, and per-tick processing. Test each
limit and its release on disconnect; limits should be fair to legitimate
players behind shared IP addresses. Network-level and volumetric attack
mitigation also require infrastructure outside the MUD process (for example,
provider filtering/proxying and connection controls); application code alone
cannot prevent uplink saturation.
