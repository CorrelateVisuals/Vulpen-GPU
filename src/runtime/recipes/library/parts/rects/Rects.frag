#include "Rects.glsl"

layout(location = 0) flat in vec4 fill;
layout(location = 0) out vec4 color;

void main() {
  color = fill;
}
