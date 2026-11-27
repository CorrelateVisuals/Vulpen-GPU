#include "hit/Hit.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/hit: the Rect under the pointer, and a press there sends the
// command of the Item shown in it, or its close command on its close mark; the pointer
// coming onto an Item not current sends its hover; an Item dragged onto another of the
// same move sends its move with the other's last word; a right press sends its menu. A
// press goes through the terminal's type, where one is registered, unless typed=no.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event button(std::string name, bool down) {
  return {.kind = Event::Kind::button, .down = down, .name = std::move(name)};
}

// Four tabs in a row, 20 high: a, with a close mark, a move, a menu and a hover, 0 to 50;
// b current, 50 to 100; c, with no command and another kind of move, 100 to 150; d, drawn
// over c's last 10 pixels, 140 to 160.
struct Framed {
  Running<Hit> node;
  FakeInput input;
  FakeCommands commands;

  explicit Framed(Params params) : node(std::move(params)) {
    node.bound();
    node.cook.input_port = &input;
    node.cook.command_port = &commands;
    node.port<VP_VIEW::Items>("items") = {{.label = "a",
                                           .command = "open a",
                                           .close = "close a",
                                           .move = "tab move a",
                                           .menu = "menu a",
                                           .hover = "peek a"},
                                          {.label = "b",
                                           .command = "open b",
                                           .current = true,
                                           .move = "tab move b",
                                           .hover = "peek b"},
                                          {.label = "c", .move = "other move c"},
                                          {.label = "d", .command = "open d"}};
    node.port<VP_VIEW::Rects>("rects") = {{.offset = {0, 0}, .extent = {50, 20}},
                                          {.offset = {50, 0}, .extent = {50, 20}},
                                          {.offset = {100, 0}, .extent = {50, 20}},
                                          {.offset = {140, 0}, .extent = {20, 20}}};
    node.port<VP_VIEW::Rects>("marks") = {{.offset = {35, 5}, .extent = {10, 10}}};
  }
  // What a frame of these events sent.
  std::string cooked(std::vector<Event> events, Rows &rows) {
    input.given = std::move(events);
    commands.sent.clear();
    try {
      node.cooked();
    } catch (...) {
      return thrown();
    }
    return "sent " + show(commands.sent, rows.names());
  }
};

} // namespace

void cases(Bench &bench) {
  bench.check("Hit.cpp/Hit::bind", [](Rows &rows) {
    for (const std::string_view typed : {"yes", "no", "maybe", ""}) {
      Running<Hit> node({{"typed", std::string(typed)}});
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("typed {} -> {}", typed, said));
    }
  });

  // The pointer over the tabs, presses, a close mark, drags, right presses and the tab
  // drawn over another, with a terminal's type registered.
  bench.check("Hit.cpp/Hit::cook", [](Rows &rows) {
    Framed frame({{"typed", "yes"}});
    frame.commands.listed = {"type <line>", "node add <name>"};
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events), rows)));
    };
    const auto left = [](bool down) { return button("left", down); };
    step({pointer({10, 10})}, "onto a");
    step({pointer({12, 10})}, "on a still");
    step({pointer({60, 10})}, "onto b, current");
    step({pointer({300, 300})}, "onto nothing");
    step({pointer({10, 10})}, "onto a again");
    step({left(true), left(false)}, "a pressed and let go");
    step({pointer({40, 10}), left(true), left(false)}, "a's close mark pressed");
    step({pointer({10, 10}), left(true), pointer({60, 10}), left(false)},
         "a dragged onto b");
    step({pointer({10, 10}), left(true), pointer({120, 10}), left(false)},
         "a dragged onto c, another kind of move");
    step({pointer({10, 10}), left(true), pointer({15, 10}), left(false)},
         "a pressed and moved 5 pixels");
    step({left(false)}, "let go with nothing pressed");
    step({pointer({10, 10}), button("right", true)}, "a pressed right");
    step({pointer({60, 10}), button("right", true)}, "b pressed right, no menu");
    step({pointer({145, 10}), left(true), left(false)}, "where d is drawn over c");
    step({pointer({110, 10}), left(true), left(false)}, "c, with no command");
    step({pointer({300, 300}), left(true), left(false)}, "nothing pressed");
  });

  // The tabs changed while the button is down, as a press on a close mark closes its
  // tab the frame after: c's close mark pressed, the strip then without c, the pointer
  // dragged onto a and let go; and c itself pressed, then the same.
  bench.check("Hit.cpp/Hit::release, the Items changed under a press", [](Rows &rows) {
    for (const bool on_mark : {true, false}) {
      Framed frame({{"typed", "no"}});
      VP_VIEW::Items &items = frame.node.port<VP_VIEW::Items>("items");
      VP_VIEW::Rects &rects = frame.node.port<VP_VIEW::Rects>("rects");
      items = {{.label = "a", .move = "tab move a"},
               {.label = "b", .move = "tab move b"},
               {.label = "c", .close = "close c", .move = "tab move c"},
               {.label = "d", .move = "tab move d"}};
      rects = {{.offset = {0, 0}, .extent = {50, 20}},
               {.offset = {50, 0}, .extent = {50, 20}},
               {.offset = {100, 0}, .extent = {50, 20}},
               {.offset = {150, 0}, .extent = {50, 20}}};
      frame.node.port<VP_VIEW::Rects>("marks") = {
          {}, {}, {.offset = {135, 5}, .extent = {10, 10}}};
      const std::string pressed =
          frame.cooked({pointer(on_mark ? glm::vec2(140, 10) : glm::vec2(110, 10)),
                        button("left", true)},
                       rows);
      items.erase(items.begin() + 2);
      rects.pop_back();
      rows.row(
          std::format("{} pressed -> {}; c gone, dragged onto a and let go -> {}",
                      on_mark ? "c's close mark" : "c",
                      pressed,
                      frame.cooked({pointer({10, 10}), button("left", false)}, rows)));
    }
  });

  // Where a press goes: through type with a terminal's registered; as it is with none,
  // or with typed=no, which asks the port for no usages.
  bench.check("Hit.cpp/Hit::send", [](Rows &rows) {
    for (const auto &[typed, listed] :
         std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"yes", {"type <line>"}}, {"yes", {"types <line>"}}, {"yes", {}}}) {
      Framed frame({{"typed", typed}});
      frame.commands.listed = listed;
      rows.row(
          std::format("typed {}, usages {} -> {}",
                      typed,
                      show(listed, rows.names()),
                      frame.cooked({pointer({10, 10}), button("left", true)}, rows)));
    }
    Framed frame({{"typed", "no"}});
    rows.row("typed no, no usages listed -> " +
             frame.cooked({pointer({10, 10}), button("left", true)}, rows));
  });
}

} // namespace VP::bench
