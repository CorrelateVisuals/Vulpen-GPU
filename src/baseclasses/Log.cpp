#include "baseclasses/Log.h"

#include "baseclasses/Platform.h"

#include <cstdio>
#include <format>

namespace VP {

namespace {

// The time column: the date and time at a line that starts a new second, blank at the
// lines after it, so a burst of lines reads as one block.
constexpr const char *time_format = " %y.%m.%d %H:%M:%S ";
constexpr int time_width = 19;

// The header and the footer; with the time column, each line fits 80 columns.
constexpr std::string_view rule =
    "+---------------------------------------------------------+";
constexpr std::string_view banner = "              . - < < { V U L P E N } > > - .";
constexpr std::string_view credit = "           << Jakob Povel | Correlate Visuals >>";

// ANSI colors. Red and yellow are kept for problems, so nothing else takes them.
constexpr const char *red = "\033[1;31m";
constexpr const char *yellow = "\033[1;33m";
constexpr const char *green = "\033[32m";
constexpr const char *blue = "\033[94m";
constexpr const char *magenta = "\033[35m";
constexpr const char *cyan = "\033[36m";
constexpr const char *bold = "\033[1m";
constexpr const char *dim = "\033[2m";
constexpr const char *plain = "\033[0m";

// A tag as it prints, and the one color it prints in.
struct Mark {
  std::string_view text;
  const char *color;
};

// By Tag.
constexpr std::array marks{Mark{"{run}", bold},
                           Mark{"{gpu}", green},
                           Mark{"{swp}", green},
                           Mark{"{nod}", blue},
                           Mark{"{mem}", blue},
                           Mark{"{mod}", magenta},
                           Mark{"{out}", cyan}};
// By Level: an error, a warning.
constexpr std::array problems{Mark{"{!!!}", red}, Mark{"{ ! }", yellow}};
constexpr Mark repeat{"{rep}", dim};
constexpr std::size_t count_capacity = 64;

} // namespace

Log::Log(Level level, std::string_view title)
    : _level(level), _colors(Terminal::colors()) {
  for (const std::string_view line : {rule, banner, rule})
    emit({}, plain, line);
  print(Level::info, Tag::run, {}, title);
}

Log::~Log() {
  print_repeats();
  for (const std::string_view line : {rule, credit})
    emit({}, plain, line);
}

Level Log::level() const {
  return _level;
}

void Log::write(Level level, Tag tag, std::string_view text) const {
  if (level <= _level)
    print(level, tag, {}, text);
}

void Log::write(Level level,
                Level shown,
                Tag tag,
                std::string_view node,
                std::string_view text) const {
  if (level <= shown)
    print(level, tag, node, text);
}

// A line the same as the one before only counts, until another line comes.
void Log::print(Level level,
                Tag tag,
                std::string_view node,
                std::string_view text) const {
  const Mark mark = level <= Level::warn ? problems[static_cast<std::size_t>(level)]
                                         : marks[static_cast<std::size_t>(tag)];
  _line.assign(mark.text).append(" ");
  if (!node.empty())
    _line.append(node).append(": ");
  // Text read from a file, such as a build's output, ends in a newline the line adds.
  _line.append(text.substr(0, text.find_last_not_of('\n') + 1));
  if (_line == _previous) {
    ++_repeats;
    return;
  }
  print_repeats();
  emit(mark.text, mark.color, std::string_view(_line).substr(mark.text.size() + 1));
  _line.swap(_previous);
}

void Log::print_repeats() const {
  if (_repeats == 0)
    return;
  std::array<char, count_capacity> text{};
  const char *const end =
      std::format_to_n(text.data(), text.size(), "previous line repeated {}x", _repeats)
          .out;
  _repeats = 0;
  emit(repeat.text, repeat.color, {text.data(), end});
}

// A tag takes its color, and each further line of the text starts under the first.
void Log::emit(std::string_view tag, const char *color, std::string_view text) const {
  std::array<char, time_width + 1> time{};
  if (const std::time_t now = std::time(nullptr); now != _second) {
    _second = now;
    const std::tm local = local_time(now);
    std::strftime(time.data(), time.size(), time_format, &local);
  }
  std::printf("%-*s", time_width, time.data());
  if (!tag.empty())
    std::printf("%s%.*s%s ",
                _colors ? color : "",
                static_cast<int>(tag.size()),
                tag.data(),
                _colors ? plain : "");
  const int indent = time_width + static_cast<int>(tag.empty() ? 0 : tag.size() + 1);
  for (std::size_t end = text.find('\n'); end != std::string_view::npos;
       end = text.find('\n')) {
    std::printf("%.*s\n%*s", static_cast<int>(end), text.data(), indent, "");
    text.remove_prefix(end + 1);
  }
  std::printf("%.*s\n", static_cast<int>(text.size()), text.data());
  std::fflush(stdout);
}

} // namespace VP
