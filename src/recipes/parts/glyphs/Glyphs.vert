#version 460
#extension GL_GOOGLE_include_directive : require
#include "Glyphs.glsl"

layout(location = 0) out vec2 uv;
layout(location = 1) flat out vec4 ink;

// The font is monospace, so a character's place follows from its column alone, its cell
// the font's times the label's scale. Each corner is kept inside the label's room, its
// atlas coordinate moved as far, so a character half out of the room is cut at its edge,
// and one wholly out folds flat, without a discard (GLSL02). One outside its label's
// characters, or whose code the font lacks, folds to a point and draws nothing; so does
// every one until the font is written, whose zeros would divide below.
void main() {
  const Character character = pass.characters.at[gl_InstanceIndex];
  const Label label = pass.labels.at[character.label];
  const Font font = pass.font.at[0];
  const uint column = uint(gl_InstanceIndex) - label.first;
  const uint glyph = character.code - font.first;
  const vec2 atlas_cell = vec2(font.cell);
  const vec2 cell = atlas_cell * (label.scale == 0.0 ? 1.0 : label.scale);
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  uv = vec2(0.0);
  ink = vec4(0.0);
  if (column >= label.count || glyph >= font.count)
    return;
  const vec2 from = vec2(label.offset) + vec2(column, 0u) * cell;
  // max, then min, as clamp is undefined for a room that ends before it starts (GLSL02).
  const vec2 kept = min(max(from + quad_corner(gl_VertexIndex) * cell,
                            vec2(label.offset) + vec2(label.cut)),
                        vec2(label.offset) + vec2(label.extent));
  gl_Position = pixel_clip(kept);
  const vec2 origin = vec2(glyph % font.columns, glyph / font.columns) * atlas_cell;
  uv = (origin + (kept - from) / cell * atlas_cell) / vec2(font.atlas);
  ink = role_color(pass.palette, label.role);
}
