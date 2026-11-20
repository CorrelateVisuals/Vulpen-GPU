// The pass block both of the mesh's shaders include, so the loader finds the same block
// in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Camera.glsl"
#include "contracts/Mesh.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Triangles triangles; // a connection: a model's, an instance each
  Cameras camera;      // a connection: the eye and the frame it looks along
  vec4 color;          // tints every triangle
  vec4 light;          // xyz: toward the light, which need not be unit
} pass;
