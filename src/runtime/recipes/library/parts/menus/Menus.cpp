#include "contracts/Item.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "shared/Ini.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view menu_file = "menu.ini";
constexpr char between = '/'; // a menu's name, then its item's
// After the label of an item whose line wants a word, as a menu marks one that asks for
// more.
constexpr std::string_view wants_more = "...";

// A menu.ini row: the menu, its item and the line the item types, up to its first
// placeholder when it has one, which the person then types.
struct Entry {
  std::string menu;
  std::string item;
  std::string line;
  bool wants = false;
};

Entry entry_of(std::string_view text, std::span<const VP::Usage> usages) {
  const auto pair = key_value(text);
  const std::size_t slash = pair ? pair->first.find(between) : std::string_view::npos;
  if (slash == std::string_view::npos || trim(pair->first.substr(0, slash)).empty() ||
      trim(pair->first.substr(slash + 1)).empty())
    throw std::runtime_error(
        "a line is a menu, /, an item, = and the line it sends, as file/write = write");
  const std::string_view line = pair->second;
  const std::size_t placeholder = line.find_first_of("<[");
  Entry entry{.menu = std::string(trim(pair->first.substr(0, slash))),
              .item = std::string(trim(pair->first.substr(slash + 1))),
              .line = std::string(trim(line.substr(0, placeholder))),
              .wants = placeholder != std::string_view::npos};
  if (!registered(entry.line, usages))
    throw std::runtime_error(std::format("no command takes {}", line));
  return entry;
}

// Each row an item of a menu, in the order they come.
std::vector<Entry> entries_of(std::string_view text, std::span<const VP::Usage> usages) {
  std::vector<Entry> entries;
  for (const Row &row : rows_of(text)) {
    try {
      Entry entry = entry_of(row.text, usages);
      if (std::ranges::any_of(entries, [&](const Entry &given) {
            return given.menu == entry.menu && given.item == entry.item;
          }))
        throw std::runtime_error(
            std::format("{}/{} is given twice", entry.menu, entry.item));
      entries.push_back(std::move(entry));
    } catch (const std::runtime_error &mistake) {
      throw std::runtime_error(
          std::format("{}:{}: {}", menu_file, row.line, mistake.what()));
    }
  }
  return entries;
}

