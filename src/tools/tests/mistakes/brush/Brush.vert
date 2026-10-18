#version 460

layout(location = 0) out vec2 uv;    // 0 to 1 across the image, from its top left
layout(location = 1) out float shade; // 1, so ink's color is each pixel's place exactly

// One triangle over the whole image, its corners at (-1, -1), (3, -1) and (-1, 3), so uv
// at a pixel's middle is that middle over the image's size.
void main() {
  const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  uv = corner;
  shade = 1.0;
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
