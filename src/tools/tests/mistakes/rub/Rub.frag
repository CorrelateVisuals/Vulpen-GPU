layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 tint; // which Rub.vert writes nothing to
layout(location = 0) out vec4 color;

void main() {
  color = vec4(uv, 0.0, 1.0) * vec4(tint, 1.0);
}
