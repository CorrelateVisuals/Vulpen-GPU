#include "Mesh.glsl"

layout(location = 0) out vec3 normal;
layout(location = 1) flat out vec4 tint;

// A point as the camera sees it, square in the middle of any window: across the picture,
// up it, and ahead. Its depth is 1 - near / ahead, 0 at the near plane and nearing 1
// far off, with no far plane to clip; what lies nearer than near is clipped.
vec4 seen(vec3 at) {
  const Camera camera = pass.camera.at[0];
  const vec3 from = at - camera.eye.xyz;
  const vec3 view = vec3(
      dot(from, camera.right.xyz), dot(from, camera.up.xyz), dot(from, camera.ahead.xyz));
  const vec2 size = vec2(frame.resolution);
  const vec2 lens = camera.ahead.w * min(size.yx / size, vec2(1.0));
  return vec4(view.x * lens.x, -view.y * lens.y, view.z - pass.near, view.z);
}

// "For in him dwelleth all the fulness of the Godhead bodily." Colossians 2:9

void main() {
  const Triangle triangle = pass.triangles.at[gl_InstanceIndex];
  const int corner = gl_VertexIndex;
  const vec4 at = corner == 0 ? triangle.a : corner == 1 ? triangle.b : triangle.c;
  normal = (corner == 0   ? triangle.normal_a
            : corner == 1 ? triangle.normal_b
                          : triangle.normal_c).xyz;
  tint = triangle.color * pass.color;
  gl_Position = seen(at.xyz);
}