// The menus menu.ini gives, a line an item, so a menu does nothing a typed line cannot
// (V06) and a person changes it in a file. It hands on a menu a name, as names, whose
// press opens it, and the items of the one open, as items, whose press types the item's
// line into the node its line param names, as the terminal, and sends it there, so the
// line goes where a typed one goes and what it answers shows there; one whose line wants
// a word, marked ..., is begun there instead, completed up to the word, which the person
// types, and keeps the focus. While a menu is open it holds the pointer, so nothing under
// its items takes a press, and the focus, so escape shuts it; a press anywhere but on
// another name shuts it too, giving the focus back. A change takes effect the next frame,
// as the focus does, so a press on an item reaches its hit before the items go. The file
// is read again once it changes; a mistake in it stops the node, naming the line (A02).
class Menus final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _menu = node.command("menu [<value>...]",
                         "opens a menu of menu.ini, its items below its name; with an "
                         "item after it, types the item's line into the terminal and "
                         "sends it, or begins it there when it wants a word; with none, "
                         "shuts the menu open");
    _name = node.name();
    _line = node.param<std::string>("line");
    _file = node.file(menu_file);
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _names = &node.output<VP_VIEW::Items>("names");
    _items = &node.output<VP_VIEW::Items>("items");
    _written = false;
  }

  void cook(VP::Cook &frame) override {
    const std::string_view text = frame.files().text(_file);
    if (!_read || text != *_read) {
      _entries = entries_of(text, frame.commands().usages());
      _read.emplace(text);
      _written = false;
    }
    // A press on another name both shuts this one and opens that one: the name wins.
    if (_next)
      open(frame, *std::exchange(_next, std::nullopt));
    else if (_shutting)
      open(frame, {});
    _shutting = false;
    if (!_open.empty())
      hold(frame);
    if (!_written)
      write();
  }

  // Shut with none, giving the focus back unless an item began a line.
  void open(VP::Cook &frame, std::string menu) {
    if (menu == _open)
      return;
    if (_open.empty()) {
      _returning = _typed->focus;
      _begun = false;
      frame.commands().send(std::format("focus {}", _name));
    } else if (menu.empty() && !std::exchange(_begun, false)) {
      frame.commands().send(std::format("focus {}", _returning));
    }
    _open = std::move(menu);
    _written = false;
  }

  // As it holds the pointer, every press reaches it, and its own names' hit too.
  void hold(VP::Cook &frame) {
    frame.hold_pointer();
    for (const VP::Event &event : frame.input().events())
      _shutting = _shutting || (event.kind == VP::Event::Kind::button && event.down);
    for (const VP::Event &event : _typed->to(_name))
      _shutting = _shutting || (event.kind == VP::Event::Kind::key && event.down &&
                                event.name == "escape");
  }

  // Only as they change, so a frame allocates nothing for them (CPP10).
  void write() {
    _written = true;
    _names->clear();
    _items->clear();
    for (const Entry &entry : _entries) {
      if (std::ranges::none_of(*_names, [&](const VP_VIEW::Item &named) {
            return named.label == entry.menu;
          }))
        _names->push_back({.label = entry.menu,
                           .command = entry.menu == _open ? "menu" : "menu " + entry.menu,
                           .current = entry.menu == _open});
      if (entry.menu == _open)
        _items->push_back(
            {.label = entry.wants ? entry.item + std::string(wants_more) : entry.item,
             .command = std::format("menu {} {}", entry.menu, entry.item)});
    }
  }

  // An item types its line as a person would, in the terminal, so the line shows there,
  // its history keeps it and its view is the current one: enter sends it, and Tab
  // finishes the words a line that wants one has, a blank after them.
  void command(VP::Call &call) override {
    if (!call.is(_menu))
      return;
    const std::span<const std::string_view> words = call.arguments();
    if (words.empty()) {
      _next.emplace();
      return;
    }
    const auto named = [&](const Entry &entry) { return entry.menu == words.front(); };
    if (std::ranges::none_of(_entries, named))
      throw std::runtime_error(std::format(
          "no menu is named {}; {} gives {}", words.front(), menu_file, menus()));
    if (words.size() == 1) {
      _next.emplace(words.front());
      return;
    }
    std::string item(words[1]);
    for (const std::string_view word : words.subspan(2))
      item.append(1, ' ').append(word);
    const auto entry = std::ranges::find_if(_entries, [&](const Entry &entry) {
      return named(entry) && entry.item == item;
    });
    if (entry == _entries.end())
      throw std::runtime_error(
          std::format("menu {} has no item {}", words.front(), item));
    const std::string_view key = entry->wants ? "tab" : "enter";
    call.commands().send(std::format("focus {}", _line));
    call.commands().send(std::format("input text {}", entry->line));
    call.commands().send(std::format("input key down {}", key));
    call.commands().send(std::format("input key up {}", key));
    _begun = entry->wants;
    _next.emplace();
  }

  std::string menus() const {
    std::string menus;
    for (const VP_VIEW::Item &named : *_names)
      menus += std::format("{}{}", menus.empty() ? "" : ", ", named.label);
    return menus;
  }

  VP::Command _menu;
  std::string _name;
  std::string _line; // the node whose line an item types into
  VP::File _file;
  std::optional<std::string> _read; // the file the entries are from
  std::vector<Entry> _entries;
  const VP_VIEW::Typed *_typed = nullptr;
  VP_VIEW::Items *_names = nullptr;
  VP_VIEW::Items *_items = nullptr;
  std::string _open;                // the menu open; empty for none
  std::optional<std::string> _next; // the menu a line opened, for the next frame
  bool _shutting = false;           // a press or escape, which shuts it the next frame
  bool _begun = false;              // an item began a line, which keeps the focus
  std::string _returning;           // the node in focus as it opened
  bool _written = false;            // the names and items are as the above say
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Menus>("Menus");
}
