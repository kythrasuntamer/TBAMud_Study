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

# Regression inputs: known crashes that are fixed, kept in corpus/player/ (so
# fuzz_player_corpus replays them too) and also replayed one by one, so a
# failure names the bug.
#   PLAYER-1 regression-vars-noname: read_saved_vars() ignored
#     get_filename()'s result and fopen()ed the unwritten fn[] for a character
#     with no name (empty Name tag, or an Act word that sets the NPC bit);
#     stack over-read in fopen. Fixed by 577cae0: the vars file is refused
#     (dg/vars-filename) when no name can be built.
#   PLAYER-3 regression-name-path-login: a pfile Name tag "../x" read before
#     login. get_filename() rejects it (persist/filename-unsafe); the crash
#     that remained was PLAYER-1's fopen of the refused buffer (577cae0).
#   PLAYER-3 regression-name-path-index: a player index record named "../"
#     reached get_filename() through update_obj_file()/Crash_clean_file(),
#     which built a path outside lib/. Fixed by 2f7889b: get_filename()
#     rejects the name (persist/filename-unsafe).
#   PLAYER-2 regression-hcontrol-time: hcontrol show prints built_on/last_pay after
#     tb_strftime()/localtime() failed on an out-of-range time_t from the
#     binary hcontrol file and left the buffer unwritten; the %-15s then
#     reads a non-terminated stack buffer (house.c:498-509).
#     Fixed by 84f2c75: Unknown is printed for a time localtime() rejects.
#   PLAYER-4 regression-fread-string-nul: fread_string_checked() does point-- then reads
#     *point before the "point > tmp" guard, so a '~'-string line fgets()
#     returns empty (its first byte is NUL) underflows tmp[] by one
#     (players.c:273). Reached by Desc, ADes, EDes. Fixed by d52f3e5: a
#     string line that starts with NUL is dropped.
#   PLAYER-5 regression-controw-leak: Crash_load_objs() parks a rent object nested in a
#     container whose record never arrives in cont_row[] and neither places
#     nor frees it, so it stays on object_list (leak). objsave.c
#     Crash_load_objs/handle_obj. Fixed by e94c9bb (audit-deferred-player-
#     load): leftover rows spill to inventory.
#   PLAYER-6 regression-restrung-leak: objsave.c sets temp->name/short_description/
#     description with a raw strdup(line) and no free, so a record repeating
#     Name:/Shrt:/Desc: leaks the first copy (objsave.c:1404/1468/1490) --
#     the players.c PFILE_SET_STRING fix was never applied here. Fixed by
#     f8de88c: a repeated tag replaces the object's own copy.
#   PLAYER-7 regression-alias-dup-count0: a pfile with several "Alis:" tags,
#     one of which ("Alis:Gold 3") parses to count 0; read_aliases_ascii()
#     set GET_ALIASES(ch) to NULL for count 0 without freeing the aliases an
#     earlier block had loaded (leak, players.c). Fixed on audit/p2-alias-leak:
#     a duplicate Alis: tag is reported (pfile-alias-dup) and the later block
#     replaces the earlier list, which is freed.
foreach(_crash vars-noname name-path-login name-path-index hcontrol-time
               fread-string-nul controw-leak restrung-leak alias-dup-count0)
    add_test(NAME fuzz_player_regression_${_crash}
             COMMAND fuzz_player ${CMAKE_CURRENT_SOURCE_DIR}/corpus/player/regression-${_crash})
    set_tests_properties(fuzz_player_regression_${_crash} PROPERTIES
        LABELS "fuzz"
        ENVIRONMENT "ASAN_OPTIONS=${TBAMUD_ASAN_OPTIONS};UBSAN_OPTIONS=${TBAMUD_UBSAN_OPTIONS}")
endforeach()

# World-file loaders (db.c, dg_db_scripts.c): rooms, mobiles, objects, zones
# and triggers, and the fread_string()/get_line()/file_to_string() readers
# under them. Builders and implementors write these files (OLC or by hand);
# a malformed one must be rejected with a diagnostic, never crash. The
# harness #includes dg_db_scripts.c and a copy of db.c whose record cursors
# it can reset (fuzz_world_db.cmake); the rest is linked from src/, and
# --gc-sections drops what the loaders never reach.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fuzz_world_db.c
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/src/db.c
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/fuzz_world_db.c
            -P ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_world_db.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/src/db.c ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_world_db.cmake
    COMMENT "Generating fuzz_world_db.c (db.c with resettable record cursors)"
    VERBATIM)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/fuzz_world_db.c
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
add_mud_fuzzer(fuzz_world world
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_world.c
    ${CMAKE_CURRENT_BINARY_DIR}/fuzz_world_db.c
    ${CMAKE_SOURCE_DIR}/src/utils.c
    ${CMAKE_SOURCE_DIR}/src/config.c
    ${CMAKE_SOURCE_DIR}/src/constants.c
    ${CMAKE_SOURCE_DIR}/src/interpreter.c
    ${CMAKE_SOURCE_DIR}/src/modify.c
    ${CMAKE_SOURCE_DIR}/src/genzon.c
    ${CMAKE_SOURCE_DIR}/src/genolc.c
    ${CMAKE_SOURCE_DIR}/src/act.informative.c
    ${CMAKE_SOURCE_DIR}/src/handler.c)
