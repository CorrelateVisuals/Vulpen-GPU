#include "modes/Modes.cpp"

#include "baseclasses/Engine.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Gpu.h"

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/modes: edit mode gives the dock the window, perform mode the view
// presented, its pointer handed to it there, letterboxed in the area; mode lines switch,
// staying in edit mode with nothing to present; perform mode gives way to edit mode as
// what it presents goes or the log prints an error; escape in edit mode runs quit; a
// project loaded shows the switch's first half and, with something to perform, the panel
// its shows param names; its buttons edit, run, gpu and, with a window, the frames a
// second. Its bind asks the engine which kind of GPU the other launch takes, so its rows
// run on lavapipe, which has one kind: the gpu button stuck.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// The commands in the order bind registers them, each its index.
enum Line : std::uint32_t { edit_mode, perform_mode, toggle_mode };

// The ide's view, presenting the view named, or none; the switch whose first half shows
// it, its shown param as given, none for unset.
View own_view(std::string_view presented, std::optional<std::string> shown = "second") {
  View view;
  view.name = "ide";
  Node toggle{.name = "ide.preview"};
  if (shown)
    toggle.params.push_back({.key = "shown", .value = *shown});
  view.nodes.push_back(std::move(toggle));
  if (!presented.empty())
    view.connections.push_back({.name = "present",
                                .from = {std::string(presented), ""},
                                .to = {{"ide.screen", "picture"}}});
  return view;
}

View top_view(const std::vector<std::string> &hosted) {
  View view;
  view.children.push_back({.name = "ide"});
  for (const std::string &name : hosted)
    view.children.push_back({.name = name});
  return view;
}

// A project's view as the next frame runs it: a draw into the window, or a dispatch.
View project(bool draws) {
  View view;
  view.nodes.push_back(
      {.name = "n", .files = {draws ? "Draw.frag" : "Fill.comp", "Draw.vert"}});
  return view;
}

std::string rect_of(const VP_VIEW::Rect &rect, Names &names) {
  return show(rect, names);
}

// The buttons as a bar shows them: label, line, lit, stuck; the frames a second's count
// as N, which the clock decides.
std::string buttons_text(const VP_VIEW::Items &buttons) {
  std::string text;
  for (const VP_VIEW::Item &button : buttons) {
    std::string label = button.label.ends_with(" fps") ? "N fps" : button.label;
    if (button.label.empty() && button.command.empty())
      label = "|";
    text += (text.empty() ? "" : "; ") + label;
    if (!button.command.empty())
      text += " > " + button.command;
    if (button.current)
      text += ", lit";
    if (button.role)
      text += ", " + std::string(VP_VIEW::role_names.at(*button.role));
    if (button.stuck)
      text += ", stuck";
  }
  return text;
}

struct Framed {
  Running<Modes> node;
  FakeCommands commands;
  FakeInput input;

  Framed(const Engine &engine, Params params) : node(std::move(params)) {
    node.bind.node_name = "ide.modes";
    node.bind.view_named = "ide";
    node.bind.engine_made = &engine;
    node.bind.in_view = own_view("");
    node.bound();
    node.cook.command_port = &commands;
    node.cook.input_port = &input;
    node.cook.size = {400, 300};
    node.cook.views = {{"", top_view({})}};
    node.cook.unfolded_views = {{"none", View{}}};
    node.port<VP_VIEW::Rect>("area") = {.offset = {10, 20}, .extent = {200, 100}};
    commands.now = "";
    commands.logged.emplace();
    commands.answer = "the next launch takes the integrated GPU";
    input.window = false;
  }
  // The views as a frame finds them: those hosted, the one presented, the current one.
  void views(const std::vector<std::string> &hosted,
             std::string_view presented,
             std::string current = "",
             std::optional<std::string> shown = "second") {
    node.cook.views.at("") = top_view(hosted);
    node.bind.in_view = own_view(presented, std::move(shown));
    node.cook.views.insert_or_assign("ide", node.bind.in_view);
    commands.now = std::move(current);
  }
  // A frame of these events and log lines: what it sent, the rects it gave, where the
  // pointer went.
  std::string
  cooked(Rows &rows, std::vector<Event> events = {}, std::vector<Logged> log = {}) {
    node.port<VP_VIEW::Typed>("typed").events = std::move(events);
    commands.logged = std::move(log);
    commands.sent.clear();
    node.cook.handed.clear();
    node.cook.held = 0;
    node.cooked();
    Names &names = rows.names();
    std::string pointer = node.cook.handed.empty() ? "" : show(node.cook.handed, names);
    if (node.cook.held != 0)
      pointer += std::format("{}held {}", pointer.empty() ? "" : ", ", node.cook.held);
    return std::format("sent {}; edit {}, perform {}; pointer {}",
                       show(commands.sent, names),
                       rect_of(node.port<VP_VIEW::Rect>("edit"), names),
                       rect_of(node.port<VP_VIEW::Rect>("perform"), names),
                       pointer.empty() ? "none" : pointer);
  }
  // A mode line, at the views as the frame left them: what it sent and answered.
  std::string ran(Line line, Rows &rows) {
    FakeCall call;
    call.running = {line};
    call.views = node.cook.views;
    call.views.insert_or_assign("ide", node.bind.in_view);
    call.command_port = &commands;
    commands.sent.clear();
    try {
      node.called(call);
    } catch (...) {
      return thrown();
    }
    return std::format("sent {}; answered {}",
                       show(commands.sent, rows.names()),
                       show(call.replies, rows.names()));
  }
  // A param set, as the line mode perform sends rebinds the node.
  void rebound(std::string mode) {
    for (auto &[key, value] : node.bind.params)
      if (key == "mode")
        value = std::move(mode);
    node.bound();
  }
};

