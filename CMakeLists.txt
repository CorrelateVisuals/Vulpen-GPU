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
# The same input gives the same bits on every machine (C01, C10). GCC fuses a*b+c into
# one rounding wherever the CPU has FMA, as ARM64 always does, so release would differ
# from debug and from x86; MSVC does not fuse by default.
if(NOT MSVC)
  add_compile_options(-ffp-contract=off)
endif()
# Standard containers check their bounds in a debug build (A02).
add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
# The asan and tsan presets (docs/plans/tests.md): a sanitizer report ends the run.
set(VULPEN_SANITIZERS "" CACHE STRING "Sanitizers to build with, as -fsanitize names them")
if(VULPEN_SANITIZERS)
  add_compile_options(-fsanitize=${VULPEN_SANITIZERS} -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${VULPEN_SANITIZERS})
endif()

# The baseclasses, the GPU layer, are a library of their own, which vulpen and the
# baseclasses' triangle link, so they build and run without the runtime (RA00). An object
# library, as an archive would leave out the objects vulpen itself never calls, which a
# live build's modules may call (src/runtime/cmake/nodes.cmake).
add_library(baseclasses OBJECT
  src/baseclasses/Engine.cpp
  src/baseclasses/Log.cpp
  src/baseclasses/Mechanics.cpp
  src/baseclasses/Passes.cpp
  src/baseclasses/Pipelines.cpp
  src/baseclasses/Platform.cpp
  src/baseclasses/Resources.cpp
  src/baseclasses/Shader.cpp
  src/baseclasses/Swapchain.cpp)
target_compile_options(baseclasses PRIVATE "${warnings}")
target_include_directories(baseclasses PUBLIC src)
# Vendored code is not ours to warn about (RA02).
target_include_directories(baseclasses SYSTEM PRIVATE src/external-libraries)
target_link_libraries(baseclasses PUBLIC Vulkan::Vulkan PRIVATE glfw ${CMAKE_DL_LIBS})
set_target_properties(baseclasses PROPERTIES COMPILE_WARNING_AS_ERROR ON)

add_executable(vulpen
  src/main.cpp
  src/runtime/Binder.cpp
  src/runtime/Checks.cpp
  src/runtime/Commands.cpp
  src/runtime/Edits.cpp
  src/runtime/Manifest.cpp
  src/runtime/Modules.cpp
  src/runtime/Ports.cpp
  src/runtime/Runtime.cpp
  src/runtime/Schedule.cpp
  src/runtime/Views.cpp)
target_compile_options(vulpen PRIVATE "${warnings}")
target_include_directories(vulpen SYSTEM PRIVATE src/external-libraries)
# GLFW builds from its pinned source; the vendored copy leaves out its docs, tests and
# examples, so they must stay off.
set(GLFW_BUILD_DOCS OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_INSTALL OFF)
add_subdirectory(src/external-libraries/glfw EXCLUDE_FROM_ALL)
target_link_libraries(vulpen PRIVATE baseclasses)
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
# built; mutable-globals reads the objects, so it runs after the link (C13). Statistics run
# after the link at every build, so docs/statistics/ never goes stale; only a release
# build is timed, since debug timings say nothing about what ships.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(tools ${PROJECT_SOURCE_DIR}/src/tools)
add_custom_target(gates COMMAND ${Python3_EXECUTABLE} ${tools}/gates.py ${CMAKE_BINARY_DIR}
                  VERBATIM)
add_dependencies(baseclasses gates)
add_dependencies(vulpen gates)
if(NOT MSVC) # it reads the objects with objdump
  add_custom_target(globals ALL
    COMMAND ${Python3_EXECUTABLE} ${tools}/mutable-globals.py ${CMAKE_BINARY_DIR} VERBATIM)
  # barriers: src/tools/tests/tests.cmake
  add_dependencies(globals vulpen baseclasses-triangle barriers)
endif()
add_custom_target(statistics ALL
  COMMAND ${Python3_EXECUTABLE} ${tools}/fetch-statistics.py
          "$<$<CONFIG:Release>:--binary;$<TARGET_FILE:vulpen>>"
  COMMAND_EXPAND_LISTS VERBATIM)
add_dependencies(statistics vulpen)

# GLSL to SPIR-V at <out>/<name>.spv, as Shader loads it; the arguments after out are the
# compiler's include flags. A driver may take invalid SPIR-V without a word, or crash on
# it, so <name>.spv.valid, which a target depends on, exists only once spirv-val passed,
# and the build fails while any shader does not (RV02).
find_program(GLSLANG glslangValidator REQUIRED)
find_program(SPIRV_VAL spirv-val REQUIRED)
function(vulpen_spirv shader out)
  get_filename_component(name ${shader} NAME)
  add_custom_command(OUTPUT ${out}/${name}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out}
    COMMAND ${GLSLANG} --target-env vulkan1.2 --quiet ${ARGN}
            --depfile ${out}/${name}.d -o ${out}/${name}.spv ${shader}
    DEPENDS ${shader} DEPFILE ${out}/${name}.d VERBATIM)
  add_custom_command(OUTPUT ${out}/${name}.spv.valid
    COMMAND ${SPIRV_VAL} --target-env vulkan1.2 ${out}/${name}.spv
    COMMAND ${CMAKE_COMMAND} -E touch ${out}/${name}.spv.valid
    DEPENDS ${out}/${name}.spv VERBATIM)
endfunction()

# The baseclasses' triangle, with its shaders beside it, where it finds them (RP02).
set(triangle ${PROJECT_SOURCE_DIR}/src/baseclasses/examples/triangle)
set(triangle_out ${CMAKE_BINARY_DIR}/baseclasses/examples/triangle)
vulpen_spirv(${triangle}/Triangle.vert ${triangle_out})
vulpen_spirv(${triangle}/Triangle.frag ${triangle_out})
add_executable(baseclasses-triangle ${triangle}/main.cpp
  ${triangle_out}/Triangle.vert.spv.valid ${triangle_out}/Triangle.frag.spv.valid)
target_compile_options(baseclasses-triangle PRIVATE "${warnings}")
target_include_directories(baseclasses-triangle PRIVATE ${generated})
target_link_libraries(baseclasses-triangle PRIVATE baseclasses)
set_target_properties(baseclasses-triangle PROPERTIES COMPILE_WARNING_AS_ERROR ON
  OUTPUT_NAME triangle RUNTIME_OUTPUT_DIRECTORY ${triangle_out}$<0:>)
add_dependencies(baseclasses-triangle commit)

# Every folder under src/runtime/examples/ and projects/ with a view.vlp is a view the
# build compiles; a project made while vulpen runs builds at the next live build.
include(src/runtime/cmake/nodes.cmake)
file(GLOB views LIST_DIRECTORIES true CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/src/runtime/examples/* ${PROJECT_SOURCE_DIR}/projects/*)
foreach(view IN LISTS views)
  if(EXISTS ${view}/view.vlp)
    vulpen_view(${view})
  endif()
endforeach()
# The nodes the fail-loud test gets wrong on purpose. They build only as modules, so no
# test code links into a release binary.
if(VULPEN_LIVE)
  vulpen_view(${PROJECT_SOURCE_DIR}/src/tools/tests/mistakes)
endif()
vulpen_recipes(${PROJECT_SOURCE_DIR}/src/runtime/recipes)
vulpen_link_modules(${generated}/linked-modules.h)

include(src/tools/tests/tests.cmake)
