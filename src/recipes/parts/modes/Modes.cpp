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

// The view present shows, as the view's connections are; empty for none.
std::string_view shown_by(const VP::View &view) {
  const auto made =
      std::ranges::find(view.connections, presenting, &VP::Connection::name);
  return made != view.connections.end() && made->from.view()
             ? std::string_view(made->from.node)
             : std::string_view();
}

// Chooses what reaches the window. In edit mode the dock gets the Rect its area gives;
// in perform mode the view presented gets all of it and the dock none, so the dock draws
// and takes nothing (no room, no work). The mode is its param, so a save keeps it and a
// replay restores it. present connects the window of a view its view hosts to the images
// its to param names, so the log keeps one connection a switch (V08); a view it hosts
// draws nowhere else, since its view draws into the window itself, and keeps running.
// Each view hosted is presented as it comes, and has a tab that presents it when pressed,
// and a close mark that closes it, as view close does. Perform mode needs a view
// presented, so it refuses while none is, and gives way to edit mode once none is; it
// hides the log, so it gives way too once the log shows an error, as a line refused's,
// which the terminal then shows.
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
      switch_mode(call, call.is(_perform_mode));
    else if (call.is(_toggle_mode))
      switch_mode(call, !_perform);
  }

  // At once, from the connections as the lines so far left them, so the connection lands
  // before the next frame and the view never shows anywhere but in the images. Only a view
  // this view hosts itself: one a view it hosts hosts in turn shows through that view, as
  // that view composes its window (V03).
  void present(VP::Call &call, std::string_view name) {
    const VP::View &own = *call.view("");
    if (std::ranges::find(own.children, name, &VP::Child::name) == own.children.end())
      throw std::runtime_error(
          call.view(name)
              ? std::format("{} is hosted by a view this view hosts, and shows through it",
                            name)
              : std::format("no view named {} is hosted; : view list lists those here",
                            name));
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
  // it back in edit mode. With nothing presented it refuses, rather than give the window
  // to nothing; a view hosted that this part has not presented yet it presents first, as
  // the frame would, so a mode line just after a view load finds the view.
  void switch_mode(VP::Call &call, bool perform) {
    if (perform && shown_by(*call.view("")).empty()) {
      const std::string next(unseen(*call.view("")));
      if (next.empty())
        throw std::runtime_error(
            "nothing is presented to perform; present <name> shows a view this view "
            "hosts");
      present(call, next);
    }
    VP::CommandPort &commands = call.commands();
    if (perform && !_perform) {
      _returning = _typed->focus;
      commands.send(": focus");
    } else if (!perform && !_returning.empty()) {
      commands.send(std::format(": focus {}", std::exchange(_returning, {})));
    }
    commands.send(
        std::format(": param set {} mode {}", _name, perform ? "perform" : "edit"));
  }

  // Perform mode gives way to edit mode as escape asks, once nothing is presented nor
  // about to be, as after view close of the view presented, and once the log printed an
  // error since the frame before, which perform mode would hide.
  void cook(VP::Cook &frame) override {
    const VP::View &view = frame.view();
    const std::string_view presented = shown_by(view);
    const bool presenting = present_new(frame, view, presented);
    name_tabs(view, presented);
    bool leave = false;
    for (const VP::Event &event : _typed->to(""))
      if (event.kind == VP::Event::Kind::key && event.down && event.name == escape) {
        leave = leave || _perform;
        if (!_perform)
          frame.commands().send("quit");
      }
    const auto erred = [](const VP::Logged &line) {
      return line.level == VP::Level::error;
    };
    if (_perform && (leave || (presented.empty() && !presenting) ||
                     std::ranges::any_of(frame.commands().log(), erred)))
      frame.commands().send("mode edit");
    *_edit = _perform ? VP_VIEW::Rect{} : *_area;
    *_shown = _perform ? *_area : VP_VIEW::Rect{};
  }

  // The newest view hosted that this part has not seen yet; empty for none.
  std::string_view unseen(const VP::View &view) const {
    std::string_view newest;
    for (const VP::Child &child : view.children)
      if (std::ranges::find(_seen, child.name) == _seen.end())
        newest = child.name;
    return newest;
  }

  // The newest view hosted since the frame before. At the first frame, which a swap of
  // this part starts again, only when none is presented, so a swap changes nothing shown.
  // Whether it presented one.
  bool present_new(VP::Cook &frame, const VP::View &view, std::string_view presented) {
    const auto seen = [&](const VP::Child &child) {
      return std::ranges::find(_seen, child.name) != _seen.end();
    };
    if (_started && std::ranges::all_of(view.children, seen) &&
        _seen.size() == view.children.size())
      return false;
    const std::string_view newest = unseen(view);
    const bool presenting = !newest.empty() && (_started || presented.empty());
    if (presenting)
      frame.commands().send(std::format("present {}", newest));
    _started = true;
    _seen.clear();
    for (const VP::Child &child : view.children)
      _seen.push_back(child.name);
    return presenting;
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
