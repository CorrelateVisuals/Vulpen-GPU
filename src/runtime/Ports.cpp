#include "runtime/Ports.h"

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace VP {

namespace {

// A relative path would resolve against the working directory (RP02).
std::filesystem::path absolute(std::string_view path) {
  if (!std::filesystem::path(path).is_absolute())
    throw std::runtime_error(std::format(
        "{} is relative; a recipe names a file from its folder, and a command's <file> "
        "comes resolved",
        path));
  return path;
}

} // namespace

std::string Ports::read(std::string_view file) {
  const std::filesystem::path path = absolute(file);
  std::ifstream in(path, std::ios::binary);
  std::error_code gone;
  if (!in || !std::filesystem::is_regular_file(path, gone))
    throw std::runtime_error(std::format("{}: cannot be read", file));
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Ports::save(std::string_view file, std::string_view text) {
  Files::save(absolute(file), text);
}

std::vector<std::string> Ports::list(std::string_view folder) {
  std::error_code failed;
  const std::filesystem::directory_iterator entries(absolute(folder), failed);
  if (failed && failed != std::errc::no_such_file_or_directory)
    throw std::runtime_error(
        std::format("{}: cannot be listed: {}", folder, failed.message()));
  std::vector<std::string> names;
  for (const std::filesystem::directory_entry &entry : entries) {
    std::error_code gone; // a file an editor is replacing may vanish mid-listing
    names.push_back(entry.path().filename().string() +
                    (entry.is_directory(gone) ? "/" : ""));
  }
  std::ranges::sort(names);
  return names;
}

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
