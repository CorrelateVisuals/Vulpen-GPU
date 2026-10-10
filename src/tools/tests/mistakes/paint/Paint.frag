#version 460

layout(location = 0) out vec4 color;

// Each pixel's place, as bytes R8G8B8A8_UNORM holds exactly, so a reader that finds
// another value read another pixel. Opaque, so blending over the cleared image leaves
// the value as it is.
void main() {
  const uvec2 pixel = uvec2(gl_FragCoord.xy);
  color = vec4(vec2(pixel & 255u) / 255.0, 0.0, 1.0);
}
