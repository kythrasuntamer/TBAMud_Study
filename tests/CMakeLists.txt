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

# Production lists and mud events (lists.c, mud_event.c, dg_event.c).
add_mud_test(test_mud_event
    ${MUD_SRCDIR}/lists.c
    ${MUD_SRCDIR}/mud_event.c
    ${MUD_SRCDIR}/dg_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mud_event.c
)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(test_mud_event PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_mud_event PRIVATE -Wl,--gc-sections)
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
