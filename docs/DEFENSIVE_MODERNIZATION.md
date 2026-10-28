# Defensive modernization roadmap

This is an incremental plan, not a claim that all controls are implemented.
No change may regress existing class IDs, levels 1-50, endgame AP/XP/HP/mana,
character persistence, quests, builder content or other existing features.

## Required on every change

1. Inventory affected features and inspect changes against master.
2. Keep all compiler/analyzer output with `set -o pipefail` and `2>&1 | tee`.
3. Preserve previous behavior unless the change is explicitly authorized.
4. Add executable regression tests for new behavior; prefer native tests to
   source-string checks.
5. Run clean compiler/analyzer build, CTest, Python tests and world syntax check.
6. Test with disposable player directories; never use live saves as fixtures.
7. Do not merge if new warnings, crashes, unresolved test failures, or gameplay
   regressions exist. Do not suppress diagnostics merely to make checks pass.

## P0: Data safety and public-server boundaries

- [ ] Get native player round-trip CTest linking and passing for all 12 classes,
      maximum mortal level, skills, quest flags, endgame progression and equipment.
- [ ] Add fault-injection tests: failed create/write/flush/close/rename, torn
      files, interrupted saves, and recovery retaining previous valid saves.
- [ ] Validate loaded class IDs, levels, resource limits, string lengths, and
      all serialized values before using them as array indexes or allocations.
- [ ] Audit input parsing in telnet/login, OLC, scripting and world files.
- [ ] Strengthen account-level/IP-level throttling with measured resource bounds,
      strong authentication, least-privilege staff operations and audit logs.
- [ ] Make memory sanitizers and fuzz inputs required automated checks; verify
      CI never discards compiler or analyzer diagnostics.
- [ ] Resolve world syntax diagnostics including zone 301 reset VNUM 30170.

## P1: Engine architecture and error locality

- [ ] Add validated class metadata descriptors and registration checks;
      migrate existing class switches incrementally, retaining IDs and semantics.
- [ ] Centralize skill capability/level validation and verified skill handlers;
      prevent unimplemented skill registration from becoming available.
- [ ] Use explicit, documented ownership for dynamically allocated objects,
      rooms, descriptors and persistent player state.
- [ ] Define typed status/error results at file, parser and command boundaries;
      preserve errno plus operation/context at the original failing call.
- [ ] Define explicit network connection-state transitions and permission checks.
- [ ] Introduce contract and property tests for class and skill state machines.

## P2: Optimizations after invariants

- [ ] Profile CPU, memory, allocation and network hotspots with representative
      workloads and adversarial inputs.
- [ ] Optimize hot paths only when their correctness, validation and memory
      safety properties are preserved.

## Current baseline

- CI build retains compiler output; this branch adds Python regression checks
  and treats the existing invalid world reset as an error to resolve.
- Native C class tests cover the 12 class archetypes and 50 mortal levels.
- Player round-trip integration harness (test_player_roundtrip) saves and
  reloads every class at level 50 with endgame data under ASan.
- Guild and arcane specialization features (PR #36) are gated like every
  other change: native tests, sanitizers, Fil-C, e2e soaks.
