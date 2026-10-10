#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view keymap_file = "keymap.ini";
constexpr std::string_view typed_port = "typed";
constexpr std::string_view blanks = " \t\r";
// The keys a chord holds while it presses its last, as the input port names them; a bit
// each, in this order, in what a chord holds.
constexpr std::array<std::string_view, 4> modifiers{"shift", "control", "alt", "super"};

using Held = std::uint32_t;

// A keymap line: the keys held, the key pressed, and the line it sends.
struct Chord {
  Held held = 0;
  std::string key;
  std::string command;
};

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// A modifier's bit; none for any other key.
std::optional<Held> modifier(std::string_view key) {
  const auto found = std::ranges::find(modifiers, key);
  if (found == modifiers.end())
    return std::nullopt;
  return Held{1} << (found - modifiers.begin());
}

// The keys held and the key pressed, joined by +, as control+l.
Chord chord_of(std::string_view text, const VP::InputPort &input) {
  Chord chord;
  for (std::size_t plus = text.find('+'); plus != std::string_view::npos;
       plus = text.find('+')) {
    const std::optional<Held> bit = modifier(text.substr(0, plus));
    if (!bit)
      throw std::runtime_error(
          std::format("{} is no key a chord holds: shift, control, alt or super",
                      text.substr(0, plus)));
    chord.held |= *bit;
    text.remove_prefix(plus + 1);
  }
  if (modifier(text))
    throw std::runtime_error(std::format(
        "{} only holds: a chord ends in the key it presses, as control+l", text));
  if (!input.names_key(text))
    throw std::runtime_error(
        std::format("{} is no key a chord presses: one named by the character it prints "
                    "without shift, as l, or as enter, f1 and the like, as the input "
                    "command names it",
                    text));
  chord.key = text;
  return chord;
}

// Whether a command registered now takes the line: its usage's words before the first
// placeholder start it, so a misspelt command is found when the keymap is read.
bool registered(std::string_view line, std::span<const VP::Usage> usages) {
  return std::ranges::any_of(usages, [&](const VP::Usage &usage) {
    const std::string_view name =
        trim(usage.usage.substr(0, usage.usage.find_first_of("<[")));
    return line.starts_with(name) &&
           (line.size() == name.size() || line[name.size()] == ' ');
  });
}

// Each line binds a chord to the line it sends, as control+l = clear; # starts a comment.
std::vector<Chord> chords_of(std::string_view keymap,
                             const VP::InputPort &input,
                             std::span<const VP::Usage> usages) {
  std::vector<Chord> chords;
  for (std::size_t line = 1; !keymap.empty(); ++line) {
    const std::size_t end = std::min(keymap.find('\n'), keymap.size());
    const std::string_view text = trim(keymap.substr(0, std::min(keymap.find('#'), end)));
    keymap.remove_prefix(std::min(end + 1, keymap.size()));
    if (text.empty())
      continue;
    const std::size_t equals = text.find('=');
    try {
      if (equals == std::string_view::npos)
        throw std::runtime_error("a line is a chord, = and the line it sends");
      Chord chord = chord_of(trim(text.substr(0, equals)), input);
      chord.command = trim(text.substr(equals + 1));
      if (std::ranges::any_of(chords, [&](const Chord &given) {
            return given.held == chord.held && given.key == chord.key;
          }))
        throw std::runtime_error(
            std::format("{} is given twice", trim(text.substr(0, equals))));
      if (!registered(chord.command, usages))
        throw std::runtime_error(std::format("no command takes {}", chord.command));
      chords.push_back(std::move(chord));
    } catch (const std::runtime_error &mistake) {
      throw std::runtime_error(
          std::format("{}:{}: {}", keymap_file, line, mistake.what()));
    }
  }
  return chords;
}

