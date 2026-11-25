# The bench (src/tools/bench/bench.py): a binary per case in cases/, each linking
# the bench's core and every engine object but the one its case includes. A case named
# like an engine .cpp includes that file, so it reaches the file's unnamed namespace,
# compiled as the engine compiles it; the engine's own objects stay as the engine builds
# them. Outside ALL, so a build of vulpen never waits for it; bench.py builds the review
# target. Included by the top-level CMakeLists.txt once every target is defined.
set(bench ${PROJECT_SOURCE_DIR}/src/tools/bench)
add_library(bench-core OBJECT EXCLUDE_FROM_ALL ${bench}/Bench.cpp
                                                ${bench}/main.cpp)
target_include_directories(bench-core PUBLIC ${PROJECT_SOURCE_DIR}/src ${generated})
target_include_directories(bench-core SYSTEM PUBLIC
                           ${PROJECT_SOURCE_DIR}/src/external-libraries)
target_link_libraries(bench-core PUBLIC Vulkan::Vulkan)
target_compile_options(bench-core PUBLIC "${warnings}")
set_target_properties(bench-core PROPERTIES COMPILE_WARNING_AS_ERROR ON)
# A sanitizer's build counts no allocation (bench/main.cpp says why).
set(bench_counts 1)
if(VULPEN_SANITIZERS)
  set(bench_counts 0)
endif()
target_compile_definitions(bench-core PRIVATE VP_BENCH_COUNTS=${bench_counts})
# The bench reads the environment by std::getenv, as standard C++ has it (C11), which
# MSVC calls unsafe, in favour of a call of its own.
target_compile_definitions(bench-core PUBLIC
                           $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
add_dependencies(bench-core gates commit)

# A build that links the nodes into vulpen, as release does, links them into each bench
# too, since Modules.cpp names them.
get_property(linked GLOBAL PROPERTY vulpen_linked)
set(bench_nodes "")
foreach(entry IN LISTS linked)
  string(REGEX REPLACE ".*=vp_module_" "" target ${entry})
  list(APPEND bench_nodes $<TARGET_OBJECTS:${target}>)
endforeach()

add_custom_target(bench)
# The benches' folder holds the build's module stamp, as vulpen's does, so a bench tells a
# module a build makes again from one built against other engine headers, as vulpen does.
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/bench/${module_stamp_file}
     CONTENT "${module_stamp}\n")
file(GLOB bench_cases CONFIGURE_DEPENDS ${bench}/cases/*.cpp)
foreach(case IN LISTS bench_cases)
  get_filename_component(name ${case} NAME_WE)
  set(engine "$<TARGET_OBJECTS:baseclasses>;$<TARGET_OBJECTS:runtime>")
  foreach(folder baseclasses runtime)
    if(EXISTS ${PROJECT_SOURCE_DIR}/src/${folder}/${name}.cpp)
      set(engine "$<FILTER:${engine},EXCLUDE,/${folder}/${name}\\.cpp\\.o(bj)?$>")
    endif()
  endforeach()
  add_executable(bench-${name} EXCLUDE_FROM_ALL ${case} "${engine}" ${bench_nodes})
  # The case includes its engine .cpp, whose classes may hold types of its unnamed
  # namespace, which GCC warns of in any file but the one compiled.
  target_compile_options(bench-${name} PRIVATE
                         $<$<CXX_COMPILER_ID:GNU>:-Wno-subobject-linkage>)
  target_link_libraries(bench-${name} PRIVATE bench-core glfw ${CMAKE_DL_LIBS})
  # The engine's objects hold the speakers' sokol_audio, which calls PulseAudio where
  # baseclasses links it (CMakeLists.txt), so a bench links it too.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND VULPEN_PULSE_INCLUDE AND VULPEN_PULSE_SIMPLE
     AND VULPEN_PULSE)
    target_link_libraries(bench-${name} PRIVATE ${VULPEN_PULSE_SIMPLE} ${VULPEN_PULSE})
  endif()
  foreach(library baseclasses runtime)
    target_compile_definitions(bench-${name} PRIVATE
                               $<TARGET_PROPERTY:${library},COMPILE_DEFINITIONS>)
  endforeach()
  # As vulpen exports its symbols to the node modules a live build loads, which some
  # call, a bench that loads them does.
  set_target_properties(bench-${name} PROPERTIES COMPILE_WARNING_AS_ERROR ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bench$<0:>
    ENABLE_EXPORTS ${VULPEN_LIVE})
  add_dependencies(bench bench-${name})
endforeach()
