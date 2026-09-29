cmake_minimum_required(VERSION 3.24)
project(vulpen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vulpen src/main.cpp)
target_compile_options(vulpen PRIVATE "$<IF:$<BOOL:${MSVC}>,/W4,-Wall;-Wextra;-Wpedantic>")
# The $<0:> stops multi-config generators (Visual Studio) from adding a Debug/ or
# Release/ folder, so the run scripts find the binary at out/build/<preset>/.
set_target_properties(vulpen PROPERTIES COMPILE_WARNING_AS_ERROR ON
                                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}$<0:>)

# Stamped at every build, not at configure: a configure-time stamp keeps naming
# the old commit after the next `git commit`, and never sees a dirty tree.
set(generated ${CMAKE_BINARY_DIR}/generated)
add_custom_target(commit
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${generated}/commit.h
          -P ${PROJECT_SOURCE_DIR}/src/runtime/cmake/commit.cmake
  BYPRODUCTS ${generated}/commit.h
  VERBATIM)
add_dependencies(vulpen commit)
target_include_directories(vulpen PRIVATE ${generated})

# Gates run before the compile, so a broken rule fails the build before any object is
# built. Statistics run after the link at every build, so docs/statistics/ never goes
# stale; only a release build is timed, since debug timings say nothing about what ships.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(tools ${PROJECT_SOURCE_DIR}/src/tools)
add_custom_target(gates COMMAND ${Python3_EXECUTABLE} ${tools}/gates.py VERBATIM)
add_dependencies(vulpen gates)
add_custom_target(statistics ALL
  COMMAND ${Python3_EXECUTABLE} ${tools}/fetch-statistics.py
          "$<$<CONFIG:Release>:--binary;$<TARGET_FILE:vulpen>>"
  COMMAND_EXPAND_LISTS VERBATIM)
add_dependencies(statistics vulpen)

enable_testing()
add_test(NAME vulpen COMMAND vulpen)
