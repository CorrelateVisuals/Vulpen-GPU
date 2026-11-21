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

// "Forasmuch then as the children are partakers of flesh and blood, he also himself
// likewise took part of the same; that through death he might destroy him that had the
// power of death, that is, the devil;" Hebrews 2:14

// Lit as a matte surface: by how squarely it faces the light, a back as the other way, so
// a model that is open, as a plane, shows from either side; the depth keeps what is
// nearest.
void main() {
  const vec3 facing = gl_FrontFacing ? normal : -normal;
  const float lit =
      ambient + (1.0 - ambient) * max(dot(unit(facing), unit(pass.light.xyz)), 0.0);
  color = vec4(tint.rgb * lit, tint.a);
}
