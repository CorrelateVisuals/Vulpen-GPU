#include "baseclasses/Engine.h"
#include "contracts/Item.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Params.h"
#include "shared/Presented.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view escape = "escape";
// The kind of GPU the gpu button lights in the build curves' orange, and the other one
// gpu toggle takes, as the engine names them, and what gpu answers before the kind the
// next launch takes.
constexpr std::string_view discrete = "discrete";
constexpr std::string_view integrated = "integrated";
constexpr std::string_view takes = "takes the ";
// How long the frames a second are counted over, so the count shown changes once in it.
constexpr auto rate_every = std::chrono::seconds(1);

// Chooses what reaches the window. In edit mode the dock gets the Rect its area gives;
// in perform mode the view presented gets all of it and the dock none, so the dock draws
// and takes nothing (no room, no work). The mode is its param, so a save keeps it and a
// replay restores it. The view presented is the one shared/Presented.h names, which the
// panes part connects. Perform mode needs a view presented, so it refuses while none is
// nor about to be, and gives way to edit mode once none is; it hides the log, so it gives
// way too once the log shows an error, as a line refused's, which the terminal then
// shows. A project presented anew, as a press on its tab presents it, shows: the switch
// its switch param names gives the view presented its first half, in place of a preview.
// Once the project to present has something to perform, a draw into its window, as one
// view load hosts that draws has at once and one view new makes has at its first draw,
// the panel its shows param names comes back if it was hidden, or the panel holding the
// node it names, as panel show finds it; a project with nothing to show leaves it hidden.
// It takes the keys no part in focus takes: escape leaves perform mode, and in edit mode
// ends the run, once the part in focus let go of its selection and of the focus. It hands
// on the buttons of the run, for a bar: edit and run, perform mode, named so beside the
// Perform panel's own button, a switch of two whose mode in force is on, and past a
// divider gpu, which makes the next launch prefer the other kind of GPU (gpu toggle), lit
// as the kind it takes: discrete in the build curves' orange, integrated in the accent,
// as edit and run are; and beside it, muted, the frames a second the run shows in its
// window, a person's reading, never what a node computes from (C01). Run with nothing to
// present, and gpu on a machine without both kinds, are stuck: a press flashes them, and
// mode perform then warns, rather than refuse, as gpu toggle does.
class Modes final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string mode = node.param<std::string>("mode");
    check_word("mode", mode, {"edit", "perform"});
    _perform = mode == "perform";
    _shows = node.sets("shows") ? node.param<std::string>("shows") : std::string();
    _switch = node.sets("switch") ? node.param<std::string>("switch") : std::string();
    _name = node.name();
    _view = node.view_name();
    _area = &node.input<VP_VIEW::Rect>("area");
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _edit = &node.output<VP_VIEW::Rect>("edit");
    _shown = &node.output<VP_VIEW::Rect>("perform");
    const VP::Engine &engine = node.engine();
    _gpu_stuck =
        engine.takes(integrated) != integrated || engine.takes(discrete) != discrete;
    // The mode is a param, so a change rebinds the node: the buttons are written here.
    _buttons = &node.output<VP_VIEW::Items>("buttons");
    write_buttons();
    _edit_mode = node.command("mode edit",
                              "gives the window to the dock, which shows what is "
                              "presented in its panel");
    _perform_mode = node.command(
        "mode perform", "gives the window to what is presented, and the dock no room");
    _toggle_mode = node.command("mode toggle", "switches to the other mode");
  }

  // Lines go to the view this node runs in, whichever view the line that ran the command
  // addressed.
  void command(VP::Call &call) override {
    if (call.is(_edit_mode) || call.is(_perform_mode))
      switch_mode(call, call.is(_perform_mode));
    else if (call.is(_toggle_mode))
      switch_mode(call, !_perform);
  }

  // Perform mode leaves the panels no room, so the part in focus lets it go, and takes
  // it back in edit mode. With nothing presented nor about to be it stays in edit mode,
  // rather than give the window to nothing, and the next frame says so, as a warning,
  // since a command writes no log line; with the current view's project about to be, as
  // just after a view load, it goes on, as the panes part presents that project in the
  // frame.
  void switch_mode(VP::Call &call, bool perform) {
    const std::string_view presented = shown_by(*call.view(_view));
    if (perform && presented.empty() &&
        wanted([&](std::string_view name) { return call.view(name); },
               *call.view(""),
               _view,
               call.commands().current(),
               presented,
               _started)
            .empty()) {
      _unperformed = true;
      return;
    }
    VP::CommandPort &commands = call.commands();
    if (perform && !_perform) {
      _returning = _typed->focus;
      commands.send(std::format("{}: focus", _view));
    } else if (!perform && !_returning.empty()) {
      commands.send(std::format("{}: focus {}", _view, std::exchange(_returning, {})));
    }
    commands.send(std::format(
        "{}: param set {} mode {}", _view, _name, perform ? "perform" : "edit"));
  }

  // Perform mode gives way to edit mode as escape asks, once nothing is presented nor
  // about to be, as after view close of the view presented, and once the log printed an
  // error since the frame before, which perform mode would hide.
  // The projects are read before a line goes, as one may replace the view they are in.
  void cook(VP::Cook &frame) override {
    read_gpu(frame);
    const std::string_view presented = shown_by(frame.view());
    auto hosted = projects(*frame.view(""), _view);
    const std::string_view next =
        wanted([&](std::string_view name) { return frame.view(name); },
               *frame.view(""),
               _view,
               frame.commands().current(),
               presented,
               _started);
    const bool none = next.empty();
    watch_buttons(frame, presented.empty() && none);
    const bool loaded =
        _started && std::ranges::any_of(hosted, [&](const VP::Child &child) {
          return std::ranges::find(_known, child.name) == _known.end();
        });
    const bool anew = presented != _presented && !presented.empty();
    if (anew)
      _presented = presented;
    const bool begun = begins_to_perform(frame, next);
    if (_started && (loaded || anew))
      show_presented(frame.commands(), frame.view());
    if (begun && !_shows.empty())
      frame.commands().send(std::format("{}: panel show {}", _view, _shows));
    if (!std::ranges::equal(_known, hosted, {}, {}, &VP::Child::name)) {
      _known.clear();
      for (const VP::Child &child : hosted)
        _known.push_back(child.name);
    }
    _started = true;
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
    if (_perform && (leave || (presented.empty() && none) ||
                     std::ranges::any_of(frame.commands().log(), erred)))
      frame.commands().send("mode edit");
    *_edit = _perform ? VP_VIEW::Rect{} : *_area;
    *_shown = _perform ? *_area : VP_VIEW::Rect{};
  }

  void write_buttons() {
    *_buttons = {
        {.label = "edit", .command = "mode edit", .current = !_perform, .toggles = true},
        {.label = "run",
         .command = "mode perform",
         .current = _perform,
         .toggles = true,
         .stuck = _run_stuck},
        {}, // a divider
        {.label = "gpu",
         .command = "gpu toggle",
         .current = true,
         .toggles = true,
         .role = _discrete ? std::optional(VP_VIEW::role("number")) : std::nullopt,
         .stuck = _gpu_stuck}};
    if (_windowed)
      _buttons->push_back({.label = _rate, .role = VP_VIEW::role("muted")});
  }

  // The buttons anew only as what they show changes, so a frame allocates nothing for
  // them: run stuck or not, and the frames a second, counted over each second from the
  // first frame, shown only while a window is open, where a person reads them, so a run
  // with none, as one that takes the README's pictures, draws the same bytes every time
  // (C01). A mode line that found nothing to present says so here.
  void watch_buttons(VP::Cook &frame, bool nothing) {
    if (std::exchange(_unperformed, false))
      frame.log(VP::Level::warn,
                "nothing is presented to perform, so the run stays in edit mode; view "
                "load <project> or view switch <name> presents one");
    const auto now = std::chrono::steady_clock::now();
    if (_frames++ == 0)
      _since = now;
    const std::chrono::duration<double> spent = now - _since;
    const bool counted = spent >= rate_every;
    if (counted) {
      _rate = std::format("{} fps",
                          std::lround(static_cast<double>(_frames - 1) / spent.count()));
      _since = now;
      _frames = 1;
    }
    const bool windowed = frame.input().windowed();
    const bool stuck_changed = nothing != std::exchange(_run_stuck, nothing);
    const bool window_changed = windowed != std::exchange(_windowed, windowed);
    if (stuck_changed || window_changed || (counted && windowed))
      write_buttons();
  }

  // The kind of GPU the next launch takes, as gpu toggle flips it, which gpu names. Asked
  // at the first frame and after a warning, as gpu toggle writes one, so a frame asks
  // nothing otherwise.
  void read_gpu(VP::Cook &frame) {
    const auto warned = [](const VP::Logged &line) {
      return line.level == VP::Level::warn;
    };
    if (_gpu_read && std::ranges::none_of(frame.commands().log(), warned))
      return;
    _gpu_read = true;
    const std::string said = frame.commands().send("gpu");
    const std::size_t named = said.find(takes);
    const bool now =
        named != std::string::npos &&
        std::string_view(said).substr(named + takes.size()).starts_with(discrete);
    if (now != std::exchange(_discrete, now))
      write_buttons();
  }

  // Whether the project to present has something to perform it had not the frame before:
  // hosted since then and drawing at once, or the same project, its first draw into its
  // window made; never at the first frame, nor as another project is presented, so a
  // switch between projects leaves a panel hidden as it is.
  bool begins_to_perform(const VP::Cook &frame, std::string_view next) {
    const VP::View *const view = next.empty() ? nullptr : frame.unfolded(next);
    const bool performs = view && VP::draws_into_window(*view);
    const bool hosted_anew = std::ranges::find(_known, next) == _known.end();
    const bool begun =
        _started && performs && (hosted_anew || (next == _performer && !_performed));
    if (next != _performer)
      _performer = next;
    _performed = performs;
    return begun;
  }

  // The view presented in its panel, not a preview, so a project presented shows at once.
  // The switch's param set only while it shows the preview, so a switch between projects
  // edits nothing more.
  void show_presented(VP::CommandPort &commands, const VP::View &own) const {
    const auto node = std::ranges::find(own.nodes, _switch, &VP::Node::name);
    if (node == own.nodes.end())
      return;
    const VP::Param *const shown = node->param("shown");
    if (shown && shown->value != "first")
      commands.send(std::format("{}: param set {} shown first", _view, _switch));
  }

  bool _perform = false;
  std::string _shows;  // the panel a project to perform shows in, or a node it holds
  std::string _switch; // and the switch giving the view presented its first half
  std::string _presented; // the view presented, as the frame before found it
  std::vector<std::string> _known; // the projects hosted the frame before
  // The project to present the frame before, and whether it had something to perform.
  std::string _performer;
  bool _performed = false;
  std::string _name;
  std::string _view; // the one it runs in, which its lines change
  std::string _returning; // in focus as perform mode began, and again in edit mode
  bool _started = false; // after the first frame, which may present the view hosted last
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  VP_VIEW::Rect *_edit = nullptr;
  VP_VIEW::Rect *_shown = nullptr;
  VP_VIEW::Items *_buttons = nullptr;
  bool _discrete = false; // the kind the next launch takes, which the gpu button shows
  bool _gpu_read = false; // since the node bound
  // Stuck: gpu where the machine has not both kinds, and run while nothing is presented,
  // nor about to be; and whether a mode line found nothing to present since the last
  // frame.
  bool _gpu_stuck = false;
  bool _run_stuck = false;
  bool _unperformed = false;
  // The frames a second, as the button beside gpu shows them while a window is open; the
  // frames cooked since the count began, that one included, and when it began.
  bool _windowed = false;
  std::string _rate = "fps";
  std::uint64_t _frames = 0;
  std::chrono::steady_clock::time_point _since;
  VP::Command _edit_mode;
  VP::Command _perform_mode;
  VP::Command _toggle_mode;
};

} // namespace

VP_OPERATOR(Modes);
