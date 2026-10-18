#version 460

layout(location = 0) flat in vec4 color; // the cell's
layout(location = 0) out vec4 pixel;

void main() {
  pixel = color;
}