Params ide(std::string mode = "edit") {
  return {{"mode", std::move(mode)}, {"shows", "ide.perform"}, {"switch", "ide.preview"}};
}

// The node in the start view, as the ide runs it, its own view the top one; each line to
// it a copy in its place, as Views edits a view: the copy before it gone, the view the
// frame began with kept till the frame's end.
struct Started {
  Running<Modes> node;
  FakeCommands commands;
  FakeInput input;
  std::optional<decltype(node.cook.views)::node_type> began;

  explicit Started(const Engine &engine) : node(ide()) {
    node.bind.node_name = "ide.modes";
    node.bind.engine_made = &engine;
    node.bound();
    node.cook.command_port = &commands;
    node.cook.input_port = &input;
    node.cook.size = {400, 300};
    node.cook.unfolded_views = {{"none", View{}}};
    node.port<VP_VIEW::Rect>("area") = {.offset = {10, 20}, .extent = {200, 100}};
    commands.now = "";
    commands.logged.emplace();
    commands.answer = "the next launch takes the integrated GPU";
    // mode edit runs the node's own command, whose param set edits the view.
    commands.answers = [this](std::string_view line) {
      if (line.starts_with(": ") || line == "mode edit")
        edited();
      return commands.answer;
    };
    input.window = false;
  }
  // The start view as a frame begins with it: hosting these, presenting that.
  void views(const std::vector<std::string> &hosted, std::string_view presented) {
    View top = own_view(presented);
    top.name = "";
    top.children = top_view(hosted).children;
    node.cook.views.clear();
    node.cook.views.emplace("", std::move(top));
    node.cook.in_view = &node.cook.views.at("");
  }
  void edited() {
    View copy = node.cook.views.at("");
    auto was = node.cook.views.extract("");
    node.cook.views.emplace("", std::move(copy));
    node.cook.in_view = &node.cook.views.at("");
    if (!began)
      began = std::move(was);
  }
  // A frame, another node's line editing the view before this one cooks where asked.
  std::string cooked(Rows &rows, bool edited_before) {
    began.reset();
    if (edited_before)
      edited();
    commands.sent.clear();
    node.cook.handed.clear();
    node.cooked();
    return std::format("sent {}; pointer {}",
                       show(commands.sent, rows.names()),
                       show(node.cook.handed, rows.names()));
  }
};

Event key(std::string name) {
  return {.kind = Event::Kind::key, .down = true, .name = std::move(name)};
}

// The rows of a check on an engine of its own, on lavapipe; a row saying why where none.
template <class Body> void with_engine(Rows &rows, Body &&body) {
  on_gpu(rows, [&](Gpu &gpu) {
    const auto engine =
        rows.quiet([&] { return std::make_unique<Engine>(gpu.log, nullptr, "cpu"); });
    body(*engine);
  });
}

} // namespace

