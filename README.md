Files for tbaMUD.

## Build and test

From the repository root, with a C compiler and CMake installed:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
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
