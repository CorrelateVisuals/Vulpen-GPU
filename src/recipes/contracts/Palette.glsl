// The colors of one theme, one for each role a part draws in, in the order
// contracts/Palette.h names the roles, so a part asks for a role and a theme edit
// reaches every panel.
layout(buffer_reference, std430) readonly buffer Palette {
  vec4 at[];
};

// As many roles as contracts/Palette.h names. A role past them draws in the last, so no
// read leaves the buffer (GLSL02).
const uint roles = 5u;

// Premultiplied, as every draw blends.
vec4 role_color(Palette palette, uint role) {
  const vec4 color = palette.at[min(role, roles - 1u)];
  return vec4(color.rgb * color.a, color.a);
}
