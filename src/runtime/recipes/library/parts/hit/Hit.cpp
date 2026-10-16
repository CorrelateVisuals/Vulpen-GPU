#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace {

// Finds the Rect under the pointer, and a press there sends the command of the Item
// shown in it: rects[i] is where items[i] shows, as a list hands them on; a press on its
// close mark, where marks[i] is, sends its close command instead. Connecting it makes
// anything laid out as Rects pressable, so no drawing part reads the pointer, and what
// is drawn is what is pressed.
class Hit final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _items = &node.input<VP_VIEW::Items>("items");
    _rects = &node.input<VP_VIEW::Rects>("rects");
    _marks = &node.input<VP_VIEW::Rects>("marks");
  }

  // In the order the events came, so a press is tested where the pointer was then.
  void cook(VP::Cook &frame) override {
    for (const VP::Event &event : frame.input().events()) {
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      else if (event.kind == VP::Event::Kind::button && event.down &&
               event.name == "left")
        press(frame);
    }
  }

  // Of two Rects that hold the point, the later is drawn over the other, so it answers.
  // The command is copied first, since what it runs may change the Items.
  void press(VP::Cook &frame) const {
    for (std::size_t at = std::min(_items->size(), _rects->size()); at-- > 0;) {
      if (!VP_VIEW::contains((*_rects)[at], _pointer))
        continue;
      const VP_VIEW::Item &item = (*_items)[at];
      const bool marked =
          at < _marks->size() && VP_VIEW::contains((*_marks)[at], _pointer);
      if (const std::string command = marked ? item.close : item.command;
          !command.empty())
        frame.commands().send(command);
      return;
    }
  }

  const VP_VIEW::Items *_items = nullptr;
  const VP_VIEW::Rects *_rects = nullptr;
  const VP_VIEW::Rects *_marks = nullptr;
  glm::vec2 _pointer{}; // as the events so far left it
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Hit>("Hit");
}
