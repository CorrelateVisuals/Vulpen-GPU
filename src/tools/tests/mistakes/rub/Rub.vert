layout(location = 0) out vec2 uv; // 0 to 1 across the image, from its top left

// One triangle over the whole image: its corners at (-1, -1), (3, -1) and (-1, 3).
void main() {
  const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  uv = corner;
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
