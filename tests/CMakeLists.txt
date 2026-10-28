# tests/CMakeLists.txt
# Unity-based unit tests for tbaMUD
#
# Each test executable is built from:
#   vendor/unity/unity.c   — test framework
#   test_stubs.c           — weak-symbol stubs for all unresolved mud globals
#   src/<module>.c         — the source file(s) under test
#   test_<name>.c          — the test file itself
#
# Only the source files being tested are compiled — in particular comm.c
# (which contains main()) is never included.

cmake_minimum_required(VERSION 3.12)

enable_testing()

# ---------------------------------------------------------------------------
# Common settings shared by all test targets
# ---------------------------------------------------------------------------

set(UNITY_SRC  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity/unity.c)
set(STUBS_SRC  ${CMAKE_CURRENT_SOURCE_DIR}/test_stubs.c)

# Keep third-party Unity code byte-for-byte upstream. CERT/clang-tidy findings
# are enforced on tbaMUD-owned code, not vendored test framework internals.
add_library(unity_vendor OBJECT ${UNITY_SRC})
set_target_properties(unity_vendor PROPERTIES C_CLANG_TIDY "")
target_include_directories(unity_vendor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity
)

set(MUD_SRCDIR ${CMAKE_SOURCE_DIR}/src)

# Include paths: mud source dir for conf.h / structs.h etc.,
# CMake binary dir for the generated conf.h (cmake builds place it there),
# and the Unity header dir.
set(TEST_INCLUDES
    ${MUD_SRCDIR}
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity
)

# Suppress warnings that fire in generated stubs / vendored code and in the
# mud sources when compiled outside their normal full-build context.
set(TEST_CFLAGS)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    list(APPEND TEST_CFLAGS
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-unused-variable
    )
endif()

# Helper macro: add_mud_test(name SRC1 [SRC2 …])
# Creates an executable, registers it with CTest.
macro(add_mud_test TEST_NAME)
    add_executable(${TEST_NAME}
        $<TARGET_OBJECTS:unity_vendor>
        ${STUBS_SRC}
        ${ARGN}
    )
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDES})
    target_compile_options(${TEST_NAME} PRIVATE ${TEST_CFLAGS})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES LABELS "native;unit")
endmacro()

# ---------------------------------------------------------------------------
# test_utils  — covers src/utils.c
# ---------------------------------------------------------------------------
add_mud_test(test_utils
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_utils.c
)

