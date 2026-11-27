#include "keys/Keys.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/keys: where a key goes. A keymap chord runs the line it binds in
// the terminal, through type; any other key, and text, goes to the node in focus, handed
// on at typed. Its focus param gives the focus at the start, and the focus command moves
// it, to a node that reads its keys only; the keymap is read again once it changes, and a
// mistake in it is refused naming its line.

namespace VP::bench {

namespace {

// The keys the window names, and the usages the commands list: a terminal's type among
// them.
constexpr std::array<std::string_view, 11> named_keys{
    "a", "s", "f", "=", "+", "escape", "enter", "shift", "control", "alt", "super"};
constexpr std::array<std::string_view, 4> usages{
    "type <line>...", "write", "find", "zoom in"};

Event key(std::string name, bool down = true) {
  return {.kind = Event::Kind::key, .down = down, .name = std::move(name)};
}
Event text(std::string typed) {
  return {.kind = Event::Kind::text, .name = std::move(typed)};
}
Event press() {
  return {.kind = Event::Kind::button, .down = true, .name = "left", .presses = 1};
}

// The view v, where keys hands typed to editor and terminal, and console another typed to
// shell.
View view_of() {
  View view;
  view.name = "v";
  view.connections.push_back({.name = "typed",
                              .from = {.node = "keys", .port = "typed"},
                              .to = {{.node = "editor", .port = "typed"},
                                     {.node = "terminal", .port = "typed"}}});
  // Another node's typed, and keys' own other port, which hand no keys of this node's.
  view.connections.push_back({.name = "console",
                              .from = {.node = "console", .port = "typed"},
                              .to = {{.node = "shell", .port = "typed"}}});
  view.connections.push_back({.name = "keys-state",
                              .from = {.node = "keys", .port = "state"},
                              .to = {{.node = "status", .port = "state"}}});
  return view;
}

// The keys node in view v, its focus param editor, its keymap in a folder of its own.
struct Framed {
  Running<Keys> node;
  FakeInput input;
  FakeCommands commands;
  std::filesystem::path keymap;

