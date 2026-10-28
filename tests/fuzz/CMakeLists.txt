# libFuzzer targets (ENABLE_FUZZING=ON, Clang only). Each target is built
# with ASan + UBSan and its own copy of the code under test, and is also
# registered as a CTest regression test that replays the seed corpus once
# (-runs=0), so a crash input saved into the corpus fails ctest.
#
# Fuzz for a while:   bin/fuzz_protocol -max_total_time=300 -jobs=4 \
#                       tests/fuzz/corpus/protocol
# New crash inputs (crash-*) reproduce with: bin/fuzz_protocol crash-<hash>
# Once fixed, minimise one with -minimize_crash=1 and add it to the corpus.

set(TB_FUZZ_SANITIZE "-fsanitize=fuzzer,address,undefined")

macro(add_mud_fuzzer NAME CORPUS)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
    target_compile_options(${NAME} PRIVATE ${TB_FUZZ_SANITIZE}
        -fno-omit-frame-pointer -fno-sanitize-recover=all
        -ffunction-sections -fdata-sections)
    target_link_options(${NAME} PRIVATE ${TB_FUZZ_SANITIZE} -Wl,--gc-sections)
    add_test(NAME ${NAME}_corpus
             COMMAND ${NAME} -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${CORPUS})
    set_tests_properties(${NAME}_corpus PROPERTIES
        LABELS "fuzz" TIMEOUT 120
        ENVIRONMENT "ASAN_OPTIONS=${TBAMUD_ASAN_OPTIONS};UBSAN_OPTIONS=${TBAMUD_UBSAN_OPTIONS}")
endmacro()

# Telnet negotiation, subnegotiation (TTYPE/NAWS/CHARSET/MSDP/GMCP/MSSP),
# MXP and colour expansion: everything a client's bytes pass through before
# the command interpreter.
add_mud_fuzzer(fuzz_protocol protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_protocol.c
    ${CMAKE_SOURCE_DIR}/src/protocol.c)

# Regression inputs: crashes the fuzzer found, kept in corpus/protocol/ and
# replayed as ordinary tests so the bugs stay fixed.
#   FUZZ-1 regression-output-trailing-tab-overread: ProtocolOutput()'s loop
#     condition read apData[j] before testing bTerminate, so text ending in
#     a tab read one byte past the terminator.
#   FUZZ-2 regression-mssp-callback-function-type: the GetMSSP_* callbacks
#     had no prototype and were called through "const char *(*)(void)";
#     UBSan -fsanitize=function rejected the call.
foreach(_crash output-trailing-tab-overread mssp-callback-function-type)
    add_test(NAME fuzz_protocol_regression_${_crash}
             COMMAND fuzz_protocol ${CMAKE_CURRENT_SOURCE_DIR}/corpus/protocol/regression-${_crash})
    set_tests_properties(fuzz_protocol_regression_${_crash} PROPERTIES
        LABELS "fuzz"
        ENVIRONMENT "ASAN_OPTIONS=${TBAMUD_ASAN_OPTIONS};UBSAN_OPTIONS=${TBAMUD_UBSAN_OPTIONS}")
endforeach()
