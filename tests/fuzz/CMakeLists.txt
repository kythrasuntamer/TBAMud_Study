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

# Player files, the player index, rent (crash-save) files, player variable
# files and house files: every persistent file a damaged disk or a hand edit
# can feed to a loader. The loaders and the object/character lifecycle
# behind them are production code; fuzz_player_stubs.c replaces only output
# and game systems the loaders never reach (those abort if reached).
# --gc-sections drops everything else in these files.
set(FUZZ_PLAYER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_player.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_player_stubs.c
    ${CMAKE_SOURCE_DIR}/src/players.c
    ${CMAKE_SOURCE_DIR}/src/objsave.c
    ${CMAKE_SOURCE_DIR}/src/house.c
    ${CMAKE_SOURCE_DIR}/src/handler.c
    ${CMAKE_SOURCE_DIR}/src/db.c
    ${CMAKE_SOURCE_DIR}/src/utils.c
    ${CMAKE_SOURCE_DIR}/src/genobj.c
    ${CMAKE_SOURCE_DIR}/src/genolc.c
    ${CMAKE_SOURCE_DIR}/src/dg_scripts.c
    ${CMAKE_SOURCE_DIR}/src/dg_variables.c
    ${CMAKE_SOURCE_DIR}/src/dg_handler.c
    ${CMAKE_SOURCE_DIR}/src/dg_db_scripts.c
    ${CMAKE_SOURCE_DIR}/src/interpreter.c
    ${CMAKE_SOURCE_DIR}/src/quest.c
    ${CMAKE_SOURCE_DIR}/src/class.c
    ${CMAKE_SOURCE_DIR}/src/modify.c
    ${CMAKE_SOURCE_DIR}/src/config.c
    ${CMAKE_SOURCE_DIR}/src/constants.c)
add_mud_fuzzer(fuzz_player player ${FUZZ_PLAYER_SOURCES})
# The harness sees every structured diagnostic, turns exit() and a boot-time
# TB_FATAL refusal into a clean return, checks file names built from file
# content, and makes core_dump() a finding (see fuzz_player.c).
# -z start-stop-gc: libFuzzer refers to __start___sancov_pcs, which by
# default keeps every function's coverage table, and so every function, out
# of --gc-sections; with it only the reachable code is linked.
target_link_options(fuzz_player PRIVATE -Wl,-z,start-stop-gc
    -Wl,--wrap=tb_error_log_at -Wl,--wrap=exit -Wl,--wrap=core_dump_real
    -Wl,--wrap=get_filename -Wl,--wrap=autoquest_trigger_check
    -Wl,--wrap=page_string)
# Uninitialised stack buffers hold a non-NUL pattern, so a string that a
# loader leaves unwritten (e.g. after a failed strftime()) runs into an ASan
# redzone when it is printed, instead of depending on stale stack bytes.
target_compile_options(fuzz_player PRIVATE -ftrivial-auto-var-init=pattern)

# Known crashes the player/object-save fuzzer found in code this harness does
# not own (players.c, objsave.c, house.c, dg_scripts.c). Each is a DISABLED
# test so ctest lists it as "Disabled" until the bug is fixed; then delete
# DISABLED and move the file into corpus/player/. The fuzzer itself steps
# around these with FUZZ_PLAYER_MASK=<name>[,...] so a campaign keeps finding
# new bugs; the tests run them unmasked, so each still reproduces.
#   PLAYER-1 vars-noname: read_saved_vars() ignores get_filename()'s result
#     and fopen()s the unwritten fn[] when the character has no name (empty
#     Name tag, or an Act word that sets the NPC bit); stack over-read in
#     fopen (dg_scripts.c:3197-3198). Reached at login.
#   PLAYER-2 hcontrol-time: hcontrol show prints built_on/last_pay after
#     tb_strftime()/localtime() failed on an out-of-range time_t from the
#     binary hcontrol file and left the buffer unwritten; the %-15s then
#     reads a non-terminated stack buffer (house.c:498-509).
#   PLAYER-3 name-path-index / name-path-login: get_filename() builds a path
#     from a file-supplied player name (an index record, or a pfile Name tag
#     read before login) without rejecting "..", so a name like "../x" or
#     "a/../../etc/x" escapes lib/ for every file the name keys (unlink in
#     remove_player, fopen in Crash_*/read_saved_vars). utils.c get_filename.
#   PLAYER-4 fread-string-nul: fread_string_checked() does point-- then reads
#     *point before the "point > tmp" guard, so a '~'-string line fgets()
#     returns empty (its first byte is NUL) underflows tmp[] by one
#     (players.c:273). Reached by Desc, ADes, EDes.
#   PLAYER-5 controw-leak: Crash_load_objs() parks a rent object nested in a
#     container whose record never arrives in cont_row[] and neither places
#     nor frees it, so it stays on object_list (leak). objsave.c
#     Crash_load_objs/handle_obj. Fixed on audit-deferred-player-load
#     (e94c9bb: spill leftover rows to inventory).
#   PLAYER-6 restrung-leak: objsave.c sets temp->name/short_description/
#     description with a raw strdup(line) and no free, so a record repeating
#     Name:/Shrt:/Desc: leaks the first copy (objsave.c:1404/1468/1490) --
#     the players.c PFILE_SET_STRING fix was never applied here.
foreach(_crash vars-noname hcontrol-time name-path-index name-path-login
               fread-string-nul controw-leak restrung-leak)
    add_test(NAME fuzz_player_known_${_crash}
             COMMAND fuzz_player ${CMAKE_CURRENT_SOURCE_DIR}/known-crashes/player/${_crash})
    set_tests_properties(fuzz_player_known_${_crash} PROPERTIES
        LABELS "fuzz" DISABLED TRUE
        ENVIRONMENT "ASAN_OPTIONS=${TBAMUD_ASAN_OPTIONS};UBSAN_OPTIONS=${TBAMUD_UBSAN_OPTIONS}")
endforeach()
