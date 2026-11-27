#include "image/Image.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/image: a picture seated in its area, one instance, none while the
// area is empty; with moves=yes a drag and the wheel move and zoom it, h brings it home
// and escape lets the focus go; with interact=yes too, the pointer goes to the view whose
// window it shows instead. A still connected shows its held frame; a loader turns until
// the view whose window it shows has shown it.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event button(std::string name, bool down) {
  return {.kind = Event::Kind::button,
          .down = down,
          .name = std::move(name),
          .presses = down ? 1u : 0u};
}
Event wheel(float steps) {
  return {.kind = Event::Kind::wheel, .turn = {0.0f, steps}};
}
Event key(std::string name) {
  return {.kind = Event::Kind::key, .down = true, .name = std::move(name)};
}

// The view v, where the window of the view p reaches the image's picture, and, where
// held, a still's still reaches it too.
View view_of(bool held = false, std::string_view writer = "p") {
  View view;
  view.name = "v";
  view.connections.push_back({.name = "shown",
                              .from = {.node = std::string(writer)},
                              .to = {{.node = "image", .port = "picture"}}});
  if (held)
    view.connections.push_back({.name = "held",
                                .from = {.node = "still", .port = "still"},
                                .to = {{.node = "image", .port = "still"}}});
  return view;
}

// The node image, its area 100 by 50 at (10, 20), the window 400 by 200, its typed's
// focus on it, the view p hosted, a node draw in it, running and shown.
struct Framed {
  Running<Image> node;
  FakeInput input;
  FakeCommands commands;

  explicit Framed(Params params, View view = view_of()) : node(std::move(params)) {
    node.bind.node_name = "image";
    node.bind.in_view = std::move(view);
    node.bound();
    node.cook.input_port = &input;
    node.cook.command_port = &commands;
    node.cook.size = {400, 200};
    View shown;
    shown.nodes.emplace_back().name = "draw";
    node.cook.views = {{"p", std::move(shown)}};
    node.cook.view_costs = {{"p", {Cost{.node = "draw", .cooks = true}}}};
    node.port<VP_VIEW::Rect>("area") = {.offset = {10, 20}, .extent = {100, 50}};
    node.port<VP_VIEW::Typed>("typed").focus = "image";
  }
  // A frame of these events: its place, pan, zoom, held and loading, what it sent,
  // handed and said it reads none of.
  std::string cooked(std::vector<Event> events, Rows &rows) {
    input.given = events;
    node.port<VP_VIEW::Typed>("typed").events = std::move(events);
    commands.sent.clear();
    node.cook.handed.clear();
    node.cook.unread_ports.clear();
    node.cooked();
    const std::size_t places = node.cook.used.back();
    std::string place = "none";
    if (places != 0) {
      const auto &rect =
          *reinterpret_cast<const VP_VIEW::Rect *>(node.cook.written.back().data());
      place = std::format(
          "{},{} {}x{}", rect.offset.x, rect.offset.y, rect.extent.x, rect.extent.y);
    }
    const glm::vec2 pan = node.value<glm::vec2>("pan");
    return std::format("place {}; pan ({} {}) zoom {}; holds {} loading {}; sent {}; "
                       "handed {}; unread {}",
                       place,
                       number_text(pan.x),
                       number_text(pan.y),
                       number_text(node.value<float>("zoom")),
                       node.value<std::uint32_t>("holds"),
                       node.value<std::uint32_t>("loading"),
                       show(commands.sent, rows.names()),
                       show(node.cook.handed, rows.names()),
                       show(node.cook.unread_ports, rows.names()));
  }
};

} // namespace

