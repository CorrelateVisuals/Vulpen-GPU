#version 460

layout(location = 0) in vec3 color; // the corners' colors, blended across the face
layout(location = 0) out vec4 pixel;

void main() {
  pixel = vec4(color, 1.0);
}
