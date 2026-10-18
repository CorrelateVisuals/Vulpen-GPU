#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace VP_VIEW {

// The roles a palette colors, in the order of its colors, each by the key theme.ini
// gives its color under: the ground and its frames, text, and what tells text apart, as
// lines that matter less are muted and code shows its kinds of words; highlight, a tint
// of the accent, for what the pointer is over on a thing the accent marks; a ramp from
// cold to hot, for what costs least to what costs most, as the graph's heat fills a box
// between the two roles its cost falls between; and what a box in the graph is, a node,
// a view, a folder or a kind of file, which its border and its name show.
// contracts/Palette.glsl counts as many.
inline constexpr auto role_names = std::to_array<std::string_view>({"background",
                                                                    "panel",
                                                                    "border",
                                                                    "text",
                                                                    "accent",
                                                                    "error",
                                                                    "warning",
                                                                    "muted",
                                                                    "keyword",
                                                                    "type",
                                                                    "string",
                                                                    "number",
                                                                    "comment",
                                                                    "directive",
                                                                    "highlight",
                                                                    "cold",
                                                                    "cool",
                                                                    "mild",
                                                                    "warm",
                                                                    "hot",
                                                                    "node",
                                                                    "view",
                                                                    "folder",
                                                                    "file",
                                                                    "hidden",
                                                                    "markdown",
                                                                    "python",
                                                                    "cpp",
                                                                    "header",
                                                                    "cmake",
                                                                    "glsl",
                                                                    "vert",
                                                                    "frag",
                                                                    "comp",
                                                                    "tesc",
                                                                    "tese",
                                                                    "geom",
                                                                    "manifest",
                                                                    "shell",
                                                                    "ini"});

// A role as a Rect or a Label names it, its color's place in the palette, so a name that
// is no role does not compile.
consteval std::uint32_t role(std::string_view name) {
  for (std::uint32_t i = 0; i < role_names.size(); ++i)
    if (role_names[i] == name)
      return i;
  throw "no role has that name";
}

} // namespace VP_VIEW
