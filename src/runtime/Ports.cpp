#include "runtime/Ports.h"

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"

#include <cstdio>
#include <optional>
#include <utility>

namespace VP {

void Ports::frame() {
  _lines.clear();
  _read = false;
}

// Once a frame, for every node that asks. A line ends at its break; one that standard
// input ends in counts too.
std::span<const std::string> Ports::lines() {
  if (_read || _ended)
    return _lines;
  _read = true;
  const std::optional<std::string> text = Terminal::input();
  if (!text) {
    _ended = true;
    if (!_partial.empty())
      _lines.push_back(std::exchange(_partial, {}));
    return _lines;
  }
  _partial += *text;
  for (std::size_t end = _partial.find('\n'); end != std::string::npos;
       end = _partial.find('\n')) {
    _lines.push_back(_partial.substr(0, end));
    if (_lines.back().ends_with('\r'))
      _lines.back().pop_back();
    _partial.erase(0, end + 1);
  }
  return _lines;
}

bool Ports::ended() const {
  return _ended;
}

void Ports::print(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  if (!text.ends_with('\n'))
    std::fputc('\n', stdout);
  std::fflush(stdout);
}

void Ports::command(Call &) {}

} // namespace VP
