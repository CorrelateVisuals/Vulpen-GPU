#version 460
#extension GL_GOOGLE_include_directive : require
#include "Rects.glsl"

layout(location = 0) flat out vec4 fill;

void main() {
  const Rect rect = pass.rects.at[gl_InstanceIndex];
  gl_Position =
      pixel_clip(vec2(rect.offset) + quad_corner(gl_VertexIndex) * vec2(rect.extent));
  fill = role_color(pass.palette, rect.role);
}
