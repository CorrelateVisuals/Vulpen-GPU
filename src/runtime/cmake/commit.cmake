# Run by the build as `cmake -DSOURCE_DIR=… -DOUTPUT=… -P commit.cmake`.
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${SOURCE_DIR}
                OUTPUT_VARIABLE commit
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
# -dirty when a file the build or a run reads differs from the commit. The documents are
# neither, so a note deleted from docs/ and the statistics each build rewrites leave the
# stamp the commit's; git diff exits 1 for a difference, and higher for a failure.
execute_process(COMMAND git diff --quiet HEAD -- . ":(exclude)docs" ":(exclude)*.md"
                WORKING_DIRECTORY ${SOURCE_DIR}
                RESULT_VARIABLE changed)
if(changed EQUAL 1)
  string(APPEND commit "-dirty")
elseif(NOT changed EQUAL 0)
  message(FATAL_ERROR "git diff failed: ${changed}")
endif()
# Rewrites the header only when the commit changed, so an unchanged tree
# recompiles nothing.
file(CONFIGURE OUTPUT ${OUTPUT} CONTENT "#define VULPEN_COMMIT \"${commit}\"\n")
