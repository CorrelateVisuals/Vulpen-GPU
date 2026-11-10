#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "shared/Ini.h"
#include "shared/Params.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr float drag_from = 8.0f; // pixels the pointer goes from a press before it drags
// The command a terminal in a window runs a line with, as typed, as a key or a menu
// runs one.
constexpr std::string_view type_command = "type";

// Finds the Rect under the pointer, and a press there sends the command of the Item
// shown in it: rects[i] is where items[i] shows, as a list hands them on; a press on its
// close mark, where marks[i] is, sends its close command instead. The pointer coming onto
// an Item that is not current sends its hover, as a menu's word opens the words after
// it. An Item dragged onto another, both with a move, sends its move with the
// other's last word after it, as a tab moves among its strip's; a right press on one
// sends its menu, as a tab's opens what to do with it. Connecting it makes
// anything laid out as Rects pressable, so no drawing part reads the pointer, and what is
// drawn is what is pressed. With typed=no a press sends its command as it is, as a
// menu's word does: the menus hand the line it ends to the terminal themselves, and its
// press must reach them in the frame the press shuts them.
class Hit final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string typed = node.param<std::string>("typed");
    check_word("typed", typed, {"yes", "no"});
    _typed = typed != "no";
    _items = &node.input<VP_VIEW::Items>("items");
    _rects = &node.input<VP_VIEW::Rects>("rects");
    _marks = &node.input<VP_VIEW::Rects>("marks");
  }

  // In the order the events came, so a press is tested where the pointer was then.
  void cook(VP::Cook &frame) override {
    for (const VP::Event &event : frame.input().events()) {
      if (event.kind == VP::Event::Kind::pointer) {
        _pointer = event.at;
        hover(frame);
      } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
        if (event.down)
          press(frame);
        else
          release(frame);
      } else if (event.kind == VP::Event::Kind::button && event.name == "right" &&
                 event.down) {
        context(frame);
      }
    }
  }

  void context(VP::Cook &frame) const {
    const std::optional<std::size_t> at = under();
    if (at && !(*_items)[*at].menu.empty())
      frame.commands().send(std::string((*_items)[*at].menu));
  }

  // Onto another Item that moves as this one does, past a few pixels from the press.
  void release(VP::Cook &frame) {
    const std::optional<std::size_t> from = std::exchange(_pressed, std::nullopt);
    const std::optional<std::size_t> onto = under();
    if (!from || !onto || *from == *onto || glm::distance(_pointer, _from) <= drag_from)
      return;
    const std::string_view moving = (*_items)[*from].move;
    const std::string_view there = (*_items)[*onto].move;
    const std::size_t word = moving.rfind(' ');
    const std::size_t other = there.rfind(' ');
    if (word == std::string_view::npos || other == std::string_view::npos ||
        moving.substr(0, word) != there.substr(0, other))
      return;
    frame.commands().send(std::format("{} {}", moving, there.substr(other + 1)));
  }

  // Of two Rects that hold the point, the later is drawn over the other, so it answers;
  // none for none.
  std::optional<std::size_t> under() const {
    for (std::size_t at = std::min(_items->size(), _rects->size()); at-- > 0;)
      if (VP_VIEW::contains((*_rects)[at], _pointer))
        return at;
    return std::nullopt;
  }

  // The command is copied first, since what it runs may change the Items.
  void press(VP::Cook &frame) {
    const std::optional<std::size_t> at = under();
    _pressed = at;
    _from = _pointer;
    if (!at)
      return;
    const VP_VIEW::Item &item = (*_items)[*at];
    const bool marked =
        *at < _marks->size() && VP_VIEW::contains((*_marks)[*at], _pointer);
    if (const std::string command = marked ? item.close : item.command; !command.empty())
      send(frame.commands(), command);
  }

  // A press runs its line in the terminal, as a key's chord and a menu's line do, so the
  // terminal shows it and what it answers, and undo takes it back as one step; with no
  // terminal in a window to type it, or typed=no, as it is.
  void send(VP::CommandPort &commands, const std::string &line) const {
    const bool terminal = _typed && registered(type_command, commands.usages());
    commands.send(terminal ? std::format("{} {}", type_command, line) : line);
  }

  // Once as the pointer comes onto an Item, so resting on it sends nothing more.
  void hover(VP::Cook &frame) {
    const std::optional<std::size_t> at = under();
    if (at == _hovered)
      return;
    _hovered = at;
    if (at && !(*_items)[*at].current && !(*_items)[*at].hover.empty())
      frame.commands().send(std::string((*_items)[*at].hover));
  }

  const VP_VIEW::Items *_items = nullptr;
  const VP_VIEW::Rects *_rects = nullptr;
  const VP_VIEW::Rects *_marks = nullptr;
  bool _typed = true; // a press goes through the terminal's type
  glm::vec2 _pointer{}; // as the events so far left it
  std::optional<std::size_t> _hovered; // the Item the pointer is on, as last seen
  std::optional<std::size_t> _pressed; // the Item a press is on, until it is let go
  glm::vec2 _from{};                   // where that press was
};

} // namespace

VP_OPERATOR(Hit);
