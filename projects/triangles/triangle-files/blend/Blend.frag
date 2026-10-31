layout(location = 0) in vec3 color; // a connection: face's, blended across the face
layout(location = 0) out vec4 pixel;

void main() {
  pixel = vec4(color, 1.0);
}
