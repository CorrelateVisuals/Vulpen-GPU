#include "contracts/Palette.h"
#include "runtime/Operator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view theme_file = "theme.ini";
constexpr std::uint32_t roles = VP_VIEW::role_names.size();
constexpr glm::length_t channels = 4; // red, green, blue and alpha
constexpr glm::length_t opaque = 3;   // the channels a color gives when its alpha is 1
constexpr std::string_view blanks = " \t\r";

using Colors = std::array<glm::vec4, roles>;

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// Linear, as the window's sRGB images take it; none when a channel is no number from 0
// to 1, or there are too few or too many.
std::optional<glm::vec4> color_of(std::string_view text) {
  glm::vec4 color(1.0f);
  glm::length_t given = 0;
  for (text = trim(text); !text.empty(); text = trim(text)) {
    const std::size_t length = std::min(text.find_first_of(blanks), text.size());
    float channel = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + length, channel);
    if (given == channels || error != std::errc{} || end != text.data() + length ||
        !(channel >= 0.0f && channel <= 1.0f))
      return std::nullopt;
    color[given++] = channel;
    text.remove_prefix(length);
  }
  if (given < opaque)
    return std::nullopt;
  return color;
}

// Each line gives a role its color, as role = red green blue, with alpha after when it
// is not opaque; # starts a comment.
Colors colors_of(std::string_view theme) {
  Colors colors{};
  std::array<bool, roles> given{};
  for (std::size_t line = 1; !theme.empty(); ++line) {
    const std::size_t end = std::min(theme.find('\n'), theme.size());
    const std::string_view text = trim(theme.substr(0, std::min(theme.find('#'), end)));
    theme.remove_prefix(std::min(end + 1, theme.size()));
    const auto mistake = [&](std::string_view why) {
      return std::runtime_error(std::format("{}:{}: {}", theme_file, line, why));
    };
    if (text.empty())
      continue;
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos)
      throw mistake("a line is a role, = and its color");
    const std::string_view name = trim(text.substr(0, equals));
    const std::string_view value = trim(text.substr(equals + 1));
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
