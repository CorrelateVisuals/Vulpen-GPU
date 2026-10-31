// A ray a pixel: where it starts, where it goes, and how much of the light it finds
// reaches the eye, as rays casts it and each bounce's reflect sends it on. Its weight
// rides as half floats in the slots past origin and direction, so a ray is 32 bytes, not
// 48: the chain moves more bytes from node to node than it computes on, every node
// reading or writing a ray a pixel.
struct Ray {
  vec3 origin;
  uint weight_rg; // the weight's red and green, as packHalf2x16 packs them
  vec3 direction; // unit
  uint weight_b;  // its blue, likewise, beside a 0
};

// What reaches the eye of the light the ray finds: red, green and blue, none once the ray
// ended.
vec3 weight_of(Ray ray) {
  return vec3(unpackHalf2x16(ray.weight_rg), unpackHalf2x16(ray.weight_b).x);
}

Ray ray_of(vec3 origin, vec3 direction, vec3 weight) {
  return Ray(origin, packHalf2x16(weight.rg), direction, packHalf2x16(vec2(weight.b, 0.0)));
}

// A ray that ended carries no light.
bool goes_on(Ray ray) {
  return any(greaterThan(weight_of(ray), vec3(0.0)));
}

VP_READS(Rays, Ray);
VP_WRITES(RaysOut, Ray);
