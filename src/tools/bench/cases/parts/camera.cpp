#include "camera/Camera.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The library's parts/camera: a camera turned and panned about a target with the pointer,
// a left drag turning the scene as a ball or level, a right or middle drag moving the
// target, the wheel bringing the eye nearer or farther, a double left press home, where
// its params put it; turning about up by itself every frames_per_turn frames. It writes
// one Camera a frame: the eye, the frame it looks along, the lens, near and far.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

constexpr double frames_a_second = 60.0;

// Its params as its view.vlp drops it, those given set otherwise.
Params dropped(const Params &changed = {}) {
  Params params{{"target", "0,0,0"},
                {"eye", "0,0,-2"},
                {"up", "0,-1,0"},
                {"lens", "2"},
                {"turn", "ball"},
                {"frames_per_turn", "0"},
                {"near", "0.01"},
                {"far", "1000"},
                {"double_click", "home"},
                {"glide_speed", "0"},
                {"ground_margin", "0.5"}};
  for (const auto &[key, value] : changed)
    for (auto &param : params)
      if (param.first == key)
        param.second = value;
  return params;
}

// A number to 4 places, as a turn leaves its last bits to the library's sine and cosine.
std::string rounded(float value) {
  constexpr float places = 1e4f;
  const float kept = std::round(value * places) / places;
  return number_text(kept == 0.0f ? 0.0f : kept);
}

std::string xyz(const glm::vec4 &vector) {
  return std::format(
      "({} {} {})", rounded(vector.x), rounded(vector.y), rounded(vector.z));
}

std::string camera_text(const VP_VIEW::Camera &camera) {
  return std::format("eye {} ahead {} right {} up {}; lens {} near {} far {}",
                     xyz(camera.eye),
                     xyz(camera.ahead),
                     xyz(camera.right),
                     xyz(camera.up),
                     rounded(camera.ahead.w),
                     rounded(camera.right.w),
                     rounded(camera.up.w));
}

Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event button(std::string name, bool down, std::uint32_t presses = 1) {
  return {.kind = Event::Kind::button,
          .down = down,
          .name = std::move(name),
          .presses = down ? presses : 0};
}
Event wheel(float steps) {
  return {.kind = Event::Kind::wheel, .turn = {0.0f, steps}};
}
Event key(std::string name, bool down) {
  return {.kind = Event::Kind::key, .down = down, .name = std::move(name)};
}
// A press at from, the pointer moved to to, let go: a drag of that button.
std::vector<Event> drag(std::string name, glm::vec2 from, glm::vec2 to) {
  return {pointer(from), button(name, true), pointer(to), button(std::move(name), false)};
}

// The node, its picture 200 by 100, so a drag across 100 pixels turns half a turn; with
// grounded, a connection hands its ground the ground the case sets.
struct Framed {
  Running<Camera> node;
  FakeInput input;

  explicit Framed(Params params, bool grounded = false) : node(std::move(params)) {
    node.bind.node_name = "camera";
    if (grounded)
      node.bind.in_view.connections = {{.name = "land",
                                        .from = {.node = "terrain", .port = "ground"},
                                        .to = {{.node = "camera", .port = "ground"}}}};
    node.bound();
    node.cook.input_port = &input;
    node.cook.size = {200, 100};
  }
  // The Camera a frame of these events writes, in the room its last bind asked for; the
  // frame's time as 60 frames a second count it.
  std::string cooked(std::vector<Event> events, std::uint64_t frame = 0) {
    input.given = std::move(events);
    node.cook.frame = frame;
    node.cook.clock = static_cast<double>(frame) / frames_a_second;
    node.cooked();
    return camera_text(
        *reinterpret_cast<const VP_VIEW::Camera *>(node.cook.written.back().data()));
  }
};

} // namespace

