# The bench (src/tools/bench/bench.py): a binary per case in cases/, each linking
# the bench's core and every engine object but the one its case includes. A case named
# like an engine .cpp includes that file, so it reaches the file's unnamed namespace,
# compiled as the engine compiles it; the engine's own objects stay as the engine builds
# them. A case named like a header of the library's shared code includes it, as a part
# does, and a case in cases/parts/ includes a part's C++. Outside ALL, so a build of
# vulpen never waits for it; bench.py builds the review target. Included by the
# top-level CMakeLists.txt once every target is defined.
set(bench ${PROJECT_SOURCE_DIR}/src/tools/bench)
set(library ${PROJECT_SOURCE_DIR}/src/runtime/recipes/vulpen/library)
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
file(GLOB part_cases CONFIGURE_DEPENDS ${bench}/cases/parts/*.cpp)
foreach(case IN LISTS bench_cases part_cases)
  get_filename_component(name ${case} NAME_WE)
  # A part's case is named for the part's folder, a module of its own (RV08), and its
  # bench for the part, as a part's folder may share a name with an engine file.
  set(part OFF)
  if(case IN_LIST part_cases)
    set(part ON)
    set(name part-${name})
  endif()
  set(engine "$<TARGET_OBJECTS:baseclasses>;$<TARGET_OBJECTS:runtime>")
  # The engine's object of the file the case includes, as each generator names it:
  # Makefiles and Ninja by the source's path, runtime/Live.cpp.o, and Visual Studio by
  # its name in the target's folder, runtime.dir/Debug/Live.obj.
  foreach(folder baseclasses runtime)
    if(EXISTS ${PROJECT_SOURCE_DIR}/src/${folder}/${name}.cpp)
      set(object "(/${folder}/${name}\\.cpp|/${folder}\\.dir/[^/]+/${name})\\.o(bj)?$")
      set(engine "$<FILTER:${engine},EXCLUDE,${object}>")
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
  foreach(engine_library baseclasses runtime)
    target_compile_definitions(bench-${name} PRIVATE
                               $<TARGET_PROPERTY:${engine_library},COMPILE_DEFINITIONS>)
  endforeach()
  # contracts/ and shared/ resolve from the library, as for its parts (nodes.cmake), and
  # the contracts' types go in a namespace of the bench's own, as each view's go in its
  # own, so a release build's linked parts keep theirs.
  if(part OR EXISTS ${library}/parts/shared/${name}.h)
    target_include_directories(bench-${name} PRIVATE ${library} ${library}/parts)
    target_compile_definitions(bench-${name} PRIVATE VP_VIEW=vp_bench)
  endif()
  # A part's one exported symbol registers its operators, named here as a release build
  # names a linked part's, so it meets none of theirs.
  if(part)
    target_compile_definitions(bench-${name} PRIVATE VP_MODULE_ENTRY=vp_bench_part
                                                     VP_MODULE_EXPORT=)
  endif()
  # As vulpen exports its symbols to the node modules a live build loads, which some
  # call, a bench that loads them does.
  set_target_properties(bench-${name} PROPERTIES COMPILE_WARNING_AS_ERROR ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bench$<0:>
    ENABLE_EXPORTS ${VULPEN_LIVE})
  add_dependencies(bench bench-${name})
endforeach()