target_include_directories(fuzz_world PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# The sancov PC tables are __start_/__stop_ sections, which ld keeps as GC
# roots by default; they reference every instrumented function, so nothing
# would be collected and the unreached parts of the linked files (which call
# into the whole game) would fail to link.
target_link_options(fuzz_world PRIVATE -Wl,-z,start-stop-gc)

# Known crashes: inputs fuzz_world finds (minimised) in loaders this harness
# does not own become DISABLED tests here (ctest lists them as "Disabled")
# until the bug is fixed; then the file moves into corpus/world/ as
# regression-<name> and is replayed below. A fuzzing campaign can step past
# an open one with FUZZ_WORLD_MASK=<name> (see fuzz_world.c). None is open:
# FUZZ-W1 to FUZZ-W5 are fixed (see the regression inputs below).

# Regression inputs: known crashes that are fixed, kept in corpus/world/ (so
# fuzz_world_corpus replays them too) and also replayed one by one, so a
# failure names the bug.
#   FUZZ-W1 regression-trg-empty-script: parse_trigger() did
#     strdup(strtok(s, "\n\r")) on the script fread_string() returned; for an
#     empty '~' script s was NULL (SEGV in strtok_r, or strtok() resumed in
#     the previous trigger's freed buffer), and for a script of only line
#     breaks strtok() returned NULL (strdup(NULL)). Fixed by 63a3ab6 (eed9ae3
#     on audit-deferred-gen-olc): the body loads as "* No script" with a
#     dg/trigger-parse-empty warning.
#   FUZZ-W5 regression-obj-note-exdesc-null-keyword: an empty "~" extra-
#     description keyword is stored as NULL, and check_object() passes a
#     note's extra descriptions to find_exdesc(), which read *keyword: NULL
#     dereference. Fixed by cd48dc1 (c183774 on audit-deferred-commands):
#     find_exdesc() skips the entry and reports act/exdesc-keyword-missing.
#     e2f4f26 on audit-deferred-db-loaders also stops the loaders storing
#     the NULL.
#   FUZZ-W2 regression-obj-convert-zone-nowhere: parse_object() converts a
#     legacy (3- or 4-field) object line and, with bitsavetodisk on (the
#     default), read zone_table[real_zone_by_thing(vnum)].number; for a vnum
#     in no zone that is zone_table[NOWHERE], 65535 * 72 bytes past the
#     table (wild read, SEGV). Fixed by bf8a1d9: a legacy object in no zone
#     no longer indexes zone_table[NOWHERE].
#   FUZZ-W3 regression-room-convert-zone-nowhere: the same read in
#     parse_room(), reachable because the zone walk attached a room in a gap
#     between two zones to the next zone without checking its bottom. Fixed
#     by c1036fd: a room between two zones is fatal at boot, naming both
#     zones. The harness turns that boot-time exit(1) into a clean return
#     (fuzz_world.c's exit() redirect, with the parser's orphans swept and
#     leak-checked), so a fatal rejection passes and only a crash fails.
#   FUZZ-W4 regression-mob-convert-zone-nowhere: the same read in
#     parse_mobile() for a legacy 4-field mob flag line. Fixed by 5e258d2.
foreach(_crash trg-empty-script obj-note-exdesc-null-keyword
               obj-convert-zone-nowhere room-convert-zone-nowhere
               mob-convert-zone-nowhere)
    add_test(NAME fuzz_world_regression_${_crash}
             COMMAND fuzz_world ${CMAKE_CURRENT_SOURCE_DIR}/corpus/world/regression-${_crash})
    set_tests_properties(fuzz_world_regression_${_crash} PROPERTIES
        LABELS "fuzz"
        ENVIRONMENT "ASAN_OPTIONS=${TBAMUD_ASAN_OPTIONS};UBSAN_OPTIONS=${TBAMUD_UBSAN_OPTIONS}")
endforeach()
