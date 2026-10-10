#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// The connection present makes, from the window of the view it shows.
constexpr std::string_view presenting = "present";

// Whether what child list answers, a line a hosted view, its name and its file, names it.
bool lists(std::string_view children, std::string_view name) {
  for (std::size_t start = 0; start < children.size();) {
    const std::size_t end = std::min(children.find('\n', start), children.size());
    const std::string_view line = children.substr(start, end - start);
    if (line.substr(0, line.find(' ')) == name)
      return true;
    start = end + 1;
  }
  return false;
}

// Chooses what reaches the window. In edit mode the dock gets the Rect its area gives;
// in perform mode the view presented gets all of it and the dock none, so the dock draws
// and takes nothing (no room, no work). The mode is its param, so a save keeps it and a
// replay restores it. present connects the window of a view its view hosts to the images
// its to param names, so the log keeps one connection a switch (V08), and a tab for each
// hosted view presents it when pressed.
class Modes final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string mode = node.param<std::string>("mode");
    if (!mode.empty() && mode != "edit" && mode != "perform") // the loader names it unset
      throw std::runtime_error(std::format("param mode = {}: edit or perform", mode));
    _perform = mode == "perform";
    _to = node.param<std::string>("to");
    std::ranges::replace(_to, ',', ' ');
    _name = node.name();
    _area = &node.input<VP_VIEW::Rect>("area");
    _edit = &node.output<VP_VIEW::Rect>("edit");
    _shown = &node.output<VP_VIEW::Rect>("perform");
    _tabs = &node.output<VP_VIEW::Items>("tabs");
    _present = node.command("present <name>",
                            "shows the window of a view this view hosts in the images "
                            "that show what is presented");
    _edit_mode = node.command("mode edit",
                              "gives the window to the dock, which shows what is "
                              "presented in its panel");
    _perform_mode = node.command(
        "mode perform", "gives the window to what is presented, and the dock no room");
  }

  // A view is presented as the next frame cooks, from the view as that frame runs it,
  // so the connection made or dropped is the one there is.
  void command(VP::Call &call) override {
    if (call.is(_present)) {
      const std::string_view view = call.arguments().front();
      if (!lists(call.commands().send(": child list"), view))
        throw std::runtime_error(
            std::format("no view named {} is hosted; child list lists them", view));
      _wanted = view;
    } else if (call.is(_edit_mode) || call.is(_perform_mode)) {
      call.commands().send(std::format(
          ": param set {} mode {}", _name, call.is(_edit_mode) ? "edit" : "perform"));
    }
  }

  void cook(VP::Cook &frame) override {
    const VP::View &view = frame.view();
    const auto made =
        std::ranges::find(view.connections, presenting, &VP::Connection::name);
    const bool connected = made != view.connections.end();
    const std::string presented = connected && made->from.view() ? made->from.node : "";
    if (!_wanted.empty() && _wanted != presented) {
      if (connected)
        frame.commands().send(std::format(": disconnect {}", presenting));
      frame.commands().send(std::format(": connect {} {}: {}", presenting, _wanted, _to));
    }
    _wanted.clear();
    name_tabs(view, presented);
    *_edit = _perform ? VP_VIEW::Rect{} : *_area;
    *_shown = _perform ? *_area : VP_VIEW::Rect{};
  }

  // Only when the views hosted or the one presented changed, so a frame allocates
  // nothing for them.
  void name_tabs(const VP::View &view, std::string_view presented) {
    const auto shows = [&](const VP_VIEW::Item &tab, const VP::Child &child) {
      return tab.label == child.name && tab.current == (child.name == presented);
    };
    if (std::ranges::equal(*_tabs, view.children, shows))
      return;
    _tabs->clear();
    for (const VP::Child &child : view.children)
      _tabs->push_back({.label = child.name,
                        .command = std::format("present {}", child.name),
                        .current = child.name == presented});
  }

  bool _perform = false;
  std::string _to; // the ports present connects a view's window to, blank between
  std::string _name;
  std::string _wanted; // the view to present as the next frame cooks
  const VP_VIEW::Rect *_area = nullptr;
  VP_VIEW::Rect *_edit = nullptr;
  VP_VIEW::Rect *_shown = nullptr;
  VP_VIEW::Items *_tabs = nullptr;
  VP::Command _present;
  VP::Command _edit_mode;
  VP::Command _perform_mode;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Modes>("Modes");
}
