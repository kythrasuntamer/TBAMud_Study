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
    add_test(NAME test_dg_wait_suffix_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dg_wait_suffix_contract.py
                ${MUD_SRCDIR}/dg_scripts.c)
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