// One owner for where a key goes: a keymap chord becomes the line it sends, and any other
// key, and text, goes to the node in focus, so a key does nothing a typed command cannot.
// Its focus param names the node that has the focus at first, and the focus command moves
// it. The keymap is read again once it changes; a mistake in it stops the node, naming
// the line (A02).
class Keys final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _focus = node.command("focus <node>",
                          "sends the keys and text no chord takes to a node that reads "
                          "them from the keys part");
    _name = node.name();
    _start = node.param<std::string>("focus");
    _keymap = node.file(keymap_file);
    _typed = &node.output<VP_VIEW::Typed>(typed_port);
  }

  // The events go first, so a node stopped by its keymap hands on none again.
  void cook(VP::Cook &frame) override {
    _typed->events.clear();
    const std::string_view keymap = frame.files().text(_keymap);
    if (!_read || keymap != *_read) {
      _chords = chords_of(keymap, frame.input(), frame.commands().usages());
      _read.emplace(keymap);
    }
    give_focus(frame);
    const std::span<const VP::Event> events = frame.input().events();
    _waiting.insert(_waiting.end(), events.begin(), events.end());
    bool pressed = false;
    std::size_t routed = 0;
    while (routed < _waiting.size() && route(frame, _waiting[routed], pressed))
      ++routed;
    _waiting.erase(_waiting.begin(),
                   _waiting.begin() + static_cast<std::ptrdiff_t>(routed));
  }

  // A focus line takes effect here, before the keys are handed on, so every node that
  // reads them in a frame finds the same node in focus. The param gives the focus at the
  // start, unless a line gave it first, as a script may; a rebuild leaves it.
  void give_focus(VP::Cook &frame) {
    if (!_started && !_next)
      frame.commands().send(std::format("focus {}", _start));
    _started = true;
    if (_next)
      _typed->focus = *std::exchange(_next, std::nullopt);
  }

  // False for what waits for the next frame, with all that came after it, so each lands
  // in the order it came: a key after a press, which may give another node the focus,
  // and a chord after keys handed on, whose line would run before the node in focus took
  // them. The window losing focus lets every modifier go, since their release then
  // reaches no window.
  bool route(VP::Cook &frame, const VP::Event &event, bool &pressed) {
    if (event.kind == VP::Event::Kind::focus && !event.down)
      _held = 0;
    pressed = pressed || (event.kind == VP::Event::Kind::button && event.down);
    if (event.kind != VP::Event::Kind::key && event.kind != VP::Event::Kind::text)
      return true;
    if (pressed)
      return false;
    const Chord *const chord =
        event.kind == VP::Event::Kind::key ? chord_pressed(event) : nullptr;
    if (chord && !_typed->events.empty())
      return false;
    if (chord)
      frame.commands().send(chord->command);
    else
      _typed->events.push_back(event);
    return true;
  }

  // The chord the key presses, if any. A modifier only changes what is held, and goes on
  // as any other key.
  const Chord *chord_pressed(const VP::Event &key) {
    if (const std::optional<Held> bit = modifier(key.name)) {
      _held = key.down ? _held | *bit : _held & ~*bit;
      return nullptr;
    }
    const auto chord = std::ranges::find_if(_chords, [&](const Chord &chord) {
      return chord.held == _held && chord.key == key.name;
    });
    return key.down && chord != _chords.end() ? &*chord : nullptr;
  }

  // Only a node that reads what this one hands on takes the focus, so a misspelt name is
  // refused, naming those that do, rather than leaving every key unread.
  void command(VP::Call &call) override {
    if (!call.is(_focus))
      return;
    const std::string_view node = call.arguments().front();
    std::string readers;
    for (const VP::Connection &connection : call.view().connections)
      if (connection.from.node == _name && connection.from.port == typed_port)
        for (const VP::Endpoint &to : connection.to) {
          if (to.node == node) {
            _next.emplace(node);
            return;
          }
          readers += std::format("{}{}", readers.empty() ? "" : ", ", to.node);
        }
    throw std::runtime_error(
        std::format("{} reads no keys from {}; {}",
                    node,
                    _name,
                    readers.empty() ? "no node does" : "these do: " + readers));
  }

  VP::Command _focus;
  std::string _name;
  std::string _start;               // the node its focus param names
  bool _started = false;            // whether the param gave the focus yet
  std::optional<std::string> _next; // the node a focus line gave, for the next frame
  VP::File _keymap;
  std::optional<std::string> _read; // the keymap the chords are from
  std::vector<Chord> _chords;
  Held _held = 0;                  // the modifiers down now
  std::vector<VP::Event> _waiting; // from the first that waits, for the next frame
  VP_VIEW::Typed *_typed = nullptr;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Keys>("Keys");
}
