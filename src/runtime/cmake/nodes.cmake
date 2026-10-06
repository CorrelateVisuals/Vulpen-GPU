# The build side of live code (docs/plans/live-code.md). Each node is a folder (RV08),
# which compiles its shaders to SPIR-V and its C++ to one module (live) or into vulpen
# (release), at paths that mirror the view under <build>/views/, so the runtime finds them
# by path alone. Included by the top-level CMakeLists.txt after the vulpen and gates
# targets.
find_program(GLSLANG glslangValidator REQUIRED)
find_program(SPIRV_VAL spirv-val REQUIRED)

string(COMPARE EQUAL "${CMAKE_BUILD_TYPE}" Debug live_default)
option(VULPEN_LIVE "Swap the nodes' C++ and GLSL while vulpen runs" ${live_default})

# Every engine header, hashed into a module's entry name: a module built against other
# headers than the running vulpen then fails to load, naming why, instead of crashing it.
file(GLOB engine_headers CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/src/baseclasses/*.h ${PROJECT_SOURCE_DIR}/src/runtime/*.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${engine_headers})
set(hashes "")
foreach(header IN LISTS engine_headers)
  file(SHA256 ${header} hash)
  string(APPEND hashes ${hash})
endforeach()
string(SHA256 stamp "${hashes}")
string(SUBSTRING ${stamp} 0 16 module_stamp)

# How a module marks its entry exported: a DLL exports only what is marked so, and GCC
# and Clang, told to hide every symbol, export what is marked visible.
if(WIN32)
  set(module_export "__declspec(dllexport)")
else()
  set(module_export "[[gnu::visibility(\"default\")]]")
endif()

# A node's module may call the engine by hand (native C++), so a live build's vulpen
# exports its symbols to the modules it loads; a release build links the nodes in.
if(VULPEN_LIVE)
  set_target_properties(vulpen PROPERTIES ENABLE_EXPORTS ON)
endif()

# What a live build rebuilds; the include map guards it like every build (A00).
add_custom_target(vulpen_modules)
add_dependencies(vulpen_modules gates)
add_dependencies(vulpen vulpen_modules)
target_compile_definitions(vulpen PRIVATE
  VP_LIVE=$<BOOL:${VULPEN_LIVE}>
  VP_CMAKE_COMMAND="${CMAKE_COMMAND}"
  VP_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}"
  VP_MODULE_STAMP="${module_stamp}")

# A view: each folder in it is a node, but its contracts/ (RV05).
function(vulpen_view folder)
  get_filename_component(view ${folder} NAME)
  vulpen_nodes(${view} ${folder} ${folder})
endfunction()

# The library (V02) builds as one view named library, which runtime/Manifest.cpp names the
# same: each recipe of its kinds (RV06) is a node's folder, so an app runs from the
# library.
function(vulpen_library folder)
  foreach(kind parts components apps)
    file(GLOB recipes LIST_DIRECTORIES true CONFIGURE_DEPENDS ${folder}/${kind}/*)
    foreach(recipe IN LISTS recipes)
      if(IS_DIRECTORY ${recipe})
        vulpen_node(library ${recipe} ${folder})
        vulpen_nodes(library ${recipe} ${folder})
      endif()
    endforeach()
  endforeach()
endfunction()

# The nodes inside a folder: each folder of it, and the nodes inside those. A folder with
# a view.vlp of its own is a view the first hosts (V03), and builds as a view.
# root: the view's folder, or the library's, where contracts/ is (RV05).
function(vulpen_nodes view folder root)
  file(GLOB entries LIST_DIRECTORIES true CONFIGURE_DEPENDS ${folder}/*)
  foreach(entry IN LISTS entries)
    get_filename_component(name ${entry} NAME)
    if(NOT IS_DIRECTORY ${entry} OR name MATCHES "^\\." OR entry STREQUAL "${root}/contracts")
      continue()
    endif()
    if(EXISTS ${entry}/view.vlp)
      vulpen_view(${entry})
    else()
      vulpen_node(${view} ${entry} ${root})
      vulpen_nodes(${view} ${entry} ${root})
    endif()
  endforeach()
endfunction()

# One node's folder: its shaders and its C++, which builds as one module.
function(vulpen_node view folder root)
  file(RELATIVE_PATH relative ${root} ${folder})
  string(MAKE_C_IDENTIFIER "${view}_${relative}" target)
  set(out ${CMAKE_BINARY_DIR}/views/${view}/${relative})

  file(GLOB shaders CONFIGURE_DEPENDS ${folder}/*.comp ${folder}/*.vert ${folder}/*.frag)
  set(spirv "")
  foreach(shader IN LISTS shaders)
    get_filename_component(name ${shader} NAME)
    add_custom_command(OUTPUT ${out}/${name}.spv
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out}
      COMMAND ${GLSLANG} --target-env vulkan1.2 --quiet -I${PROJECT_SOURCE_DIR}/src -I${root}
              --depfile ${out}/${name}.d -o ${out}/${name}.spv ${shader}
      DEPENDS ${shader} DEPFILE ${out}/${name}.d VERBATIM)
    # A driver may take invalid SPIR-V without a word, or crash on it. The stamp exists
    # only once spirv-val passed, so the build fails while any shader does not (RV02).
    add_custom_command(OUTPUT ${out}/${name}.spv.valid
      COMMAND ${SPIRV_VAL} --target-env vulkan1.2 ${out}/${name}.spv
      COMMAND ${CMAKE_COMMAND} -E touch ${out}/${name}.spv.valid
      DEPENDS ${out}/${name}.spv VERBATIM)
    list(APPEND spirv ${out}/${name}.spv.valid)
  endforeach()
  if(spirv)
    add_custom_target(${target}_spirv DEPENDS ${spirv})
    add_dependencies(vulpen_modules ${target}_spirv)
  endif()

  file(GLOB sources CONFIGURE_DEPENDS ${folder}/*.cpp)
  if(NOT sources)
    return()
  endif()
  if(VULPEN_LIVE)
    add_library(${target} MODULE ${sources})
    # Every module exports one entry name, each in its own symbol scope. Without
    # -fno-gnu-unique, GCC's unique symbols keep a module mapped after dlclose, and a
    # swap would silently run the old code. Every module is named module, so the import
    # library a DLL comes with lands in its own folder too, not in one all of them share.
    set_target_properties(${target} PROPERTIES
      PREFIX "" OUTPUT_NAME module LIBRARY_OUTPUT_DIRECTORY ${out}$<0:>
      ARCHIVE_OUTPUT_DIRECTORY ${out}$<0:>
      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
    target_compile_definitions(${target} PRIVATE
      VP_MODULE_ENTRY=vp_module_${module_stamp} "VP_MODULE_EXPORT=${module_export}")
  else()
    add_library(${target} OBJECT ${sources})
    target_link_libraries(vulpen PRIVATE ${target})
    target_compile_definitions(${target} PRIVATE
      VP_MODULE_ENTRY=vp_module_${target} VP_MODULE_EXPORT=)
    set_property(GLOBAL APPEND PROPERTY vulpen_linked "${view}/${relative}=vp_module_${target}")
  endif()
  # contracts/ resolves from the top of the view or the library, as it does for GLSL, and
  # its C++ types live in namespace VP_VIEW, one per view, so two views' copies of a
  # contract stay two types in one release binary (live code, rule 1).
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src ${root})
  string(MAKE_C_IDENTIFIER "vp_${view}" view_namespace)
  target_compile_definitions(${target} PRIVATE VP_VIEW=${view_namespace})
  # runtime/Operator.h takes GLSL's vectors from glm.
  target_include_directories(${target} SYSTEM PRIVATE
                             ${PROJECT_SOURCE_DIR}/src/external-libraries)
  target_compile_options(${target} PRIVATE "${warnings}")
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  add_dependencies(vulpen_modules ${target})
endfunction()

# The table a release build finds its linked modules in; a live build's is empty.
function(vulpen_link_modules output)
  get_property(linked GLOBAL PROPERTY vulpen_linked)
  list(LENGTH linked count)
  set(declarations "")
  set(rows "")
  foreach(entry IN LISTS linked)
    string(REPLACE "=" ";" parts ${entry})
    list(GET parts 0 module)
    list(GET parts 1 symbol)
    string(APPEND declarations "extern \"C\" void ${symbol}(VP::Registry &);\n")
    string(APPEND rows "    VP::LinkedModule{\"${module}\", &${symbol}},\n")
  endforeach()
  file(CONFIGURE OUTPUT ${output} CONTENT
"// Generated by src/runtime/cmake/nodes.cmake: the modules this build links in.
${declarations}
inline constexpr std::array<VP::LinkedModule, ${count}> linked_modules{
${rows}};
")
endfunction()