# ---------------------------------------------------------------------------
# test_random — covers src/random.c and rand_number/dice in src/utils.c
# ---------------------------------------------------------------------------
add_mud_test(test_random
    ${MUD_SRCDIR}/random.c
    ${MUD_SRCDIR}/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_random.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    # Capture random.c's TB_WARN_MSG records (see test_random.c).
    target_link_options(test_random PRIVATE -Wl,--wrap=tb_error_log_at)
endif()

# ---------------------------------------------------------------------------
# test_interpreter — covers string helpers in src/interpreter.c
# ---------------------------------------------------------------------------
add_mud_test(test_interpreter
    ${MUD_SRCDIR}/interpreter.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_interpreter.c
)

# crypt() is referenced from interpreter.c (nanny password hashing).
# Reuse the crypt library detected by the top-level build when one is needed;
# on platforms where crypt() is provided by libc, no extra link library is
# required.
if(CRYPT_LIBRARY)
    target_link_libraries(test_interpreter PRIVATE ${CRYPT_LIBRARY})
endif()
target_link_libraries(test_interpreter PRIVATE ${SODIUM_LIBRARY})
# Count the structured diagnostics interpreter.c emits (see the test's
# __wrap_tb_error_log_at).
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_link_options(test_interpreter PRIVATE -Wl,--wrap=tb_error_log_at)
endif()

# ---------------------------------------------------------------------------
# test_class — covers src/class.c
# ---------------------------------------------------------------------------
add_mud_test(test_class
    ${MUD_SRCDIR}/class.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_class.c
)
# class.c's table diagnostics are captured (tb_error_capture.h).
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_link_options(test_class PRIVATE -Wl,--wrap=tb_error_log_at)
endif()


# ---------------------------------------------------------------------------
# test_error_handling — strict parsing and transactional failure containment
# ---------------------------------------------------------------------------
add_mud_test(test_error_handling
    ${CMAKE_CURRENT_SOURCE_DIR}/test_error_handling.c
)
target_compile_definitions(test_error_handling PRIVATE TB_TEST_FAILURE_INJECTION=1)

# ---------------------------------------------------------------------------
# test_oasis_list — OLC listing commands (src/oasis_list.c)
# ---------------------------------------------------------------------------
add_mud_test(test_oasis_list
    ${MUD_SRCDIR}/oasis_list.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${MUD_SRCDIR}/constants.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_oasis_list.c
)

# ---------------------------------------------------------------------------
# test_oasis_copy — rcopy/dig/buildwalk (src/oasis_copy.c) and cleanup_olc
# (src/oasis.c); editor back ends are faked in the test file.
# ---------------------------------------------------------------------------
add_mud_test(test_oasis_copy
    ${MUD_SRCDIR}/oasis_copy.c
    ${MUD_SRCDIR}/oasis.c
    ${MUD_SRCDIR}/oasis_delete.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${MUD_SRCDIR}/constants.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_oasis_copy.c
)

# Exercise the actual DG memory-trigger functions under native sanitizers.
# Discard unrelated dg_triggers.c sections so no game-server linker
# dependencies are needed for this focused fixture.
add_mud_test(test_dg_memory_runtime
    ${MUD_SRCDIR}/dg_triggers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_memory_runtime.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_memory_runtime PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_memory_runtime PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/dg_triggers.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production DG UID lookup table: rebinding-safe removal and ID allocation.
add_mud_test(test_dg_lookup_table
    ${MUD_SRCDIR}/dg_scripts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_lookup_table.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_lookup_table PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_lookup_table PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/dg_scripts.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production DG substitution, eval and script_driver(): player text must not
# become variable syntax, and a script stops once its owner is freed.
add_mud_test(test_dg_script_runtime
    ${MUD_SRCDIR}/dg_variables.c
    ${MUD_SRCDIR}/dg_scripts.c
    ${MUD_SRCDIR}/interpreter.c
    ${MUD_SRCDIR}/constants.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_script_runtime.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_script_runtime PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_script_runtime PRIVATE -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at -Wl,--wrap=script_command_interpreter
        -Wl,--wrap=command_interpreter)
    target_link_libraries(test_dg_script_runtime PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})
    set_source_files_properties(${MUD_SRCDIR}/dg_variables.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production DG event queue: queue keys never wrap negative.
add_mud_test(test_dg_event
    ${MUD_SRCDIR}/dg_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_event.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_event PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_event PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/dg_event.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production DG event queue: an event cancelled from its own callback is
# freed once, after the callback returns, and never re-enqueued.
add_mud_test(test_dg_event_cancel
    ${MUD_SRCDIR}/dg_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_event_cancel.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_event_cancel PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_event_cancel PRIVATE -Wl,--gc-sections)
endif()

# Production trigedit script formatter (dg_olc.c format_script).
add_mud_test(test_dg_format_script
    ${MUD_SRCDIR}/dg_olc.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_format_script.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_format_script PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_format_script PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/dg_olc.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production mob script commands (dg_mobcmd.c).
add_mud_test(test_dg_mobcmd
    ${MUD_SRCDIR}/dg_mobcmd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_mobcmd.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_mobcmd PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_mobcmd PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/dg_mobcmd.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production room editor setup (redit.c).
add_mud_test(test_redit
    ${MUD_SRCDIR}/redit.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_redit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_redit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_redit PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/redit.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production help editor (hedit.c).
add_mud_test(test_hedit
    ${MUD_SRCDIR}/hedit.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hedit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_hedit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_hedit PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/hedit.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production preference editor (prefedit.c).
add_mud_test(test_prefedit
    ${MUD_SRCDIR}/prefedit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_prefedit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_prefedit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_prefedit PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/prefedit.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production act()/perform_act() formatter. comm.c owns main(); rename it and
# discard sections the formatter does not reach. test_stubs.c is omitted
# because it defines comm.c's globals; the test supplies its own fixtures.
add_executable(test_act_format
    $<TARGET_OBJECTS:unity_vendor>
    ${MUD_SRCDIR}/comm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_act_format.c
)
target_include_directories(test_act_format PRIVATE ${TEST_INCLUDES})
target_compile_options(test_act_format PRIVATE ${TEST_CFLAGS})
add_test(NAME test_act_format COMMAND test_act_format)
set_tests_properties(test_act_format PROPERTIES LABELS "native;unit")
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_act_format PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_act_format PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/comm.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections;-Dmain=tb_comm_main_unused")
endif()

# Production descriptor output path (vwrite_to_output/process_output).
# test_comm_output.c #includes comm.c to reach its file-static functions, and
# wraps write() so a test can script short writes (a full socket buffer).
add_executable(test_comm_output
    $<TARGET_OBJECTS:unity_vendor>
    ${CMAKE_CURRENT_SOURCE_DIR}/test_comm_output.c
)
target_include_directories(test_comm_output PRIVATE ${TEST_INCLUDES})
target_compile_options(test_comm_output PRIVATE ${TEST_CFLAGS})
add_test(NAME test_comm_output COMMAND test_comm_output)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_comm_output PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_comm_output PRIVATE -Wl,--gc-sections -Wl,--wrap=write -Wl,--wrap=read)
endif()

# Production KaVir protocol layer (ProtocolOutput/ProtocolInput).
# test_protocol.c #includes protocol.c to reach its file-static helpers.
add_executable(test_protocol
    $<TARGET_OBJECTS:unity_vendor>
    ${CMAKE_CURRENT_SOURCE_DIR}/test_protocol.c
)
target_include_directories(test_protocol PRIVATE ${TEST_INCLUDES})
target_compile_options(test_protocol PRIVATE ${TEST_CFLAGS})
add_test(NAME test_protocol COMMAND test_protocol)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_protocol PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_protocol PRIVATE -Wl,--gc-sections)
endif()

# Production DG sub_write() message builder (dg_comm.c).
add_mud_test(test_dg_sub_write
    ${MUD_SRCDIR}/dg_comm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_sub_write.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_dg_sub_write PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_dg_sub_write PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/dg_comm.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production pager (modify.c page_string/next_page/show_string).
add_mud_test(test_modify_pager
    ${MUD_SRCDIR}/modify.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_modify_pager.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_modify_pager PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_modify_pager PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/modify.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production improved editor (improved-edit.c format_text and commands).
add_mud_test(test_improved_edit
    ${MUD_SRCDIR}/improved-edit.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_improved_edit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_improved_edit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_improved_edit PRIVATE -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
    set_source_files_properties(${MUD_SRCDIR}/improved-edit.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production site-ban loader and commands (ban.c).
add_mud_test(test_ban
    ${MUD_SRCDIR}/ban.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ban.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_ban PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_ban PRIVATE -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
    set_source_files_properties(${MUD_SRCDIR}/ban.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()


# Production world/text loaders (db.c). The test #includes db.c so its
# file-scope parsers are reachable, and redirects exit() so fatal boot paths
# can be asserted. utils.c is compiled into a private object library with
# tb_error_log_at renamed, so tb_error_capture.h receives every diagnostic.
# test_stubs.c is omitted because it defines db.c's globals.
add_library(db_parse_utils OBJECT ${MUD_SRCDIR}/utils.c)
target_include_directories(db_parse_utils PRIVATE ${TEST_INCLUDES})
target_compile_options(db_parse_utils PRIVATE ${TEST_CFLAGS})
target_compile_definitions(db_parse_utils PRIVATE tb_error_log_at=tb_error_log_at_real)
add_executable(test_db_parse
    $<TARGET_OBJECTS:unity_vendor>
    $<TARGET_OBJECTS:db_parse_utils>
    ${MUD_SRCDIR}/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_db_parse.c
)
target_include_directories(test_db_parse PRIVATE ${TEST_INCLUDES})
target_compile_options(test_db_parse PRIVATE ${TEST_CFLAGS})
add_test(NAME test_db_parse COMMAND test_db_parse)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(db_parse_utils PRIVATE -ffunction-sections -fdata-sections)
    target_compile_options(test_db_parse PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_db_parse PRIVATE -Wl,--gc-sections)
endif()


# Production rent/crash-save loader and writer (objsave.c), driven through
# Crash_load()/Crash_listrent() against scratch rent files.
add_mud_test(test_objsave_persist
    ${MUD_SRCDIR}/objsave.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_objsave_persist.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_objsave_persist PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_objsave_persist PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/objsave.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production player-file loader, index and tag parser (players.c), driven
# through load_char()/build_player_index() against scratch player files.
add_mud_test(test_players_persist
    ${MUD_SRCDIR}/players.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_players_persist.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_players_persist PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_players_persist PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/players.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production mud-mail store (mail.c) against a scratch lib/etc tree.
# players.c supplies fread_string_checked(); the rest of it is discarded.
add_mud_test(test_mail_persist
    ${MUD_SRCDIR}/mail.c
    ${MUD_SRCDIR}/players.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mail_persist.c
)
# A regression of the receive loop (B2-06) spins forever; fail it instead.
set_tests_properties(test_mail_persist PROPERTIES TIMEOUT 60)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_mail_persist PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_mail_persist PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/mail.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production player-house control file and commands (house.c) against a scratch lib tree.
add_mud_test(test_house_persist
    ${MUD_SRCDIR}/house.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_house_persist.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_house_persist PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_house_persist PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/house.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production bulletin boards (boards.c): headline bounds, message-index and
# slot bookkeeping, and the board-file loader. interpreter.c supplies the
# argument helpers; utils.c's tb_error_log_at() is wrapped for capture.
add_mud_test(test_boards
    ${MUD_SRCDIR}/boards.c
    ${MUD_SRCDIR}/interpreter.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_boards.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_boards PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_boards PRIVATE
        -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
endif()
target_link_libraries(test_boards PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})

# Production Ideas/Bugs/Typos lists (ibt.c) and the db.c readers that load
# them. db.c defines the world globals, so test_stubs.c is not linked, and
# interpreter.c is left out (its cmd_info[] would pull in every do_*); the
# test supplies the few outside symbols the kept sections reference.
add_executable(test_ibt
    $<TARGET_OBJECTS:unity_vendor>
    ${MUD_SRCDIR}/ibt.c
    ${MUD_SRCDIR}/db.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ibt.c
)
target_include_directories(test_ibt PRIVATE ${TEST_INCLUDES})
target_compile_options(test_ibt PRIVATE ${TEST_CFLAGS})
add_test(NAME test_ibt COMMAND test_ibt)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_ibt PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_ibt PRIVATE
        -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
endif()
target_link_libraries(test_ibt PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})

# Production GenOLC core (genolc.c, #included by the test for its file-scope
# export writers): the save-list driver and zone export.
add_mud_test(test_genolc
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_genolc.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_genolc PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_genolc PRIVATE
        -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
endif()
target_link_libraries(test_genolc PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})

# Production GenOLC mobile deleter (genmob.c) and zone-command removal
# (genzon.c): zone-command, live-mob and shopkeeper renumbering.
add_mud_test(test_genmob
    ${MUD_SRCDIR}/genmob.c
    ${MUD_SRCDIR}/genzon.c
    ${MUD_SRCDIR}/genobj.c
    ${MUD_SRCDIR}/genolc.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_genmob.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_genmob PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_genmob PRIVATE
        -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
endif()
target_link_libraries(test_genmob PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})

# Production generic OLC renumberers and writers plus the DG trigger loader.
# The test supplies the engine services they reach (lookups, object moves,
# save list) as small models; everything else is discarded by section GC.
set(GENOLC_SAFETY_SOURCES
    ${MUD_SRCDIR}/genobj.c
    ${MUD_SRCDIR}/genzon.c
    ${MUD_SRCDIR}/genwld.c
    ${MUD_SRCDIR}/genqst.c
    ${MUD_SRCDIR}/genshp.c
    ${MUD_SRCDIR}/dg_db_scripts.c
)
add_mud_test(test_genolc_safety
    ${GENOLC_SAFETY_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_genolc_safety.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_genolc_safety PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_genolc_safety PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${GENOLC_SAFETY_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# test_dg_handler — DG script/trigger memory handlers (src/dg_handler.c)
add_mud_test(test_dg_handler
    ${MUD_SRCDIR}/dg_handler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_handler.c
)

# DG script commands against the real server objects and a tiny world.
add_subdirectory(dg_world)

# Production OLC editors outside F1 (hedit.c, ...), driven through their entry
# points with a fixture world; collaborators outside the editors are supplied
# by the test, and unrelated sections are discarded.
set(OLC_MISC_EDITOR_SOURCES
    ${MUD_SRCDIR}/hedit.c
    ${MUD_SRCDIR}/prefedit.c
    ${MUD_SRCDIR}/qedit.c
    ${MUD_SRCDIR}/aedit.c
    ${MUD_SRCDIR}/tedit.c
    ${MUD_SRCDIR}/msgedit.c
)
add_mud_test(test_olc_misc_editors
    ${OLC_MISC_EDITOR_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_olc_misc_editors.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_olc_misc_editors PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_olc_misc_editors PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${OLC_MISC_EDITOR_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production informative commands (act.informative.c): help search, room and
# object display, score, areas, where and history. Unreferenced sections are
# discarded; the test supplies the fixtures the reached code needs.
# utils.c supplies the real string builders and table printers; its
# tb_error_log_at is renamed so tb_error_capture.h can capture diagnostics.
add_library(act_informative_utils OBJECT ${MUD_SRCDIR}/utils.c)
set_target_properties(act_informative_utils PROPERTIES C_CLANG_TIDY "")
target_include_directories(act_informative_utils PRIVATE ${TEST_INCLUDES})
target_compile_options(act_informative_utils PRIVATE ${TEST_CFLAGS})
target_compile_definitions(act_informative_utils PRIVATE
    tb_error_log_at=tb_error_log_at_utils_unused)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(act_informative_utils PRIVATE -ffunction-sections -fdata-sections)
endif()
add_executable(test_act_informative
    $<TARGET_OBJECTS:unity_vendor>
    $<TARGET_OBJECTS:act_informative_utils>
    ${MUD_SRCDIR}/act.informative.c
    ${MUD_SRCDIR}/constants.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_act_informative.c
)
target_include_directories(test_act_informative PRIVATE ${TEST_INCLUDES})
target_compile_options(test_act_informative PRIVATE ${TEST_CFLAGS})
add_test(NAME test_act_informative COMMAND test_act_informative)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_act_informative PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_act_informative PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/act.informative.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production communication commands (act.comm.c) and socials (act.social.c):
# act() format building, '@' code checks, history and social messages.
# act() is replaced by a recorder in the test.
add_executable(test_act_comm
    $<TARGET_OBJECTS:unity_vendor>
    ${MUD_SRCDIR}/act.comm.c
    ${MUD_SRCDIR}/act.social.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_act_comm.c
)
target_include_directories(test_act_comm PRIVATE ${TEST_INCLUDES})
target_compile_options(test_act_comm PRIVATE ${TEST_CFLAGS})
add_test(NAME test_act_comm COMMAND test_act_comm)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_act_comm PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_act_comm PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/act.comm.c ${MUD_SRCDIR}/act.social.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production miscellaneous player commands (act.other.c), with lists.c for
# group iteration.
add_executable(test_act_other
    $<TARGET_OBJECTS:unity_vendor>
    ${MUD_SRCDIR}/act.other.c
    ${MUD_SRCDIR}/lists.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_act_other.c
)
target_include_directories(test_act_other PRIVATE ${TEST_INCLUDES})
target_compile_options(test_act_other PRIVATE ${TEST_CFLAGS})
add_test(NAME test_act_other COMMAND test_act_other)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_act_other PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_act_other PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/act.other.c ${MUD_SRCDIR}/lists.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Gameplay regression tests (act.item.c, act.movement.c, act.offensive.c,
# fight.c, magic.c). Each links the production gameplay sources plus the real
# handler.c/utils.c/dg_scripts.c, and gameplay_fixture.c for DG trigger entry
# points and world setup. utils.c's rand_number(), core_dump_real(),
# basic_mud_log() and tb_error_log_at() are wrapped so tests can drive and
# observe them (tb_error_capture.h provides __wrap_tb_error_log_at).
set(GAMEPLAY_SRCS
    ${MUD_SRCDIR}/act.item.c
    ${MUD_SRCDIR}/act.movement.c
    ${MUD_SRCDIR}/act.offensive.c
    ${MUD_SRCDIR}/fight.c
    ${MUD_SRCDIR}/magic.c
    ${MUD_SRCDIR}/handler.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${MUD_SRCDIR}/interpreter.c
    ${MUD_SRCDIR}/constants.c
    ${MUD_SRCDIR}/dg_scripts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gameplay_fixture.c
)
macro(add_gameplay_test TEST_NAME TEST_SRC)
    add_mud_test(${TEST_NAME} ${GAMEPLAY_SRCS} ${TEST_SRC})
    if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${TEST_NAME} PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(${TEST_NAME} PRIVATE
            -Wl,--gc-sections
            -Wl,--wrap=tb_error_log_at
            -Wl,--wrap=rand_number
            -Wl,--wrap=core_dump_real
            -Wl,--wrap=basic_mud_log)
    endif()
    target_link_libraries(${TEST_NAME} PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})
endmacro()

add_gameplay_test(test_act_movement ${CMAKE_CURRENT_SOURCE_DIR}/test_act_movement.c)
add_gameplay_test(test_act_item ${CMAKE_CURRENT_SOURCE_DIR}/test_act_item.c)
add_gameplay_test(test_fight ${CMAKE_CURRENT_SOURCE_DIR}/test_fight.c)
add_gameplay_test(test_magic ${CMAKE_CURRENT_SOURCE_DIR}/test_magic.c)

# Production event_countdown() (mud_event.c): room-keyed event expiry.
add_mud_test(test_mud_event
    ${MUD_SRCDIR}/mud_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mud_event.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_mud_event PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_mud_event PRIVATE -Wl,--gc-sections)
endif()


# Production handler.c primitives: fname bounds, furniture back-pointers,
# affect_join overflow, obj_from_room guards.
add_mud_test(test_handler
    ${MUD_SRCDIR}/handler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_handler.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_handler PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_handler PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/handler.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production shop buyword evaluator and shop-file readers. test_shop.c
# includes shop.c to reach its file-static helpers.
add_mud_test(test_shop
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shop.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_shop PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_shop PRIVATE -Wl,--gc-sections)
endif()


# Production autoquest completion and lookups (quest.c).
add_mud_test(test_quest
    ${MUD_SRCDIR}/quest.c
    ${MUD_SRCDIR}/genqst.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_quest.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_quest PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_quest PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/quest.c ${MUD_SRCDIR}/genqst.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production point_update() object pass and gold/bank caps (limits.c).
add_mud_test(test_limits
    ${MUD_SRCDIR}/limits.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_limits.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_limits PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_limits PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/limits.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production ASCII map grid bounds and glyph lookups (asciimap.c).
add_mud_test(test_asciimap
    ${MUD_SRCDIR}/asciimap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_asciimap.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_asciimap PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_asciimap PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/asciimap.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production lists and mud events (lists.c, mud_event.c, dg_event.c): owner
# cancel loops and list bookkeeping (H3); test_mud_event covers event_countdown.
add_mud_test(test_mud_event_owners
    ${MUD_SRCDIR}/lists.c
    ${MUD_SRCDIR}/mud_event.c
    ${MUD_SRCDIR}/dg_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mud_event_owners.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_mud_event_owners PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_mud_event_owners PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/lists.c ${MUD_SRCDIR}/mud_event.c
        ${MUD_SRCDIR}/dg_event.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production mobile_activity() (mobact.c).
add_mud_test(test_mobact
    ${MUD_SRCDIR}/mobact.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mobact.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_mobact PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_mobact PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/mobact.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production spec procs (spec_procs.c).
add_mud_test(test_spec_procs
    ${MUD_SRCDIR}/spec_procs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_spec_procs.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_spec_procs PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_spec_procs PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/spec_procs.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production spec-proc name table (spec_assign.c).
add_mud_test(test_spec_assign
    ${MUD_SRCDIR}/spec_assign.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_spec_assign.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_spec_assign PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_spec_assign PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/spec_assign.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Name tables (constants.c) line up with their structs.h constants.
add_mud_test(test_constants
    ${MUD_SRCDIR}/constants.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_constants.c
)

# MEMORY_DEBUG allocator (zmalloc.c) as built: misuse is reported and aborts;
# under ASan freed blocks and guard pads are poisoned; calloc() is checked.
add_mud_test(test_zmalloc
    ${MUD_SRCDIR}/zmalloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_zmalloc.c
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_dg_memory_callback_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_memory_callback_contract.py
                ${MUD_SRCDIR}/dg_triggers.c)

    add_test(NAME test_crash_save_header_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_crash_save_header_contract.py
                ${MUD_SRCDIR}/objsave.c)
    add_test(NAME test_dg_trigedit_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_trigedit_contract.py
                ${MUD_SRCDIR}/dg_olc.c)
    add_test(NAME test_dg_objcmd_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_objcmd_contract.py
                ${MUD_SRCDIR}/dg_objcmd.c)
    add_test(NAME test_dg_transform_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_transform_contract.py
                ${MUD_SRCDIR}/dg_objcmd.c ${MUD_SRCDIR}/dg_mobcmd.c)
    add_test(NAME test_door_trigger_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_door_trigger_contract.py
                ${MUD_SRCDIR}/act.movement.c)
    add_test(NAME test_dg_detach_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_detach_contract.py
                ${MUD_SRCDIR}/dg_scripts.c)

    # Runs the built src/util tools on crafted inputs and asserts on the
    # TBERR operation each malformed input must report.
    #
    # The two extra wld2html builds below recompile the same source that
    # src/util's wld2html target already runs clang-tidy on, so tidy is
    # disabled for them (under _FORTIFY_SOURCE it would only re-report
    # memcpy as the checked builtin).
    #
    # Compile wld2html as C23 too: the autoconf build (src/util/Makefile.in)
    # passes no -std, and GCC 15 defaults to C23, where the old
    # "typedef char bool;" was a hard error (I3-13).
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21 AND
       "c_std_23" IN_LIST CMAKE_C_COMPILE_FEATURES)
        add_library(wld2html_c23 OBJECT ${MUD_SRCDIR}/util/wld2html.c)
        target_include_directories(wld2html_c23 PRIVATE ${TEST_INCLUDES})
        target_compile_definitions(wld2html_c23 PRIVATE CIRCLE_UTIL)
        set_target_properties(wld2html_c23 PROPERTIES
            C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
            C_CLANG_TIDY "")
    endif()

    # wld2html_fortified is the same source built the way distributions
    # build it (-O2 -D_FORTIFY_SOURCE=3), so glibc's object-size checks
    # catch a copy whose stated bound exceeds its real destination (I3-04).
    # Debug and sanitizer trees do not fortify, and ASan replaces the
    # checked string functions, so it is only built without sanitizers.
    set(WLD2HTML_FORTIFIED "-")
    if(NOT ENABLE_SANITIZERS AND
       CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        add_executable(wld2html_fortified ${MUD_SRCDIR}/util/wld2html.c)
        set_target_properties(wld2html_fortified PROPERTIES C_CLANG_TIDY "")
        target_include_directories(wld2html_fortified PRIVATE ${TEST_INCLUDES})
        target_compile_definitions(wld2html_fortified PRIVATE CIRCLE_UTIL)
        target_compile_options(wld2html_fortified PRIVATE
            -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
        set(WLD2HTML_FORTIFIED $<TARGET_FILE:wld2html_fortified>)
    endif()
    add_test(NAME test_util_world_tools
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_util_world_tools.py
                $<TARGET_FILE:wld2html> $<TARGET_FILE:split>
                ${WLD2HTML_FORTIFIED})
    add_test(NAME test_dg_wait_suffix_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_wait_suffix_contract.py
                ${MUD_SRCDIR}/dg_scripts.c)

    # Standalone src/util tools, run as built (sanitized in sanitizer trees)
    # on crafted input; asserts on their TBERR diagnostics.
    add_test(NAME test_util_tools
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_util_tools.py
                autowiz=$<TARGET_FILE:autowiz>
                plrtoascii=$<TARGET_FILE:plrtoascii>
                rebuildMailIndex=$<TARGET_FILE:rebuildMailIndex>
                rebuildIndex=$<TARGET_FILE:rebuildIndex>
                shopconv=$<TARGET_FILE:shopconv>
                asciipasswd=$<TARGET_FILE:asciipasswd>
                sign=$<TARGET_FILE:sign>)
endif()

# Production utils.c guards (column_list, followers, get_line, colour
# counters, strfrmt, ...): asserts each guard reports through tb_error_log_at,
# by reading the records it writes to logfile.
add_mud_test(test_utils_guards
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_utils_guards.c
)

# Source-text contract checks run CPython, not tbaMUD code: keep them out of
# Valgrind/ASan selections that use -L native.
if(Python3_Interpreter_FOUND)
    set_tests_properties(
        test_dg_memory_callback_contract test_crash_save_header_contract
        test_dg_trigedit_contract test_dg_objcmd_contract
        test_dg_transform_contract test_door_trigger_contract
        test_dg_detach_contract test_dg_wait_suffix_contract
        PROPERTIES LABELS "contract")
endif()

# ---------------------------------------------------------------------------
# Properties shared by every test registered above
# ---------------------------------------------------------------------------
# A hung test (an infinite loop in a guard, a blocked read) must fail rather
# than stall CI until the job timeout. 120 s is ~100x the slowest test under
# Valgrind today.
get_directory_property(_tb_all_tests TESTS)
set_tests_properties(${_tb_all_tests} PROPERTIES TIMEOUT 120)

# In a sanitizer build every test runs with the project's ASAN/UBSAN option
# strings (see TBAMUD_ASAN_OPTIONS in the top-level CMakeLists.txt), so a local
# ctest traps the same things CI does.
if(ENABLE_SANITIZERS)
    set_tests_properties(${_tb_all_tests} PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=${TBAMUD_ASAN_OPTIONS};UBSAN_OPTIONS=${TBAMUD_UBSAN_OPTIONS};LSAN_OPTIONS=report_objects=1")

    # End-to-end: boot the real server and require the login name prompt.
    add_test(NAME test_login_greeting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_login_greeting.py
                $<TARGET_FILE:circle>
                ${CMAKE_SOURCE_DIR}/lib)
    # End-to-end: boot the real server and look up help past the last key.
    add_test(NAME test_help_search
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_help_search.py
                $<TARGET_FILE:circle>
                ${CMAKE_SOURCE_DIR}/lib)
endif()

# ---------------------------------------------------------------------------
# test_dg_engine — the DG script engine (dg_scripts.c, dg_variables.c,
# dg_misc.c) driven through script_driver()/var_subst() with the complete
# production server linked in. comm.c's main() is renamed (see the comm.c
# source property above) and the test supplies a small hand-built world.
# utils.c is compiled separately with its logger renamed so that
# tests/tb_error_capture.h captures every structured diagnostic.
# ---------------------------------------------------------------------------
file(GLOB TB_ENGINE_SRC ${MUD_SRCDIR}/*.c)
list(REMOVE_ITEM TB_ENGINE_SRC ${MUD_SRCDIR}/utils.c)
add_library(tb_engine_utils OBJECT ${MUD_SRCDIR}/utils.c)
target_include_directories(tb_engine_utils PRIVATE ${TEST_INCLUDES})
target_compile_definitions(tb_engine_utils PRIVATE
    tb_error_log_at=tb_error_log_at_production)
set_target_properties(tb_engine_utils PROPERTIES C_CLANG_TIDY "")
add_executable(test_dg_engine
    $<TARGET_OBJECTS:unity_vendor>
    $<TARGET_OBJECTS:tb_engine_utils>
    ${TB_ENGINE_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_engine.c
)
target_include_directories(test_dg_engine PRIVATE ${TEST_INCLUDES})
target_compile_options(test_dg_engine PRIVATE ${TEST_CFLAGS})
target_link_libraries(test_dg_engine PRIVATE ${EXTRA_LIBS})
# The production sources are already analysed as part of the circle target.
set_target_properties(test_dg_engine PROPERTIES C_CLANG_TIDY "")
add_test(NAME test_dg_engine COMMAND test_dg_engine)

# ---------------------------------------------------------------------------
# test_dg_olc — the trigger editor (dg_olc.c): trigedit_save() renumbering,
# format_script() bounds and the editor's input handling, with the complete
# production server linked in exactly as for test_dg_engine.
# ---------------------------------------------------------------------------
add_executable(test_dg_olc
    $<TARGET_OBJECTS:unity_vendor>
    $<TARGET_OBJECTS:tb_engine_utils>
    ${TB_ENGINE_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_olc.c
)
target_include_directories(test_dg_olc PRIVATE ${TEST_INCLUDES})
target_compile_options(test_dg_olc PRIVATE ${TEST_CFLAGS})
target_link_libraries(test_dg_olc PRIVATE ${EXTRA_LIBS})
set_target_properties(test_dg_olc PROPERTIES C_CLANG_TIDY "")
add_test(NAME test_dg_olc COMMAND test_dg_olc)

# Production OasisOLC prototype editors (oedit/medit/redit/zedit/sedit).
# The editors reach most of the server through menus; the test supplies small
# fixtures for the tables and stubs for the generic OLC library, and discards
# unreachable sections.
add_mud_test(test_olc_editors
    ${MUD_SRCDIR}/oedit.c
    ${MUD_SRCDIR}/medit.c
    ${MUD_SRCDIR}/redit.c
    ${MUD_SRCDIR}/zedit.c
    ${MUD_SRCDIR}/sedit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_olc_editors.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_olc_editors PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_olc_editors PRIVATE -Wl,--gc-sections)
    set_source_files_properties(
        ${MUD_SRCDIR}/oedit.c ${MUD_SRCDIR}/medit.c ${MUD_SRCDIR}/redit.c
        ${MUD_SRCDIR}/zedit.c ${MUD_SRCDIR}/sedit.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production immortal commands (act.wizard.c): guards on file-loaded players,
# descriptor-less callers and out-of-range world data.
add_mud_test(test_act_wizard
    ${MUD_SRCDIR}/act.wizard.c
    ${MUD_SRCDIR}/interpreter.c
    ${MUD_SRCDIR}/constants.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_act_wizard.c
)
target_link_libraries(test_act_wizard PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_act_wizard PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_act_wizard PRIVATE -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
    set_source_files_properties(${MUD_SRCDIR}/act.wizard.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# tb_strftime() (sysdep.h): dst is a terminated string on every failure
# path, and each failure is reported at the caller (G1-19, B2-26).
add_mud_test(test_strftime
    ${CMAKE_CURRENT_SOURCE_DIR}/test_strftime.c
)

# Production manual spells (spells.c): create water refuses a liquid type
# outside the drink tables (H1-07).
add_mud_test(test_spells
    ${MUD_SRCDIR}/spells.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_spells.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_spells PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_spells PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/spells.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production call_magic() and mag_objectmagic() (spell_parser.c) with the
# production handler.c: cast triggers that purge participants (H2-01) and
# the staff's per-occupant loop (H2-10).
add_mud_test(test_spell_parser
    ${MUD_SRCDIR}/spell_parser.c
    ${MUD_SRCDIR}/handler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_spell_parser.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_spell_parser PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_spell_parser PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/spell_parser.c ${MUD_SRCDIR}/handler.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production mag_areas() (magic.c) with the production handler.c: damage
# and death triggers that teleport or purge occupants mid-spell (H1-19).
add_mud_test(test_mag_areas
    ${MUD_SRCDIR}/magic.c
    ${MUD_SRCDIR}/handler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mag_areas.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_mag_areas PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_mag_areas PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/magic.c ${MUD_SRCDIR}/handler.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# test_handler runs update_char_objects() on a small-stack thread (H2-25).
find_package(Threads REQUIRED)
target_link_libraries(test_handler PRIVATE Threads::Threads)

# test_handler also links the production lists.c for the group lists
# (H2-26).
target_sources(test_handler PRIVATE ${MUD_SRCDIR}/lists.c)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set_source_files_properties(${MUD_SRCDIR}/lists.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production social editor (aedit.c): name-length refusals and the report of
# messages a saved social cannot run without.  utils.c supplies str_cmp,
# mudlog and convert_from_tabs; the test supplies the OLC, social and
# command-table collaborators and discards the rest by section GC.
add_mud_test(test_aedit
    ${MUD_SRCDIR}/aedit.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_aedit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_aedit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_aedit PRIVATE
        -Wl,--gc-sections
        -Wl,--wrap=tb_error_log_at)
endif()
target_link_libraries(test_aedit PRIVATE ${CRYPT_LIBRARY} ${SODIUM_LIBRARY})

# C2-16: test_genolc_safety wraps fopen() so a test can hand the GenOLC
# writers a stream whose error indicator is already set.
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_link_options(test_genolc_safety PRIVATE -Wl,--wrap=fopen)
endif()

# C1-29: test_genmob wraps fopen() the same way for save_mobiles().
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_link_options(test_genmob PRIVATE -Wl,--wrap=fopen)
endif()

# Production zone and editor access checks (oasis.c can_edit_zone and
# can_use_editor): bounded zone_table index, editor sentinels kept out of
# the real zone range (F3-20). Unreferenced oasis.c sections are discarded;
# tb_error_capture.h in the test supplies tb_error_log_at.
add_mud_test(test_oasis_access
    ${MUD_SRCDIR}/oasis.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_oasis_access.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_oasis_access PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_oasis_access PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/oasis.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# test_rnum_macros — the VALID_*_RNUM checks (utils.h tb_rnum_valid and the
# checks built on it in genzon.h, oasis.h, olc_checks.h and dg_scripts.h): a
# negative value of any integer type is refused before a table is indexed,
# the sentinels never pass, and VALID_TRIG_RNUM never reads trig_index[-1].
# The test source is compiled with -Wtype-limits -Werror: the checks must
# keep comparing as long (no "always true" warning for the unsigned rnum
# types), and a VALID_ZONE_RNUM copy that drifts from utils.h's ("redefined")
# fails the build.
add_mud_test(test_rnum_macros
    ${CMAKE_CURRENT_SOURCE_DIR}/test_rnum_macros.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/test_rnum_macros.c
        PROPERTIES COMPILE_OPTIONS "-Wtype-limits;-Werror")
endif()

# Leftovers O6c/O6d: the production text-file editor (tedit.c) and
# combat-message editor (msgedit.c).  A live text must never become NULL and
# a failed save must never stop the server; the test supplies the editor,
# OLC and output collaborators and discards the rest by section GC.
add_mud_test(test_tedit_msgedit
    ${MUD_SRCDIR}/tedit.c
    ${MUD_SRCDIR}/msgedit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tedit_msgedit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_tedit_msgedit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_tedit_msgedit PRIVATE -Wl,--gc-sections)
endif()

# test_tedit_msgedit wraps fopen() so a test can hand the tedit and msgedit
# savers a stream whose error indicator is already set (the C2-16 check).
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_link_options(test_tedit_msgedit PRIVATE -Wl,--wrap=fopen)
endif()
