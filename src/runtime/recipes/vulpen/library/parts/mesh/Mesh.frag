#include "Mesh.glsl"

layout(location = 0) in vec3 normal;
layout(location = 1) flat in vec4 tint;
layout(location = 0) out vec4 color;

const float ambient = 0.2; // how lit a side the light misses is

// v at unit length; none for none, where normalize is undefined (GLSL02).
vec3 unit(vec3 v) {
  const float l = length(v);
  return l > 0.0 ? v / l : vec3(0.0);
}

// Lit as a matte surface: by how squarely it faces the light. A triangle's back is not
// drawn, so with no depth buffer a convex model's far side never paints over its near.
void main() {
  if (!gl_FrontFacing)
    discard;
  const float lit =
      ambient + (1.0 - ambient) * max(dot(unit(normal), unit(pass.light.xyz)), 0.0);
  color = vec4(tint.rgb * lit, tint.a);
}
