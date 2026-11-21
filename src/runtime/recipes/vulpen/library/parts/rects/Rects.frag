#include "Rects.glsl"

layout(location = 0) flat in vec4 fill;
layout(location = 0) out vec4 color;

// "The stone which the builders refused is become the head stone of the corner."
// Psalm 118:22

void main() {
  color = fill;
}
