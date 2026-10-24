layout(location = 0) in vec3 uv;    // brush writes a vec2 there
layout(location = 2) in float shade; // brush writes it at location 1

layout(location = 0) out vec4 color;

void main() {
  color = vec4(uv, shade);
}
