#version 460

// The classic triangle in clip space, y pointing down, and a color a corner, picked by
// the vertex's index, so no buffer holds them.
const vec2 corners[] = vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
const vec3 colors[] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));

layout(location = 0) out vec3 color;

void main() {
  gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
  color = colors[gl_VertexIndex];
}
