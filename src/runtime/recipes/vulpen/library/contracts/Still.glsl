// The frame a still holds, as its dispatch stores it and an image samples it: rgba8, as
// images[] declares it, each color sRGB-encoded, as a window's own bytes are, so the
// darks keep the steps the window shows them in, and decoded where it is sampled, so it
// shows as the frame did. Alpha stays as it was, premultiplied, as every draw blends.
// The numbers are the sRGB standard's own curve, which no theme or view tunes.
vec3 encoded(vec3 linear) {
  return mix(linear * 12.92,
             1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
             greaterThan(linear, vec3(0.0031308)));
}

vec3 decoded(vec3 encoded) {
  return mix(encoded / 12.92,
             pow((encoded + 0.055) / 1.055, vec3(2.4)),
             greaterThan(encoded, vec3(0.04045)));
}