  Framed(const Rows &rows, std::string_view text) : node({{"focus", "editor"}}) {
    keymap = planted(rows, {"keymap.ini=" + std::string(text)}) / "keymap.ini";
    node.bind.node_name = "keys";
    node.bind.view_named = "v";
    node.bound();
    node.files.node_files = {keymap.string()};
    node.cook.file_port = &node.files;
    node.cook.input_port = &input;
    node.cook.command_port = &commands;
    input.keys.assign(named_keys.begin(), named_keys.end());
    commands.listed.emplace(usages.begin(), usages.end());
  }
  // A frame of these events: what it sent, the node in focus, and what it handed on.
  std::string cooked(std::vector<Event> events, Rows &rows) {
    input.given = std::move(events);
    commands.sent.clear();
    try {
      node.cooked();
    } catch (...) {
      return thrown();
    }
    const VP_VIEW::Typed &typed = node.port<VP_VIEW::Typed>("typed");
    return std::format("sent {}; focus {}; handed {}",
                       show(commands.sent, rows.names()),
                       typed.focus.empty() ? "none" : typed.focus,
                       show(typed.events, rows.names()));
  }
  // The focus command with these words, as the view v runs it.
  std::string focused(std::vector<std::string> words) {
    FakeCall call;
    call.running = {0};
    call.words = std::move(words);
    call.views = {{"v", view_of()}};
    try {
      node.called(call);
    } catch (...) {
      return thrown();
    }
    return "taken";
  }
};

} // namespace

void cases(Bench &bench) {
  bench.check("Keys.cpp/Keys::bind", [](Rows &rows) {
    Running<Keys> node({{"focus", "editor"}});
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });

  // Each row a chord, its keys held joined by + to the key it presses, = and its line;
  // each mistake refused naming the row.
  bench.check("Keys.cpp/chords_of", [](Rows &rows) {
    const std::vector<std::pair<std::string_view, std::string>> keymaps{
        {"a chord", "control+s = write"},
        {"two held", "shift+control+s = write"},
        {"= pressed", "control+= = zoom in"},
        {"+ pressed", "control++ = zoom in"},
        {"no key held", "f = find"},
        {"a key held that holds none", "hyper+s = write"},
        {"a key held alone", "control = write"},
        {"a key the window does not name", "control+q = write"},
        {"no =", "control+s write"},
        {"a chord twice", "control+s = write\ncontrol+s = find"},
        {"a line no command takes", "control+s = save all"},
        {"comments and blanks", "# keys\n\ncontrol+s = write\n"}};
    for (const auto &[what, keymap] : keymaps) {
      std::string said;
      try {
        FakeInput input;
        input.keys.assign(named_keys.begin(), named_keys.end());
        std::vector<Usage> listed;
        for (const std::string_view usage : usages)
          listed.push_back({.usage = usage, .help = {}});
        const std::vector<Chord> chords = chords_of(keymap, input, listed);
        for (const Chord &chord : chords)
          said += std::format("{}held {} {} = {}",
                              said.empty() ? "" : "; ",
                              chord.held,
                              chord.key,
                              chord.command);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", what, said));
    }
    FakeInput input;
    input.keys.assign(named_keys.begin(), named_keys.end());
    std::string said;
    try {
      chords_of("control+s = write", input, std::vector<Usage>{{.usage = "write"}});
    } catch (...) {
      said = thrown();
    }
    rows.row("no terminal registers type -> " + said);
  });

  // The focus its param names at the first frame; a key, and text, handed to the node in
  // focus; a chord's line run through type, the keys before it handed on first; a press,
  // escape or a chord, after which the rest waits a frame, as each may move the focus;
  // modifiers going on as keys, held down again for a node taking the focus, and let go
  // as the window loses the focus.
  bench.check("Keys.cpp/Keys::cook", [](Rows &rows) {
    Framed frame(rows, "control+s = write\n");
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events), rows)));
    };
    step({}, "the first frame");
    frame.focused({"editor"});
    step({}, "focus editor, the frame after");
    step({key("a"), text("a"), key("a", false)}, "a typed");
    step({key("control"), key("s"), key("s", false), key("control", false)}, "control+s");
    step({key("a"), key("control"), key("s")}, "a, then control+s");
    step({}, "the frame after");
    step({key("control", false), key("s", false), key("a", false)}, "let go");
    step({press(), key("a")}, "a press, then a");
    step({}, "the frame after");
    step({key("escape"), key("f")}, "escape, then f");
    step({}, "the frame after");
    step({key("control")}, "control held");
    frame.focused({"terminal"});
    step({}, "focus terminal, control still held");
    step({{.kind = Event::Kind::focus, .down = false}, key("s")},
         "the window's focus gone, then s");
    step({key("s", false), key("control"), key("a"), key("a", false), key("s")},
         "control held, a typed, then s");
    step({}, "the frame after, control still held");
  });

  // The focus command: a node that reads its keys takes the focus the next frame; a
  // node that reads none is refused naming those that do; with no node, no node is in
  // focus.
  bench.check("Keys.cpp/Keys::command", [](Rows &rows) {
    Framed frame(rows, "");
    frame.cooked({}, rows);
    for (const std::vector<std::string> &words : {std::vector<std::string>{"terminal"},
                                                  std::vector<std::string>{"graph"},
                                                  std::vector<std::string>{"shell"},
                                                  std::vector<std::string>{"status"},
                                                  std::vector<std::string>{}}) {
      const std::string took = frame.focused(words);
      rows.row(std::format("focus {} -> {}; the frame after -> {}",
                           words.empty() ? "(none)" : words.front(),
                           took,
                           frame.cooked({}, rows)));
    }
  });

  // Read again once its text changes: a chord added, then a mistake, which stops the
  // node naming its line.
  bench.check("Keys.cpp/Keys::cook, the keymap edited", [](Rows &rows) {
    Framed frame(rows, "control+s = write\n");
    frame.cooked({}, rows);
    Files::save(frame.keymap.string(), "control+s = write\ncontrol+f = find\n");
    rows.row("control+f added, then control+f -> " +
             frame.cooked({key("control"), key("f")}, rows));
    Files::save(frame.keymap.string(), "control+s = write\ncontrol+f find\n");
    rows.row("then a line with no = -> " + frame.cooked({}, rows));
  });
}

} // namespace VP::bench
