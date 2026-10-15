#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The connection present makes, from the window of the view it shows.
constexpr std::string_view presenting = "present";
constexpr std::string_view escape = "escape";

// Chooses what reaches the window. In edit mode the dock gets the Rect its area gives;
// in perform mode the view presented gets all of it and the dock none, so the dock draws
// and takes nothing (no room, no work). The mode is its param, so a save keeps it and a
// replay restores it. present connects the window of a view its view hosts to the images
// its to param names, so the log keeps one connection a switch (V08); a view it hosts
// draws nowhere else, since its view draws into the window itself, and keeps running.
// Each view hosted is presented as it comes, and has a tab that presents it when pressed,
// and a close mark that closes it, as view close does.
// It takes the keys no part in focus takes: escape leaves perform mode, and in edit mode
// ends the run, once the part in focus let go of its selection and of the focus.
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
    _typed = &node.input<VP_VIEW::Typed>("typed");
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
    _toggle_mode = node.command("mode toggle", "switches to the other mode");
  }

  // Lines go to the view this node runs in, the one vulpen started with (`:`), whichever
  // view the line that ran the command addressed.
  void command(VP::Call &call) override {
    if (call.is(_present))
      present(call, call.arguments().front());
    else if (call.is(_edit_mode) || call.is(_perform_mode))
      switch_mode(call.commands(), call.is(_perform_mode));
    else if (call.is(_toggle_mode))
      switch_mode(call.commands(), !_perform);
  }

  // At once, from the connections as the lines so far left them, so the connection lands
  // before the next frame and the view never shows anywhere but in the images.
  void present(VP::Call &call, std::string_view name) {
    const VP::View &own = *call.view("");
    if (std::ranges::find(own.children, name, &VP::Child::name) == own.children.end())
      throw std::runtime_error(
          std::format("no view named {} is hosted; child list lists them", name));
    const auto made =
        std::ranges::find(own.connections, presenting, &VP::Connection::name);
    if (made != own.connections.end()) {
      if (made->from.view() && made->from.node == name)
        return;
      call.commands().send(std::format(": disconnect {}", presenting));
    }
    call.commands().send(std::format(": connect {} {}: {}", presenting, name, _to));
  }

  // Perform mode leaves the panels no room, so the part in focus lets it go, and takes
  // it back in edit mode.
  void switch_mode(VP::CommandPort &commands, bool perform) {
    if (perform && !_perform) {
      _returning = _typed->focus;
      commands.send(": focus");
    } else if (!perform && !_returning.empty()) {
      commands.send(std::format(": focus {}", std::exchange(_returning, {})));
    }
    commands.send(
        std::format(": param set {} mode {}", _name, perform ? "perform" : "edit"));
  }

  void cook(VP::Cook &frame) override {
    const VP::View &view = frame.view();
    const auto made =
        std::ranges::find(view.connections, presenting, &VP::Connection::name);
    const bool connected = made != view.connections.end();
    const std::string_view presented =
        connected && made->from.view() ? std::string_view(made->from.node) : "";
    present_new(frame, view, presented);
    name_tabs(view, presented);
    for (const VP::Event &event : _typed->to(""))
      if (event.kind == VP::Event::Kind::key && event.down && event.name == escape)
        frame.commands().send(_perform ? "mode edit" : "quit");
    *_edit = _perform ? VP_VIEW::Rect{} : *_area;
    *_shown = _perform ? *_area : VP_VIEW::Rect{};
  }

  // The newest view hosted since the frame before. At the first frame, which a swap of
  // this part starts again, only when none is presented, so a swap changes nothing shown.
  void present_new(VP::Cook &frame, const VP::View &view, std::string_view presented) {
    const auto seen = [&](const VP::Child &child) {
      return std::ranges::find(_seen, child.name) != _seen.end();
    };
    if (_started && std::ranges::all_of(view.children, seen) &&
        _seen.size() == view.children.size())
      return;
    std::string_view newest;
    for (const VP::Child &child : view.children)
      if (!seen(child))
        newest = child.name;
    if (!newest.empty() && (_started || presented.empty()))
      frame.commands().send(std::format("present {}", newest));
    _started = true;
    _seen.clear();
    for (const VP::Child &child : view.children)
      _seen.push_back(child.name);
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
                        .current = child.name == presented,
                        .close = std::format("view close {}", child.name)});
  }

  bool _perform = false;
  std::string _to; // the ports present connects a view's window to, blank between
  std::string _name;
  std::string _returning; // in focus as perform mode began, and again in edit mode
  std::vector<std::string> _seen; // the views hosted at the frame before
  bool _started = false;
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  VP_VIEW::Rect *_edit = nullptr;
  VP_VIEW::Rect *_shown = nullptr;
  VP_VIEW::Items *_tabs = nullptr;
  VP::Command _present;
  VP::Command _edit_mode;
  VP::Command _perform_mode;
  VP::Command _toggle_mode;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Modes>("Modes");
}
