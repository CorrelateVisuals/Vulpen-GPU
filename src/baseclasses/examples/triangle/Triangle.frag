layout(location = 0) in vec3 color; // the corners' colors, blended across the face
layout(location = 0) out vec4 pixel;

// "Then spake Jesus again unto them, saying, I am the light of the world: he that
// followeth me shall not walk in darkness, but shall have the light of life." John 8:12

void main() {
  pixel = vec4(color, 1.0);
}
