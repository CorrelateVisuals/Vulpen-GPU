#include "runtime/Ports.h"

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/View.h"

#include <algorithm>
#include <charconv>
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
        "{} is relative; a node names a file from its folder, and a command's <file> "
        "comes resolved",
        path));
  return path;
}

float number(std::string_view word) {
  float value = 0;
  const auto [end, error] =
      std::from_chars(word.data(), word.data() + word.size(), value);
  if (error != std::errc{} || end != word.data() + word.size())
    throw std::runtime_error(std::format("{} is not a number", word));
  return value;
}

std::string checked_key(std::string_view name) {
  if (!key_named(name))
    throw std::runtime_error(std::format(
        "{} is no key: a key is named by the character it prints without shift, as a, "
        "or as space, enter, escape, tab, backspace, insert, delete, left, right, up, "
        "down, page_up, page_down, home, end, shift, control, alt, super, or f1 to f12",
        name));
  return std::string(name);
}

std::string checked_button(std::string_view name) {
  if (name != "left" && name != "right" && name != "middle")
    throw std::runtime_error(
        std::format("{} is no button: a button is left, right or middle", name));
  return std::string(name);
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

void Ports::remove(std::string_view file) {
  Files::remove(absolute(file));
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

// As a view loads it, so a recipe read here is what a drop copies.
View Ports::manifest(std::string_view file) {
  View view = Manifest::load(absolute(file));
  Manifest::refresh(view);
  return Manifest::flatten(view);
}

void Ports::frame() {
  ++_frames;
  _lines.clear();
  _read = false;
  _events.clear();
  std::swap(_events, _pending);
  for (const Event &event : _events)
    if (event.kind == Event::Kind::pointer)
      _pointer = event.at;
}

void Ports::add(Event event) {
  if (event.kind == Event::Kind::pointer && !_pending.empty() &&
      _pending.back().kind == Event::Kind::pointer)
    _pending.back() = std::move(event);
  else
    _pending.push_back(std::move(event));
}

void Ports::listen(Commands &commands) {
  const auto form = [&](std::string_view usage, std::string_view help) {
    return commands.add(usage, help, *this);
  };
  _input = {
      .key_down = form("input key down <value>",
                       "presses a key, named by the character it prints, or as space, "
                       "enter or f1, for the next frame, as a window does"),
      .key_up = form("input key up <value>", "lets a key go"),
      .text = form("input text <value>...", "types the words, joined by single blanks"),
      .pointer =
          form("input pointer <value> <value>",
               "moves the pointer to x and y, in pixels from the window's top left"),
      .button_down = form("input button down <value>",
                          "presses a pointer button: left, right or middle"),
      .button_up = form("input button up <value>", "lets a pointer button go"),
      .wheel = form("input wheel <value> <value>", "turns the wheel by x and y"),
      .focus_on = form("input focus on", "gives the window focus"),
      .focus_off = form("input focus off", "takes focus from the window")};
}

std::span<const Event> Ports::events() const {
  return _events;
}

glm::vec2 Ports::pointer() const {
  return _pointer;
}

bool Ports::names_key(std::string_view name) const {
  return key_named(name);
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

// What the window hands on for each, so a headless run makes the same events (V07).
void Ports::command(Call &call) {
  const std::span<const std::string_view> words = call.arguments();
  if (call.is(_input.key_down) || call.is(_input.key_up)) {
    add({.kind = Event::Kind::key,
         .down = call.is(_input.key_down),
         .name = checked_key(words[0])});
  } else if (call.is(_input.text)) {
    std::string typed;
    for (const std::string_view word : words)
      typed.append(typed.empty() ? "" : " ").append(word);
    add({.kind = Event::Kind::text, .name = std::move(typed)});
  } else if (call.is(_input.pointer)) {
    add({.kind = Event::Kind::pointer, .at = {number(words[0]), number(words[1])}});
  } else if (call.is(_input.button_down) || call.is(_input.button_up)) {
    add({.kind = Event::Kind::button,
         .down = call.is(_input.button_down),
         .name = checked_button(words[0])});
  } else if (call.is(_input.wheel)) {
    add({.kind = Event::Kind::wheel, .turn = {number(words[0]), number(words[1])}});
  } else {
    add({.kind = Event::Kind::focus, .down = call.is(_input.focus_on)});
  }
}

} // namespace VP
