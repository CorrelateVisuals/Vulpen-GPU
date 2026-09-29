#pragma once

#include <array>
#include <cstdio>
#include <optional>
#include <string_view>

namespace VP {

enum class Level { error, warn, info, debug };

inline constexpr std::array level_names{std::string_view{"error"},
                                        std::string_view{"warn"},
                                        std::string_view{"info"},
                                        std::string_view{"debug"}};

inline std::optional<Level> level_named(std::string_view name) {
  for (std::size_t index = 0; index < level_names.size(); ++index)
    if (level_names[index] == name)
      return static_cast<Level>(index);
  return std::nullopt;
}

// The bottom of the tree: every module logs, so Log includes nothing of ours.
class Log {
public:
  explicit Log(Level level) : _level(level) {}

  Level level() const {
    return _level;
  }
  void write(Level level, std::string_view text) const {
    write(level, _level, text);
  }
  // A node's own level wins over the process level (V09).
  void write(Level level, Level shown, std::string_view text) const {
    if (level > shown)
      return;
    const std::string_view prefix = level <= Level::warn
                                        ? level_names[static_cast<std::size_t>(level)]
                                        : std::string_view{};
    std::printf("%.*s%s%.*s\n",
                static_cast<int>(prefix.size()),
                prefix.data(),
                prefix.empty() ? "" : ": ",
                static_cast<int>(text.size()),
                text.data());
    std::fflush(stdout);
  }

private:
  Level _level;
};

} // namespace VP
