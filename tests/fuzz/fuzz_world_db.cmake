# Make a copy of src/db.c whose record cursors the world-file fuzzer can reset.
#
#   cmake -DIN=<src/db.c> -DOUT=<build>/fuzz_world_db.c -P fuzz_world_db.cmake
#
# parse_room(), parse_mobile(), parse_object() and load_zones() keep their
# slot cursor in a function-local static that only ever moves forward: the
# server boots once, so they never need resetting. A fuzzer boots millions of
# tiny worlds in one process, and a cursor that keeps growing would index past
# any fixture table (object and mob rnums are 16-bit, so it cannot simply be
# offset). Each cursor declaration below gets FUZZ_WORLD_CURSOR(...) appended
# ON THE SAME LINE: the harness defines it to zero the cursor when it starts a
# new input, exactly as a fresh boot would. Nothing else changes, and a #line
# directive maps every line back to src/db.c, so diagnostics, sanitizer stack
# traces and __FILE__ name the real file and line.
#
# Each pattern must match exactly once; if db.c is edited so that one no longer
# does, the build fails here with the pattern named instead of silently
# fuzzing a harness whose cursors never reset.

if(NOT IN OR NOT OUT)
  message(FATAL_ERROR "fuzz_world_db.cmake: pass -DIN=<db.c> -DOUT=<copy>")
endif()

file(READ "${IN}" text)

# Pattern (with enough context to be unique) and the text to put after it.
# Separate variables, not a list: the C text contains ';', which CMake lists
# would split on.
set(_pat0 "void parse_room(FILE *fl, int virtual_nr)\n{\n  static int room_nr = 0, zone = 0;")
set(_set0 " FUZZ_WORLD_CURSOR(room, room_nr = 0; zone = 0;)")
set(_pat1 "void parse_mobile(FILE *mob_f, int nr)\n{\n  static int i = 0;")
set(_set1 " FUZZ_WORLD_CURSOR(mob, i = 0;)")
set(_pat2 "char *parse_object(FILE *obj_f, int nr)\n{\n  static int i = 0;")
set(_set2 " FUZZ_WORLD_CURSOR(obj, i = 0;)")
set(_pat3 "static void load_zones(FILE *fl, char *zonename)\n{\n  static zone_rnum zone = 0;")
set(_set3 " FUZZ_WORLD_CURSOR(zone, zone = 0;)")

foreach(_k 0 1 2 3)
  set(_pat "${_pat${_k}}")
  set(_reset "${_set${_k}}")
  string(FIND "${text}" "${_pat}" _first)
  string(FIND "${text}" "${_pat}" _again REVERSE)
  if(_first EQUAL -1)
    message(FATAL_ERROR "fuzz_world_db.cmake: cursor declaration not found in ${IN}:\n${_pat}\n"
                        "Update the pattern in tests/fuzz/fuzz_world_db.cmake.")
  endif()
  if(NOT _first EQUAL _again)
    message(FATAL_ERROR "fuzz_world_db.cmake: cursor declaration is not unique in ${IN}:\n${_pat}")
  endif()
  string(REPLACE "${_pat}" "${_pat}${_reset}" text "${text}")
endforeach()

file(WRITE "${OUT}.tmp" "#line 1 \"${IN}\"\n${text}")
file(RENAME "${OUT}.tmp" "${OUT}")
