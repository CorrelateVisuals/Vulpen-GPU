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

File Ports::open(const std::filesystem::path &file) {
  for (auto &[id, open] : _open)
    if (open.path == file) {
      ++open.nodes;
      return {id};
    }
  Open open{.path = file};
  refresh(open);
  _open.emplace(++_opened, std::move(open));
  return {_opened};
}

void Ports::close(File file) {
  if (const auto found = _open.find(file.index);
      found != _open.end() && --found->second.nodes == 0)
    _open.erase(found);
}

Ports::Open &Ports::opened(File file) {
  const auto found = _open.find(file.index);
  if (found == _open.end())
    throw std::runtime_error("a file this node did not open when it last bound");
  return found->second;
}

// A file that is gone reads as empty, and is read again once it is back.
void Ports::refresh(Open &open) {
  std::error_code gone;
  const auto written = std::filesystem::last_write_time(open.path, gone);
  if (written == open.written)
    return;
  open.text = gone ? std::string() : read(open.path.string());
  open.written = written;
}

// Compared with the disk once a frame at most, so a node may ask every frame.
std::string_view Ports::text(File file) {
  Open &open = opened(file);
  if (open.checked != _frames) {
    open.checked = _frames;
    refresh(open);
  }
  return open.text;
}

void Ports::save(File file, std::string_view text) {
  Open &open = opened(file);
  Files::save(open.path, text);
  open.text = text;
  std::error_code gone;
  open.written = std::filesystem::last_write_time(open.path, gone);
}

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
  ++_frames;
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

void Ports::prompt(std::string_view text) {
  if (!Terminal::typed())
    return;
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

void Ports::command(Call &) {}

} // namespace VP
