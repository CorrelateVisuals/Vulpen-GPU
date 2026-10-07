#include "runtime/Operator.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

// The library's contracts/Rect.h and Label.h, member for member, as this fixture's view
// holds no copy of them; the loader holds each to the shader that reads it (RA03).
struct alignas(8) Rect {
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  std::uint32_t role = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Rect, offset), VP_MEMBER(Rect, extent), VP_MEMBER(Rect, role)};
  }
};

struct alignas(8) Label {
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  std::uint32_t role = 0;
  std::uint32_t first = 0;
  std::uint32_t count = 0;

  static constexpr auto members() {
    return std::array{VP_MEMBER(Label, offset),
                      VP_MEMBER(Label, extent),
                      VP_MEMBER(Label, role),
                      VP_MEMBER(Label, first),
                      VP_MEMBER(Label, count)};
  }
};

struct Character {
  std::uint32_t code = 0;
  std::uint32_t label = 0;

  static constexpr auto members() {
    return std::array{VP_MEMBER(Character, code), VP_MEMBER(Character, label)};
  }
};

// The roles in the order the library's contracts/Palette.h names them.
constexpr std::uint32_t panel = 1;
constexpr std::uint32_t border = 2;
constexpr std::uint32_t text = 3;
constexpr std::uint32_t accent = 4;

constexpr std::array rects{
    Rect{.offset = {24, 24}, .extent = {592, 88}, .role = border},
    Rect{.offset = {25, 25}, .extent = {590, 86}, .role = panel},
    Rect{.offset = {25, 25}, .extent = {4, 86}, .role = accent},
};
// Every printable character, more than its label has room for, so the last are cut.
constexpr std::array<std::string_view, 2> lines{
    "Vulpen draws text in its own font.",
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnop"
    "qrstuvwxyz{|}~"};
constexpr glm::ivec2 first_line{40, 40};
constexpr std::int32_t line_height = 24;
constexpr glm::uvec2 room{560, 18};
constexpr std::uint32_t characters = lines[0].size() + lines[1].size();

// Writes the Rects and Labels of a sign, which the library's rects and glyphs draw in
// the palette's colors and the font, as a part that shows text would.
class Sign final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _rects = node.upload<Rect>("rects", rects.size());
    _labels = node.upload<Label>("labels", lines.size());
    _characters = node.upload<Character>("characters", characters);
  }
  void cook(VP::Cook &frame) override {
    std::ranges::copy(rects, frame.write(_rects, rects.size()).begin());
    const std::span<Label> labels = frame.write(_labels, lines.size());
    const std::span<Character> written = frame.write(_characters, characters);
    std::uint32_t next = 0;
    for (std::uint32_t line = 0; line < lines.size(); ++line) {
      labels[line] = {.offset = first_line + glm::ivec2(0, line_height * static_cast<std::int32_t>(line)),
                      .extent = room,
                      .role = line == 0 ? text : accent,
                      .first = next,
                      .count = static_cast<std::uint32_t>(lines[line].size())};
      for (const char code : lines[line])
        written[next++] = {.code = static_cast<std::uint32_t>(code), .label = line};
    }
  }

  VP::Upload<Rect> _rects;
  VP::Upload<Label> _labels;
  VP::Upload<Character> _characters;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Sign>("Sign");
}
