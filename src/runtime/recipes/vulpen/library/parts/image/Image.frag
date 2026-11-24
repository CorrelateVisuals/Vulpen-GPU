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

// "That which was from the beginning, which we have heard, which we have seen with our
// eyes, which we have looked upon, and our hands have handled, of the Word of life;"
// 1 John 1:1

// The loader: an arc of a ring in the middle of the Rect, turning, in a grey that shows
// on a dark theme and a light one alike, as the part reads no palette.
const float loader_radius = 16.0; // pixels
const float loader_width = 3.0;   // pixels
const float loader_arc = 0.3;     // of a turn
const float loader_turns = 1.0;   // a second
const vec4 loader_grey = vec4(0.5, 0.5, 0.5, 1.0);
const float turn_radians = 6.2831853;

// The arc where uv lies in the Rect, turning with the frame's time, which counts frames,
// so a replay draws it alike (C01). Its edges fade over a pixel. Away from the ring it
// returns before atan, which no GPU defines at the middle (GLSL02).
vec4 loader(vec2 uv) {
  const vec2 at = (uv - 0.5) * vec2(pass.place.at[0].extent);
  const float across = abs(length(at) - loader_radius);
  if (across > loader_width)
    return vec4(0.0);
  const float turn = fract(atan(at.y, at.x) / turn_radians - frame.time * loader_turns);
  const float along = min(turn, loader_arc - turn) * turn_radians * loader_radius;
  return loader_grey * clamp(loader_width * 0.5 - across + 0.5, 0.0, 1.0) *
         clamp(along + 0.5, 0.0, 1.0);
}

// Zoomed past a screen pixel a pixel of the image, each shows as a square of its own
// color, so a deep zoom shows what the view drew rather than a blur of it.
void main() {
  if (pass.loading != 0u)
    color = loader(uv);
  else if (pass.holds != 0u)
    color = texel > 1.0 ? held_at(uv) : held_mixed(uv);
  else
    color = texel > 1.0 ? sample_nearest(pass.picture, uv)
                        : sample_linear(pass.picture, uv);
}
