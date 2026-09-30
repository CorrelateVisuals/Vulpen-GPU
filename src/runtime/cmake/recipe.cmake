# The build side of live code (docs/plans/live-code.md). Each recipe folder of a view
# compiles its shaders to SPIR-V and its C++ to a module (live) or into vulpen (release),
# at paths that mirror the view under <build>/views/, so the runtime finds them by path
# alone. Included by the top-level CMakeLists.txt after the vulpen and gates targets.
find_program(GLSLANG glslangValidator REQUIRED)
find_program(SPIRV_VAL spirv-val REQUIRED)

string(COMPARE EQUAL "${CMAKE_BUILD_TYPE}" Debug live_default)
option(VULPEN_LIVE "Swap recipe C++ and GLSL while vulpen runs" ${live_default})

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
string(SUBSTRING ${stamp} 0 16 recipe_stamp)

# What a live build rebuilds; the include map guards it like every build (A00).
add_custom_target(vulpen_recipes)
add_dependencies(vulpen_recipes gates)
add_dependencies(vulpen vulpen_recipes)
target_compile_definitions(vulpen PRIVATE
  VP_LIVE=$<BOOL:${VULPEN_LIVE}>
  VP_CMAKE_COMMAND="${CMAKE_COMMAND}"
  VP_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}"
  VP_RECIPE_STAMP="${recipe_stamp}")

function(vulpen_view folder)
  get_filename_component(view ${folder} NAME)
  file(GLOB recipes LIST_DIRECTORIES true CONFIGURE_DEPENDS ${folder}/recipes/*)
  foreach(recipe IN LISTS recipes)
    if(IS_DIRECTORY ${recipe})
      vulpen_recipe(${view} ${recipe})
    endif()
  endforeach()
endfunction()

function(vulpen_recipe view folder)
  get_filename_component(recipe ${folder} NAME)
  string(MAKE_C_IDENTIFIER "${view}_${recipe}" target)
  set(out ${CMAKE_BINARY_DIR}/views/${view}/recipes/${recipe})

  file(GLOB shaders CONFIGURE_DEPENDS ${folder}/*.comp ${folder}/*.vert ${folder}/*.frag)
  set(spirv "")
  foreach(shader IN LISTS shaders)
    get_filename_component(name ${shader} NAME)
    add_custom_command(OUTPUT ${out}/${name}.spv
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out}
      COMMAND ${GLSLANG} --target-env vulkan1.2 --quiet -I${PROJECT_SOURCE_DIR}/src
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
  add_custom_target(${target}_spirv DEPENDS ${spirv})
  add_dependencies(vulpen_recipes ${target}_spirv)

  file(GLOB sources CONFIGURE_DEPENDS ${folder}/*.cpp)
  if(NOT sources)
    return()
  endif()
  if(VULPEN_LIVE)
    add_library(${target} MODULE ${sources})
    # Every module exports one entry name, each in its own symbol scope. Without
    # -fno-gnu-unique, GCC's unique symbols keep a module mapped after dlclose, and a
    # swap would silently run the old code.
    set_target_properties(${target} PROPERTIES
      PREFIX "" OUTPUT_NAME recipe LIBRARY_OUTPUT_DIRECTORY ${out}$<0:>
      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
    target_compile_definitions(${target} PRIVATE
      VP_RECIPE_ENTRY=vp_recipe_${recipe_stamp}
      "VP_RECIPE_EXPORT=[[gnu::visibility(\"default\")]]")
  else()
    add_library(${target} OBJECT ${sources})
    target_link_libraries(vulpen PRIVATE ${target})
    target_compile_definitions(${target} PRIVATE
      VP_RECIPE_ENTRY=vp_recipe_${target} VP_RECIPE_EXPORT=)
    set_property(GLOBAL APPEND PROPERTY vulpen_linked "${view}/${recipe}=vp_recipe_${target}")
  endif()
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  # runtime/Operator.h takes GLSL's vectors from glm.
  target_include_directories(${target} SYSTEM PRIVATE
                             ${PROJECT_SOURCE_DIR}/src/external-libraries)
  target_compile_options(${target} PRIVATE "${warnings}")
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  add_dependencies(vulpen_recipes ${target})
endfunction()

# The table a release build finds its linked recipes in; a live build's is empty.
function(vulpen_link_recipes output)
  get_property(linked GLOBAL PROPERTY vulpen_linked)
  list(LENGTH linked count)
  set(declarations "")
  set(rows "")
  foreach(entry IN LISTS linked)
    string(REPLACE "=" ";" parts ${entry})
    list(GET parts 0 recipe)
    list(GET parts 1 symbol)
    string(APPEND declarations "extern \"C\" void ${symbol}(VP::Registry &);\n")
    string(APPEND rows "    VP::LinkedRecipe{\"${recipe}\", &${symbol}},\n")
  endforeach()
  file(CONFIGURE OUTPUT ${output} CONTENT
"// Generated by src/runtime/cmake/recipe.cmake: the recipes this build links in.
${declarations}
inline constexpr std::array<VP::LinkedRecipe, ${count}> linked_recipes{
${rows}};
")
endfunction()
