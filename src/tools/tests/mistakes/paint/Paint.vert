#version 460

// One triangle over the whole image: its corners at (-1, -1), (3, -1) and (-1, 3).
void main() {
  const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
