#version 460
#extension GL_GOOGLE_include_directive : require
#include "Image.glsl"
#include "contracts/Still.glsl"

layout(location = 0) in vec2 uv;
layout(location = 1) flat in float texel;
layout(location = 0) out vec4 color;

// The texel of the frame held nearest uv, decoded as the still encoded it.
vec4 held_at(vec2 uv) {
  const vec4 stored = sample_nearest(pass.held, uv);
  return vec4(decoded(stored.rgb), stored.a);
}

// The four texels of the frame held about uv, mixed once decoded, as the sampler mixes
// the picture's in linear light; mixed encoded, every edge would show darker than the
// picture did.
vec4 held_mixed(vec2 uv) {
  const vec2 size =
      vec2(textureSize(sampler2D(textures[pass.held.index], samplers[nearest_clamp]), 0));
  const vec2 at = uv * size - 0.5;
  const vec2 part = at - floor(at);
  const vec2 step = 1.0 / size;
  const vec2 first = (floor(at) + 0.5) * step;
  return mix(mix(held_at(first), held_at(first + vec2(step.x, 0.0)), part.x),
             mix(held_at(first + vec2(0.0, step.y)), held_at(first + step), part.x),
             part.y);
}

// Zoomed past a screen pixel a pixel of the image, each shows as a square of its own
// color, so a deep zoom shows what the view drew rather than a blur of it.
void main() {
  if (pass.holds != 0u)
    color = texel > 1.0 ? held_at(uv) : held_mixed(uv);
  else
    color = texel > 1.0 ? sample_nearest(pass.picture, uv)
                        : sample_linear(pass.picture, uv);
}
