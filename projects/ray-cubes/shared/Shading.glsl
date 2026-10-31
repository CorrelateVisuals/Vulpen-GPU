// How light leaves a face, and what a ray leaving the scene sees, for shade, reflect and
// sky alike, so what a face mirrors and what it shows of its own add up to what met it.

const vec3 horizon = vec3(0.86, 0.80, 0.74);
const vec3 zenith = vec3(0.08, 0.22, 0.56);
const vec3 below = vec3(0.04, 0.04, 0.05);
const float grazing_power = 5.0; // Schlick's

// Schlick's: how much a face mirrors, from how much it mirrors head on and the cosine
// between its normal and the way back along the ray; all of it at a grazing angle.
float fresnel(float head_on, float cosine) {
  return head_on + (1.0 - head_on) * pow(1.0 - clamp(cosine, 0.0, 1.0), grazing_power);
}

// What a ray sees once it leaves the scene: pale at the horizon, deeper above, dark
// below.
vec3 sky(vec3 direction) {
  const float up = clamp(direction.y, -1.0, 1.0);
  return up >= 0.0 ? mix(horizon, zenith, sqrt(up)) : mix(horizon, below, sqrt(-up));
}
