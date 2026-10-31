// Rays against instances of the unit cube, its sides from -1 to 1 along each of its axes:
// trace looks for the nearest one a ray meets, shadow for any between a point and the
// lamp. A node includes it after contracts/Instance.glsl. One instance's test is the same
// code for every cube, so the cubes differ only in their Instance, as instancing does.

// Smaller than any part of a direction that counts, so none divides by zero (GLSL02).
const float tiny = 1.0e-6;
// Of a face's tiles, the darker ones' share of the cube's color.
const float dark_tiles = 0.35;

// A vector in the cube's own frame, scaled to the unit cube: a point once its center is
// taken off, or a direction, along which the distances stay the world's.
vec3 into(Instance cube, vec3 v) {
  return vec3(dot(v, cube.x.xyz) / cube.x.w,
              dot(v, cube.y.xyz) / cube.y.w,
              dot(v, cube.z.xyz) / cube.z.w);
}

// Each part at least tiny away from zero, keeping its sign.
vec3 away_from_zero(vec3 v) {
  return mix(max(v, vec3(tiny)), min(v, vec3(-tiny)), lessThan(v, vec3(0.0)));
}

// How far along the ray it meets the cube, and the face's normal there, in the world;
// negative when it meets none ahead of its origin. The slabs between each pair of faces
// give where the ray is inside each; it is inside the cube where it is inside all three.
// A ray starting inside a cube meets none of it: every ray starts outside, nudged off
// the face it leaves.
float box_hit(Instance cube, vec3 origin, vec3 direction, out vec3 normal) {
  normal = vec3(0.0);
  const vec3 from = into(cube, origin - cube.center.xyz);
  const vec3 along = away_from_zero(into(cube, direction));
  const vec3 low = (-1.0 - from) / along;
  const vec3 high = (1.0 - from) / along;
  const vec3 enter = min(low, high);
  const vec3 leave = max(low, high);
  const float near = max(max(enter.x, enter.y), enter.z);
  const float far = min(min(leave.x, leave.y), leave.z);
  if (far < near || near <= 0.0)
    return -1.0;
  // The slab entered last is the face it comes in through.
  const vec3 face = enter.x >= enter.y && enter.x >= enter.z ? vec3(1.0, 0.0, 0.0)
                    : enter.y >= enter.z                     ? vec3(0.0, 1.0, 0.0)
                                                             : vec3(0.0, 0.0, 1.0);
  const vec3 facing = -sign(along) * face;
  normal = facing.x * cube.x.xyz + facing.y * cube.y.xyz + facing.z * cube.z.xyz;
  return near;
}

// The nearest of the first count cubes the ray meets: how far, negative for none, the
// face's normal and which cube.
float nearest(Instances cubes,
              uint count,
              vec3 origin,
              vec3 direction,
              out vec3 normal,
              out uint which) {
  float best = -1.0;
  normal = vec3(0.0);
  which = 0u;
  for (uint cube = 0u; cube < count; ++cube) {
    vec3 face;
    const float far = box_hit(cubes.at[cube], origin, direction, face);
    if (far > 0.0 && (best < 0.0 || far < best)) {
      best = far;
      normal = face;
      which = cube;
    }
  }
  return best;
}

// Whether any of the first count cubes stands between a point and what lies as far away
// as given along the direction.
bool blocked(Instances cubes, uint count, vec3 origin, vec3 direction, float distance) {
  for (uint cube = 0u; cube < count; ++cube) {
    vec3 face;
    const float far = box_hit(cubes.at[cube], origin, direction, face);
    if (far > 0.0 && far < distance)
      return true;
  }
  return false;
}

// The cube's color at a point on it: its own, or checkered, tiles across each face. On a
// face one of the point's parts is -1 or 1, and clamped it keeps one tile over the whole
// face, so only the other two make the checker.
vec3 surface(Instance cube, vec3 point) {
  const float tiles = cube.look.x;
  if (tiles <= 0.0)
    return cube.color.rgb;
  const vec3 local = into(cube, point - cube.center.xyz);
  const ivec3 tile = ivec3(clamp(floor((local + 1.0) * 0.5 * tiles), 0.0, tiles - 1.0));
  const bool dark = ((tile.x + tile.y + tile.z) & 1) != 0;
  return dark ? dark_tiles * cube.color.rgb : cube.color.rgb;
}
