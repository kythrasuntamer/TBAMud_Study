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

# ---------------------------------------------------------------------------
# test_class — covers src/class.c
# ---------------------------------------------------------------------------
add_mud_test(test_class
    ${MUD_SRCDIR}/class.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_class.c
)


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

# Production social editor (aedit.c).
add_mud_test(test_aedit
    ${MUD_SRCDIR}/aedit.c
    ${MUD_SRCDIR}/constants.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_aedit.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_aedit PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_aedit PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${MUD_SRCDIR}/aedit.c
        PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
endif()

# Production zone-command editing (genzon.c).
add_mud_test(test_genzon
    ${MUD_SRCDIR}/genzon.c
    ${MUD_SRCDIR}/utils.c
    ${MUD_SRCDIR}/random.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_genzon.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_genzon PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_genzon PRIVATE -Wl,--gc-sections)
    # Fill uninitialised locals with a pattern so a field left unset shows
    # up as garbage rather than as a lucky zero.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-ftrivial-auto-var-init=pattern TB_HAVE_AUTO_VAR_INIT)
    if(TB_HAVE_AUTO_VAR_INIT)
        set(TB_GENZON_TEST_FLAGS "-ffunction-sections;-fdata-sections;-ftrivial-auto-var-init=pattern")
    else()
        set(TB_GENZON_TEST_FLAGS "-ffunction-sections;-fdata-sections")
    endif()
    set_source_files_properties(${MUD_SRCDIR}/genzon.c
        PROPERTIES COMPILE_OPTIONS "${TB_GENZON_TEST_FLAGS}")
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

# Boot the shipped world in syntax-check mode (circle -c): loads every zone,
# room, object, mob, shop, trigger and quest, then frees it all.  Under the
# sanitizer builds this catches load and shutdown memory errors (it found a
# read of the freed world in destroy_db()).
add_test(NAME test_world_boot
    COMMAND $<TARGET_FILE:circle> -c -d ${CMAKE_SOURCE_DIR}/lib)

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
    add_test(NAME test_olc_cleanup_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_olc_cleanup_contract.py
                ${MUD_SRCDIR})
    add_test(NAME test_olc_entry_guard_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_olc_entry_guard_contract.py
                ${MUD_SRCDIR})
    add_test(NAME test_iedit_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_iedit_contract.py
                ${MUD_SRCDIR}/oasis.h ${MUD_SRCDIR}/oedit.c)
    add_test(NAME test_mob_load_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mob_load_contract.py
                ${MUD_SRCDIR}/db.c)
    add_test(NAME test_zedit_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_zedit_contract.py
                ${MUD_SRCDIR}/zedit.c)
    add_test(NAME test_olc_delete_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_olc_delete_contract.py
                ${MUD_SRCDIR})
    add_test(NAME test_quest_type_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_quest_type_contract.py
                ${MUD_SRCDIR}/quest.h ${MUD_SRCDIR}/qedit.c)
    add_test(NAME test_oasis_list_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_oasis_list_contract.py
                ${MUD_SRCDIR}/oasis_list.c)
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
    # End-to-end: boot the real server and require the login name prompt.
    add_test(NAME test_login_greeting
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_login_greeting.py
                $<TARGET_FILE:circle>
                ${CMAKE_SOURCE_DIR}/lib)
endif()
