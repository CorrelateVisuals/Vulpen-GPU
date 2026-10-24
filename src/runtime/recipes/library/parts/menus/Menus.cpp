#include "contracts/Item.h"
#include "contracts/Rect.h"
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
// The word before what a menu opened for fills its items' lines with, as a tab's.
constexpr std::string_view for_word = "for";

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

// What a press or a line opens: the menu whose items show, none for the names alone, at
// a corner; with none, where they show, or the area's top left as the frame lays it out;
// and what it opens for, as a tab's file, which fills the word its items' lines want, at
// the pointer.
struct Opening {
  std::string menu;
  std::optional<glm::ivec2> corner;
  std::string subject;
};

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
// (V06) and a person changes it in a file. A right press in its area opens them there, as
// a column of names, which it hands on as names, and the place they drop below as at; a
// press on a name opens that menu's items beside it, which it hands on as items, and a
// press on an item types the item's line into the node its line param names, as the
// terminal, and sends it there, so the line goes where a typed one goes and what it
// answers shows there; one whose line wants a word, marked ..., is begun there instead,
// completed up to the word, which the person types, and keeps the focus. While it shows
// it holds the pointer, so nothing under it takes a press, and the focus, so escape shuts
// it; a press anywhere but on a name or an item shuts it too, giving the focus back, and
// a right press in its area opens it there anew. A change takes effect the next frame, as
// the focus does, so a press on an item reaches its hit before the items go. The file is
// read again once it changes; a mistake in it stops the node, naming the line (A02).
class Menus final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _menu = node.command("menu [<value>...]",
                         "opens a menu of menu.ini, its items beside its name; with an "
                         "item after it, types the item's line into the terminal and "
                         "sends it, or begins it there when it wants a word; with for "
                         "and a word, opens it alone at the pointer, the word ending the "
                         "lines of its items that want one; with none, shuts the menus");
    _name = node.name();
    _line = node.param<std::string>("line");
    _file = node.file(menu_file);
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _area = &node.input<VP_VIEW::Rect>("area");
    _names = &node.output<VP_VIEW::Items>("names");
    _items = &node.output<VP_VIEW::Items>("items");
    _at = &node.output<VP_VIEW::Rect>("at");
    _written = false;
  }

  void cook(VP::Cook &frame) override {
    read(frame.files(), frame.commands());
    // A line opening it for a subject came in before this frame's pointer reached this
    // node, as a tab's right press sends it, so it opens where the frame left the
    // pointer.
    if (_next && !_next->subject.empty())
      for (const VP::Event &event : frame.input().events())
        if (event.kind == VP::Event::Kind::pointer)
          _next->corner = glm::ivec2(event.at);
    // A press on a name both shuts the menus and opens that one: the name wins.
    const bool opening = _next.has_value();
    if (_next)
      open(frame, *std::exchange(_next, std::nullopt));
    else if (_shutting)
      shut(frame);
    _shutting = false;
    follow(frame, opening);
    *_at = _shown ? VP_VIEW::Rect{.offset = _corner, .extent = {1, 0}} : VP_VIEW::Rect{};
    if (!_written)
      write();
  }

  // As it changed, or for a line before the first frame, as a script's, once every
  // command it names has registered.
  void read(VP::FilePort &files, VP::CommandPort &commands) {
    const std::string_view text = files.text(_file);
    if (_read && text == *_read)
      return;
    _entries = entries_of(text, commands.usages());
    _read.emplace(text);
    _written = false;
  }

  // Shown at the corner given, with the menu named open, or none.
  void open(VP::Cook &frame, Opening opening) {
    if (!_shown) {
      _returning = _typed->focus;
      _begun = false;
      frame.commands().send(std::format("focus {}", _name));
    }
    _corner = opening.corner.value_or(_shown ? _corner : _area->offset);
    _shown = true;
    _open = std::move(opening.menu);
    _subject = std::move(opening.subject);
    _written = false;
  }

  // Gives the focus back unless an item began a line.
  void shut(VP::Cook &frame) {
    if (!_shown)
      return;
    if (!std::exchange(_begun, false))
      frame.commands().send(std::format("focus {}", _returning));
    _shown = false;
    _open.clear();
    _subject.clear();
    _written = false;
  }

  // While shut, a right press in the area opens it there. While it shows it holds the
  // pointer, so every press reaches it, and its own hits too: a press shuts it, as its
  // hits send what a name or an item does, which wins; a right press in the area opens it
  // anew there, and escape shuts it. A press in the frame it opens in shuts nothing, as
  // it is the press another node opened it for, as a tab's right press.
  void follow(VP::Cook &frame, bool opening) {
    if (_shown)
      frame.hold_pointer();
    for (const VP::Event &event : frame.input().events()) {
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      if (event.kind != VP::Event::Kind::button || !event.down)
        continue;
      if (event.name == "right" && VP_VIEW::contains(*_area, _pointer))
        _next.emplace(Opening{.corner = glm::ivec2(_pointer)});
      else if (_shown && !opening)
        _shutting = true;
    }
    for (const VP::Event &event : _typed->to(_name))
      _shutting = _shutting || (event.kind == VP::Event::Kind::key && event.down &&
                                event.name == "escape");
  }

  // Only as they change, so a frame allocates nothing for them (CPP10). Opened for a
  // subject, the names hold its menu alone, and no item wants more.
  void write() {
    _written = true;
    _names->clear();
    _items->clear();
    if (!_shown)
      return;
    for (const Entry &entry : _entries) {
      if ((_subject.empty() || entry.menu == _open) &&
          std::ranges::none_of(*_names, [&](const VP_VIEW::Item &named) {
            return named.label == entry.menu;
          }))
        _names->push_back({.label = entry.menu,
                           .command = "menu " + entry.menu,
                           .current = entry.menu == _open});
      if (entry.menu == _open)
        _items->push_back({.label = entry.wants && _subject.empty()
                                        ? entry.item + std::string(wants_more)
                                        : entry.item,
                           .command = std::format("menu {} {}", entry.menu, entry.item)});
    }
  }

  // An item types its line as a person would, in the terminal, so the line shows there,
  // its history keeps it and its view is the current one: enter sends it, and Tab
  // finishes the words a line that wants one has, a blank after them. A menu opened for a
  // subject, as a tab's, sends its item's line itself, whole, as the tab's own close mark
  // sends close, so it acts while the terminal is hidden too, which takes no keys then.
  // A line opening a menu while the menus are shut shows them at the area's top left.
  void command(VP::Call &call) override {
    if (!call.is(_menu))
      return;
    read(call.files(), call.commands());
    const std::span<const std::string_view> words = call.arguments();
    if (words.empty()) {
      _shutting = true;
      return;
    }
    const auto named = [&](const Entry &entry) { return entry.menu == words.front(); };
    if (std::ranges::none_of(_entries, named))
      throw std::runtime_error(std::format(
          "no menu is named {}; {} gives {}", words.front(), menu_file, menus()));
    if (words.size() == 1) {
      _next.emplace(Opening{.menu = std::string(words.front())});
      return;
    }
    if (words.size() == 3 && words[1] == for_word) {
      _next.emplace(Opening{.menu = std::string(words.front()),
                            .corner = glm::ivec2(_pointer),
                            .subject = std::string(words[2])});
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
    _shutting = true;
    // A subject is the word the line wants, so it runs whole.
    if (!_subject.empty()) {
      call.reply(call.commands().send(
          entry->wants ? std::format("{} {}", entry->line, _subject) : entry->line));
      return;
    }
    const std::string_view key = entry->wants ? "tab" : "enter";
    call.commands().send(std::format("focus {}", _line));
    call.commands().send(std::format("input text {}", entry->line));
    call.commands().send(std::format("input key down {}", key));
    call.commands().send(std::format("input key up {}", key));
    _begun = entry->wants;
  }

  std::string menus() const {
    std::string menus;
    for (const Entry &entry : _entries)
      if (menus.find(entry.menu) == std::string::npos)
        menus += std::format("{}{}", menus.empty() ? "" : ", ", entry.menu);
    return menus;
  }

  VP::Command _menu;
  std::string _name;
  std::string _line; // the node whose line an item types into
  VP::File _file;
  std::optional<std::string> _read; // the file the entries are from
  std::vector<Entry> _entries;
  const VP_VIEW::Typed *_typed = nullptr;
  const VP_VIEW::Rect *_area = nullptr; // where a right press opens the menus
  VP_VIEW::Items *_names = nullptr;
  VP_VIEW::Items *_items = nullptr;
  VP_VIEW::Rect *_at = nullptr;
  bool _shown = false;               // the names show, below the corner
  glm::ivec2 _corner{};              // where they were opened
  std::string _open;                 // the menu whose items show; empty for none
  std::string _subject;              // what it opened for, as a tab's file, or none
  std::optional<Opening> _next;      // what a press or a line opened, for the next frame
  bool _shutting = false;            // a press or escape, which shuts it the next frame
  bool _begun = false;               // an item began a line, which keeps the focus
  std::string _returning;            // the node in focus as the menus opened
  glm::vec2 _pointer{};              // as the events so far left it
  bool _written = false;             // the names and items are as the above say
};

} // namespace

VP_OPERATOR(Menus);
