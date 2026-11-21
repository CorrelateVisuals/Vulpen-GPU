#include "Rects.glsl"

layout(location = 0) flat out vec4 fill;

// "For other foundation can no man lay than that is laid, which is Jesus Christ."
// 1 Corinthians 3:11

void main() {
  const Rect rect = pass.rects.at[gl_InstanceIndex];
  gl_Position =
      pixel_clip(vec2(rect.offset) + quad_corner(gl_VertexIndex) * vec2(rect.extent));
  fill = mix(role_color(pass.palette, rect.role),
             role_color(pass.palette, rect.blend_role),
             rect.blend);
}
