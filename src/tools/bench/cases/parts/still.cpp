#include "still/Still.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/still: a frame of its picture held still. Space, while its host
// has the focus, or a double click in its area, holds the frame and either again lets it
// go; its host is its host param, or the node its held reaches. The frame stays held
// while what wrote it writes the picture, through its connection going and coming back;
// a change of the window's size lets go. Its shader copies only the frame a hold begins.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

Connection connection(std::string from_node,
                      std::string from_port,
                      std::string to_node,
                      std::string to_port) {
  return {.name = from_node + "." + from_port,
          .from = {.node = std::move(from_node), .port = std::move(from_port)},
          .to = {{.node = std::move(to_node), .port = std::move(to_port)}}};
}

Event key(bool down, std::string name = "space") {
  return {.kind = Event::Kind::key, .down = down, .name = std::move(name)};
}
Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event click(std::uint32_t presses) {
  return {.kind = Event::Kind::button, .down = true, .name = "left", .presses = presses};
}

// The node still in a view where win writes its picture and, with reaches, its held
// reaches img's picture; img in focus, its area 100 by 50 from the top left, the window
// 640 by 360.
struct Framed {
  Running<Still> node;
  FakeInput input;

  explicit Framed(Params params = {}, bool reaches = true) : node(std::move(params)) {
    node.bind.node_name = "still";
    node.bind.in_view.connections = {connection("win", "out", "still", "picture")};
    if (reaches)
      node.bind.in_view.connections.push_back(
          connection("still", "held", "img", "picture"));
    node.bound();
    node.cook.input_port = &input;
    node.cook.pass.resize(sizeof(std::uint32_t));
    node.cook.size = {640, 360};
    node.port<VP_VIEW::Rect>("area") = {.offset = {0, 0}, .extent = {100, 50}};
    node.port<VP_VIEW::Typed>("typed").focus = "img";
  }
  // A frame with these events, the keys the host's: whether it holds, whether its shader
  // copies, and whether it reads the picture.
  std::string cooked(std::vector<Event> events) {
    node.port<VP_VIEW::Typed>("typed").events = events;
    input.given = std::move(events);
    node.cook.unread_ports.clear();
    node.cooked();
    std::uint32_t shoot = 0;
    std::memcpy(&shoot, node.cook.pass.data(), sizeof shoot);
    return std::format("holds {}, shoots {}, unread {}",
                       node.port<VP_VIEW::Still>("still").holds,
                       shoot,
                       node.cook.unread_ports.empty() ? "none" : "picture");
  }
};

} // namespace

void cases(Bench &bench) {
  // Its host: the param, else the node its held reaches, else refused naming the node;
  // the host's space holds a frame, another node's does not.
  bench.check("Still.cpp/Still::bind", [](Rows &rows) {
    for (const Params &params : {Params{{"host", "pane"}}, Params{}})
      for (const bool reaches : {true, false}) {
        const std::string what = std::format("{}, held reaches {}",
                                             show(params, rows.names()),
                                             reaches ? "img" : "nothing");
        try {
          std::string held;
          for (const std::string_view focus : {"pane", "img"}) {
            Framed frame(params, reaches);
            frame.node.port<VP_VIEW::Typed>("typed").focus = focus;
            held += std::format(
                "; space in {}: {}", focus, frame.cooked({key(true), key(false)}));
          }
          rows.row(
              std::format("{} -> {}{}",
                          what,
                          show(Framed(params, reaches).node.bind.asked, rows.names()),
                          held));
        } catch (...) {
          rows.row(std::format("{} -> {}", what, thrown()));
        }
      }
  });

  // Space held and let go; a key repeated, in its frame and the frame after; another key;
  // keys while another node has the focus; double clicks in and out of its area; the
  // window resized while held; the picture's writer gone and back; no writer at all, and
  // the frame still held once the writer is back.
  bench.check("Still.cpp/Still::cook", [](Rows &rows) {
    Framed frame;
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events))));
    };
    step({}, "no events");
    step({key(true)}, "space down");
    step({key(true)}, "space repeated, the frame after");
    step({key(false)}, "space up");
    step({key(true), key(true), key(false)}, "space down, repeated, up");
    step({key(true), key(false)}, "space again");
    step({key(true, "a"), key(false, "a")}, "another key");
    frame.node.port<VP_VIEW::Typed>("typed").focus = "other";
    step({key(true), key(false)}, "space while other has the focus");
    frame.node.port<VP_VIEW::Typed>("typed").focus = "img";
    step({pointer({10, 10}), click(1)}, "a click in its area");
    step({pointer({10, 10}), click(2)}, "a double click in its area");
    step({pointer({200, 10}), click(2)}, "a double click out of it");
    step({pointer({10, 10}), click(2), click(2)}, "two double clicks in a frame");
    step({key(true), key(false)}, "space, before a resize");
    frame.node.cook.size = {800, 600};
    step({}, "the window resized while held");
    step({key(true), key(false)}, "space after");
    View &view = frame.node.bind.in_view;
    view.connections[0] = connection("other", "out", "still", "picture");
    step({}, "another node writes the picture");
    view.connections[0] = connection("win", "out", "still", "picture");
    step({}, "win writes it again");
    view.connections.erase(view.connections.begin());
    step({}, "nothing writes it");
    step({key(true), key(false)}, "nothing writes it, space");
    view.connections.insert(view.connections.begin(),
                            connection("win", "out", "still", "picture"));
    step({}, "win writes it once more");
  });
}

} // namespace VP::bench
