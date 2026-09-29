cmake_minimum_required(VERSION 3.24)
project(vulpen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
# CPP11 fills Vulkan structs with designated initializers and leaves the zero fields out,
# which GCC's -Wextra would flag in every one of them.
set(warnings
    "$<IF:$<BOOL:${MSVC}>,/W4,-Wall;-Wextra;-Wpedantic;-Wno-missing-field-initializers>")

add_executable(vulpen
  src/main.cpp
  src/baseclasses/Engine.cpp
  src/baseclasses/Mechanics.cpp
  src/baseclasses/Pipelines.cpp
  src/baseclasses/Platform.cpp
  src/baseclasses/Resources.cpp
  src/baseclasses/Swapchain.cpp
  src/runtime/Manifest.cpp
  src/runtime/Recipes.cpp
  src/runtime/Runtime.cpp
  src/runtime/Schedule.cpp)
target_compile_options(vulpen PRIVATE "${warnings}")
target_include_directories(vulpen PRIVATE src)
# Vendored code is not ours to warn about (RA02).
target_include_directories(vulpen SYSTEM PRIVATE src/external-libraries)
# GLFW builds from its pinned source; the vendored copy leaves out its docs, tests and
# examples, so they must stay off.
set(GLFW_BUILD_DOCS OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_INSTALL OFF)
add_subdirectory(src/external-libraries/glfw EXCLUDE_FROM_ALL)
target_link_libraries(vulpen PRIVATE Vulkan::Vulkan glfw ${CMAKE_DL_LIBS})
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

# Every folder under src/examples/ with a view.vlp is a view the build compiles.
include(src/runtime/cmake/recipe.cmake)
file(GLOB examples LIST_DIRECTORIES true CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/examples/*)
foreach(example IN LISTS examples)
  if(EXISTS ${example}/view.vlp)
    vulpen_view(${example})
  endif()
endforeach()
vulpen_link_recipes(${generated}/linked-recipes.h)

enable_testing()
add_test(NAME vulpen COMMAND vulpen)
add_test(NAME wave COMMAND vulpen ${PROJECT_SOURCE_DIR}/src/examples/wave/view.vlp
                           --frames 120 --fps 0)
# Opens a window, so it needs a display.
add_test(NAME triangle COMMAND vulpen ${PROJECT_SOURCE_DIR}/src/examples/triangle/view.vlp
                               --frames 120 --fps 0)
