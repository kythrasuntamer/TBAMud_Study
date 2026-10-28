# tests/dg_world/CMakeLists.txt
# Whole-server DG command tests.
#
# The DG command files (dg_mobcmd.c, dg_objcmd.c, dg_wldcmd.c) call into
# almost every handler: object/character placement, extraction, read_object,
# the lookup table and the script driver. Stubbing those would test the stubs,
# so these tests link the real server objects (every src/*.c, comm.c's main()
# renamed) and build a tiny in-memory world in the test (dg_world_fixture.h).
#
# Kept in its own directory because source-file properties are directory
# scoped: renaming main() here must not affect the other test targets.
#
# tb_error_log_at() is wrapped at link time (-Wl,--wrap) so the tests can
# assert on the structured diagnostic each guard emits.

set(DG_WORLD_SRCDIR ${CMAKE_SOURCE_DIR}/src)
file(GLOB DG_WORLD_SERVER_SRC ${DG_WORLD_SRCDIR}/*.c)

add_library(dg_world_server OBJECT ${DG_WORLD_SERVER_SRC})
target_include_directories(dg_world_server PRIVATE ${DG_WORLD_SRCDIR} ${CMAKE_BINARY_DIR})
# The same sources are already analysed as part of the circle target.
set_target_properties(dg_world_server PROPERTIES C_CLANG_TIDY "")
set_source_files_properties(${DG_WORLD_SRCDIR}/comm.c
    PROPERTIES COMPILE_DEFINITIONS "main=tb_comm_main_unused")

macro(add_dg_world_test TEST_NAME)
    add_executable(${TEST_NAME}
        $<TARGET_OBJECTS:unity_vendor>
        $<TARGET_OBJECTS:dg_world_server>
        ${CMAKE_CURRENT_SOURCE_DIR}/../${TEST_NAME}.c
    )
    target_include_directories(${TEST_NAME} PRIVATE
        ${DG_WORLD_SRCDIR} ${CMAKE_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/unity)
    target_link_options(${TEST_NAME} PRIVATE "LINKER:--wrap=tb_error_log_at")
    target_link_libraries(${TEST_NAME} PRIVATE ${EXTRA_LIBS})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro()

add_dg_world_test(test_dg_objcmd_runtime)
