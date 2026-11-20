#pragma once

#include "runtime/Operator.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace VP_VIEW {

// The characters a font bakes, printable ASCII from the space to the tilde, and so what a
// part that takes typed text keeps, since no other shows.
inline constexpr char first_printable = ' ';
inline constexpr char last_printable = '~';

// contracts/Font.glsl's Font, member for member, as C++ writes it; the loader holds it
// to the shader's (RA03). std430 aligns it as its uvec2, so a list of them steps 40 bytes.
struct alignas(8) Font {
  glm::uvec2 cell{};
  glm::uvec2 base{};
  glm::uvec2 atlas{};
  std::uint32_t columns = 0;
  std::uint32_t first = 0;
  std::uint32_t count = 0;

  static constexpr auto members() {
    return std::array{VP_MEMBER(Font, cell),
                      VP_MEMBER(Font, base),
                      VP_MEMBER(Font, atlas),
                      VP_MEMBER(Font, columns),
                      VP_MEMBER(Font, first),
                      VP_MEMBER(Font, count)};
  }
};

inline constexpr std::size_t printable_count = last_printable - first_printable + 1;

// The font as C++ that lays text out reads it: the Font glyphs draws with, and how far
// each printable character moves the next one on, in pixels at the font's size, as a
// proportional face's letters are as wide as they are drawn, so text is measured with
// the font that draws it. A monospace layout counts in cells and reads only the Font.
struct Metrics {
  Font font;
  std::array<std::uint32_t, printable_count> advance{};
};

} // namespace VP_VIEW
