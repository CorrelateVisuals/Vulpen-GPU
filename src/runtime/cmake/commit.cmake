# Run by the build as `cmake -DSOURCE_DIR=… -DOUTPUT=… -P commit.cmake`.
# --exclude=* skips tag names, so the stamp is the plain hash that
# `git rev-parse --short HEAD` prints.
execute_process(COMMAND git describe --always --dirty --exclude=*
                WORKING_DIRECTORY ${SOURCE_DIR}
                OUTPUT_VARIABLE commit
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
# Rewrites the header only when the commit changed, so an unchanged tree
# recompiles nothing.
file(CONFIGURE OUTPUT ${OUTPUT} CONTENT "#define VULPEN_COMMIT \"${commit}\"\n")