void cases(Bench &bench) {
  // Its params as dropped, and each it refuses: a lens of 0 or less, a word it does not
  // take, near not beyond 0 or far not beyond near, and eye, target and up that leave no
  // frame.
  bench.check("Camera.cpp/Camera::bind", [](Rows &rows) {
    const std::vector<std::pair<std::string_view, Params>> cases{
        {"as dropped", {}},
        {"lens 0", {{"lens", "0"}}},
        {"lens -1", {{"lens", "-1"}}},
        {"lens nan", {{"lens", "nan"}}},
        {"turn x", {{"turn", "x"}}},
        {"double_click x", {{"double_click", "x"}}},
        {"near 0", {{"near", "0"}}},
        {"far below near", {{"far", "0.001"}}},
        {"far at near", {{"far", "0.01"}}},
        {"eye on the target", {{"eye", "0,0,0"}}},
        {"up along the line of sight", {{"up", "0,0,1"}}},
        {"glide_speed -1", {{"glide_speed", "-1"}}},
        {"ground_margin -1", {{"ground_margin", "-1"}}},
        {"ground_margin 0", {{"ground_margin", "0"}}}};
    for (const auto &[what, changed] : cases) {
      Running<Camera> node(dropped(changed));
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", what, said));
    }
    Framed grounded(dropped(), true);
    rows.row("a ground connected -> " + show(grounded.node.bind.asked, rows.names()));
    Running<Camera> beside(dropped());
    beside.bind.node_name = "camera";
    beside.bind.in_view.connections = {{.name = "land",
                                        .from = {.node = "terrain", .port = "ground"},
                                        .to = {{.node = "mesh", .port = "ground"}}}};
    beside.bound();
    rows.row("another node's ground connected -> " +
             show(beside.bind.asked, rows.names()));
  });

  // Home; a left drag across turns the scene as a ball under the pointer rolls, half a
  // turn for the picture's shorter side; a right drag moves the target, the scene
  // following the pointer; the wheel; a double left press home again.
  bench.check("Camera.cpp/Camera::cook", [](Rows &rows) {
    Framed frame(dropped());
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events))));
    };
    step({}, "home");
    step(drag("left", {100, 50}, {150, 50}), "a left drag 50 pixels right");
    step({pointer({100, 50}), button("left", true, 2)}, "a double left press");
    step(drag("left", {100, 50}, {100, 75}), "a left drag 25 pixels down");
    step({pointer({100, 50}), button("left", true, 2), button("left", false)},
         "home again, after the turn");
    step(drag("right", {100, 50}, {150, 50}), "a right drag 50 pixels right");
    step(drag("middle", {150, 50}, {150, 0}), "a middle drag 50 pixels up");
    step({pointer({100, 50}), button("left", true, 2), button("left", false)},
         "home again, after the pan");
    step({wheel(1)}, "the wheel a step in");
    step({wheel(-2)}, "the wheel two steps out");
    step({wheel(100)}, "the wheel 100 steps in");
    step({wheel(-200)}, "the wheel 200 steps out");
    step({pointer({120, 60})}, "the pointer moved, no button down");
    step(drag("left", {120, 60}, {120, 60}), "a left drag that moves nowhere");
  });

  // Level turning: a drag across turns about up, a drag down tilts the eye up over the
  // target, never past straight down; with double_click none a double press turns too.
  bench.check("Camera.cpp/Camera::level", [](Rows &rows) {
    Framed frame(dropped({{"turn", "level"}, {"double_click", "none"}}));
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events))));
    };
    step(drag("left", {100, 50}, {150, 50}), "a left drag 50 pixels right");
    step(drag("left", {100, 50}, {100, 75}), "a left drag 25 pixels down");
    step(drag("left", {100, 50}, {100, 250}), "a left drag 200 pixels down");
    step(drag("left", {100, 50}, {100, -350}), "a left drag 400 pixels up");
    step({pointer({100, 50}),
          button("left", true, 2),
          pointer({150, 50}),
          button("left", false)},
         "a double left press dragged 50 pixels right");
  });

  // A whole turn about up every frames_per_turn frames, counted from the frame index;
  // none for 0.
  bench.check("Camera.cpp/Camera::spin_at", [](Rows &rows) {
    for (const std::string_view frames_per_turn : {"0", "4"}) {
      Framed frame(dropped({{"frames_per_turn", std::string(frames_per_turn)}}));
      for (const std::uint64_t index : {0u, 1u, 2u, 5u})
        rows.row(std::format("frames_per_turn {}, frame {} -> {}",
                             frames_per_turn,
                             index,
                             frame.cooked({}, index)));
    }
  });

  // Keys held glide the target, and the eye with it, the way the picture faces laid flat,
  // eased in and out at glide_speed units a second; two keys between their ways and no
  // faster, opposite keys cancel; the focus gone lets every key go; glide_speed 0 leaves
  // the keys to the view.
  bench.check("Camera.cpp/Camera::glide", [](Rows &rows) {
    for (const std::string_view speed : {"6", "0"}) {
      Framed frame(dropped({{"glide_speed", std::string(speed)}}));
      std::uint64_t at = 0;
      const auto step =
          [&](std::vector<Event> events, int frames, std::string_view what) {
            std::string said = frame.cooked(std::move(events), ++at);
            for (int more = 1; more < frames; ++more)
              said = frame.cooked({}, ++at);
            rows.row(std::format(
                "glide_speed {}, {}, {} frames -> {}", speed, what, frames, said));
          };
      step({}, 1, "no key");
      step({key("w", true)}, 1, "w down");
      step({}, 59, "w held");
      step({key("w", false)}, 60, "w let go");
      step({key("d", true), key("up", true)}, 60, "d and up held");
      step({key("d", false), key("up", false), key("a", true), key("right", true)},
           60,
           "a and right held");
      step({key("a", false), key("right", false), key("s", true)}, 30, "s held");
      step({{.kind = Event::Kind::focus, .down = false}}, 60, "the focus gone");
      step({key("w", true)}, 10, "w held again");
      step({{.kind = Event::Kind::focus, .down = true}},
           20,
           "the focus back, w still held");
      step({key("w", false)}, 60, "w let go again");
    }
  });

  // Tilted, it glides level, its eye keeping its height; on its side, its right along
  // up, the way its up laid flat points; spun by frames_per_turn, the way the spun
  // picture faces.
  bench.check("Camera.cpp/Camera::flat_ways", [](Rows &rows) {
    const std::vector<std::tuple<std::string_view, Params, std::vector<Event>>> cases{
        {"tilted 45 degrees",
         dropped({{"glide_speed", "6"}}),
         drag("left", {100, 50}, {100, 75})},
        {"on its side",
         dropped({{"glide_speed", "6"}}),
         {pointer({100, 50}),
          button("left", true),
          pointer({100, 100}),
          pointer({150, 100}),
          button("left", false)}},
        {"spun a turn in 120 frames",
         dropped({{"glide_speed", "6"}, {"frames_per_turn", "120"}}),
         {}}};
    for (const auto &[what, params, setup] : cases) {
      Framed frame(params);
      const std::string set = frame.cooked(setup, 0);
      std::string glided = frame.cooked({key("w", true)}, 1);
      for (std::uint64_t at = 2; at <= 30; ++at)
        glided = frame.cooked({}, at);
      rows.row(std::format("{}: {}; w held 30 frames: {}", what, set, glided));
    }
  });

  // Over a ground a connection hands it: the eye rides ground_margin over the ground
  // where it rises to that, and comes back down to its own height past it; the target
  // stops at the ground's edge and slides along it.
  bench.check("Camera.cpp/Camera::over_ground", [](Rows &rows) {
    // y up, the eye 1 over a flat ground of 9 by 9 points a unit apart from (-4, 0, -4),
    // a ridge 2 high along its row at z = -1, which the eye passes as w glides it ahead.
    Framed frame(dropped({{"target", "0,0,0"},
                          {"eye", "0,1,-3"},
                          {"up", "0,1,0"},
                          {"glide_speed", "2"},
                          {"ground_margin", "0.5"}}),
                 true);
    VP_VIEW::Ground &ground = frame.node.port<VP_VIEW::Ground>("ground");
    ground = {.heights = std::vector<float>(81, 0.0f),
              .size = {9, 9},
              .origin = {-4.0f, 0.0f, -4.0f},
              .across = {1.0f, 0.0f, 0.0f},
              .along = {0.0f, 0.0f, 1.0f}};
    for (std::size_t column = 0; column < 9; ++column)
      ground.heights[3 * 9 + column] = 2.0f;
    std::uint64_t at = 0;
    rows.row("home -> " + frame.cooked({}, at));
    frame.cooked({key("w", true)}, ++at);
    for (int quarter = 1; quarter <= 12; ++quarter) {
      std::string said;
      for (int more = 0; more < 15; ++more)
        said = frame.cooked({}, ++at);
      rows.row(std::format("w held {} frames -> {}", quarter * 15, said));
    }
    frame.cooked({key("d", true)}, ++at);
    std::string slid;
    for (int more = 0; more < 120; ++more)
      slid = frame.cooked({}, ++at);
    rows.row("then d held too, 120 frames, at the ground's edge -> " + slid);

    // The eye behind a plateau 2 high that starts at z = 0: past the edge it reads the
    // edge's height, so it rides over the plateau from behind it and crosses the edge
    // with no drop.
    Framed behind(dropped({{"target", "0,0,2"},
                           {"eye", "0,1,-1"},
                           {"up", "0,1,0"},
                           {"glide_speed", "2"},
                           {"ground_margin", "0.5"}}),
                  true);
    behind.node.port<VP_VIEW::Ground>("ground") = {.heights =
                                                       std::vector<float>(81, 2.0f),
                                                   .size = {9, 9},
                                                   .origin = {-4.0f, 0.0f, 0.0f},
                                                   .across = {1.0f, 0.0f, 0.0f},
                                                   .along = {0.0f, 0.0f, 1.0f}};
    std::uint64_t then = 0;
    behind.cooked({key("w", true)}, then);
    for (int half = 1; half <= 2; ++half) {
      std::string said;
      for (int more = 0; more < 30; ++more)
        said = behind.cooked({}, ++then);
      rows.row(std::format(
          "the eye behind a plateau, w held {} frames -> {}", half * 30, said));
    }
  });

  // A ground whose heights do not fill its grid, or whose axes span no plane square to
  // up, refused naming why as the camera glides over it.
  bench.check("Camera.cpp/Grid::Grid", [](Rows &rows) {
    const std::vector<std::pair<std::string_view, VP_VIEW::Ground>> grounds{
        {"3 heights for 2 by 2",
         {.heights = {0.0f, 0.0f, 0.0f},
          .size = {2, 2},
          .across = {1.0f, 0.0f, 0.0f},
          .along = {0.0f, 0.0f, 1.0f}}},
        {"no points",
         {.size = {0, 3}, .across = {1.0f, 0.0f, 0.0f}, .along = {0.0f, 0.0f, 1.0f}}},
        {"across along along",
         {.heights = {0.0f, 0.0f, 0.0f, 0.0f},
          .size = {2, 2},
          .across = {1.0f, 0.0f, 0.0f},
          .along = {2.0f, 0.0f, 0.0f}}},
        {"across along up",
         {.heights = {0.0f, 0.0f, 0.0f, 0.0f},
          .size = {2, 2},
          .across = {0.0f, 1.0f, 0.0f},
          .along = {0.0f, 0.0f, 1.0f}}}};
    for (const auto &[what, made] : grounds) {
      Framed frame(dropped({{"eye", "0,1,-3"}, {"up", "0,1,0"}, {"glide_speed", "2"}}),
                   true);
      frame.node.port<VP_VIEW::Ground>("ground") = made;
      std::string said;
      try {
        frame.cooked({key("w", true)}, 1);
        said = frame.cooked({}, 2);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{}, w held -> {}", what, said));
    }
    // With glide_speed 0 the keys are the view's, so the ground is not even read.
    Framed still(dropped({{"eye", "0,1,-3"}, {"up", "0,1,0"}}), true);
    still.node.port<VP_VIEW::Ground>("ground") = grounds[0].second;
    still.cooked({key("w", true)}, 1);
    rows.row("glide_speed 0, 3 heights for 2 by 2, w held -> " + still.cooked({}, 2));
  });

  // Bound again, as the engine binds every node of a view whose schedule it builds anew:
  // after any edit of the view, and as its picture moves between the window and a pane.
  bench.check("Camera.cpp/Camera::bind, bound again", [](Rows &rows) {
    for (const auto &[key, value] :
         Params{{"eye", "0,0,-2"}, {"lens", "3"}, {"eye", "0,0,-4"}}) {
      Framed frame(dropped());
      std::string turned = frame.cooked(drag("left", {100, 50}, {150, 50}));
      turned += "; " + frame.cooked({wheel(1)});
      frame.node.bind.params = dropped({{key, value}});
      frame.node.bound();
      rows.row(std::format("turned and nearer: {}; bound again, {} {}: {}",
                           turned,
                           key,
                           value,
                           frame.cooked({})));
    }
  });
}

} // namespace VP::bench
