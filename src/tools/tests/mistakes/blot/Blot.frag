#version 460

layout(set = 1, binding = 0) uniform Pass {
  float amount; // which a fragment node may not declare
} pass;

layout(location = 0) in vec2 uv;
layout(location = 1) in float shade;

layout(location = 0) out vec4 color;

void main() {
  color = vec4(uv, shade, pass.amount);
}
