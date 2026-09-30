// A draw that writes a buffer, which the loader must refuse before it makes a pipeline.
#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Floats values;
} pass;