void cases(Bench &bench) {
  // Its mode a word of two; what it asks for; its buttons as the mode and the machine
  // make them.
  bench.check("Modes.cpp/Modes::bind", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      for (const Params &params :
           {ide(), ide("perform"), Params{{"mode", "edit"}}, ide("run")}) {
        std::string said;
        try {
          Framed frame(engine, params);
          said = std::format("asked {}; buttons {}",
                             show(frame.node.bind.asked, rows.names()),
                             buttons_text(frame.node.port<VP_VIEW::Items>("buttons")));
        } catch (...) {
          said = thrown();
        }
        rows.row(std::format("{} -> {}", show(params, rows.names()), said));
      }
    });
  });

  // Perform mode with a view presented or about to be: the focus let go, the param set;
  // with nothing to present, it stays in edit mode and says so; edit mode takes the
  // focus back; toggle switches to the other.
  bench.check("Modes.cpp/Modes::switch_mode", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      Framed frame(engine, ide());
      frame.cooked(rows);
      rows.row("perform, nothing hosted -> " + frame.ran(perform_mode, rows));
      frame.views({"p1"}, "");
      rows.row("perform, p1 hosted, none presented yet -> " +
               frame.ran(perform_mode, rows));
      frame.views({"p1"}, "p1");
      frame.node.port<VP_VIEW::Typed>("typed").focus = "ide.editor";
      rows.row("perform, p1 presented, the editor in focus -> " +
               frame.ran(perform_mode, rows));
      frame.rebound("perform");
      frame.node.port<VP_VIEW::Typed>("typed").focus = "";
      rows.row("perform again, in perform mode -> " + frame.ran(perform_mode, rows));
      rows.row("edit -> " + frame.ran(edit_mode, rows));
      frame.rebound("edit");
      rows.row("edit again, in edit mode -> " + frame.ran(edit_mode, rows));
      rows.row("toggle, in edit mode -> " + frame.ran(toggle_mode, rows));
      frame.rebound("perform");
      rows.row("toggle, in perform mode -> " + frame.ran(toggle_mode, rows));
      frame.views({}, "");
      rows.row("toggle in perform mode, nothing hosted -> " +
               frame.ran(toggle_mode, rows));
    });
  });

  // Edit mode gives the dock the area and holds the pointer for the GUI; perform mode
  // gives the view presented the area and hands it the pointer, letterboxed as the image
  // there shows it; with none presented it hands nothing.
  bench.check("Modes.cpp/Modes::hold", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      Framed edit(engine, ide());
      edit.views({"p1"}, "p1");
      rows.row("edit, p1 presented -> " + edit.cooked(rows));
      Framed perform(engine, ide("perform"));
      perform.views({"p1"}, "p1");
      rows.row("perform, p1 presented, the window 400x300 -> " + perform.cooked(rows));
      perform.node.cook.size = {100, 400};
      rows.row("the window 100x400 -> " + perform.cooked(rows));
      perform.node.cook.size = {0, 0};
      rows.row("no window size -> " + perform.cooked(rows));
      perform.views({"p1"}, "");
      rows.row("p1 hosted, none presented -> " + perform.cooked(rows));
    });
  });

  // Perform mode gives way to edit mode once nothing is presented nor about to be, and
  // once the log printed an error; a warning does not. Escape in edit mode runs quit in
  // the terminal; in perform mode the project takes it.
  bench.check("Modes.cpp/Modes::cook", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      Framed perform(engine, ide("perform"));
      perform.views({"p1"}, "p1");
      rows.row("perform, p1 presented -> " + perform.cooked(rows));
      rows.row("a warning logged -> " +
               perform.cooked(rows, {}, {{.level = Level::warn, .text = "w"}}));
      rows.row("an error logged -> " +
               perform.cooked(rows, {}, {{.level = Level::error, .text = "e"}}));
      rows.row("escape, p1 presented -> " + perform.cooked(rows, {key("escape")}));
      perform.views({}, "p1");
      rows.row("p1 presented, no longer hosted -> " + perform.cooked(rows));
      perform.views({}, "");
      rows.row("nothing presented, none hosted -> " + perform.cooked(rows));
      Framed edit(engine, ide());
      rows.row("edit, escape -> " + edit.cooked(rows, {key("escape")}));
      rows.row("edit, another key -> " + edit.cooked(rows, {key("a")}));
      Event up = key("escape");
      up.down = false;
      rows.row("edit, escape let go -> " + edit.cooked(rows, {up}));
      edit.node.port<VP_VIEW::Typed>("typed").focus = "ide.editor";
      rows.row("edit, escape to the editor in focus -> " +
               edit.cooked(rows, {key("escape")}));
    });
  });

  // A project hosted after the first frame, or another presented, shows the switch's
  // first half where it shows the second; one that draws into the window, hosted anew
  // or drawing at last, brings back the panel its shows param names. Never at the first
  // frame, nor for a project that does not draw.
  bench.check("Modes.cpp/Modes::begins_to_perform", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      Framed frame(engine, ide());
      frame.node.cook.unfolded_views = {{"p1", project(true)}, {"p2", project(false)}};
      frame.views({"p1"}, "p1");
      rows.row("1 p1 hosted and presented at the first frame -> " + frame.cooked(rows));
      frame.views({"p1", "p2"}, "p1");
      rows.row("2 p2, which draws nothing, hosted -> " + frame.cooked(rows));
      frame.views({"p1", "p2"}, "p2");
      rows.row("3 p2 presented -> " + frame.cooked(rows));
      frame.node.cook.unfolded_views.insert_or_assign("p2", project(true));
      rows.row("4 p2 draws at last -> " + frame.cooked(rows));
      frame.node.cook.unfolded_views.insert_or_assign("p3", project(true));
      frame.views({"p1", "p2", "p3"}, "p2");
      rows.row("5 p3, which draws, hosted -> " + frame.cooked(rows));
      rows.row("6 the same again -> " + frame.cooked(rows));
      frame.node.cook.unfolded_views.insert_or_assign("p4", project(true));
      frame.views({"p1", "p2", "p3", "p4"}, "p2", "p4");
      rows.row("6b p4, which draws, hosted and switched to, as view load does -> " +
               frame.cooked(rows));
      frame.views({"p1", "p2", "p3"}, "p3", "", "first");
      rows.row("7 p3 presented, the switch on its first half -> " + frame.cooked(rows));
      frame.views({"p1", "p2", "p3"}, "p1", "", std::nullopt);
      rows.row("8 p1 presented, the switch's shown unset -> " + frame.cooked(rows));
      Framed bare(engine, Params{{"mode", "edit"}});
      bare.node.cook.unfolded_views = {{"p1", project(true)}};
      bare.cooked(rows);
      bare.views({"p1"}, "p1");
      rows.row("no shows nor switch, p1 hosted later -> " + bare.cooked(rows));
    });
  });

  // In the start view, as in the ide, after another node's line edited the view this
  // frame, as keys' F1 runs mode toggle: a project hosted or presented sends a line that
  // replaces the view again, and what it read of the view before stays its own.
  bench.check("Modes.cpp/Modes::cook, its view edited in the frame", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      Started frame(engine);
      frame.views({}, "");
      rows.row("1 the first frame -> " + frame.cooked(rows, false));
      frame.views({"p1"}, "");
      rows.row("2 p1 hosted, the view edited before -> " + frame.cooked(rows, true));
      frame.views({"p1", "p2"}, "p2");
      frame.node.cook.unfolded_views.insert_or_assign("p2", project(true));
      rows.row("3 p2 hosted, drawing and presented, the view edited before -> " +
               frame.cooked(rows, true));
      frame.node.bind.params = ide("perform");
      frame.node.bound();
      frame.node.cook.in_view = &frame.node.cook.views.at("");
      rows.row("4 in perform mode, the view edited before -> " +
               frame.cooked(rows, true));
      frame.commands.logged = {{.level = Level::error, .text = "e"}};
      rows.row("5 an error logged, the view edited before -> " +
               frame.cooked(rows, true));
    });
  });

  // The kind of GPU the next launch takes, asked at the first frame and after a warning,
  // the gpu button lit in the build curves' orange for a discrete one; run stuck while
  // nothing is presented nor about to be; the frames a second while a window is open.
  bench.check("Modes.cpp/Modes::watch_buttons", [](Rows &rows) {
    with_engine(rows, [&](const Engine &engine) {
      Framed frame(engine, ide());
      const auto buttons = [&](std::string_view what, std::vector<Logged> log = {}) {
        frame.cooked(rows, {}, std::move(log));
        rows.row(std::format("{} -> sent {}; buttons {}",
                             what,
                             show(frame.commands.sent, rows.names()),
                             buttons_text(frame.node.port<VP_VIEW::Items>("buttons"))));
      };
      buttons("1 nothing hosted");
      buttons("2 again");
      frame.commands.answer = "the next launch takes the discrete GPU";
      buttons("3 the answer now discrete, unasked");
      buttons("4 a warning logged", {{.level = Level::warn, .text = "w"}});
      frame.views({"p1"}, "");
      buttons("5 p1 hosted");
      frame.input.window = true;
      buttons("6 a window open");
      frame.commands.answer = "no GPU named";
      buttons("7 a warning, the answer naming none",
              {{.level = Level::warn, .text = "w"}});
    });
  });
}

} // namespace VP::bench
