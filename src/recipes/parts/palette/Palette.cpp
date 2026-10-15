#include "contracts/Palette.h"
#include "runtime/Operator.h"
#include "shared/Ini.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view theme_file = "theme.ini";
constexpr std::uint32_t roles = VP_VIEW::role_names.size();
constexpr std::size_t channels = 4; // red, green, blue and alpha
constexpr std::size_t opaque = 3;   // the channels a color gives when its alpha is 1

using Colors = std::array<glm::vec4, roles>;

// Linear, as the window's sRGB images take it; none when a channel is no number from 0
// to 1, or there are too few or too many.
std::optional<glm::vec4> color_of(std::string_view text) {
  const std::vector<float> given = numbers_of(text);
  const auto unit = [](float channel) { return channel >= 0.0f && channel <= 1.0f; };
  if (given.size() < opaque || given.size() > channels ||
      !std::ranges::all_of(given, unit))
    return std::nullopt;
  glm::vec4 color(1.0f);
  for (std::size_t at = 0; at < given.size(); ++at)
    color[static_cast<glm::length_t>(at)] = given[at];
  return color;
}

// Each row gives a role its color, as role = red green blue, with alpha after when it is
// not opaque.
Colors colors_of(std::string_view theme) {
  Colors colors{};
  std::array<bool, roles> given{};
  for (const Row &row : rows_of(theme)) {
    const auto mistake = [&](std::string_view why) {
      return std::runtime_error(std::format("{}:{}: {}", theme_file, row.line, why));
    };
    const auto pair = key_value(row.text);
    if (!pair)
      throw mistake("a line is a role, = and its color");
    const auto [name, value] = *pair;
    const auto role = std::ranges::find(VP_VIEW::role_names, name);
    if (role == VP_VIEW::role_names.end()) {
      std::string names;
      for (const std::string_view known : VP_VIEW::role_names)
        names += std::format("{}{}", names.empty() ? "" : ", ", known);
      throw mistake(std::format("{} is no role; the roles are {}", name, names));
    }
    const auto index = static_cast<std::size_t>(role - VP_VIEW::role_names.begin());
    if (given[index])
      throw mistake(std::format("{} is given twice", name));
    const std::optional<glm::vec4> color = color_of(value);
    if (!color)
      throw mistake(std::format("{} is no color: red, green and blue from 0 to 1, with "
                                "alpha after when it is not opaque",
                                value));
    colors[index] = *color;
    given[index] = true;
  }
  for (std::size_t role = 0; role < roles; ++role)
    if (!given[role])
      throw std::runtime_error(
          std::format("{} gives {} no color", theme_file, VP_VIEW::role_names[role]));
  return colors;
}

// Reads theme.ini again once it changes, so a theme edit reaches every panel while the
// view runs. A mistake in it stops the node, naming the line (A02).
class Palette final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _theme = node.file(theme_file);
    _palette = node.upload<glm::vec4>("palette", roles);
  }
  // Written every frame, as it is a few bytes, so a buffer made anew for a connection
  // holds the colors too.
  void cook(VP::Cook &frame) override {
    const std::string_view theme = frame.files().text(_theme);
    if (!_read || theme != *_read) {
      _colors = colors_of(theme);
      _read.emplace(theme);
    }
    std::ranges::copy(_colors, frame.write(_palette).begin());
  }

  VP::File _theme;
  VP::Upload<glm::vec4> _palette;
  std::optional<std::string> _read; // the theme the colors are from
  Colors _colors{};
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Palette>("Palette");
}
