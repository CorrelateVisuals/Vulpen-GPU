# The tests (docs/plans/tests.md), all in this folder: the Python that runs vulpen and
# judges it (harness.py and one script per test), the barrier test's C++, the golden, the
# sanitizer suppressions and layer settings the presets name, the fixture view mistakes/,
# and nightly.sh. Included by the top-level CMakeLists.txt once vulpen and every view are
# defined.
#
# Headless runs take the driver VULPEN_TEST_DRIVER names: lavapipe where it is installed,
# so a machine without a GPU runs them (V07), and the Mesa build pinned beside the repo
# when it is there, so a driver update never moves the goldens (C01).
enable_testing()
find_file(VULPEN_TEST_DRIVER lvp_icd.json
  PATHS ${PROJECT_SOURCE_DIR}/../vulpen-lavapipe/usr/share/vulkan/icd.d
        /usr/share/vulkan/icd.d
  NO_DEFAULT_PATH DOC "The Vulkan driver manifest headless tests run on")
if(NOT VULPEN_TEST_DRIVER)
  set(VULPEN_TEST_DRIVER "")
endif()
set(test_environment VULPEN_TEST_DRIVER=${VULPEN_TEST_DRIVER})
# Under sanitizers, windows run on lavapipe too: a closed-source driver's leaks show up
# as an unknown module once it is unloaded, so no suppression could name them without
# also hiding a leak in one of our unloaded recipe modules.
if(VULPEN_SANITIZERS AND VULPEN_TEST_DRIVER)
  list(APPEND test_environment VK_DRIVER_FILES=${VULPEN_TEST_DRIVER})
endif()

# TSan maps its shadow memory where this kernel's address randomization may already have
# put something; without randomization it always finds room.
set(launch "")
if(VULPEN_SANITIZERS MATCHES thread)
  set(launch setarch -R)
endif()
set(vulpen_file $<TARGET_FILE:vulpen>)
set(python ${launch} ${Python3_EXECUTABLE})
set(tests ${CMAKE_CURRENT_LIST_DIR})
set(examples ${PROJECT_SOURCE_DIR}/src/examples)

# A window under TSan reports only noise: GTK, which draws its decorations, takes locks
# TSan cannot see. So tsan builds test without a display, and skip what needs one.
set(no_display "")
if(VULPEN_SANITIZERS MATCHES thread)
  set(no_display DISPLAY=unset: WAYLAND_DISPLAY=unset:)
endif()

function(vulpen_test name)
  add_test(NAME ${name} COMMAND ${ARGN})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${test_environment}"
                                          ENVIRONMENT_MODIFICATION "${no_display}")
endfunction()

vulpen_test(vulpen ${launch} ${vulpen_file})
vulpen_test(wave ${python} ${tests}/golden.py ${vulpen_file})
if(VULPEN_TEST_DRIVER AND NOT VULPEN_SANITIZERS)
  # The same goldens on this machine's own GPU, within the tolerance between drivers.
  vulpen_test(wave-gpu ${python} ${tests}/golden.py ${vulpen_file} --own-gpu)
endif()
vulpen_test(triangle ${launch} ${vulpen_file} ${examples}/triangle/view.vlp
            --frames 120 --fps 0)
# It opens a window, so where no display exists it is skipped, not failed (V07).
set_tests_properties(triangle PROPERTIES SKIP_REGULAR_EXPRESSION "glfwInit failed")
# The harness checks what its runs print; these run vulpen directly (RVK00).
set_tests_properties(vulpen triangle PROPERTIES
  FAIL_REGULAR_EXPRESSION "Validation (Error|Warning|Performance Warning):")

add_executable(barriers ${tests}/Barriers.cpp ${PROJECT_SOURCE_DIR}/src/baseclasses/Passes.cpp)
target_include_directories(barriers PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(barriers PRIVATE Vulkan::Headers)
target_compile_options(barriers PRIVATE "${warnings}")
set_target_properties(barriers PROPERTIES COMPILE_WARNING_AS_ERROR ON)
add_dependencies(barriers gates)
vulpen_test(barriers ${launch} $<TARGET_FILE:barriers>)

# Its recipes build only as modules.
if(VULPEN_LIVE)
  vulpen_test(fail-loud ${python} ${tests}/fail-loud.py ${vulpen_file})
endif()
if(VULPEN_SANITIZERS MATCHES address)
  vulpen_test(fuzz ${python} ${tests}/fuzz.py ${vulpen_file} --count 300)
  # Each script runs on a started engine, which costs five times a refused manifest.
  vulpen_test(fuzz-commands ${python} ${tests}/fuzz.py ${vulpen_file} --commands
              --count 60)
  vulpen_test(fuzz-nightly ${python} ${tests}/fuzz.py ${vulpen_file} --count 10000
              --seed today)
  vulpen_test(fuzz-commands-nightly ${python} ${tests}/fuzz.py ${vulpen_file} --commands
              --count 2000 --seed today)
  set_tests_properties(fuzz-nightly fuzz-commands-nightly PROPERTIES LABELS nightly
                                                                     TIMEOUT 7200)
endif()
# Live swaps are where a second thread runs, so the tsan preset swaps too (A01).
if(VULPEN_SANITIZERS MATCHES thread)
  vulpen_test(swaps ${python} ${tests}/soak.py ${vulpen_file} --swaps 6 --races-only)
  set_tests_properties(swaps PROPERTIES TIMEOUT 600)
endif()
# Sanitizers hold freed memory back, so a soak under them would read as growth.
if(NOT VULPEN_SANITIZERS)
  vulpen_test(soak ${python} ${tests}/soak.py ${vulpen_file} --minutes 10)
  set_tests_properties(soak PROPERTIES LABELS nightly TIMEOUT 900)
  if(VULPEN_LIVE)
    vulpen_test(swap-soak ${python} ${tests}/soak.py ${vulpen_file} --swaps 500)
    set_tests_properties(swap-soak PROPERTIES LABELS nightly TIMEOUT 7200)
  endif()
endif()
