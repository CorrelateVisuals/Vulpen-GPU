# The build side of live code. Each node is a folder (RV08),
# which compiles its shaders to SPIR-V and its C++ to one module (live) or into vulpen
# (release), at paths that mirror the view under <build>/views/, so the runtime finds them
# by path alone. Included by the top-level CMakeLists.txt after the runtime, vulpen and
# gates targets, VULPEN_LIVE, and vulpen_spirv, which compiles a shader.

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
# Beside vulpen, for a live build's runtime to read before it swaps: modules built against
# other engine headers than its own cannot load, so then it swaps none in.
set(module_stamp_file module-stamp)
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/${module_stamp_file} CONTENT "${module_stamp}\n")
# The files vulpen itself compiles, one a line, which no live build builds into the
# running vulpen, so its runtime says, once one changes, that a restart runs it.
set(engine_sources_file engine-sources)
set(views_file built-views)
get_target_property(engine_sources vulpen SOURCES)
get_target_property(runtime_sources runtime SOURCES)
get_target_property(layer_sources baseclasses SOURCES)
list(APPEND engine_sources ${runtime_sources} ${layer_sources})
list(TRANSFORM engine_sources PREPEND ${PROJECT_SOURCE_DIR}/)
list(APPEND engine_sources ${engine_headers})
list(JOIN engine_sources "\n" engine_list)
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/${engine_sources_file} CONTENT "${engine_list}\n")
# The file those compile into, beside vulpen, whose time a change is newer than: vulpen, or
# a Windows live build's engine DLL, which a build relinks while vulpen stays as it was.
if(TARGET engine)
  set(engine_file $<TARGET_FILE_NAME:engine>)
else()
  set(engine_file $<TARGET_FILE_NAME:vulpen>)
endif()

# How a module marks its entry exported: a DLL exports only what is marked so, and GCC
# and Clang, told to hide every symbol, export what is marked visible.
if(WIN32)
  set(module_export "__declspec(dllexport)")
else()
  set(module_export "[[gnu::visibility(\"default\")]]")
endif()

# Every view's modules and shaders, which the full build builds. vulpen needs only the
# library's, so ./run.sh builds those and the projects it opens, and the live build each
# view vulpen hosts, a target each, vulpen_view_<name>: a project that fails to build then
# holds back no other, nor the start. The include map guards each like every build (A00):
# the library by the gates over src/, a project by the gates over its own folder.
add_custom_target(vulpen_modules ALL)
add_dependencies(vulpen_modules gates)
target_compile_definitions(runtime PRIVATE
  VP_LIVE=$<BOOL:${VULPEN_LIVE}>
  VP_CMAKE_COMMAND="${CMAKE_COMMAND}"
  VP_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}"
  VP_MODULE_STAMP="${module_stamp}"
  VP_MODULE_STAMP_FILE="${module_stamp_file}"
  VP_ENGINE_SOURCES_FILE="${engine_sources_file}"
  VP_ENGINE_FILE="${engine_file}"
  VP_VIEWS_FILE="${views_file}")

# A view: each folder in it is a node, but its contracts/ and its shared/ (RV05, RV09). One
# named so no target can be is named, and not built, so it stops no other build.
function(vulpen_view folder)
  get_filename_component(view ${folder} NAME)
  if(NOT view MATCHES "^[A-Za-z0-9_.+-]+$")
    message(WARNING "${folder} is not built: a view's name is letters, digits and _.+-")
    return()
  endif()
  vulpen_name(${view} ${folder})
  add_custom_target(vulpen_view_${view}_gates
    COMMAND ${Python3_EXECUTABLE} ${tools}/gates.py ${CMAKE_BINARY_DIR} --view ${folder}
    VERBATIM)
  add_custom_target(vulpen_view_${view})
  add_dependencies(vulpen_modules vulpen_view_${view})
  # A release build links every view's C++ into vulpen and builds nothing while it runs, so
  # vulpen needs every view's shaders too, as view load may host any of them.
  if(NOT VULPEN_LIVE)
    add_dependencies(vulpen vulpen_view_${view})
  endif()
  vulpen_nodes(${view} ${folder} ${folder})
endfunction()

# The build tree mirrors each view by its name, where the runtime finds its modules, so
# two views of one name, as a project named library, would build into one place:
# the build fails, naming both (A02).
function(vulpen_name view folder)
  get_property(taken GLOBAL PROPERTY vulpen_view_${view})
  if(taken)
    message(FATAL_ERROR "${folder} and ${taken} are both views named ${view}, and the "
                        "build tree holds one view of each name; rename one")
  endif()
  set_property(GLOBAL PROPERTY vulpen_view_${view} ${folder})
  set_property(GLOBAL APPEND PROPERTY vulpen_views ${view})
endfunction()

