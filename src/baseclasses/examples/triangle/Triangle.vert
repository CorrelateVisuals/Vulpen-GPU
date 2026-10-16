#version 460

// In clip space, picked by the vertex's index, so no buffer holds them.
const vec2 corners[] = vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));

void main() {
  gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
