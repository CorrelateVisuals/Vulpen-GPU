#include "runtime/Ports.h"

#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
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

std::uint32_t presses_of(std::string_view word) {
  std::uint32_t presses = 0;
  const auto [end, error] =
      std::from_chars(word.data(), word.data() + word.size(), presses);
  if (error != std::errc{} || end != word.data() + word.size() || presses == 0)
    throw std::runtime_error(std::format(
        "{} is no count of presses: 1 for one, 2 for a double click's second", word));
  return presses;
}

// What a terminal sends for a key: a byte for most, control and a letter one below the
// blank, and for the arrows, home, end and delete an escape sequence, which ends in a
// byte from @ to ~: a letter, or ~ after a number.
constexpr unsigned char escape = 0x1b;
constexpr unsigned char erase = 0x7f;
constexpr unsigned char first_text = 0x20;
constexpr char sequence_end_first = '@';
constexpr char sequence_end_last = '~';
constexpr char control_letter = 'a' - 1; // control+a is byte 1
constexpr std::array<std::pair<std::string_view, std::string_view>, 11> sequences{{
    {"A", "up"},
    {"B", "down"},
    {"C", "right"},
    {"D", "left"},
    {"H", "home"},
    {"F", "end"},
    {"1~", "home"},
    {"7~", "home"},
    {"4~", "end"},
    {"8~", "end"},
    {"3~", "delete"},
}};

void press(std::vector<Event> &events, std::string_view name) {
  for (const bool down : {true, false})
    events.push_back({.kind = Event::Kind::key, .down = down, .name = std::string(name)});
}

// The key the bytes start with, as a window names it, into events; returns how many
// bytes it took, none while they end inside it. A sequence no key here sends is passed
// over.
std::size_t key(std::string_view bytes, std::vector<Event> &events) {
  const auto first = static_cast<unsigned char>(bytes.front());
  if (first == escape) {
    if (bytes.size() < 2)
      return 0;
    if (bytes[1] != '[' && bytes[1] != 'O') {
      press(events, "escape");
      return 1;
    }
    std::size_t end = 2;
    while (end < bytes.size() &&
           (bytes[end] < sequence_end_first || bytes[end] > sequence_end_last))
      ++end;
    if (end == bytes.size())
      return 0;
    const std::string_view sequence = bytes.substr(2, end - 1);
    for (const auto &[sent, name] : sequences)
      if (sent == sequence)
        press(events, name);
    return end + 1;
  }
  if (first == '\r' || first == '\n')
    press(events, "enter");
  else if (first == '\t')
    press(events, "tab");
  else if (first == erase || first == '\b')
    press(events, "backspace");
  else if (first < first_text) {
    events.push_back({.kind = Event::Kind::key, .down = true, .name = "control"});
    press(events, std::string(1, static_cast<char>(first + control_letter)));
    events.push_back({.kind = Event::Kind::key, .down = false, .name = "control"});
  } else {
    const auto text_end = std::ranges::find_if(bytes, [](char byte) {
      const auto value = static_cast<unsigned char>(byte);
      return value < first_text || value == erase;
    });
    const auto length = static_cast<std::size_t>(text_end - bytes.begin());
    events.push_back(
        {.kind = Event::Kind::text, .name = std::string(bytes.substr(0, length))});
    return length;
  }
  return 1;
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
  // Read in one go, not a character at a time, as a folder's reading reads megabytes; a
  // folder, or anything else no regular file, has no size. A file that shrank since its
  // size was asked keeps what was there.
  std::error_code gone;
  const std::uintmax_t size = std::filesystem::file_size(path, gone);
  if (!in || gone)
    throw std::runtime_error(std::format("{}: cannot be read", file));
  std::string text(size, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(in.gcount()));
  return text;
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

void Ports::copy(std::string_view from, std::string_view to) {
  Files::copy(absolute(from), absolute(to));
}

void Ports::rename(std::string_view from, std::string_view to) {
  Files::rename(absolute(from), absolute(to));
}

void Ports::save(const View &view, std::string_view file) {
  View moved = view;
  moved.file = absolute(file);
  Manifest::save(moved);
}

void Ports::start(std::string_view program, std::span<const std::string> arguments) {
  Shell::start(std::string(program), arguments);
}

// As a view loads it, so a recipe read here is what a drop copies.
View Ports::manifest(std::string_view file) {
  View view = Manifest::load(absolute(file));
  Manifest::refresh(view);
  return Manifest::flatten(view);
}

Ports::Ports(const Log &log) : _log(log) {}

Ports::~Ports() {
  if (!_raw)
    return;
  std::fwrite(Terminal::clear_row.data(), 1, Terminal::clear_row.size(), stdout);
  std::fflush(stdout);
  _log.clears_row(false);
}

void Ports::frame() {
  ++_frames;
  _lines.clear();
  _read = false;
  _keys.clear();
  _keyed = false;
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
      .button_down = form("input button down <value> [<value>]",
                          "presses a pointer button: left, right or middle; a count "
                          "after it, as 2, makes it the second press of a double click"),
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

void Ports::attach(const Window *window) {
  _window = window;
}

bool Ports::windowed() const {
  return _window != nullptr;
}

std::string Ports::clipboard() const {
  return _window ? _window->clipboard() : _clipboard;
}

// Kept here too, so a window that closes leaves its last copy behind.
void Ports::copy(std::string_view text) {
  _clipboard = text;
  if (_window)
    _window->copy(_clipboard);
}

bool Ports::typed() const {
  return Terminal::typed();
}

// Once a frame, for every node that asks.
std::span<const Event> Ports::keys() {
  if (_keyed || _ended)
    return _keys;
  _keyed = true;
  if (!_raw) {
    _raw = std::make_unique<RawTerminal>();
    _log.clears_row(true);
  }
  const std::optional<std::string> text = Terminal::input();
  if (!text) {
    _ended = true;
    return _keys;
  }
  _bytes += *text;
  std::size_t at = 0;
  while (at < _bytes.size()) {
    const std::size_t took = key(std::string_view(_bytes).substr(at), _keys);
    if (took == 0)
      break;
    at += took;
  }
  _bytes.erase(0, at);
  return _keys;
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

// In place of the line typed, which is drawn again after.
void Ports::print(std::string_view text) {
  if (_raw)
    std::fwrite(Terminal::clear_row.data(), 1, Terminal::clear_row.size(), stdout);
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

// As much around the caret as a row holds, so a long line scrolls across as in a window.
void Ports::line(std::string_view text, std::size_t caret) {
  const std::size_t room = std::max<std::size_t>(Terminal::columns(), 2) - 1;
  const std::size_t start = std::min(caret > room ? caret - room : 0, text.size());
  Terminal::draw(text.substr(start, room), caret - start);
}

std::size_t Ports::columns() const {
  return Terminal::columns();
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
    const bool down = call.is(_input.button_down);
    add({.kind = Event::Kind::button,
         .down = down,
         .name = checked_button(words[0]),
         .presses = !down ? 0 : words.size() > 1 ? presses_of(words[1]) : 1});
  } else if (call.is(_input.wheel)) {
    add({.kind = Event::Kind::wheel, .turn = {number(words[0]), number(words[1])}});
  } else {
    add({.kind = Event::Kind::focus, .down = call.is(_input.focus_on)});
  }
}

} // namespace VP