# The views the build compiles, a name a line, beside vulpen, so the live build builds
# a view vulpen hosts only when the build has its target, as one hosted from a folder
# outside projects/ has none.
function(vulpen_list_views)
  get_property(views GLOBAL PROPERTY vulpen_views)
  list(JOIN views "\n" listed)
  file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/${views_file} CONTENT "${listed}\n")
endfunction()

# The recipes build as one view named library, which runtime/Manifest.cpp names the same:
# each recipe of the library's kinds (RV06) is a node's folder, and so is each app beside
# the library, so an app runs from the library's recipes as they are (V11). The library's
# parts/shared/ is the code parts share, which no node is (RV09).
function(vulpen_recipes folder)
  vulpen_name(library ${folder})
  add_custom_target(vulpen_view_library)
  add_dependencies(vulpen_view_library gates)
  add_dependencies(vulpen_modules vulpen_view_library)
  add_dependencies(vulpen vulpen_view_library)
  file(GLOB recipes LIST_DIRECTORIES true CONFIGURE_DEPENDS ${folder}/library/parts/*
       ${folder}/library/components/* ${folder}/*)
  foreach(recipe IN LISTS recipes)
    if(IS_DIRECTORY ${recipe} AND NOT recipe STREQUAL "${folder}/library"
       AND NOT recipe STREQUAL "${folder}/library/parts/shared")
      vulpen_node(library ${recipe} ${folder})
      vulpen_nodes(library ${recipe} ${folder})
    endif()
  endforeach()
endfunction()

# The nodes inside a folder: each folder of it, and the nodes inside those. A folder with
# a view.vlp of its own is a view the first hosts (V03), and builds as a view.
# root: the view's folder, where contracts/ is (RV05), or the recipes' folder.
function(vulpen_nodes view folder root)
  file(GLOB entries LIST_DIRECTORIES true CONFIGURE_DEPENDS ${folder}/*)
  foreach(entry IN LISTS entries)
    get_filename_component(name ${entry} NAME)
    if(NOT IS_DIRECTORY ${entry} OR name MATCHES "^\\." OR entry STREQUAL "${root}/contracts"
       OR entry STREQUAL "${root}/shared")
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
  # Where contracts/ and shared/ resolve from: the top of the view, or of the library, which
  # keeps shared/ in parts/, beside the parts that include it (RV05, RV09).
  set(includes ${PROJECT_SOURCE_DIR}/src ${root})
  if(view STREQUAL "library")
    set(includes ${PROJECT_SOURCE_DIR}/src ${root}/library ${root}/library/parts)
  endif()
  list(TRANSFORM includes PREPEND -I OUTPUT_VARIABLE include_flags)

  # The gates the view's build runs first, so a broken rule compiles nothing of it.
  set(gates gates)
  if(TARGET vulpen_view_${view}_gates)
    set(gates vulpen_view_${view}_gates)
  endif()

  file(GLOB shaders CONFIGURE_DEPENDS ${folder}/*.comp ${folder}/*.vert ${folder}/*.frag)
  set(spirv "")
  foreach(shader IN LISTS shaders)
    vulpen_spirv(${shader} ${out} ${include_flags})
    get_filename_component(name ${shader} NAME)
    list(APPEND spirv ${out}/${name}.spv.valid)
  endforeach()
  if(spirv)
    add_custom_target(${target}_spirv DEPENDS ${spirv})
    add_dependencies(${target}_spirv ${gates})
    add_dependencies(vulpen_view_${view} ${target}_spirv)
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
    # A DLL resolves its calls into the engine at its link, from the engine's DLL.
    if(WIN32)
      target_link_libraries(${target} PRIVATE engine)
    endif()
  else()
    add_library(${target} OBJECT ${sources})
    target_link_libraries(vulpen PRIVATE ${target})
    target_compile_definitions(${target} PRIVATE
      VP_MODULE_ENTRY=vp_module_${target} VP_MODULE_EXPORT=)
    set_property(GLOBAL APPEND PROPERTY vulpen_linked "${view}/${relative}=vp_module_${target}")
  endif()
  # As for GLSL. A contract's C++ types live in namespace VP_VIEW, one per view, so two
  # views' copies of a contract stay two types in one release binary (live code, rule 1).
  target_include_directories(${target} PRIVATE ${includes})
  string(MAKE_C_IDENTIFIER "vp_${view}" view_namespace)
  target_compile_definitions(${target} PRIVATE VP_VIEW=${view_namespace})
  # runtime/Operator.h takes GLSL's vectors from glm, and an engine header a node includes
  # by hand Vulkan's types, which only Linux keeps where every compiler looks.
  target_include_directories(${target} SYSTEM PRIVATE
                             ${PROJECT_SOURCE_DIR}/src/external-libraries)
  target_link_libraries(${target} PRIVATE Vulkan::Headers)
  target_compile_options(${target} PRIVATE "${warnings}")
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  add_dependencies(${target} ${gates})
  add_dependencies(vulpen_view_${view} ${target})
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
