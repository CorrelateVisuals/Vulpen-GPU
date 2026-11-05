# Run by the build as `cmake -DSHADER=… -DOUTPUT=… -DVERSION=… -DPREAMBLE=… -P glsl.cmake`.
# OUTPUT is SHADER after its #version and PREAMBLE, then a #line that names the shader and
# its first line, so glslang reports an error at the shader's own path and line. The
# flags that would do this in glslang (--glsl-version, --preamble-text) are missing from
# 11.8, which Ubuntu 22.04 ships.
file(READ ${SHADER} text)
file(WRITE ${OUTPUT} "#version ${VERSION}\n${PREAMBLE}\n#line 1 \"${SHADER}\"\n${text}")
