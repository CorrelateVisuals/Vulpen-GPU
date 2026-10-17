#version 460
#extension GL_GOOGLE_include_directive : require
#include "Graph.glsl"

layout(location = 0) flat out vec2 origin;
layout(location = 1) flat out vec4 ink;
layout(location = 2) flat out int filled; // a box's inside, not the area's lines

// A quad over the area, whose every pixel the fragment shader finds the ground's lines
// for, so they cover it whole at any zoom. With no zoom yet it folds to a point and
// draws nothing, since the zero would divide there (GLSL02).
void main() {
  const Rect rect = pass.ground.at[gl_InstanceIndex];
  origin = vec2(rect.offset) + pass.pan;
  ink = role_color(pass.palette, rect.role);
  filled = gl_InstanceIndex > 0 ? 1 : 0;
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  if (pass.zoom <= 0.0)
    return;
  gl_Position =
      pixel_clip(vec2(rect.offset) + quad_corner(gl_VertexIndex) * vec2(rect.extent));
}
