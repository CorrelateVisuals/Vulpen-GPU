#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace VP_VIEW {

// The roles a palette colors, in the order of its colors, each by the key theme.ini
// gives its color under. contracts/Palette.glsl counts as many.
inline constexpr auto role_names = std::to_array<std::string_view>(
    {"background", "panel", "border", "text", "accent"});

// A role as a Rect or a Label names it, its color's place in the palette, so a name that
// is no role does not compile.
consteval std::uint32_t role(std::string_view name) {
  for (std::uint32_t i = 0; i < role_names.size(); ++i)
    if (role_names[i] == name)
      return i;
  throw "no role has that name";
}

} // namespace VP_VIEW
