#version 460
#extension GL_GOOGLE_include_directive : require
#include "Glyphs.glsl"

layout(location = 0) out vec2 uv;
layout(location = 1) flat out vec4 ink;

// The font is monospace, so a character's place follows from its column alone. One
// outside its label's characters or room, or whose code the font lacks, folds to a
// point and draws nothing; so does every one until the font is written, whose zeros
// would divide below (GLSL02).
void main() {
  const Character character = pass.characters.at[gl_InstanceIndex];
  const Label label = pass.labels.at[character.label];
  const Font font = pass.font.at[0];
  const uint column = uint(gl_InstanceIndex) - label.first;
  const uint glyph = character.code - font.first;
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  uv = vec2(0.0);
  ink = vec4(0.0);
  if (column >= label.count || glyph >= font.count ||
      (column + 1u) * font.cell.x > label.extent.x || font.cell.y > label.extent.y)
    return;
  const vec2 corner = quad_corner(gl_VertexIndex);
  const vec2 cell = vec2(font.cell);
  gl_Position = pixel_clip(vec2(label.offset) + vec2(column, 0u) * cell + corner * cell);
  const vec2 origin = vec2(glyph % font.columns, glyph / font.columns) * cell;
  uv = (origin + corner * cell) / vec2(font.atlas);
  ink = role_color(pass.palette, label.role);
}