void cases(Bench &bench) {
  // moves and interact yes or no; keys taken only where it moves, a still's held only
  // where one is connected.
  bench.check("Image.cpp/Image::bind", [](Rows &rows) {
    for (const auto &[params, held] : std::vector<std::pair<Params, bool>>{
             {{{"moves", "no"}, {"interact", "no"}}, false},
             {{{"moves", "yes"}, {"interact", "no"}}, false},
             {{{"moves", "no"}, {"interact", "no"}}, true},
             {{{"moves", "maybe"}, {"interact", "no"}}, false},
             {{{"moves", "yes"}, {"interact", "x"}}, false}}) {
      Running<Image> node(params);
      node.bind.node_name = "image";
      node.bind.in_view = view_of(held);
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{}{} -> {}",
                           show(params, rows.names()),
                           held ? ", a still connected" : "",
                           said));
    }
  });

  // Seated in its area, or none and its picture unread while the area is empty; a still
  // holding; the loader while the view it shows waits for its build, or has not run.
  bench.check("Image.cpp/Image::cook", [](Rows &rows) {
    Framed frame({{"moves", "no"}, {"interact", "no"}}, view_of(true));
    rows.row("seated -> " + frame.cooked({}, rows));
    frame.node.port<VP_VIEW::Rect>("area") = {};
    rows.row("no room -> " + frame.cooked({}, rows));
    frame.node.port<VP_VIEW::Rect>("area") = {.offset = {10, 20}, .extent = {100, 50}};
    frame.node.port<VP_VIEW::Still>("still").holds = true;
    rows.row("a still holding -> " + frame.cooked({}, rows));
    Framed waiting({{"moves", "no"}, {"interact", "no"}});
    waiting.node.cook.view_costs = {
        {"p", {Cost{.node = "draw", .problem = "is not built yet"}}}};
    waiting.node.cook.view_costs["p"].front().waits = true;
    rows.row("its view waiting for its build -> " + waiting.cooked({}, rows));
    waiting.node.cook.view_costs = {{"p", {Cost{.node = "draw", .cooks = true}}}};
    rows.row("then built and run -> " + waiting.cooked({}, rows));
    waiting.node.cook.view_costs = {
        {"p", {Cost{.node = "draw", .problem = "is not built yet"}}}};
    waiting.node.cook.view_costs["p"].front().waits = true;
    rows.row("then rebuilding, shown once -> " + waiting.cooked({}, rows));
    Framed unhosted({{"moves", "no"}, {"interact", "no"}}, view_of(false, "q"));
    rows.row("its writer a view not hosted -> " + unhosted.cooked({}, rows));
    Framed empty({{"moves", "no"}, {"interact", "no"}});
    empty.node.cook.views = {{"p", View{}}};
    empty.node.cook.view_costs = {{"p", {}}};
    rows.row("its writer a view of no nodes -> " + empty.cooked({}, rows));
  });

  // With moves: a left drag in its area pans it, the press giving it the focus where
  // another node had it; the wheel zooms about the pointer, a middle drag sideways about
  // where it was pressed; h home; escape lets the focus go; presses past the area move
  // nothing.
  bench.check("Image.cpp/Image::move", [](Rows &rows) {
    Framed frame({{"moves", "yes"}, {"interact", "no"}});
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events), rows)));
    };
    frame.node.port<VP_VIEW::Typed>("typed").focus = "other";
    step({pointer({20, 30}),
          button("left", true),
          pointer({40, 35}),
          button("left", false)},
         "a left drag in its area, another node in focus");
    frame.node.port<VP_VIEW::Typed>("typed").focus = "image";
    step({pointer({60, 45}), wheel(1)}, "the wheel a step in, at its middle");
    step({pointer({60, 45}),
          button("middle", true),
          pointer({100, 45}),
          button("middle", false)},
         "a middle drag 40 pixels right");
    step({pointer({300, 150}), button("left", true), pointer({320, 150}), wheel(1)},
         "a press and the wheel past its area");
    step({button("left", false), key("h")}, "h");
    step({key("escape")}, "escape");
  });

  // With interact: a press in its area takes the focus; then, from the frame after, the
  // pointer is handed to the view whose window it shows, while over the area or a press
  // made there is held; not where its picture is no view's window.
  bench.check("Image.cpp/Image::hand", [](Rows &rows) {
    Framed frame({{"moves", "yes"}, {"interact", "yes"}});
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events), rows)));
    };
    frame.node.port<VP_VIEW::Typed>("typed").focus = "other";
    step({pointer({20, 30}), button("left", true)}, "a press in its area, not in focus");
    frame.node.port<VP_VIEW::Typed>("typed").focus = "image";
    step({pointer({25, 30})}, "in focus, the press held");
    step({pointer({300, 150})}, "dragged past its area, still held");
    step({button("left", false)}, "let go past its area");
    step({pointer({30, 30})}, "over its area again");
    Framed node_writer({{"moves", "yes"}, {"interact", "yes"}}, View{});
    node_writer.node.bind.in_view.connections.push_back(
        {.name = "drawn",
         .from = {.node = "draw", .port = "picture"},
         .to = {{.node = "image", .port = "picture"}}});
    rows.row("its picture a node's, over its area -> " +
             node_writer.cooked({pointer({30, 30})}, rows));
  });

  // A picture whose writer changes starts home; one that goes and comes back from the
  // same writer stays where it was moved.
  bench.check("Image.cpp/Image::follow", [](Rows &rows) {
    Framed frame({{"moves", "yes"}, {"interact", "no"}});
    frame.cooked({pointer({20, 30}),
                  button("left", true),
                  pointer({40, 30}),
                  button("left", false)},
                 rows);
    rows.row("moved -> " + frame.cooked({}, rows));
    frame.node.bind.in_view.connections.clear();
    rows.row("its picture's connection gone -> " + frame.cooked({}, rows));
    frame.node.bind.in_view = view_of();
    rows.row("back from the same writer -> " + frame.cooked({}, rows));
    frame.node.bind.in_view = view_of(false, "q");
    rows.row("another writer -> " + frame.cooked({}, rows));
  });
}

} // namespace VP::bench
