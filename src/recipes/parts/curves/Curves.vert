#version 460
#extension GL_GOOGLE_include_directive : require
#include "Curves.glsl"

layout(location = 0) flat out vec4 fill;

// Its handles reach half the way across, and at least this far, so a curve that runs
// back to the left still bends round.
const float least_reach = 32.0;

// Along a cubic that leaves its start heading right and reaches its end from the left,
// as a wire from a writer to a reader does, a quad a segment, half_width either side.
void main() {
  const Curve curve = pass.curves.at[gl_InstanceIndex];
  const vec2 corner = quad_corner(gl_VertexIndex);
  const float t = (float(gl_VertexIndex / quad_vertices) + corner.x) / float(segments);
  const vec2 handle = vec2(max(abs(curve.to.x - curve.from.x) * 0.5, least_reach), 0.0);
  const vec2 a = curve.from;
  const vec2 b = curve.from + handle;
  const vec2 c = curve.to - handle;
  const vec2 d = curve.to;
  const float s = 1.0 - t;
  const vec2 point =
      s * s * s * a + 3.0 * s * s * t * b + 3.0 * s * t * t * c + t * t * t * d;
  const vec2 slope =
      3.0 * s * s * (b - a) + 6.0 * s * t * (c - b) + 3.0 * t * t * (d - c);
  // A slope of zero has no direction to normalize (GLSL02), so that point keeps an
  // upright side.
  const vec2 side =
      dot(slope, slope) > 0.0 ? normalize(vec2(-slope.y, slope.x)) : vec2(0.0, 1.0);
  gl_Position = pixel_clip(point + side * (corner.y * 2.0 - 1.0) * half_width);
  fill = role_color(pass.palette, curve.role);
}
