// The pass block both of the triangle's shaders read. Each includes this one
// declaration, so the loader finds the same block in both.
#include "baseclasses/GpuLayout.glsl"

layout(buffer_reference, std430) readonly buffer Corners {
  vec2 at[];
};

layout(set = 1, binding = 0) uniform Pass {
  vec4 tint;       // set by the Triangle operator every frame
  Corners corners; // written by the Triangle operator every frame, one per vertex
} pass;
