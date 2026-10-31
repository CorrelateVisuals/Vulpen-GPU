// The gathered light read as a picture, a Colors buffer size wide and high, a pixel an
// element row by row from the top left, for smooth and tone alike. A node includes it
// after contracts/Color.glsl.

// A pixel's light; one past an edge takes the edge's, so no read leaves the buffer
// (GLSL02).
vec3 light_at(Colors picture, uvec2 size, ivec2 pixel) {
  const ivec2 at = clamp(pixel, ivec2(0), ivec2(size) - 1);
  return picture.at[at.y * int(size.x) + at.x].rgb;
}

// Between the four pixels around a point, in pixels from the top left, a pixel's middle
// half a pixel past its corner.
vec3 blended(Colors picture, uvec2 size, vec2 at) {
  const vec2 from = at - 0.5;
  const ivec2 corner = ivec2(floor(from));
  const vec2 part = from - floor(from);
  return mix(mix(light_at(picture, size, corner),
                 light_at(picture, size, corner + ivec2(1, 0)),
                 part.x),
             mix(light_at(picture, size, corner + ivec2(0, 1)),
                 light_at(picture, size, corner + ivec2(1, 1)),
                 part.x),
             part.y);
}
