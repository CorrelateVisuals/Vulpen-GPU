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

// The connection that presents a view, from the window of the view it shows.
constexpr std::string_view presenting = "present";
constexpr std::string_view escape = "escape";

// The view presented, as the view's connections are; empty for none.
std::string_view shown_by(const VP::View &view) {
  const auto made =
      std::ranges::find(view.connections, presenting, &VP::Connection::name);
  return made != view.connections.end() && made->from.view()
             ? std::string_view(made->from.node)
             : std::string_view();
}

// Whether the view named is the current one or holds it, as far down as views host
// views; view_of finds a view by its name.
template <class Find>
bool holds(const Find &view_of, std::string_view name, std::string_view current) {
  if (name == current)
    return true;
  const VP::View *const view = view_of(name);
  return view && std::ranges::any_of(view->children, [&](const VP::Child &child) {
           return holds(view_of, child.name, current);
         });
}

// The view the view hosts that is the current one or holds it, so a nested view shows
// through its project; empty while the current view is none of them.
template <class Find>
std::string_view project_of(const Find &view_of,
                            const VP::View &view,
                            std::string_view current) {
  for (const VP::Child &child : view.children)
    if (!current.empty() && holds(view_of, child.name, current))
      return child.name;
  return {};
}

// Chooses what reaches the window. In edit mode the dock gets the Rect its area gives;
// in perform mode the view presented gets all of it and the dock none, so the dock draws
// and takes nothing (no room, no work). The mode is its param, so a save keeps it and a
// replay restores it. It presents the current view while that is a view its view hosts,
// or the one holding it, and keeps the one presented while the current view is its own;
// at the first frame, with none presented, the one hosted last. To present one, it
// connects that view's window to the images its to param names, so the log keeps one
// connection a switch (V08); a view it hosts draws nowhere else, since its view draws
// into the window itself, and keeps running. Each view hosted has a tab that makes it the
// current view when pressed, and a close mark that closes it, as view close does. Perform
// mode needs a view presented, so it refuses while none is, and gives way to edit mode
// once none is; it hides the log, so it gives way too once the log shows an error, as a
// line refused's, which the terminal then shows.
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
    if (call.is(_edit_mode) || call.is(_perform_mode))
      switch_mode(call, call.is(_perform_mode));
    else if (call.is(_toggle_mode))
      switch_mode(call, !_perform);
  }

  // At once, from the connections as own holds them, the view as the lines so far left
  // it, so the connection lands before the next frame, a mode line in the same frame
  // finds it made, and the view never shows anywhere but in the images. Only a view this
  // view hosts itself: one a view it hosts hosts in turn shows through that view, as
  // that view composes its window (V03). A line it sends replaces own, so name must not
  // point into it.
  void present(VP::CommandPort &commands, const VP::View &own, std::string_view name) {
    const auto made =
        std::ranges::find(own.connections, presenting, &VP::Connection::name);
    if (made != own.connections.end()) {
      if (made->from.view() && made->from.node == name)
        return;
      commands.send(std::format(": disconnect {}", presenting));
    }
    commands.send(std::format(": connect {} {}: {}", presenting, name, _to));
  }

  // Perform mode leaves the panels no room, so the part in focus lets it go, and takes
  // it back in edit mode. With nothing presented it refuses, rather than give the window
  // to nothing; the current view's project it presents first, as the frame would, so a
  // mode line just after a view load finds the view.
  void switch_mode(VP::Call &call, bool perform) {
    const VP::View &own = *call.view("");
    if (perform && shown_by(own).empty()) {
      const std::string project(project_of(
          [&](std::string_view name) { return call.view(name); },
          own,
          call.commands().current()));
      if (project.empty())
        throw std::runtime_error("nothing is presented to perform; view switch <name> "
                                 "shows a view this view hosts");
      present(call.commands(), own, project);
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
    const std::string_view project = project_of(
        [&](std::string_view name) { return frame.view(name); },
        view,
        frame.commands().current());
    const std::string_view newest =
        view.children.empty() ? std::string_view() : view.children.back().name;
    const std::string_view wanted = !project.empty()                    ? project
                                    : !_started && presented.empty() ? newest
                                                                     : presented;
    const bool presenting = !wanted.empty() && wanted != presented;
    if (presenting)
      present(frame.commands(), *frame.view(""), wanted);
    _started = true;
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
                        .command = std::format("view switch {}", child.name),
                        .current = child.name == presented,
                        .close = std::format("view close {}", child.name)});
  }

  bool _perform = false;
  std::string _to; // the ports present connects a view's window to, blank between
  std::string _name;
  std::string _returning; // in focus as perform mode began, and again in edit mode
  bool _started = false; // after the first frame, which may present the view hosted last
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  VP_VIEW::Rect *_edit = nullptr;
  VP_VIEW::Rect *_shown = nullptr;
  VP_VIEW::Items *_tabs = nullptr;
  VP::Command _edit_mode;
  VP::Command _perform_mode;
  VP::Command _toggle_mode;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Modes>("Modes");
}
