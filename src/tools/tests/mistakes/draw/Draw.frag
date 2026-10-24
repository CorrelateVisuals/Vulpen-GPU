#include "Draw.glsl"

layout(location = 0) out vec4 color;

void main() {
  color = vec4(pass.values.at[0]);
}
